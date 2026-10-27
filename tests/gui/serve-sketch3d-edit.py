"""A 3D sketch edited from a browser (docs/Sketch3D.md S4).

The 3D sketch's edit runs on the same binding as the 2D sketch's served edit
(docs/ThinClient.md sec 8.9 item 4): its view is the connection's mirror, its
tools are started by the `command` op, and the pointer and the keys arrive as
'E' frames. This drives one over a real socket the way the browser viewer
does, and reads each step back on the GUI thread:

  - the `edit` op enters the 3D sketch's edit in the client's view, with no
    3D view made for it, and the leaving edge is pushed;
  - Sketcher_CreateLine starts the 3D Line tool, and two clicks in empty
    space draw a line where the client pointed, on the working plane;
  - a number typed while placing opens the entry (a value editor, which on a
    mirror takes its keys from the wire), Enter places the next point that
    far toward the pointer and makes the length a Distance;
  - a press on a point and a drag moves it;
  - the 3D sketch's own commands are on the browser allowlist: Along X on a
    selected line, and the working plane's cycle;
  - `resetEdit` ends it.

The camera looks straight down the sketch's Z, so a pixel maps to a point of
the XY working plane by the height angle alone.

Run through scripts/gui-test.sh (xvfb, isolated configuration, external
timeout); registered in ctest by tests/gui/CMakeLists.txt.
"""
import json
import math
import os
import queue
import threading
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

import wsclient
from wsclient import WS, free_port

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeSketch3DEdit"
OBJ = "Sketch3D"
CLIENT_WAIT_S = 150

EYE = (0.0, 0.0, 60.0)
QUAT = (0.0, 0.0, 0.0, 1.0)
HEIGHT_ANGLE = math.radians(45.0)
NEAR, FAR = 10.0, 200.0
VW, VH = 800, 600

# Off centre in both axes and clear of the origin and the axes, so nothing
# is picked or snapped to instead of the plane.
START = (-18.0, -9.0)
END = (14.0, 7.0)
TYPED_FROM = (-10.0, 12.0)
TOWARD = (0.0, 12.0)
DRAGGED = (-18.0, -3.0)
# A pixel is about 1/12 of a unit at this camera
TOL = 0.5

ESC, ENTER = 0xff1b, 0xff0d

state = {"doc": None, "client": None, "done": False, "t0": clock(), "seen": {}}
requests = queue.Queue()


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def camera_frame():
    return wsclient.camera_frame(EYE, QUAT, HEIGHT_ANGLE, NEAR, FAR, VW, VH)


def pixel_of(x, y, z=0.0):
    """The client pixel (top-left origin) showing world (x, y, z), as in
    serve-sketch-click-placement.py."""
    depth = EYE[2] - z
    half = depth * math.tan(HEIGHT_ANGLE / 2.0)
    scale = (VH / 2.0) / half
    return (int(round(VW / 2.0 + (x - EYE[0]) * scale)),
            int(round(VH / 2.0 - 1.0 - (y - EYE[1]) * scale)))


def reply_of(raw):
    if raw is None:
        return {}
    try:
        return json.loads(raw.decode("utf-8"))
    except Exception:
        return {}


