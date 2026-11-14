"""A browser plays a served document's Animation by itself.

docs/Animation.md sec 9, "Streaming", the drawing half
(tests/gui/serve-animation.py reads the wire with a synthetic client). The
host plays an Animation that moves a red box past a green one; it sends
the viewer a clip and its clock and then publishes nothing for the move.
No synthetic client can see what that is for: the box moving on a canvas
that was sent no frame.

The reading is pixels by colour and what the viewer says it is playing.
What is asserted:

  - at rest the viewer draws both boxes and plays nothing;
  - playing, it holds the clip and the red box is drawn somewhere else
    from reading to reading while the green one stays;
  - all on one publish: the scene's version does not move as the box does;
  - paused, the box stays where it is, at the host's Time, on a publish
    taken there;
  - stopped, the red box is back where it rested.

Needs the built viewer (build/wasm/fcviewer.html; `cmake --build
build/wasm`), node, a puppeteer-core install named by PUPPETEER_PATH, and
a Chrome named by CHROME. Skips rather than fails when any is missing.

  PUPPETEER_PATH=/path/to/node_modules/puppeteer-core \
  CHROME=/path/to/chrome \
  scripts/gui-test.sh tests/gui/serve-animation-browser.py \
      /tmp/anim-web --timeout 600
"""
import functools
import http.server
import json as jsonlib
import os
import subprocess
import threading
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

from wsclient import free_port

# The platform's backend, as a user gets it: Direct3D 11 on Windows, Metal
# on macOS, OpenGL elsewhere (RenderParams::engineType, docs/Testing.md).
ENGINE = "Default"

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WASM = os.path.join(REPO, "build", "wasm")
DRIVER = os.path.join(REPO, "scripts", "animation-drive.js")
DOC = "ServeAnimationBrowser"
SETTLE_MS = 9000
RUN_WAIT_S = 300

state = {"doc": None, "done": False, "run": None, "t0": clock(), "acted": set(), "host": {}}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def skip(why):
    note("SKIP " + why)
    finish()


class Run(threading.Thread):
    """The browser, driven from off the GUI thread. Log and phase markers
    go to files: node buffers stdout to a pipe."""

    def __init__(self, url):
        super().__init__(daemon=True)
        self.url = url
        self.error = None
        self.log_path = os.path.join(OUT, "drive.log")
        self.phase_path = os.path.join(OUT, "phases.txt")
        self.result_path = os.path.join(OUT, "drive-result.json")

    def run(self):
        try:
            env = dict(os.environ)
            env["ANIMATION_PHASES"] = self.phase_path
            env["ANIMATION_RESULT"] = self.result_path
            with open(self.log_path, "w") as log:
                proc = subprocess.Popen(
                    [os.environ.get("NODE", "node"), DRIVER, self.url, str(SETTLE_MS)],
                    stdout=log, stderr=subprocess.STDOUT, env=env)
                proc.wait()
            if proc.returncode != 0:
                self.error = "node exited %d:\n%s" % (proc.returncode, self.tail())
        except Exception:
            self.error = traceback.format_exc()

    def phases(self):
        try:
            with open(self.phase_path) as f:
                return f.read().split()
        except OSError:
            return []

    def tail(self, lines=25):
        try:
            with open(self.log_path) as f:
                return "".join(f.readlines()[-lines:])
        except OSError:
            return "(no driver log)"

    def result(self):
        try:
            with open(self.result_path) as f:
                return jsonlib.load(f)
        except Exception:
            return {}


def serve_viewer():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=WASM)
    handler.log_message = lambda *a, **k: None
    port = free_port()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    state["httpd"] = server
    return port


def build():
    try:
        if not os.path.exists(os.path.join(WASM, "fcviewer.html")):
            return skip("no built viewer at %s (cmake --build build/wasm)" % WASM)
        if not os.environ.get("PUPPETEER_PATH"):
            return skip("PUPPETEER_PATH is unset (a puppeteer-core install)")
        if not os.environ.get("CHROME"):
            return skip("CHROME is unset (a Chrome executable)")

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE)
        FreeCADGui.getMainWindow().resize(1300, 900)
        # Not hidden: the Play command plays the active document's Animation
        doc = FreeCAD.newDocument(DOC)
        state["doc"] = doc
        boxes = (("Box", 0, (1.0, 0.0, 0.0)), ("Still", 40, (0.0, 1.0, 0.0)))
        for name, x, colour in boxes:
            box = doc.addObject("Part::Box", name)
            box.Length = box.Width = box.Height = 10
            box.Placement.Base = FreeCAD.Vector(x, 0, 0)
        doc.recompute()
        for name, x, colour in boxes:
            vo = doc.getObject(name).ViewObject
            vo.ShapeColor = colour
            vo.LineColor = colour
            vo.PointColor = colour
        anim = doc.addObject("App::Animation", "Anim")
        anim.FrameRate = 20
        anim.Loop = "PingPong"
        anim.addKey("Box.Placement", 0.0)
        anim.addKey("Box.Placement", 3.0,
                    FreeCAD.Placement(FreeCAD.Vector(25, 0, 0), FreeCAD.Rotation()))
        doc.recompute()
        state["anim"] = anim

        port = free_port()
        if not check("the document is served", FreeCADGui.serveDocument(doc, port),
                     "port %d" % port):
            return finish()
        http_port = serve_viewer()
        note("viewer on %d, scene on %d" % (http_port, port))
        url = ("http://127.0.0.1:%d/fcviewer.html?scene=http://127.0.0.1:%d"
               % (http_port, port))
        state["run"] = Run(url)
        state["t0"] = clock()
        state["run"].start()
        QtCore.QTimer.singleShot(50, poll)
    except Exception:
        note("FAIL build:\n" + traceback.format_exc())
        finish()


