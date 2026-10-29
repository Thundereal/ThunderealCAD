"""A finger on an edit's handles, in a real browser.

docs/OnViewValueEditing.md sec 9.3 (entry 189): touch sent no input frames
at all, and the long press was off while editing. The client now picks the
edit session's overlay at a finger's first touch -- a handle to drag, or
the camera to orbit -- with its own ray pick, as no round trip per touch
could decide it in time.

A PartDesign box with a fillet on Edge1, radius 2, served headless; the
built viewer served over HTTP; scripts/handle-touch-drive.js drives a
headful Chrome with touch emulated (CDP touch events). The page enters the
fillet's edit itself. Every reading of the model is taken here, in the
serving process, on the driver's phase marks:

  - a tap on the arrow: press and release go up, and the value is active
    (its arrow green, red before);
  - a tap on empty space: a click there, which clears the active value as
    a mouse click does (entry 159), with the camera still;
  - a finger on the radius arrow, moved: the radius changes, by the coarse
    step, and the camera stays (as the page's HUD reads it);
  - a finger held on the arrow, under the loupe: the handle's menu ("One
    radius"), not the view's;
  - its Settings, "Default drag", "Fine", tapped in that menu: the
    preference is fine -- a phone has no Shift;
  - Escape with that menu open closes the menu and nothing else: the edit
    goes on, and the key never goes up (entry 201);
  - the same drag again: the radius changes off the coarse step;
  - a tap on the value's label: its box in the page, and a value typed
    into it is the radius -- by keys, then as a phone keyboard types (a
    keydown naming no key, 229, and the text by CDP's Input.insertText),
    then through an IME composition;
  - a finger off the handles, moved: the camera turns, and nothing goes up;
  - then a sketch's edit: a finger on its line, moved, drags the line as a
    mouse press would, and the camera stays; a finger on empty space
    orbits and sends nothing. The fillet's see-through preview is under an
    Unpickable selection root and takes no finger (Material::unpickable).

Needs build/wasm (cmake --build build/wasm), node (NODE, or on PATH), a
puppeteer-core install (PUPPETEER_PATH) and a Chrome (CHROME). Skips
when any is missing. It opens a Chrome window on the desktop.
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

TOKEN = "touch-browser-invite"
CLIENT = "touch-browser"

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
# FCAD_REPO where this script is run by a wrapper that execs it
REPO = os.environ.get("FCAD_REPO") or os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WASM = os.path.join(REPO, "build", "wasm")
DRIVER = os.path.join(REPO, "scripts", "handle-touch-drive.js")
DOC = "ServeHandleTouch"
SETTLE_MS = 8000
RUN_WAIT_S = 300
GIZMOS = "User parameter:BaseApp/Preferences/Gui/Gizmos"
VIEW = "User parameter:BaseApp/Preferences/View"
# The page's camera (?cam=yaw,pitch,dist,centre,pan): from high above, so
# the radius arrow of the vertical Edge1 lies across the screen. The
# fitted view looks along it, and an arrow seen end-on has no direction
# on the screen to be dragged along.
CAMERA = "0.79,1.3,23.72,5,5,5,0,0"

state = {"doc": None, "done": False, "run": None, "seen": 0, "samples": {},
         "radii": [], "t0": clock()}


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
    """The browser, off the GUI thread; its log and phase marks to files"""

    def __init__(self, url, lift):
        super().__init__(daemon=True)
        self.url = url
        self.lift = lift
        self.error = None
        self.log_path = os.path.join(OUT, "drive.log")
        self.phase_path = os.path.join(OUT, "phases.txt")
        self.result_path = os.path.join(OUT, "drive-result.json")

    def run(self):
        try:
            env = dict(os.environ)
            # The bundled Chrome needs libasound from the conda env this
            # build runs against, and conda activation replaces
            # LD_LIBRARY_PATH before we get here (scripts/wasm-chrome.js).
            env["LD_LIBRARY_PATH"] = (
                os.path.join(REPO, ".conda", "freecad", "lib")
                + os.pathsep + env.get("LD_LIBRARY_PATH", ""))
            env["TOUCH_PHASES"] = self.phase_path
            env["TOUCH_RESULT"] = self.result_path
            env["TOUCH_SHOTS"] = OUT
            env["TOUCH_LIFT"] = str(self.lift)
            with open(self.log_path, "w") as log:
                proc = subprocess.Popen(
                    [os.environ.get("NODE", "node"), DRIVER, self.url, str(SETTLE_MS)],
                    stdout=log, stderr=subprocess.STDOUT, env=env)
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


def radius():
    return round(state["doc"].getObject("Fillet").Radius.Value, 4)


def viewer_client():
    rows = [c for c in FreeCADGui.serveClients() if c.get("viewer")]
    return rows[-1] if rows else None


def line_start():
    p = state["doc"].getObject("Sketch").Geometry[0].StartPoint
    return (round(p.x, 3), round(p.y, 3))


def in_edit():
    vp = FreeCADGui.getDocument(DOC).getInEdit()
    return vp.Object.Name if vp is not None else None


def sample():
    client = viewer_client()
    return {"radius": radius(),
            "edit": in_edit(),
            "line": line_start(),
            "inputs": client["inputMsgs"] if client else None,
            "coarse": FreeCAD.ParamGet(GIZMOS).GetInt("DefaultCoarseDragBehavior", 0)}


def build():
    try:
        import Part

        if not os.path.exists(os.path.join(WASM, "fcviewer.html")):
            skip("no built viewer at %s" % WASM)
            return
        if not os.environ.get("PUPPETEER_PATH"):
            skip("PUPPETEER_PATH is unset (a puppeteer-core install)")
            return
        if not os.environ.get("CHROME"):
            skip("CHROME is unset (a Chrome executable)")
            return

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet(VIEW).SetInt("RenderCache", 3)
        FreeCAD.ParamGet(VIEW + "/Render").SetString("Type", "bgfx - OpenGL")
        gizmos = FreeCAD.ParamGet(GIZMOS)
        gizmos.SetInt("DefaultCoarseDragBehavior", 0)
        gizmos.SetBool("EnableCoarseSnap", True)
        doc = FreeCAD.newDocument(DOC, hidden=True)
        state["doc"] = doc
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1"])
        fillet.Radius = 2
        doc.recompute()
        if not check("the fillet is valid", fillet.isValid() and not fillet.Shape.isNull()):
            finish()
            return
        # A sketch outside the body, beside the box: one line, magenta when
        # edited, which is how the driver tells it from the sketch's axes
        FreeCAD.ParamGet(VIEW).SetUnsigned("EditedEdgeColor", 0xFF00FFFF)
        sketch = doc.addObject("Sketcher::SketchObject", "Sketch")
        sketch.addGeometry(Part.LineSegment(FreeCAD.Vector(0, -3, 0),
                                            FreeCAD.Vector(10, -3, 0)), False)
        doc.recompute()

        port = free_port()
        if not check("the document is served headless",
                     FreeCADGui.serveDocument(doc, port), "port %d" % port):
            finish()
            return
        http_port = serve_viewer()
        FreeCADGui.serveSetGrants(wsclient.invitation(TOKEN, CLIENT))
        url = ("http://127.0.0.1:%d/fcviewer.html?scene=http://127.0.0.1:%d"
               "&token=%s&client=%s&hud&cam=%s"
               % (http_port, port, TOKEN, CLIENT, CAMERA))
        note("viewer on %d, scene on %d" % (http_port, port))
        lift = FreeCAD.ParamGet(VIEW).GetFloat("TouchLoupeLift", 28.0)
        state["run"] = Run(url, lift)
        state["t0"] = clock()
        state["run"].start()
        QtCore.QTimer.singleShot(50, poll)
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()


def poll():
    run = state["run"]
    marked = run.phases()
    while len(marked) > state["seen"]:
        name = marked[state["seen"]]
        state["seen"] += 1
        state["samples"][name] = sample()
    if state["doc"] is not None:
        r = radius()
        if not state["radii"] or state["radii"][-1][1] != r:
            state["radii"].append((round(clock() - state["t0"], 2), r))
    if run.is_alive():
        if clock() - state["t0"] > RUN_WAIT_S:
            check("the browser run finished", False, "still running after %ds" % RUN_WAIT_S)
            note(run.tail())
            finish()
            return
        QtCore.QTimer.singleShot(50, poll)
        return
    QtCore.QTimer.singleShot(600, verify)


def steps_between(radii, t0, t1):
    return [r for t, r in radii if t0 <= t <= t1]


def verify():
    run = state["run"]
    try:
        said = run.result()
        s = state["samples"]
        note("phases: %s" % run.phases())
        for name in run.phases():
            note("  %-12s %s" % (name, s.get(name)))
        note("radius trace (t, radius): %s" % state["radii"])
        note("the page said: %s" % jsonlib.dumps(said))
        if not check("the browser run had no error", run.error is None and not said.get("error"),
                     run.error or said.get("error")):
            note(run.tail())
        for name in ("entered", "tapped", "cleared", "dragged", "held", "fine", "menuescaped",
                     "finedragged", "typed", "phonetyped", "imetyped",
                     "orbited", "sketchentered", "linedragged", "sketchorbited"):
            if name not in s:
                check("the run reached '%s'" % name, False, run.phases())
                finish()
                return

        check("the page entered the fillet's edit", said.get("editing") == "Fillet"
              and s["entered"]["edit"], (said.get("editing"), s["entered"]))
        check("the client finds the radius arrow by its own pick", said.get("handle") is not None,
              said.get("enteredHits"))

        # tap
        r0 = s["entered"]["radius"]
        check("a tap on the arrow goes up as a press and a release",
              (s["tapped"]["inputs"] or 0) - (s["entered"]["inputs"] or 0) >= 2,
              (s["entered"]["inputs"], s["tapped"]["inputs"]))
        check("... and changes no value", s["tapped"]["radius"] == r0, s["tapped"]["radius"])
        check("... and makes the value active (its arrow green, after none was)",
              said.get("greenBeforeTap") == 0 and (said.get("greenAfterTap") or 0) > 20,
              (said.get("greenBeforeTap"), said.get("greenAfterTap")))

        # a tap on empty space clears the active value (entry 159)
        check("a tap on empty space is off the handles", said.get("clearOnHandle") is False,
              said.get("clearOnHandle"))
        check("... goes up as a click (a move, a press and a release)",
              (s["cleared"]["inputs"] or 0) - (s["tapped"]["inputs"] or 0) >= 3,
              (s["tapped"]["inputs"], s["cleared"]["inputs"]))
        check("... clears the active value (its arrow no longer green)",
              (said.get("greenAfterTap") or 0) > 20 and said.get("greenAfterClear") == 0,
              (said.get("greenAfterTap"), said.get("greenAfterClear")))
        check("... changes no value and leaves the camera still",
              s["cleared"]["radius"] == r0
              and said.get("cameraBeforeClear") is not None
              and said.get("cameraAfterClear") == said.get("cameraBeforeClear"),
              (s["cleared"]["radius"], said.get("cameraBeforeClear"),
               said.get("cameraAfterClear")))

        # drag
        r1 = s["dragged"]["radius"]
        check("a finger on the arrow, moved, changes the radius", r1 != r0, (r0, r1))
        check("... by the coarse step", abs(r1 * 2 - round(r1 * 2)) < 1e-6, r1)
        check("... and its press, moves and release went up",
              (s["dragged"]["inputs"] or 0) - (s["tapped"]["inputs"] or 0) >= 3,
              (s["tapped"]["inputs"], s["dragged"]["inputs"]))
        check("... and the camera stayed",
              said.get("cameraBeforeDrag") is not None
              and said.get("cameraAfterDrag") == said.get("cameraBeforeDrag"),
              (said.get("cameraBeforeDrag"), said.get("cameraAfterDrag")))

        # hold
        held = said.get("menuWhileHeld") or []
        check("a finger held on the arrow opens the handle's menu", "One radius" in held, held)
        check("... with its Settings", "Settings" in held, held)
        check("Settings, Default drag and Fine tapped in it",
              said.get("tappedSettings") and said.get("tappedDefaultDrag")
              and said.get("tappedFine"),
              (said.get("settingsRows"), said.get("defaultDragRows")))
        check("... make the default drag fine", s["fine"]["coarse"] == 1, s["fine"]["coarse"])

        # Escape on the open menu is the menu's (entry 201)
        check("Escape with the menu open closes the menu",
              bool(said.get("menuBeforeEscape")) and not said.get("menuAfterClose"),
              (said.get("menuBeforeEscape"), said.get("menuAfterClose")))
        check("... and the edit goes on, on both sides",
              s["menuescaped"]["edit"] == "Fillet"
              and said.get("editingAfterEscape") == "Fillet",
              (s["menuescaped"]["edit"], said.get("editingAfterEscape")))
        check("... and neither its press nor its release went up",
              s["menuescaped"]["inputs"] == s["fine"]["inputs"],
              (s["fine"]["inputs"], s["menuescaped"]["inputs"]))

        # fine drag
        r2, r3 = s["fine"]["radius"], s["finedragged"]["radius"]
        fine_steps = steps_between(state["radii"], 0, 1e9)
        check("the same drag again changes the radius", r3 != r2, (r2, r3))
        off_coarse = [r for r in [r3] if abs(r * 2 - round(r * 2)) > 1e-6]
        check("... off the coarse step: fine", bool(off_coarse), r3)

        # the label
        check("a tap on the value's label opens its box in the page",
              said.get("boxShown") is True, (said.get("labelAt"), said.get("labelHits")))
        note("INFO the box %s focused by the tap; it read %r, after typing %r"
             % ("was" if said.get("boxFocused") else "was not", said.get("boxText"),
                said.get("boxTextAfter")))
        check("a value typed into it is the radius", s["typed"]["radius"] == 4.0,
              s["typed"]["radius"])

        # a phone's keyboard, which names no key: the text by input events
        note("INFO the box before the phone's typing: %s; after: %r; after the IME: %r"
             % (said.get("phoneBoxBefore"), said.get("phoneBoxAfter"),
                said.get("imeBoxAfter")))
        check("a value typed with no key codes (keydown 229, insertText) is the radius",
              s["phonetyped"]["radius"] == 3.0, s["phonetyped"]["radius"])
        check("a value typed through an IME composition is the radius",
              s["imetyped"]["radius"] == 6.0, s["imetyped"]["radius"])

        # orbit
        check("the off point is off the handles", said.get("offOnHandle") is False,
              said.get("offOnHandle"))
        check("a finger off the handles turns the camera",
              said.get("cameraBeforeOrbit") is not None
              and said.get("cameraAfterOrbit") != said.get("cameraBeforeOrbit"),
              (said.get("cameraBeforeOrbit"), said.get("cameraAfterOrbit")))
        check("... sends nothing up, and changes no value",
              s["orbited"]["inputs"] == s["imetyped"]["inputs"]
              and s["orbited"]["radius"] == s["imetyped"]["radius"],
              (s["imetyped"], s["orbited"]))
        note("INFO radius values seen: %s" % fine_steps)

        # the sketch: a finger on its line is the sketcher's, as a mouse
        # press there is on the desktop; empty space orbits
        check("the page entered the sketch's edit", said.get("sketchEditing") == "Sketch"
              and s["sketchentered"]["edit"] == "Sketch",
              (said.get("sketchEditing"), s["sketchentered"]["edit"]))
        check("the client finds the sketch line by its own pick", said.get("lineAt") is not None,
              (said.get("sketchHits"), said.get("sketchLineHits")))
        l0, l1 = s["sketchentered"]["line"], s["linedragged"]["line"]
        check("a finger on the sketch line, moved, drags the line", l1 != l0, (l0, l1))
        check("... its press, moves and release went up",
              (s["linedragged"]["inputs"] or 0) - (s["sketchentered"]["inputs"] or 0) >= 3,
              (s["sketchentered"]["inputs"], s["linedragged"]["inputs"]))
        check("... and the camera stayed",
              said.get("cameraBeforeLine") is not None
              and said.get("cameraAfterLine") == said.get("cameraBeforeLine"),
              (said.get("cameraBeforeLine"), said.get("cameraAfterLine")))
        check("there is empty space off the sketch", said.get("emptyAt") is not None,
              said.get("emptyAt"))
        check("a finger on empty space in the sketch turns the camera",
              said.get("cameraBeforeSketchOrbit") is not None
              and said.get("cameraAfterSketchOrbit") != said.get("cameraBeforeSketchOrbit"),
              (said.get("cameraBeforeSketchOrbit"), said.get("cameraAfterSketchOrbit")))
        before = s.get("sketchempty") or s["linedragged"]
        check("... sends nothing up, and moves no line",
              s["sketchorbited"]["inputs"] == before["inputs"]
              and s["sketchorbited"]["line"] == l1,
              (before, s["sketchorbited"]))
    except Exception:
        note("ABORT verify:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCAD.ParamGet(GIZMOS).SetInt("DefaultCoarseDragBehavior", 0)
    except Exception:
        pass
    try:
        FreeCADGui.getDocument(DOC).resetEdit()
    except Exception:
        pass
    try:
        state["httpd"].shutdown()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


# Deferred: a script handed to FreeCAD runs before the event loop is up.
QtCore.QTimer.singleShot(1500, build)
