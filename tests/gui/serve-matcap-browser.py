"""The surface analysis matcaps in a browser (docs/Surface.md sec 7, U4).

Curvature, draft and zebra are matcap presets of the renderer; a served
document's view keeps them in its Render_Matcap* properties, and the
snapshot carries them to the page (SceneDump v90), whose wasm viewer draws
them with its own shaders. A sphere of radius 10 and a box beside it are served; at each
phase the page sets the matcap on the served view, as its property editor
does, and the next publish carries it. The page's canvas is counted by
colour (scripts/matcap-drive.js):

  studio     the baseline: little red, green or yellow;
  curvature  mean, saturating at 1.5/R: the sphere warm (orange, its
             curvature two thirds of the range), the box's flat faces
             green;
  draft      a least draft of 1 deg along +Z: the faces up green, the
             upright sides yellow;
  zebra      dark and light stripes, a row turning between them often.

The whole scene is waited for first -- six draws, none of them a bounding
box, read off the viewer's own streaming account (?stream) -- and its
never coming is a failure of its own (docs/Testing.md).

Needs the built viewer (build/wasm/fcviewer.html), node, a puppeteer-core
install named by PUPPETEER_PATH, and a Chrome named by CHROME (CHROME_LIBS
for its libraries, docs/Testing.md). Skips rather than fails when any is
missing.
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

import wsclient
from wsclient import free_port

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WASM = os.path.join(REPO, "build", "wasm")
DRIVER = os.path.join(REPO, "scripts", "matcap-drive.js")
DOC = "ServeMatcapBrowser"
# how long the whole scene has to arrive: the sphere streams in as its
# bounding box first, its mesh some 5 to 10 s later
SETTLE_MS = int(os.environ.get("MATCAP_SETTLE", "60000"))
RUN_WAIT_S = 300
TOKEN = "matcap-browser-invite"
CLIENT = "matcap-browser"
R = 10.0
RENDER = "User parameter:BaseApp/Preferences/View/Render"

state = {"doc": None, "done": False, "run": None, "t0": clock(), "seen_phases": 0}


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
    """The browser, driven from off the GUI thread"""

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
            env["LD_LIBRARY_PATH"] = (
                (env["CHROME_LIBS"] + os.pathsep if env.get("CHROME_LIBS") else "")
                + os.path.join(REPO, ".conda", "freecad", "lib")
                + os.pathsep
                + env.get("LD_LIBRARY_PATH", "")
            )
            env["MATCAP_PHASES"] = self.phase_path
            env["MATCAP_RESULT"] = self.result_path
            env.setdefault("MATCAP_SHOTS", OUT)
            with open(self.log_path, "w") as log:
                proc = subprocess.Popen(
                    ["node", DRIVER, self.url, str(SETTLE_MS)],
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    env=env,
                )
                proc.wait()
            if proc.returncode != 0:
                self.error = "node exited %d:\n%s" % (proc.returncode, self.tail())
        except Exception:
            self.error = traceback.format_exc()

    def tail(self, lines=25):
        try:
            with open(self.log_path) as f:
                return "".join(f.readlines()[-lines:])
        except OSError:
            return "(no driver log)"

    def phases(self):
        try:
            with open(self.phase_path) as f:
                return [line.strip() for line in f if line.strip()]
        except OSError:
            return []

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
            skip("no built viewer at %s (ninja -C build/wasm)" % WASM)
            return
        if not os.environ.get("PUPPETEER_PATH"):
            skip("PUPPETEER_PATH is unset (a puppeteer-core install)")
            return
        if not os.environ.get("CHROME"):
            skip("CHROME is unset (a Chrome executable)")
            return

        p = FreeCAD.ParamGet(RENDER)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0
        )
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        p.SetString("Type", "bgfx - OpenGL")
        doc = FreeCAD.newDocument(DOC, hidden=True)
        state["doc"] = doc
        sphere = doc.addObject("Part::Sphere", "Sphere")
        sphere.Radius = R
        box = doc.addObject("Part::Box", "Box")
        box.Length = box.Width = box.Height = 16
        box.Placement.Base = FreeCAD.Vector(14, -8, -8)
        doc.recompute()

        port = free_port()
        if not check("the document is served headless", FreeCADGui.serveDocument(doc, port),
                     "port %d" % port):
            finish()
            return
        http_port = serve_viewer()
        note("viewer on %d, scene on %d" % (http_port, port))
        FreeCADGui.serveSetGrants(wsclient.invitation(TOKEN, CLIENT))
        url = ("http://127.0.0.1:%d/fcviewer.html?scene=http://127.0.0.1:%d"
               "&token=%s&client=%s&stream" % (http_port, port, TOKEN, CLIENT))
        state["run"] = Run(url)
        state["t0"] = clock()
        state["run"].start()
        QtCore.QTimer.singleShot(50, poll)
    except Exception:
        note("FAIL build:\n" + traceback.format_exc())
        finish()


def poll():
    run = state["run"]
    marked = run.phases()
    while len(marked) > state["seen_phases"]:
        name = marked[state["seen_phases"]]
        state["seen_phases"] += 1
    if run.is_alive():
        if clock() - state["t0"] > RUN_WAIT_S:
            check("the browser run finished", False, "still running after %ds" % RUN_WAIT_S)
            note("---- driver log")
            note(run.tail(25))
            finish()
            return
        QtCore.QTimer.singleShot(50, poll)
        return
    QtCore.QTimer.singleShot(600, verify)


def verify():
    try:
        run = state["run"]
        out = run.result()
        if run.error or out.get("error"):
            check("the browser run", False, run.error or out.get("error"))
            note("---- driver log")
            note(run.tail(25))
        st, cu, dr, ze = (out.get(k, {}) for k in ("studio", "curvature", "draft", "zebra"))
        note("counts: " + jsonlib.dumps(out))
        check(
            "studio: little of the analysis colours",
            st and st.get("red", 1e9) < 1500 and st.get("green", 1e9) < 1500
            and st.get("yellow", 1e9) < 1500,
            st,
        )
        if not check(
            "the whole scene arrives (each object's mesh after its box)",
            out.get("fullScene") is True,
            "%s draws at most, %s with no box, %s ms"
            % (out.get("draws"), out.get("whole"), out.get("fullSceneMs")),
        ):
            note("the analyses below are read off an incomplete scene")
        replies = out.get("replies", {})
        oks = [r for phase in replies.values() for r in phase]
        check(
            "the page sets each analysis on the served view, each set taken",
            oks and all(r and '"ok":true' in r for r in oks),
            [r[:80] if r else r for r in oks],
        )
        check(
            "curvature: the sphere warm, the box's flat faces green",
            cu.get("red", 0) > 5000 and cu.get("green", 0) > 5000,
            cu,
        )
        check(
            "draft: the faces up green, the upright sides yellow",
            dr.get("green", 0) > 5000 and dr.get("yellow", 0) > 2000,
            dr,
        )
        check(
            "zebra: dark and light stripes, a row turning between them often",
            ze.get("dark", 0) > 5000 and ze.get("light", 0) > 5000 and ze.get("turns", 0) >= 12,
            ze,
        )
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, build)
