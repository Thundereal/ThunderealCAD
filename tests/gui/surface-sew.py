"""The Surface workbench's Sew command (docs/Surface.md sec 9, U0).

  - it sits on the Surface tool bar;
  - two selected faces become a Sewing of those faces, recomputed, with its
    gap readout: one shell, 6 free edges, the 0.01 gap closed at tolerance 0.1;
  - whole objects selected are sewn whole;
  - the creation is one undo step.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SurfaceSew"
V = FreeCAD.Vector
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


def sewings(doc):
    return [o for o in doc.Objects if o.TypeId == "Surface::Sewing"]


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        settle(0.5)
        bar = mw.findChild(QtWidgets.QToolBar, "Surface")
        names = [a.objectName() for a in bar.actions()] if bar else []
        check("Sew is on the Surface tool bar", "Surface_Sewing" in names, names)

        doc = FreeCAD.newDocument(DOC)
        a = doc.addObject("Part::Plane", "A")
        b = doc.addObject("Part::Plane", "B")
        a.Length = b.Length = a.Width = b.Width = 10
        b.Placement.Base = V(10.01, 0, 0)
        doc.recompute()
        settle(0.3)

        FreeCADGui.Selection.clearSelection()
        cmd = FreeCADGui.Command.get("Surface_Sewing")
        check("Sew is off with nothing selected", not cmd.isActive())
        FreeCADGui.Selection.addSelection(a, "Face1")
        FreeCADGui.Selection.addSelection(b, "Face1")
        settle(0.2)
        check("Sew is on with faces selected", cmd.isActive())
        cmd.run()
        settle(0.5)
        sews = sewings(doc)
        if check("one Sewing made", len(sews) == 1, len(sews)):
            sew = sews[0]
            check(
                "it sews the two selected faces",
                [(o.Name, subs) for o, subs in sew.ShapeList] == [("A", ("Face1",)), ("B", ("Face1",))],
                sew.ShapeList,
            )
            check("it is recomputed", sew.isValid() and not sew.Shape.isNull(), sew.State)
            check("the gap is open at the default tolerance", sew.FreeEdges == 8, sew.FreeEdges)
            sew.Tolerance = 0.1
            doc.recompute()
            check(
                "and closed at 0.1, read out",
                len(sew.Shape.Shells) == 1 and sew.FreeEdges == 6 and 0.004 < sew.MaxGap.Value < 0.1,
                (len(sew.Shape.Shells), sew.FreeEdges, sew.MaxGap),
            )

        doc.undo()
        settle(0.3)
        check("one undo step takes it away", len(sewings(doc)) == 0, len(sewings(doc)))

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(a)
        FreeCADGui.Selection.addSelection(b)
        cmd.run()
        settle(0.5)
        sews = sewings(doc)
        if check("whole objects make a Sewing", len(sews) == 1, len(sews)):
            check(
                "of the whole shapes",
                sews[0].isValid() and len(sews[0].Shape.Faces) == 2,
                (sews[0].State, sews[0].ShapeList),
            )
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