def act(phases):
    """The host's half, as the browser's phases pass"""
    anim = state["anim"]
    host = state["host"]
    for name in phases:
        if name in state["acted"]:
            continue
        state["acted"].add(name)
        if name == "ready":
            FreeCADGui.runCommand("Std_AnimationPlay")
        elif name == "played":
            host["moved"] = anim.Document.getObject("Box").Placement.Base.x
            FreeCADGui.runCommand("Std_AnimationPause")
            host["paused"] = anim.Time.Value
        elif name == "paused":
            FreeCADGui.runCommand("Std_AnimationStop")
            host["stopped"] = anim.Document.getObject("Box").Placement.Base.x


def poll():
    run = state["run"]
    try:
        act(run.phases())
    except Exception:
        note("FAIL the host's half:\n" + traceback.format_exc())
    if run.is_alive():
        if clock() - state["t0"] > RUN_WAIT_S:
            check("the browser finished", False,
                  "still running after %ds\n%s" % (RUN_WAIT_S, run.tail()))
            return finish()
        QtCore.QTimer.singleShot(50, poll)
        return
    QtCore.QTimer.singleShot(700, verify)


def spot(reading, colour):
    c = ((reading or {}).get("colours") or {}).get(colour) or {}
    return c.get("n", 0), c.get("x", 0.0), c.get("y", 0.0)


def apart(a, b):
    return ((a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5


def verify():
    run = state["run"]
    out = run.result()
    host = state["host"]
    try:
        if not check("the browser ran without error", run.error is None, run.error or ""):
            note(run.tail())
        check("the driver reported no error", not out.get("error"), out.get("error", ""))
        note("host: %s" % jsonlib.dumps(host))
        for key in ("rest", "paused1", "paused2", "stopped"):
            note("%s: %s" % (key, jsonlib.dumps(out.get(key))))
        samples = out.get("samples") or []
        for i, s in enumerate(samples):
            note("play%d: %s" % (i, jsonlib.dumps(s)))

        rest = out.get("rest")
        red0, green0 = spot(rest, "red"), spot(rest, "green")
        check("at rest the viewer draws both boxes", min(red0[0], green0[0]) > 200,
              (red0, green0))
        rs = (rest or {}).get("state") or {}
        check("and plays nothing", rs and not rs.get("plays") and rs.get("clip") == 0, rs)

        ps = out.get("playing") or {}
        check("playing, the viewer holds the clip and its clock",
              ps.get("plays") and ps.get("clip", 0) > 0 and ps.get("clock") == ps.get("clip")
              and ps.get("tracks", 0) >= 1 and ps.get("frames") == 61, ps)
        reds = [spot(s, "red") for s in samples]
        greens = [spot(s, "green") for s in samples]
        steps = [apart(reds[i], reds[i + 1]) for i in range(len(reds) - 1)]
        check("the red box is drawn somewhere else from reading to reading",
              len(steps) >= 5 and sum(1 for d in steps if d > 2.0) >= len(steps) - 2
              and max(apart(r, red0) for r in reds) > 20.0, [round(d, 1) for d in steps])
        check("the green one stays", all(apart(g, green0) < 2.0 for g in greens),
              [round(apart(g, green0), 1) for g in greens])
        versions = sorted({(s.get("state") or {}).get("version") for s in samples})
        check("all on one publish", len(versions) == 1 and versions[0], versions)
        poses = [(s.get("state") or {}).get("poses", 0) for s in samples]
        check("the viewer posing it frame after frame",
              poses and poses[-1] - poses[0] >= 20, poses)
        check("while the host's box really moved", host.get("moved", 0.0) > 0.5, host)

        p1, p2 = out.get("paused1"), out.get("paused2")
        s1 = (p1 or {}).get("state") or {}
        check("paused, the box stays where it is", apart(spot(p1, "red"), spot(p2, "red")) < 1.0
              and spot(p1, "red")[0] > 200, (spot(p1, "red"), spot(p2, "red")))
        check("at the host's Time", "paused" in host
              and abs(s1.get("time", -1) - host["paused"]) < 1e-6, (s1.get("time"), host))
        check("on a publish taken there",
              s1.get("version", 0) > (versions[0] or 0) and s1.get("based")
              and abs(s1.get("base", -1) - host.get("paused", -2)) < 1e-6, s1)

        st = out.get("stopped")
        ss = (st or {}).get("state") or {}
        check("stopped, the host's box is as designed", abs(host.get("stopped", 1.0)) < 1e-9,
              host)
        check("and the viewer's is back where it rested",
              apart(spot(st, "red"), red0) < 2.0 and not ss.get("plays")
              and ss.get("moved") == 0, (spot(st, "red"), red0, ss))
    except Exception:
        note("ABORT verify:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        state["httpd"].shutdown()
    except Exception:
        pass
    try:
        FreeCADGui.runCommand("Std_AnimationStop")
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


# Deferred: a script handed to FreeCAD runs before the event loop is up.
QtCore.QTimer.singleShot(1500, build)
