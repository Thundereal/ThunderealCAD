"""A handle's hint in a view with no widget: pushed to its client.

docs/OnViewValueEditing.md 9.2 #6. On the desktop the pointer resting on an
edit's handle shows a Qt tool tip that names the right click (entries 135,
141); Qt asks for it, after the style's wait, of the widget under the
pointer. A served view has no widget and nobody asks: the handle keeps the
same wait itself and gives the hint to the view, which pushes it to its
client (`{"op":"hint","text":...}`), and the page shows it at its pointer.

What is asserted, for a fillet of a box's edge edited from a client:

  - the pointer replayed onto the radius arrow (found by asking for the
    scene menu after each move: a hovered handle's menu has "One radius")
    pushes no hint at once;
  - resting there, a hint naming the right click is pushed;
  - the pointer moved off the handle pushes an empty hint;
  - the desktop's tool tip is not shown for it.

Run through scripts/gui-test.sh (xvfb, isolated configuration, external
timeout), or by hand as `FreeCAD <this script>` with GT_OUT set and this
directory on PYTHONPATH.
"""
import json
import math
import os
import sys
import threading
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

import wsclient
from wsclient import WS, free_port

# Metal on macOS: a Qt widget's GL context there is 2.1, below what the
# bgfx OpenGL backend takes (docs/Testing.md).
ENGINE = "bgfx - Metal" if sys.platform == "darwin" else "bgfx - OpenGL"

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeHandleHint"
CLIENT_WAIT_S = 180
HEIGHT_ANGLE = math.radians(45.0)
DIST = 60.0
VW, VH = 800, 600
# how far from the edge's middle the arrow is looked for, in pixels
REACH = 90
STEP = 3

state = {"doc": None, "client": None, "done": False, "t0": clock(), "tips": []}


def tip_label():
    """The handle's tip: Gui::ToolTip's rounded label (entry 246), shown"""
    for w in QtWidgets.QApplication.allWidgets():
        if w.objectName() == "fc_overlay_tiplabel" and w.isVisible():
            return w
    return None


def tip_shown():
    """A tip up: the handle's rounded label, or Qt's"""
    return tip_label() is not None or QtWidgets.QToolTip.isVisible()


def tip_text():
    w = tip_label()
    if w is not None:
        labels = w.findChildren(QtWidgets.QLabel)
        return labels[0].text() if labels else w.text()
    return QtWidgets.QToolTip.text() if QtWidgets.QToolTip.isVisible() else ""


def tip_hide():
    w = tip_label()
    if w is not None:
        w.hide()
    QtWidgets.QToolTip.hideText()


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def looking_at(point, normal):
    """A camera frame from DIST along `normal`, looking back at `point`:
    the point is the middle of the canvas"""
    n = FreeCAD.Vector(normal)
    n.normalize()
    eye = point + n * DIST
    rot = FreeCAD.Rotation(FreeCAD.Vector(0, 0, -1), -n)
    return eye, wsclient.camera_frame((eye.x, eye.y, eye.z), tuple(rot.Q), HEIGHT_ANGLE,
                                      1.0, 400.0, VW, VH)


def ring_points():
    """The middle of the canvas, then square rings around it"""
    cx, cy = VW // 2, VH // 2
    yield cx, cy
    for r in range(STEP, REACH, STEP):
        for d in range(-r, r + 1, STEP):
            yield cx + d, cy - r
            yield cx + d, cy + r
        for d in range(-r + STEP, r, STEP):
            yield cx - r, cy + d
            yield cx + r, cy + d


def hints(pushes, since=0):
    out = []
    for raw in pushes[since:]:
        if b'"op":"hint"' not in raw:
            continue
        try:
            out.append(json.loads(raw.decode("utf-8")).get("text"))
        except Exception:
            out.append(None)
    return out


