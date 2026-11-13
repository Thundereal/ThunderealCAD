"""A missing edge's kept face is clicked over the wire, and a base edge
replaces the reference.

Entry 189 in a real browser: a click on the preview of a fillet's missing
edge selected nothing. The preview hangs in the edit session's root and is
drawn unpickable, so that the handles over it take a press (entry 170); the
desktop picks it in a pass of its own ahead of the scene
(SoFCUnifiedSelection getPickedInfoInSession), and that pass needed a desktop
viewer. A served root has none: it picks through the replaying client's
mirror, whose event root the session root hangs under.

What is asserted, for a fillet on a wedge whose top edge went missing, edited
from a client:

  - a replayed click on the missing edge's kept face selects its '?Edge' row
    in the panel (entry 169), and changes no reference;
  - a click after it on a base edge not in use puts that edge in the missing
    one's place, in the list and in Base (entry 188).

Each click is made with the camera looking straight at the point clicked,
which is then the middle of the canvas.

Run through scripts/gui-test.sh (xvfb, isolated configuration, external
timeout), or by hand as `FreeCAD <this script>` with GT_OUT set and this
directory on PYTHONPATH.
"""
import json
import math
import os
import threading
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

import wsclient
from wsclient import WS, free_port

# The platform's backend, as a user gets it: Direct3D 11 on Windows, Metal
# on macOS, OpenGL elsewhere (RenderParams::engineType, docs/Testing.md).
ENGINE = "Default"

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeDressUpPreviewPick"
CLIENT_WAIT_S = 120
HEIGHT_ANGLE = math.radians(45.0)
DIST = 60.0
VW, VH = 800, 600

state = {"doc": None, "client": None, "done": False, "t0": clock(), "seen": []}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def looking_at(point, normal):
    """A camera frame from DIST along \a normal, looking back at \a point:
    the point is the middle of the canvas"""
    n = FreeCAD.Vector(normal)
    n.normalize()
    eye = point + n * DIST
    rot = FreeCAD.Rotation(FreeCAD.Vector(0, 0, -1), -n)
    return wsclient.camera_frame((eye.x, eye.y, eye.z), tuple(rot.Q), HEIGHT_ANGLE,
                                 1.0, 400.0, VW, VH)


def tree():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QTreeWidget):
        if w.objectName() == "treeWidgetReferences":
            return w
    return None


def rows(widget):
    return [widget.topLevelItem(i).text(0) for i in range(widget.topLevelItemCount())]


