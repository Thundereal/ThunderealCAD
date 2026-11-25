"""A PMI view made active in a real browser's view (docs/GDandT.md sec 10.16).

serve-pmi-view.py reads the wire: that the client is told what to hide and
asked to take the view's camera. This is the browser leg: the page itself,
through scripts/pmiview-drive.js, triggers the flatness's "PMI views" entry
on a view-only connection, and what is asserted is what it draws:

  - the menu, asked for from the page, names the view, kind viewer, allowed;
  - triggered, the page's picture changes: its camera turned to the view's;
  - the menu then has the view checked and "Show all annotations";
  - that triggered, the page draws what it drew before: its camera is back;
  - no 3D view was opened on the host.

Needs the built viewer (build/wasm/fcviewer.html; `ninja -C build/wasm`),
node, a puppeteer-core install named by PUPPETEER_PATH, and a Chrome named
by CHROME. Skips rather than fails when any is missing.

  PUPPETEER_PATH=/path/to/node_modules/puppeteer-core \\
  CHROME=~/.cache/puppeteer/chrome/*/chrome-linux64/chrome \\
  scripts/gui-test.sh tests/gui/serve-pmi-view-browser.py /tmp/pmi-web \\
      --timeout 300
"""
import functools
import math
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

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WASM = os.path.join(REPO, "build", "wasm")
DRIVER = os.path.join(REPO, "scripts", "pmiview-drive.js")
DOC = "ServePMIViewBrowser"
SETTLE_MS = 8000
RUN_WAIT_S = 200
VIEW_TOKEN = "view-only-pmi-browser"

state = {"doc": None, "done": False, "run": None, "t0": clock(), "views": []}


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

    def __init__(self, url, aim):
        super().__init__(daemon=True)
        self.url = url
        self.aim = aim
        self.error = None
        self.log_path = os.path.join(OUT, "drive.log")
        self.result_path = os.path.join(OUT, "drive-result.json")

    def run(self):
        try:
            env = dict(os.environ)
            env["LD_LIBRARY_PATH"] = (os.path.join(REPO, ".conda", "freecad", "lib")
                                      + os.pathsep + env.get("LD_LIBRARY_PATH", ""))
            env["PMIVIEW_RESULT"] = self.result_path
            env.setdefault("PMIVIEW_SHOTS", OUT)
            args = ["node", DRIVER, self.url, str(SETTLE_MS)] + ["%g" % v for v in self.aim]
            with open(self.log_path, "w") as log:
                proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, env=env)
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


def views_3d():
    return len(FreeCADGui.getDocument(DOC).mdiViewsOfType("Gui::View3DInventor"))


def face_at(shape, point):
    for i, face in enumerate(shape.Faces):
        if face.isInside(point, 1e-6, True):
            return "Face%d" % (i + 1)
    raise AssertionError("no face at %s" % point)


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
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        V = FreeCAD.Vector
        doc = FreeCAD.newDocument(DOC, hidden=True)
        state["doc"] = doc
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObjectAt("PartDesign::AdditiveBox", "Box")
        box.Length, box.Width, box.Height = 40, 20, 10
        doc.recompute()
        import PMI

        pmi = PMI.getSet(body)
        doc.recompute()
        solid = body.SolidBodies[0]
        top = PMI.feature(pmi, solid, [face_at(solid.Shape, V(5, 5, 10))])
        flat = doc.addObject("PMI::GeometricTolerance", "Flatness")
        pmi.addObject(flat)
        flat.Characteristic, flat.Feature, flat.Value = "Flatness", top, 0.05
        bottom = PMI.feature(pmi, solid, [face_at(solid.Shape, V(5, 5, 0))])
        datum = doc.addObject("PMI::Datum", "DatumA")
        pmi.addObject(datum)
        datum.Feature, datum.Letter = bottom, "A"
        view = doc.addObject("PMI::View", "Top")
        pmi.addObject(view)
        view.Label = "Top"
        view.Standard = "Top"
        view.Height = 30
        # Looking at the plate's middle, not at its corner
        view.Camera = FreeCAD.Placement(V(20, 10, 10), view.Camera.Rotation)
        flat.Views = [view]
        doc.recompute()
        stays = flat.Presentation.SubShapes[0]
        aim = max((v.Point for v in stays.Vertexes), key=lambda p: p.z)

        port = free_port()
        if not check("the document is served headless",
                     FreeCADGui.serveDocument(doc, port), "port %d" % port):
            finish()
            return
        FreeCADGui.serveSetGrants([{"token": VIEW_TOKEN, "access": 1}, {"access": 0}])
        http_port = serve_viewer()
        note("viewer on %d, scene on %d" % (http_port, port))
        # The page's camera, from the front and above, on the leader's end:
        # the eye is known, and the menu's ray runs from it to the leader
        yaw, pitch, dist = -0.5 * math.pi, 0.9, 120.0
        eye = aim + V(math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw),
                      math.sin(pitch)) * dist
        to = aim - eye
        cam = "%g,%g,%g,%g,%g,%g,0,0" % (yaw, pitch, dist, aim.x, aim.y, aim.z)
        url = ("http://127.0.0.1:%d/fcviewer.html?scene=http://127.0.0.1:%d&token=%s&cam=%s"
               % (http_port, port, VIEW_TOKEN, cam))
        state["run"] = Run(url, (eye.x, eye.y, eye.z, to.x, to.y, to.z))
        state["t0"] = clock()
        state["run"].start()
        QtCore.QTimer.singleShot(50, poll)
    except Exception:
        note("FAIL build:\n" + traceback.format_exc())
        finish()


def poll():
    run = state["run"]
    state["views"].append(views_3d())
    if run.is_alive():
        if clock() - state["t0"] > RUN_WAIT_S:
            check("the browser run finished", False, "still running after %ds" % RUN_WAIT_S)
            note(run.tail(25))
            finish()
            return
        QtCore.QTimer.singleShot(50, poll)
        return
    QtCore.QTimer.singleShot(300, verify)


def verify():
    run = state["run"]
    try:
        check("the browser ran without error", run.error is None, run.error)
        r = run.result()
        check("the page connected", r.get("connected") is True)
        top = r.get("top") or {}
        check("the menu asked for from the page names the view, kind viewer, allowed",
              top.get("kind") == "viewer" and top.get("allowed") is True, top)
        on = r.get("on") or {}
        check("triggered, it runs", bool(on) and on.get("ok", True) is not False, on)
        # The isometric and the top of a plate share little of the picture
        check("the page's camera turned to the view's: the picture changed",
              (r.get("onDiff") or 0) > 0.05, r.get("onDiff"))
        check("the menu then has the view checked", r.get("checked") == ["Top"],
              r.get("checked"))
        check("and offers Show all annotations",
              (r.get("offEntry") or {}).get("kind") == "viewer", r.get("offEntry"))
        off = r.get("off") or {}
        check("which runs", bool(off) and off.get("ok", True) is not False, off)
        check("let go, the page draws what it drew before: its camera is back",
              r.get("offDiff") is not None and r["offDiff"] < 0.01, r.get("offDiff"))
        check("no 3D view was opened on the host", max(state["views"] or [0]) == 0,
              sorted(set(state["views"])))
    except Exception:
        note("ABORT verify:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


# Deferred: a script handed to FreeCAD runs before the event loop is up.
QtCore.QTimer.singleShot(1500, build)
