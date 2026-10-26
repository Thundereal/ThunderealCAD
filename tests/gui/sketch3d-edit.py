"""The 3D sketch's edit mode, phase S2a (docs/Sketch3D.md section 7).

  - double-click / setEdit enters the edit: the Sketcher workbench comes up
    (EditingWorkbench, through TempoVis) with its edit-mode toolbars, as a 2D
    sketch's edit does from any workbench;
  - the edit scene draws the sketch's three legs and the external reference,
    and the task panel says the sketch is fully constrained;
  - a dimension changed during the edit redraws the scene;
  - Sketcher_LeaveSketch is active in a 3D sketch's edit and leaves it: the
    workbench and its toolbars come back;
  - Sketcher_NewSketch3D creates a 3D sketch in the active Body and opens it.

Run through scripts/gui-test.sh (xvfb, isolated configuration, external
timeout); registered in ctest by tests/gui/CMakeLists.txt.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "Sketch3DEdit"
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds=0.3):
    import time

    end = time.monotonic() + seconds
    while True:
        QtCore.QCoreApplication.processEvents()
        if time.monotonic() >= end:
            break
        time.sleep(0.01)


def in_edit():
    vp = FreeCADGui.getDocument(DOC).getInEdit()
    return vp.Object if vp else None


def toolbar_visible(name):
    bar = FreeCADGui.getMainWindow().findChild(QtWidgets.QToolBar, name)
    return bool(bar and bar.isVisible())


def edit_scene():
    """The edit's line set and marker set, found by the edit root's name."""
    from pivy import coin

    view = FreeCADGui.getDocument(DOC).ActiveView
    sa = coin.SoSearchAction()
    sa.setName("Sketch3DEdit")
    sa.setInterest(coin.SoSearchAction.FIRST)
    sa.apply(view.getViewer().getSoRenderManager().getSceneGraph())
    path = sa.getPath()
    if not path:
        return None
    root = path.getTail()
    found = {}
    for kind in (coin.SoLineSet, coin.SoMarkerSet, coin.SoCoordinate3):
        s = coin.SoSearchAction()
        s.setType(kind.getClassTypeId())
        s.setInterest(coin.SoSearchAction.FIRST)
        s.apply(root)
        found[kind.__name__] = s.getPath().getTail() if s.getPath() else None
    return found


def task_text():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QLabel):
        if w.isVisible() and ("constrained" in w.text() or "freedom" in w.text()):
            return w.text()
    return ""


def add_path(sk):
    import Part
    import Sketcher

    V = FreeCAD.Vector
    l0 = sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(9, 0.5, 0.3)))
    l1 = sk.addGeometry(Part.LineSegment(V(9, 0.5, 0.3), V(9.4, 0.2, 18)))
    l2 = sk.addGeometry(Part.LineSegment(V(9.4, 0.2, 18), V(10, 14, 19)))
    sk.addConstraint(
        [
            Sketcher.Constraint("Coincident", -1, 1, l0, 1),
            Sketcher.Constraint("Coincident", l0, 2, l1, 1),
            Sketcher.Constraint("Coincident", l1, 2, l2, 1),
            Sketcher.Constraint("AlongX", l0),
            Sketcher.Constraint("AlongZ", l1),
            Sketcher.Constraint("Perpendicular", l1, l2),
            Sketcher.Constraint("Perpendicular", l0, l2),
        ]
    )
    return sk.addConstraint(
        [
            Sketcher.Constraint("Distance", l0, 10.0),
            Sketcher.Constraint("Distance", l1, 20.0),
            Sketcher.Constraint("Distance", l2, 15.0),
        ]
    )


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument(DOC)
        box = doc.addObject("Part::Box", "Box")
        box.Placement.Base = FreeCAD.Vector(30, 0, 0)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Path")
        lengths = add_path(sk)
        doc.recompute()
        sk.addExternal(box, "Edge1")
        doc.recompute()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        settle(1.0)

        check(
            "the view provider is the 3D sketch's",
            sk.ViewObject.TypeId == "SketcherGui::ViewProviderSketch3D",
            sk.ViewObject.TypeId,
        )
        check(
            "outside an edit Leave Sketch is inactive",
            not FreeCADGui.Command.get("Sketcher_LeaveSketch").isActive(),
        )

        # -- enter -----------------------------------------------------------
        FreeCADGui.getDocument(DOC).setEdit(sk)
        settle(1.0)
        check("in edit", in_edit() == sk)
        check(
            "the Sketcher workbench came up",
            FreeCADGui.activeWorkbench().name() == "SketcherWorkbench",
            FreeCADGui.activeWorkbench().name(),
        )
        check("with its edit-mode toolbar", toolbar_visible("Sketcher edit mode"))
        check("and not its outside-edit one", not toolbar_visible("Sketcher"))
        scene = edit_scene()
        check("the edit scene is in the view", scene is not None)
        if scene:
            lines = scene["SoLineSet"]
            counts = list(lines.numVertices.getValues()) if lines else []
            # three legs and the external edge, two vertices each
            check("it draws the three legs and the external edge", counts == [2, 2, 2, 2], counts)
            markers = scene["SoMarkerSet"]
            check("and their end points", markers is not None)
        check(
            "the task panel says fully constrained", "Fully constrained" in task_text(), task_text()
        )

        sk.setDatum(lengths[2], 30.0)
        settle(0.5)
        end = sk.Geometry[2].EndPoint
        check("a dimension change moved the geometry", abs(end.y - 30) < 1e-6, end)
        scene = edit_scene()
        ys = (
            [p[1] for p in scene["SoCoordinate3"].point.getValues()]
            if scene and scene["SoCoordinate3"]
            else []
        )
        check("and the edit scene with it", any(abs(y - 30) < 1e-4 for y in ys), ys)

        # -- leave -----------------------------------------------------------
        leave = FreeCADGui.Command.get("Sketcher_LeaveSketch")
        check("Leave Sketch is active in a 3D sketch's edit", leave.isActive())
        leave.run()
        settle(1.0)
        check("Leave Sketch left the edit", in_edit() is None)
        check(
            "the workbench came back",
            FreeCADGui.activeWorkbench().name() == "PartWorkbench",
            FreeCADGui.activeWorkbench().name(),
        )
        check("the edit scene went", edit_scene() is None)

        # -- create ----------------------------------------------------------
        body = doc.addObject("PartDesign::Body", "Body")
        FreeCADGui.getDocument(DOC).ActiveView.setActiveObject("pdbody", body)
        settle(0.3)
        create = FreeCADGui.Command.get("Sketcher_NewSketch3D")
        check("Create 3D Sketch is active", create.isActive())
        create.run()
        settle(1.0)
        made = in_edit()
        check(
            "it made a 3D sketch and opened it",
            made is not None and made.TypeId == "Sketcher::Sketch3DObject",
            made.TypeId if made else None,
        )
        check("in the active Body", made is not None and made in body.Group)
        check(
            "the panel says what is free",
            "freedom" in task_text() or "constrained" in task_text(),
            task_text(),
        )
        FreeCADGui.getDocument(DOC).resetEdit()
        settle(0.5)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    if FreeCAD.listDocuments().get(DOC) and in_edit():
        FreeCADGui.getDocument(DOC).resetEdit()
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