class Client(threading.Thread):
    def __init__(self, port, camera, ray):
        super().__init__(daemon=True)
        self.port = port
        self.camera = camera
        self.ray = ray
        self.error = None
        self.snapshot = False
        self.edit = None
        self.r = {}

    def run(self):
        try:
            self.talk()
        except Exception:
            self.error = traceback.format_exc()

    def talk(self):
        ws = WS(self.port)
        ws.hello("serve-handle-hint")
        self.snapshot = ws.next_binary(20.0) is not None
        if not self.snapshot:
            return
        ws.next_binary(0.5)
        ws.send(2, self.camera)
        ws.drain(0.3)
        raw = ws.op('{"id":1,"op":"edit","obj":"Fillet","mode":0}')
        self.edit = json.loads(raw.decode("utf-8")) if raw else None
        if not (self.edit or {}).get("ok"):
            ws.close()
            return
        ws.drain(2.0)

        t = 1000
        rid = 10
        found = None
        tried = 0
        for x, y in ring_points():
            t += 20
            rid += 1
            tried += 1
            ws.send(2, wsclient.input_frame(wsclient.MOVE, x, y, time_ms=t))
            reply = ws.op('{"id":%d,"op":"contextMenu","ray":%s}' % (rid, self.ray))
            if reply and b"One radius" in reply:
                found = (x, y)
                break
        self.r["found"] = found
        self.r["tried"] = tried
        if found:
            # at once: nothing yet, the hint waits as Qt's tool tip does
            self.r["at_once"] = hints(ws.pushes)
            since = len(ws.pushes)
            ws.drain(2.0)
            self.r["resting"] = hints(ws.pushes, since)
            since = len(ws.pushes)
            t += 2000
            ws.send(2, wsclient.input_frame(wsclient.MOVE, 4, 4, time_ms=t))
            ws.drain(1.5)
            self.r["off"] = hints(ws.pushes, since)
        ws.op('{"id":2,"op":"resetEdit"}')
        ws.drain(0.5)
        ws.close()


def build():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt(
            "RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE)
        doc = FreeCAD.newDocument(DOC, hidden=True)
        state["doc"] = doc
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1"])
        fillet.Radius = 1
        doc.recompute()
        if not check("the fillet is valid", fillet.isValid() and not fillet.Shape.isNull()):
            finish()
            return

        # the edge's middle, seen between its two faces
        solid = box.Shape
        edge = solid.getElement("Edge1")
        mid = edge.valueAt((edge.FirstParameter + edge.LastParameter) / 2)
        n = FreeCAD.Vector()
        for f in solid.ancestorsOfType(edge, Part.Face):
            fu, fv = f.Surface.parameter(mid)
            n += f.normalAt(fu, fv)
        eye, camera = looking_at(mid, n)
        d = mid - eye
        d.normalize()
        ray = "[%g,%g,%g,%g,%g,%g]" % (eye.x, eye.y, eye.z, d.x, d.y, d.z)

        port = free_port()
        ok = FreeCADGui.serveDocument(doc, port)
        if not check("the document is served headless", ok, "port %d" % port):
            finish()
            return
        state["client"] = Client(port, camera, ray)
        state["client"].start()
        QtCore.QTimer.singleShot(50, poll)
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()


def poll():
    client = state["client"]
    # the desktop's tool tip, which must not show for a client's handle
    if tip_shown():
        state["tips"].append(tip_text())
    if client.is_alive():
        if clock() - state["t0"] > CLIENT_WAIT_S:
            check("the client finished", False, "still talking after %ds" % CLIENT_WAIT_S)
            finish()
            return
        QtCore.QTimer.singleShot(50, poll)
        return
    QtCore.QTimer.singleShot(300, verify)


def verify():
    client = state["client"]
    r = client.r
    try:
        check("the client ran without error", client.error is None, client.error or "")
        check("the hello was answered with a snapshot", client.snapshot)
        check("the fillet is edited from the client",
              (client.edit or {}).get("ok") is True, client.edit)
        note("INFO the arrow found at %s after %s moves" % (r.get("found"), r.get("tried")))
        check("the pointer is on the radius arrow (its menu has 'One radius')",
              r.get("found") is not None, r.get("tried"))
        check("no hint at once: it waits as a tool tip does", r.get("at_once") == [],
              r.get("at_once"))
        resting = r.get("resting") or []
        note("INFO hints while resting: %s" % resting)
        check("resting on the arrow pushes a hint naming the right click",
              len(resting) == 1 and isinstance(resting[0], str)
              and "right" in resting[0].lower(), resting)
        check("the pointer off the handle pushes an empty hint", r.get("off") == [""],
              r.get("off"))
        check("the desktop's tool tip is not shown for it", state["tips"] == [],
              state["tips"][:3])
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
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


# Deferred: a script handed to FreeCAD runs before the event loop is up.
QtCore.QTimer.singleShot(1500, build)
