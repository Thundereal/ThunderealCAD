"""A browser plays by itself an Animation that rebuilds a shape and hides one.

docs/Animation.md sec 9.1, the drawing half for what is not a rigid move
(tests/gui/serve-animation.py reads the same on the wire). The host's
Animation shrinks a red box -- its Height, so the shape is rebuilt every
frame -- and shows a green one only from 1 s to 2 s. Neither is a move:
the red box is a sequence, its draws at every frame a blob the viewer
fetches, and the green box a track that says when it is shown, with its
draws kept. The publish the viewer holds is taken as the player starts,
while the green box is hidden: what it draws of it comes from the blob.
A blue box stands still beyond them, so the bounds the viewer frames its
camera on are the same with the green one or without; its colour is keyed
from blue to yellow, which is not a move either but a look: a record a
frame on its track, set by the viewer on the draws it holds.

The reading is pixels by colour and what the viewer says it is playing.
What is asserted:

  - at rest the viewer draws both boxes and plays nothing;
  - playing, it holds the clip, a sequence and a track, every blob in;
  - the red box is drawn smaller as the Time goes on, all on one publish;
  - the green box is drawn between 1 s and 2 s and nowhere else;
  - the blue box is blue early and yellow late, its looks set by the
    viewer;
  - paused, the red box stays as it is, at the host's Time;
  - stopped, both are back as they rested.

Needs the built viewer (build/wasm/fcviewer.html; `cmake --build
build/wasm`), node, a puppeteer-core install named by PUPPETEER_PATH, and
a Chrome named by CHROME.

  PUPPETEER_PATH=/path/to/node_modules/puppeteer-core \
  CHROME=/path/to/chrome \
  scripts/gui-test.sh tests/gui/serve-animation-sequence-browser.py \
      /tmp/anim-seq-web --timeout 600
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
DOC = "ServeAnimationSequence"
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
            env.setdefault("ANIMATION_SAMPLES", "16")
            env.setdefault("ANIMATION_GAP_MS", "120")
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
        boxes = (("Box", 0, (1.0, 0.0, 0.0)), ("Lamp", 30, (0.0, 1.0, 0.0)),
                 ("Still", 60, (0.0, 0.0, 1.0)))
        for name, x, colour in boxes:
            box = doc.addObject("Part::Box", name)
            box.Length = box.Width = 10
            box.Height = 30
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
        # Slow: a screenshot under swiftshader takes the best part of a
        # second, and a reading has to say which frame it caught
        anim.Speed = 0.25
        anim.addKey("Box.Height", 0.0, 30.0)
        anim.addKey("Box.Height", 3.0, 5.0)
        anim.addKey("Lamp.Visibility", 0.0, False)
        anim.addKey("Lamp.Visibility", 1.0, True)
        anim.addKey("Lamp.Visibility", 2.0, False)
        # Not a move either: the blue box turns yellow, its looks a record
        # a frame on its track
        anim.addKey("Still.ViewObject.ShapeColor", 0.0, (0.0, 0.0, 1.0))
        anim.addKey("Still.ViewObject.ShapeColor", 3.0, (1.0, 1.0, 0.0))
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
            host["moved"] = anim.Document.getObject("Box").Height.Value
            FreeCADGui.runCommand("Std_AnimationPause")
            host["paused"] = anim.Time.Value
        elif name == "paused":
            FreeCADGui.runCommand("Std_AnimationStop")
            host["stopped"] = anim.Document.getObject("Box").Height.Value


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
        check("playing, the viewer holds the clip: a sequence and a track",
              ps.get("plays") and ps.get("clip", 0) > 0 and ps.get("clock") == ps.get("clip")
              and ps.get("sequences") == 1 and ps.get("tracks") == 2
              and ps.get("lookTracks") == 1 and ps.get("frames") == 61, ps)
        states = [s.get("state") or {} for s in samples]
        # Only the readings the shot of which is of the Time they say
        # (the earliest and the latest Time the shot can be of): the two
        # states, while they are near enough to say something
        caught = []
        for st, s in zip(states, samples):
            a, b = st.get("time", 0.0), (s.get("after") or {}).get("time", 99.0)
            if abs(b - a) < 0.3:
                caught.append(((min(a, b), max(a, b)), s))
        note("readings of a known Time: %d of %d" % (len(caught), len(samples)))
        check("with every blob in",
              states and states[-1].get("blobs", 0) > 30
              and states[-1].get("blobsIn") == states[-1].get("blobs"), states[-1:])
        timed = sorted((round((lo + hi) / 2, 3), spot(s, "red")[0]) for (lo, hi), s in caught)
        early = [n for t, n in timed if t < 0.9]
        late = [n for t, n in timed if t > 2.1]
        check("the red box is drawn smaller as the Time goes on",
              early and late and min(early) > 1.4 * max(late) and max(late) > 100, timed)
        # Wherever the Time is, the drawn size follows it: one size a time
        pairs = [(timed[i], timed[i + 1]) for i in range(len(timed) - 1)
                 if timed[i + 1][0] - timed[i][0] > 0.1]
        check("and only as the Time does",
              pairs and all(b[1] < a[1] for a, b in pairs), pairs)
        greens = [(lo, hi, spot(s, "green")[0]) for (lo, hi), s in caught]
        inside = [n for lo, hi, n in greens if 1.05 < lo and hi < 1.95]
        outside = [n for lo, hi, n in greens if hi < 0.95 or lo > 2.05]
        check("the green box is drawn between 1 s and 2 s",
              inside and all(n > 200 for n in inside), greens)
        # The navigation cube's Y axis is green too, some 60 pixels of it
        check("and nowhere else", outside and all(n < 200 for n in outside), greens)
        # Read by its yellow: the background's gradient is blue too
        yellows = sorted((round((lo + hi) / 2, 3), spot(s, "yellow")[0]) for (lo, hi), s in caught)
        early = [y for t, y in yellows if t < 0.75]
        late = [y for t, y in yellows if t > 2.25]
        check("the blue box turns yellow as the Time goes on",
              early and late and all(y < 50 for y in early) and all(y > 200 for y in late),
              yellows)
        check("its looks set by the viewer, not sent",
              any((st.get("looked") or 0) > 0 for st in states), [st.get("looked") for st in states])
        check("from its kept draws: the publish held lacks it",
              any(st.get("swapped", 0) > 0 and 1.1 < st.get("time", 0) < 1.9 for st in states)
              and all(st.get("based") and st.get("base", -1) < 1e-6 for st in states), states)
        versions = sorted({st.get("version") for st in states})
        check("all on one publish", len(versions) == 1 and versions[0], versions)
        check("while the host's box really changed", 5.0 < host.get("moved", 0.0) < 30.0, host)

        p1, p2 = out.get("paused1"), out.get("paused2")
        s1 = (p1 or {}).get("state") or {}
        check("paused, the red box stays as it is",
              abs(spot(p1, "red")[0] - spot(p2, "red")[0]) < 50 and spot(p1, "red")[0] > 100,
              (spot(p1, "red"), spot(p2, "red")))
        check("at the host's Time", "paused" in host
              and abs(s1.get("time", -1) - host["paused"]) < 1e-6, (s1.get("time"), host))

        st = out.get("stopped")
        ss = (st or {}).get("state") or {}
        check("stopped, the host's box is as designed", abs(host.get("stopped", 0) - 30.0) < 1e-9,
              host)
        check("and the viewer draws both as they rested",
              abs(spot(st, "red")[0] - red0[0]) < 0.05 * red0[0]
              and abs(spot(st, "green")[0] - green0[0]) < 0.05 * green0[0]
              and not ss.get("plays"), (spot(st, "red"), spot(st, "green"), red0, green0))
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
