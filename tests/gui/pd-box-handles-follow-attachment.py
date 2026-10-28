"""A PartDesign box's handles follow the box when it is attached in its own
edit (docs/HandsOnQueue.md entry 236, the reporter 2026-10-28: "partdesign,
box object, the one view edit handle does not sync with box orientation.
say I create a box, then attach to yz plane, the handle then controls the
wrong dimenion").

A placement change in the edit set the whole editing transform, the box's
new placement included, on the editing root: the handles, placed by that
placement in their frame, had it twice.

The handles are read off the scene graph: each dragger's matrix in the world
(SoGetMatrixAction). With the box on the XY plane at the origin in its edit,
M_xy; the box then attached to the YZ plane in the same edit, M_yz. Claims:
  - in the edit, each M_yz is the box's new placement times its M_xy;
  - the edit left and opened again, the same.
Runs offscreen as well.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets
from pivy import coin

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "BoxHandles"
STEPS = []
STATE = {}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds=0.5):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def draggers():
    """The world matrix of each dragger in the view, in scene order"""
    viewer = view().getViewer()
    # the editing root hangs under the aux root, beside the scene
    root = view().getAuxSceneGraph()
    # into the gizmo container, a node kit
    coin.SoBaseKit.setSearchingChildren(True)
    search = coin.SoSearchAction()
    search.setType(coin.SoDragger.getClassTypeId(), True)
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    out = []
    region = viewer.getSoRenderManager().getViewportRegion()
    for i in range(search.getPaths().getLength()):
        path = search.getPaths()[i]
        # the outermost dragger of a path only: a dragger's parts are
        # draggers too
        if any(path.getNode(j).isOfType(coin.SoDragger.getClassTypeId())
               for j in range(path.getLength() - 1)):
            continue
        action = coin.SoGetMatrixAction(region)
        action.apply(path)
        m = action.getMatrix()
        out.append(FreeCAD.Matrix(*[m.getValue()[c][r] for r in range(4) for c in range(4)]))
    return out


def close(a, b, tol=1e-4):
    return all(abs(a.A[i] - b.A[i]) <= tol * max(1.0, abs(b.A[i])) for i in range(16))


def start():
    doc = FreeCAD.newDocument(DOC)
    body = doc.addObject("PartDesign::Body", "Body")
    box = body.newObject("PartDesign::AdditiveBox", "Box")
    box.Length, box.Width, box.Height = 30, 20, 10
    doc.recompute()
    FreeCADGui.getDocument(DOC).setEdit(box, 0)


def on_xy():
    m = draggers()
    note("INFO %d draggers in the edit on the XY plane" % len(m))
    if not check("the box in its edit shows its handles", len(m) >= 3, len(m)):
        STEPS[:] = []
        return
    STATE["xy"] = m
    box = FreeCAD.getDocument(DOC).getObject("Box")
    body = FreeCAD.getDocument(DOC).getObject("Body")
    yz = [f for f in body.Origin.OriginFeatures if f.Name.startswith("YZ_Plane")][0]
    box.AttachmentSupport = [(yz, "")]
    box.MapMode = "FlatFace"
    FreeCAD.getDocument(DOC).recompute()


def compare(name):
    box = FreeCAD.getDocument(DOC).getObject("Box")
    P = box.Placement.toMatrix()
    m = draggers()
    xy = STATE["xy"]
    note("INFO %s: the box's placement %s" % (name, box.Placement))
    if not check("%s: as many handles" % name, len(m) == len(xy), (len(m), len(xy))):
        return
    # a handle points along its dragger's Y; how it is turned about that
    # axis is the dragger's own business
    def frame(mat):
        y = mat.multiply(FreeCAD.Vector(0, 1, 0)) - mat.multiply(FreeCAD.Vector(0, 0, 0))
        return mat.multiply(FreeCAD.Vector(0, 0, 0)), y.normalize()

    bad = []
    for i in range(len(m)):
        pos, d = frame(m[i])
        want_pos, want_d = frame(P.multiply(xy[i]))
        if (pos - want_pos).Length > 1e-4 or (d - want_d).Length > 1e-4:
            bad.append((i, tuple(round(c, 3) for c in d), tuple(round(c, 3) for c in want_d)))
    check("%s: each handle stands and points where the box's placement takes it from the XY plane"
          % name, not bad, "handles off (index, direction, wanted): %s" % bad)


def attached_in_edit():
    compare("attached to the YZ plane in the edit")
    FreeCADGui.getDocument(DOC).resetEdit()


def reopen():
    FreeCADGui.getDocument(DOC).setEdit(FreeCAD.getDocument(DOC).getObject("Box"), 0)


def reopened():
    compare("the edit opened again")
    FreeCADGui.getDocument(DOC).resetEdit()


def finish():
    try:
        FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


def advance():
    if not STEPS:
        finish()
        return
    step = STEPS.pop(0)
    try:
        step()
    except Exception:
        note("ABORT in %s: %s" % (step.__name__, traceback.format_exc().replace("\n", " | ")))
        STEPS[:] = []
    QtCore.QTimer.singleShot(2000, advance)


STEPS[:] = [start, on_xy, attached_in_edit, reopen, reopened]
QtCore.QTimer.singleShot(3000, advance)