class Client(threading.Thread):
    """The whole wire conversation, off the GUI thread."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.error = None
        self.snapshot = False
        self.replies = {}
        self.pushes = {}
        self.t = 1000

    def run(self):
        try:
            self.talk()
        except Exception:
            self.error = traceback.format_exc()

    def reading(self, tag):
        """A reading of the edit taken on the GUI thread, now."""
        done = threading.Event()
        requests.put((tag, done))
        done.wait(10.0)

    def frame(self, ws, kind, px, py, code=0, delta=0):
        self.t += 20
        ws.send(2, wsclient.input_frame(kind, px, py, code=code, delta=delta,
                                        time_ms=self.t))

    def move_to(self, ws, at, steps=1, start=None):
        px, py = pixel_of(*at)
        if start is not None and steps > 1:
            sx, sy = pixel_of(*start)
            for i in range(1, steps):
                self.frame(ws, wsclient.MOVE, sx + (px - sx) * i // steps,
                           sy + (py - sy) * i // steps)
                ws.drain(0.05)
        self.frame(ws, wsclient.MOVE, px, py)
        ws.drain(0.2)
        return px, py

    def click(self, ws, at):
        px, py = self.move_to(ws, at)
        self.frame(ws, wsclient.PRESS, px, py)
        ws.drain(0.1)
        self.frame(ws, wsclient.RELEASE, px, py)
        ws.drain(0.3)

    def key(self, ws, code, char=0):
        px, py = pixel_of(*TOWARD)
        self.frame(ws, wsclient.KEY_DOWN, px, py, code=code, delta=char)
        ws.drain(0.1)
        self.frame(ws, wsclient.KEY_UP, px, py, code=code, delta=char)
        ws.drain(0.2)

    def command(self, ws, n, name):
        self.replies[name] = reply_of(ws.op('{"id":%d,"op":"command","name":"%s"}' % (n, name)))
        ws.drain(0.4)

    def talk(self):
        ws = WS(self.port)
        ws.hello("serve-sketch3d-edit")
        self.snapshot = ws.next_binary(20.0) is not None
        if not self.snapshot:
            return
        ws.next_binary(0.5)
        ws.send(2, camera_frame())
        ws.drain(0.5)

        mark = len(ws.pushes)
        self.replies["edit"] = reply_of(ws.op('{"id":1,"op":"edit","obj":"%s","mode":0}' % OBJ))
        ws.drain(0.5)
        self.pushes["edit"] = ws.next_push("edit", 3.0, since=mark)
        self.reading("edit")

        # A line by two clicks
        self.command(ws, 2, "Sketcher_CreateLine")
        self.reading("tool")
        self.click(ws, START)
        self.click(ws, END)
        self.reading("line")
        self.key(ws, ESC)
        self.key(ws, ESC)
        self.reading("line-ended")

        # A length typed
        self.command(ws, 3, "Sketcher_CreateLine")
        self.click(ws, TYPED_FROM)
        self.move_to(ws, TOWARD, steps=4, start=TYPED_FROM)
        self.key(ws, ord("2"), ord("2"))
        self.reading("typing")
        self.key(ws, ord("5"), ord("5"))
        self.key(ws, ENTER)
        self.reading("typed")
        self.key(ws, ESC)
        self.key(ws, ESC)
        self.reading("typed-ended")

        # The first line's start dragged
        px, py = self.move_to(ws, START)
        self.frame(ws, wsclient.PRESS, px, py)
        ws.drain(0.2)
        self.move_to(ws, DRAGGED, steps=6, start=START)
        px, py = pixel_of(*DRAGGED)
        self.frame(ws, wsclient.RELEASE, px, py)
        ws.drain(0.5)
        self.reading("dragged")

        # The 3D sketch's own commands: Along X on the first line, selected by
        # a click on it, then the working plane cycled
        mid = ((DRAGGED[0] + END[0]) / 2.0, (DRAGGED[1] + END[1]) / 2.0)
        # on nothing (the manipulator went with the dragged point): nothing selected
        self.click(ws, (5.0, -15.0))
        self.click(ws, mid)
        self.reading("selected")
        self.command(ws, 4, "Sketcher_Constrain3DAlongX")
        ws.drain(0.5)
        self.reading("constrained")
        self.command(ws, 5, "Sketcher_Sketch3DCyclePlane")
        self.reading("plane")

        mark = len(ws.pushes)
        self.replies["reset"] = reply_of(ws.op('{"id":9,"op":"resetEdit"}'))
        ws.drain(0.5)
        self.pushes["reset"] = ws.next_push("edit", 3.0, since=mark)
        self.reading("left")
        ws.close()


def vp():
    return state["doc"].getObject(OBJ).ViewObject


def take(tag):
    """The edit as it stands: what the client's frames did."""
    doc = state["doc"]
    gdoc = FreeCADGui.getDocument(DOC)
    sk = doc.getObject(OBJ)
    r = {
        "in_edit": gdoc.getInEdit() is not None,
        "views3d": len(gdoc.mdiViewsOfType("Gui::View3DInventor")),
        "lines": [((g.StartPoint.x, g.StartPoint.y, g.StartPoint.z),
                   (g.EndPoint.x, g.EndPoint.y, g.EndPoint.z))
                  for g in sk.Geometry if g.TypeId == "Part::GeomLineSegment"],
        "cons": [(c.Type, c.First, c.FirstPos, c.Second, c.SecondPos, c.Value)
                 for c in sk.Constraints],
    }
    if r["in_edit"]:
        v = vp()
        r["tool"] = v.activeTool()
        r["typing"] = v.isTypingPlacement()
        r["plane"] = (v.getManipulator() or {}).get("Plane")
        r["selection"] = [tuple(g) for g in v.getSelection()[0]]
    state["seen"][tag] = r


def build():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt(
            "RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", "bgfx - OpenGL")
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetBool(
            "ShowNaviCube", False)
        doc = FreeCAD.newDocument(DOC, hidden=True)
        state["doc"] = doc
        doc.addObject("Sketcher::Sketch3DObject", OBJ)
        doc.recompute()

        port = free_port()
        ok = FreeCADGui.serveDocument(doc, port)
        if not check("the document is served headless", ok, "port %d" % port):
            finish()
            return
        state["client"] = Client(port)
        state["client"].start()
        QtCore.QTimer.singleShot(50, poll)
    except Exception:
        note("FAIL build:\n" + traceback.format_exc())
        finish()