class Client(threading.Thread):
    """The whole wire conversation, off the GUI thread. After each click it
    waits for the GUI thread to read the panel."""

    def __init__(self, port, cameras):
        super().__init__(daemon=True)
        self.port = port
        self.cameras = cameras
        self.error = None
        self.snapshot = False
        self.edit = None
        self.clicks = 0
        self.read = threading.Event()

    def run(self):
        try:
            self.talk()
        except Exception:
            self.error = traceback.format_exc()

    def click(self, ws, camera, t):
        self.read.clear()
        ws.send(2, camera)
        ws.drain(0.3)
        x, y = VW // 2, VH // 2
        ws.send(2, wsclient.input_frame(wsclient.MOVE, x, y, time_ms=t))
        ws.drain(0.3)
        ws.send(2, wsclient.input_frame(wsclient.PRESS, x, y, code=1, time_ms=t + 400))
        ws.send(2, wsclient.input_frame(wsclient.RELEASE, x, y, code=1, time_ms=t + 430))
        ws.drain(1.5)
        self.clicks += 1
        self.read.wait(30.0)

    def talk(self):
        ws = WS(self.port)
        ws.hello("serve-dressup-preview-pick")
        self.snapshot = ws.next_binary(20.0) is not None
        if not self.snapshot:
            return
        ws.next_binary(0.5)
        ws.send(2, self.cameras[0])
        ws.drain(0.3)
        raw = ws.op('{"id":1,"op":"edit","obj":"Fillet","mode":0}')
        self.edit = json.loads(raw.decode("utf-8")) if raw else None
        if not (self.edit or {}).get("ok"):
            ws.close()
            return
        ws.drain(2.0)
        for k, camera in enumerate(self.cameras):
            self.click(ws, camera, 1000 * (k + 1))
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
        wedge = body.newObject("PartDesign::AdditiveWedge", "Wedge")
        doc.recompute()
        shape = wedge.Shape
        top = bottom = None
        for i, e in enumerate(shape.Edges):
            ends = e.Vertexes
            if len(ends) != 2:
                continue
            if (all(abs(v.Point.y - shape.BoundBox.YMax) < 1e-7 for v in ends)
                    and all(abs(v.Point.z - wedge.Z2min.Value) < 1e-7 for v in ends)):
                top = "Edge%d" % (i + 1)
            if (all(abs(v.Point.y - shape.BoundBox.YMin) < 1e-7 for v in ends)
                    and all(abs(v.Point.z - wedge.Zmin.Value) < 1e-7 for v in ends)):
                bottom = "Edge%d" % (i + 1)
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (wedge, [top, bottom])
        fillet.Radius = 2
        doc.recompute()
        wedge.X2min = 5
        wedge.X2max = 5
        doc.recompute()
        kept = fillet.FailedShape
        if not check("the top edge is missing, its face kept",
                     len(fillet.FailedEdges) == 1 and not kept.isNull()
                     and len(kept.Faces) >= 1, list(fillet.FailedEdges)):
            finish()
            return
        state["base0"] = list(fillet.Base[1])
        note("INFO Base %s" % state["base0"])

        # off the middle of the kept face, where the missing edge's marker is
        face = kept.Faces[0]
        u0, u1, v0, v1 = face.ParameterRange
        u, v = u0 + (u1 - u0) * 0.5, v0 + (v1 - v0) * 0.2
        cameras = [looking_at(face.valueAt(u, v), face.normalAt(u, v))]

        # a base edge not in use, its middle seen between its two faces
        used = [s.lstrip("?") for s in state["base0"]]
        solid = wedge.Shape
        unused = None
        for i, e in enumerate(solid.Edges):
            name = "Edge%d" % (i + 1)
            faces = solid.ancestorsOfType(e, Part.Face)
            if name in used or len(faces) != 2 or e.Length < 2:
                continue
            mid = e.valueAt((e.FirstParameter + e.LastParameter) / 2)
            n = FreeCAD.Vector()
            for f in faces:
                fu, fv = f.Surface.parameter(mid)
                n += f.normalAt(fu, fv)
            if n.Length < 1e-6:
                continue
            unused = name
            cameras.append(looking_at(mid, n))
            break
        state["unused"] = unused
        note("INFO the unused edge clicked: %s" % unused)

        port = free_port()
        ok = FreeCADGui.serveDocument(doc, port)
        if not check("the document is served headless", ok, "port %d" % port):
            finish()
            return
        state["client"] = Client(port, cameras)
        state["client"].start()
        QtCore.QTimer.singleShot(50, poll)
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()


def poll():
    client = state["client"]
    if client.clicks > len(state["seen"]):
        widget = tree()
        state["seen"].append({
            "rows": rows(widget) if widget else None,
            "selected": [it.text(0) for it in widget.selectedItems()] if widget else None,
            "base": list(state["doc"].Fillet.Base[1]),
        })
        note("INFO after click %d: %s" % (len(state["seen"]), state["seen"][-1]))
        client.read.set()
    if client.is_alive():
        if clock() - state["t0"] > CLIENT_WAIT_S:
            check("the client finished", False, "still talking after %ds" % CLIENT_WAIT_S)
            finish()
            return
        QtCore.QTimer.singleShot(20, poll)
        return
    QtCore.QTimer.singleShot(300, verify)


def verify():
    client = state["client"]
    seen = state["seen"]
    missing = [s for s in state["base0"] if s.startswith("?")]
    try:
        check("the client ran without error", client.error is None, client.error or "")
        check("the hello was answered with a snapshot", client.snapshot)
        check("the fillet is edited from the client",
              (client.edit or {}).get("ok") is True, client.edit)
        check("both clicks were read", len(seen) == 2, len(seen))
        first = seen[0] if seen else {}
        check("a click on the missing edge's kept face selects its '?Edge' row",
              first.get("selected") == missing, first)
        check("... and changes no reference", first.get("base") == state["base0"], first)
        second = seen[1] if len(seen) > 1 else {}
        base = second.get("base") or []
        check("a click on an unused base edge after it: no missing reference left",
              base and not any(s.startswith("?") for s in base), base)
        check("... that edge in the missing one's place",
              base and base[state["base0"].index(missing[0])] == state["unused"],
              (state["base0"], base))
        check("... and in the list, in its row's place",
              (second.get("rows") or [])[:len(state["base0"])]
              == [state["unused"] if s in missing else s for s in state["base0"]],
              second.get("rows"))
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
