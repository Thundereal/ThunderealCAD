"""The Surface workbench's curve tools (docs/Surface.md sec 5.3, 8, U5).

  - Isocurves, Curve on surface, Intersection curve, Join curves and Extend
    curve sit on the Surface tool bar;
  - each makes its feature from the selection, built, as one undo step:
    isocurves of a face; a line laid on a face; a cylinder cut by a plane;
    two arcs joined; an edge extended;
  - a selection a command cannot use makes nothing, and opens no dialog.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import math
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SurfaceCurves"
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


def panel():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget, "SurfaceGui_Trim"):
        if w.isVisible():
            return w
    return None


def child(cls, name):
    p = panel()
    return p.findChild(cls, name) if p is not None else None


def press(which):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            settle(0.4)
            return True
    return False


def items(name):
    w = child(QtWidgets.QListWidget, name)
    return [w.item(i).text() for i in range(w.count())] if w is not None else None


COMMANDS = (
    "Surface_Isocurve",
    "Surface_CurveOnSurface",
    "Surface_IntersectionCurve",
    "Surface_JoinCurves",
    "Surface_ExtendCurve",
)


def made(doc, type_):
    return [o for o in doc.Objects if o.TypeId == type_]


def pick(*links):
    FreeCADGui.Selection.clearSelection()
    for link in links:
        FreeCADGui.Selection.addSelection(*link)


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        settle(0.5)
        bar = mw.findChild(QtWidgets.QToolBar, "Surface")
        names = [a.objectName() for a in bar.actions()] if bar else []
        check(
            "the curve tools are on the Surface tool bar",
            all(n in names for n in COMMANDS),
            names,
        )
        V = FreeCAD.Vector
        doc = FreeCAD.newDocument(DOC)
        plane = doc.addObject("Part::Plane", "Plane")
        plane.Length = plane.Width = 10
        line = doc.addObject("Part::Line", "Line")
        line.X1, line.Y1, line.Z1, line.X2, line.Y2, line.Z2 = 5, -2, 3, 5, 12, 3
        cyl = doc.addObject("Part::Cylinder", "Cyl")
        cyl.Radius, cyl.Height = 2, 6
        cyl.Placement.Base = V(30, 0, -3)
        s = 5 / math.sqrt(2)
        a = doc.addObject("Part::Feature", "A")
        a.Shape = Part.Arc(V(5, 0, 20), V(s, s, 20), V(0, 5, 20)).toShape()
        b = doc.addObject("Part::Feature", "B")
        b.Shape = Part.Arc(V(-5, 0, 20), V(-s, s, 20), V(0, 5, 20)).toShape()
        doc.recompute()
        undos = len(doc.UndoNames)

        pick((line, "Edge1"))
        FreeCADGui.runCommand("Surface_Isocurve")
        settle(0.3)
        check(
            "Isocurves with no face makes nothing, and opens no dialog",
            not made(doc, "Surface::Isocurve")
            and QtWidgets.QApplication.activeModalWidget() is None,
        )
        pick((plane, "Face1"))
        FreeCADGui.runCommand("Surface_Isocurve")
        settle(0.4)
        iso = made(doc, "Surface::Isocurve")
        check(
            "Isocurves of a face",
            len(iso) == 1 and iso[0].isValid() and len(iso[0].Shape.Edges) == 6,
            [(o.getStatusString(), len(o.Shape.Edges)) for o in iso],
        )
        check("as one undo step", len(doc.UndoNames) == undos + 1, doc.UndoNames[:3])

        pick((line, "Edge1"), (plane, "Face1"))
        FreeCADGui.runCommand("Surface_CurveOnSurface")
        settle(0.4)
        on = made(doc, "Surface::CurveOnSurface")
        check(
            "a line laid on a face",
            len(on) == 1
            and on[0].isValid()
            and plane.Shape.distToShape(on[0].Shape)[0] < 1e-7
            and abs(on[0].Shape.Length - 10) < 1e-6,
            [o.getStatusString() for o in on],
        )

        cut = doc.addObject("Part::Plane", "CutPlane")
        cut.Length = cut.Width = 20
        cut.Placement.Base = V(20, -10, 0)
        doc.recompute()
        pick((cyl, ""), (cut, ""))
        FreeCADGui.runCommand("Surface_IntersectionCurve")
        settle(0.4)
        x = made(doc, "Surface::IntersectionCurve")
        check(
            "a cylinder cut by a plane",
            len(x) == 1 and x[0].isValid() and abs(x[0].Shape.Length - 4 * math.pi) < 1e-4,
            [(o.getStatusString(), o.Shape.Length) for o in x],
        )

        pick((a, "Edge1"), (b, "Edge1"))
        FreeCADGui.runCommand("Surface_JoinCurves")
        settle(0.4)
        j = made(doc, "Surface::JoinCurves")
        check(
            "two arcs joined into one",
            len(j) == 1 and j[0].isValid() and len(j[0].Shape.Edges) == 1,
            [o.getStatusString() for o in j],
        )

        pick((line, "Edge1"))
        FreeCADGui.runCommand("Surface_ExtendCurve")
        settle(0.4)
        e = made(doc, "Surface::ExtendCurve")
        check(
            "an edge extended",
            len(e) == 1 and e[0].isValid() and abs(e[0].Shape.Length - 19) < 1e-6,
            [(o.getStatusString(), o.Shape.Length) for o in e],
        )
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
