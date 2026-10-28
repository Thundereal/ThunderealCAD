"""The Surface workbench's U3 commands (docs/Surface.md sec 5.1, 8, U3).

  - Blend surface, Two-rail sweep and Pipeshell sit on the Surface tool bar;
  - each makes its feature from the selection, in the order picked: two
    edges bridged, G1 on both sides; two rails and a section; a spine and a
    profile -- each built, as one undo step;
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
DOC = "SurfaceSweeps"
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


def made(doc, type_):
    return [o for o in doc.Objects if o.TypeId == type_]


def edgeAtX(shape, x):
    for i, e in enumerate(shape.Edges):
        bb = e.BoundBox
        if abs(bb.XMin - x) < 1e-6 and abs(bb.XMax - x) < 1e-6:
            return "Edge%d" % (i + 1)
    return None


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        settle(0.5)
        bar = mw.findChild(QtWidgets.QToolBar, "Surface")
        names = [a.objectName() for a in bar.actions()] if bar else []
        check(
            "Blend surface, Two-rail sweep and Pipeshell are on the Surface tool bar",
            all(
                n in names
                for n in ("Surface_BlendSurface", "Surface_TwoRailSweep", "Surface_PipeShell")
            ),
            names,
        )
        doc = FreeCAD.newDocument(DOC)
        V = FreeCAD.Vector
        low = doc.addObject("Part::Plane", "Low")
        low.Length = low.Width = 10
        high = doc.addObject("Part::Plane", "High")
        high.Length = high.Width = 10
        high.Placement = FreeCAD.Placement(V(15, 0, 3), FreeCAD.Rotation(V(0, 1, 0), -16.7))
        doc.recompute()
        undos = len(doc.UndoNames)

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(low, edgeAtX(low.Shape, 10))
        FreeCADGui.runCommand("Surface_BlendSurface")
        settle(0.3)
        check(
            "Blend surface with one edge makes nothing, and opens no dialog",
            not made(doc, "Surface::BlendSurface")
            and QtWidgets.QApplication.activeModalWidget() is None,
        )
        FreeCADGui.Selection.addSelection(high, edgeAtX(high.Shape, 15))
        FreeCADGui.runCommand("Surface_BlendSurface")
        settle(0.5)
        b = made(doc, "Surface::BlendSurface")
        check(
            "with two edges it bridges them, G1 on both sides",
            len(b) == 1
            and b[0].isValid()
            and len(b[0].Achieved) == 2
            and all("not met" not in x for x in b[0].Achieved),
            [(o.getStatusString(), list(o.Achieved)) for o in b],
        )
        check("as one undo step", len(doc.UndoNames) == undos + 1, doc.UndoNames[:3])

        r1 = doc.addObject("Part::Line", "Rail1")
        r1.X1, r1.Y1, r1.Z1, r1.X2, r1.Y2, r1.Z2 = 0, 0, 0, 20, 0, 0
        r2 = doc.addObject("Part::Line", "Rail2")
        r2.X1, r2.Y1, r2.Z1, r2.X2, r2.Y2, r2.Z2 = 0, 10, 0, 20, 16, 0
        sec = doc.addObject("Part::Feature", "Section")
        sec.Shape = Part.Arc(V(0, 0, 0), V(0, 5, 5), V(0, 10, 0)).toShape()
        doc.recompute()
        FreeCADGui.Selection.clearSelection()
        for o in (r1, r2, sec):
            FreeCADGui.Selection.addSelection(o, "Edge1")
        FreeCADGui.runCommand("Surface_TwoRailSweep")
        settle(0.5)
        s = made(doc, "Surface::TwoRailSweep")
        check(
            "Two-rail sweep from two rails and a section",
            len(s) == 1
            and s[0].isValid()
            and s[0].Shape.distToShape(Part.Vertex(V(20, 8, 8)))[0] < 2e-3,
            [o.getStatusString() for o in s],
        )

        helix = doc.addObject("Part::Helix", "Helix")
        helix.Pitch, helix.Height, helix.Radius = 4, 12, 5
        doc.recompute()
        e = helix.Shape.Edges[0]
        circle = doc.addObject("Part::Feature", "Circle")
        circle.Shape = Part.Wire(
            Part.makeCircle(0.5, e.valueAt(e.FirstParameter), e.tangentAt(e.FirstParameter))
        )
        doc.recompute()
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(helix)
        FreeCADGui.runCommand("Surface_PipeShell")
        settle(0.3)
        check(
            "Pipeshell with a spine alone makes nothing, and opens no dialog",
            not made(doc, "Surface::PipeShell")
            and QtWidgets.QApplication.activeModalWidget() is None,
        )
        FreeCADGui.Selection.addSelection(circle)
        FreeCADGui.runCommand("Surface_PipeShell")
        settle(0.5)
        p = made(doc, "Surface::PipeShell")
        check(
            "with a spine and a profile it sweeps",
            len(p) == 1 and p[0].isValid() and p[0].Shape.Area > 10,
            [o.getStatusString() for o in p],
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