def poll():
    client = state["client"]
    try:
        while True:
            tag, done = requests.get_nowait()
            try:
                take(tag)
            finally:
                done.set()
    except queue.Empty:
        pass
    except Exception:
        note("ABORT reading:\n" + traceback.format_exc())
    if client.is_alive():
        if clock() - state["t0"] > CLIENT_WAIT_S:
            check("the client finished", False, "still talking after %ds" % CLIENT_WAIT_S)
            finish()
            return
        QtCore.QTimer.singleShot(20, poll)
        return
    QtCore.QTimer.singleShot(500, verify)


def near(p, q, tol=TOL):
    return all(abs(a - b) < tol for a, b in zip(p, q))


def verify():
    client = state["client"]
    seen = state["seen"]

    def at(tag):
        return seen.get(tag, {})

    try:
        check("the client ran without error", client.error is None, client.error or "")
        check("the hello was answered with a snapshot", client.snapshot)

        r = client.replies.get("edit", {})
        check("the client's edit is accepted", r.get("ok") is True, r)
        pushed = json.loads(client.pushes["edit"]) if client.pushes.get("edit") else {}
        check("and pushed to it", pushed.get("editing") is True, pushed)
        check("the 3D sketch is in edit", at("edit").get("in_edit") is True, at("edit"))

        r = client.replies.get("Sketcher_CreateLine", {})
        check("Sketcher_CreateLine is accepted", r.get("ok") is True, r)
        check("and starts the 3D Line tool", at("tool").get("tool") == "Line", at("tool"))

        lines = at("line").get("lines", [])
        if check("two clicks drew a line", len(lines) == 1, at("line")):
            s, e = lines[0]
            check("from where the client pointed, on the working plane",
                  near(s, START + (0.0,)), s)
            check("to where it pointed", near(e, END + (0.0,)), e)
        check("two Escapes end the tool", at("line-ended").get("tool") == "None",
              at("line-ended").get("tool"))

        check("a digit while placing opens the entry", at("typing").get("typing") is True,
              at("typing"))
        check("Enter takes it", at("typed").get("typing") is False, at("typed").get("typing"))
        lines = at("typed").get("lines", [])
        if check("the typed length drew a second line", len(lines) == 2, lines):
            s, e = lines[1]
            check("from the click", near(s, TYPED_FROM + (0.0,)), s)
            length = math.dist(s, e)
            check("25 long", abs(length - 25.0) < 1e-6, length)
            check("toward the pointer", near(e, (TYPED_FROM[0] + 25.0, TYPED_FROM[1], 0.0)), e)
            dist = [c for c in at("typed").get("cons", []) if c[0] == "Distance" and c[1] == 1]
            check("with its length as a Distance of 25",
                  len(dist) == 1 and abs(dist[0][5] - 25.0) < 1e-9, at("typed").get("cons"))
        check("and the tool ended", at("typed-ended").get("tool") == "None",
              at("typed-ended").get("tool"))

        lines = at("dragged").get("lines", [])
        if lines:
            check("a press and a drag moved the line's start",
                  near(lines[0][0], DRAGGED + (0.0,)), lines[0][0])
            check("and left its end", near(lines[0][1], END + (0.0,)), lines[0][1])

        check("a click on the line selects it",
              at("selected").get("selection") == [(0, 0)], at("selected").get("selection"))
        r = client.replies.get("Sketcher_Constrain3DAlongX", {})
        check("Along X is on the browser allowlist", r.get("ok") is True, r)
        along = [c for c in at("constrained").get("cons", []) if c[0] == "AlongX" and c[1] == 0]
        check("and constrains the selected line", len(along) == 1, at("constrained").get("cons"))
        lines = at("constrained").get("lines", [])
        if lines:
            s, e = lines[0]
            check("which now runs along X", abs(s[1] - e[1]) < 1e-6 and abs(s[2] - e[2]) < 1e-6,
                  lines[0])
        r = client.replies.get("Sketcher_Sketch3DCyclePlane", {})
        check("Cycle Working Plane is on the browser allowlist", r.get("ok") is True, r)
        check("and moves the working plane on",
              (at("constrained").get("plane"), at("plane").get("plane")) == ("XY", "YZ"),
              (at("constrained").get("plane"), at("plane").get("plane")))

        r = client.replies.get("reset", {})
        check("resetEdit is accepted", r.get("ok") is True, r)
        pushed = json.loads(client.pushes["reset"]) if client.pushes.get("reset") else {}
        check("and the leaving edge is pushed", pushed.get("editing") is False, pushed)
        check("the edit ended", at("left").get("in_edit") is False, at("left"))
        check("no 3D view was made for any of it",
              all(r.get("views3d") == 0 for r in seen.values()),
              {t: r.get("views3d") for t, r in seen.items()})
    except Exception:
        note("ABORT verify:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.getDocument(DOC).resetEdit()
    except Exception:
        pass
    try:
        FreeCADGui.Selection.clearSelection()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


# Deferred: a script handed to FreeCAD runs before the event loop is up.
QtCore.QTimer.singleShot(1500, build)
