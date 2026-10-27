"""The Surface workbench's Blend Curve task panel (docs/Surface.md sec 9, U0;
taken from upstream, ruling i).

  - editing a Blend Curve opens its panel, which shows both edges;
  - a continuity, a parameter or a size changed in it is the feature's, and
    the curve is recomputed with it;
  - an edge picked in the view becomes the end's edge; the blend curve itself
    is not offered;
  - a continuity above the list's G4 is shown as it is, and left alone;
  - Cancel puts every value back; OK keeps them as one undo step.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SurfaceBlendCurve"
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
    # uic names the form's widget with the :: of its class name made __
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget, "SurfaceGui__BlendCurve"):
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
            settle(0.3)
            return True
    return False


def line(doc, name, a, b):
    o = doc.addObject("Part::Line", name)
    o.X1, o.Y1, o.Z1 = a
    o.X2, o.Y2, o.Z2 = b
    return o


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        settle(0.5)

        doc = FreeCAD.newDocument(DOC)
        gdoc = FreeCADGui.getDocument(DOC)
        l1 = line(doc, "L1", (0, 0, 0), (10, 0, 0))
        l2 = line(doc, "L2", (20, 5, 0), (30, 5, 0))
        l3 = line(doc, "L3", (20, -5, 0), (30, -5, 0))
        doc.recompute()
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(l1, "Edge1")
        FreeCADGui.Selection.addSelection(l2, "Edge1")
        FreeCADGui.runCommand("Surface_BlendCurve")
        settle(0.5)
        blends = [o for o in doc.Objects if o.TypeId == "Surface::FeatureBlendCurve"]
        if not check("Blend Curve made from two edges", len(blends) == 1, len(blends)):
            return finish()
        bc = blends[0]
        doc.recompute()
        poles = bc.Shape.Edges[0].Curve.NbPoles
        note(
            "INFO the curve: continuity %d/%d, %d poles"
            % (bc.StartContinuity, bc.EndContinuity, poles)
        )

        undos = len(doc.UndoNames)
        FreeCADGui.Selection.clearSelection()
        gdoc.setEdit(bc, 0)
        settle(0.5)
        if not check("editing it opens its panel", panel() is not None, gdoc.getInEdit()):
            gdoc.resetEdit()
            return finish()
        first = child(QtWidgets.QLineEdit, "firstEdgeEdit")
        second = child(QtWidgets.QLineEdit, "secondEdgeEdit")
        check(
            "the panel shows both edges",
            first.text() == "L1 [Edge1]" and second.text() == "L2 [Edge1]",
            (first.text(), second.text()),
        )

        cont = child(QtWidgets.QComboBox, "contFirstEdge")
        cont.setCurrentIndex(3)
        settle(0.3)
        check(
            "a continuity set in it is the feature's", bc.StartContinuity == 3, bc.StartContinuity
        )
        n = bc.Shape.Edges[0].Curve.NbPoles
        check("and the curve recomputed with it", n == poles + 1, (poles, n))

        param = child(QtWidgets.QDoubleSpinBox, "paramFirstEdge")
        param.setValue(0.5)
        size = child(QtWidgets.QDoubleSpinBox, "sizeSecondEdge")
        size.setValue(2.5)
        settle(0.3)
        check(
            "a parameter and a size set in it are the feature's",
            abs(bc.StartParameter - 0.5) < 1e-9 and abs(bc.EndSize - 2.5) < 1e-9,
            (bc.StartParameter, bc.EndSize),
        )
        check(
            "the curve starts at the parameter",
            bc.Shape.Vertexes[0].Point.isEqual(V(5, 0, 0), 1e-6),
            bc.Shape.Vertexes[0].Point,
        )

        button = child(QtWidgets.QToolButton, "buttonSecondEdge")
        button.setChecked(True)
        settle(0.2)
        FreeCADGui.Selection.addSelection(bc, "Edge1")
        settle(0.2)
        check(
            "the blend curve's own edge is not offered",
            not FreeCADGui.Selection.getSelectionEx() and bc.EndEdge[0] == l2,
            (FreeCADGui.Selection.getSelectionEx(), bc.EndEdge),
        )
        FreeCADGui.Selection.addSelection(l3, "Edge1")
        settle(0.4)
        check(
            "an edge picked becomes the end's edge",
            bc.EndEdge[0] == l3 and list(bc.EndEdge[1]) == ["Edge1"],
            bc.EndEdge,
        )
        check("and the panel shows it", second.text() == "L3 [Edge1]", second.text())
        check("the pick ends there", not button.isChecked())

        check(
            "Cancel closes the panel",
            press(QtWidgets.QDialogButtonBox.Cancel) and gdoc.getInEdit() is None,
            gdoc.getInEdit(),
        )
        check(
            "and puts every value back",
            bc.StartContinuity == 2
            and abs(bc.StartParameter) < 1e-9
            and abs(bc.EndSize - 1) < 1e-9
            and bc.EndEdge[0] == l2,
            (bc.StartContinuity, bc.StartParameter, bc.EndSize, bc.EndEdge),
        )
        check("with no undo step left", len(doc.UndoNames) == undos, doc.UndoNames[:3])

        gdoc.setEdit(bc, 0)
        settle(0.5)
        child(QtWidgets.QComboBox, "contSecondEdge").setCurrentIndex(4)
        settle(0.3)
        check(
            "OK closes the panel",
            press(QtWidgets.QDialogButtonBox.Ok) and gdoc.getInEdit() is None,
            gdoc.getInEdit(),
        )
        check("and keeps the value", bc.EndContinuity == 4, bc.EndContinuity)
        check("as one undo step", len(doc.UndoNames) == undos + 1, doc.UndoNames[:3])
        doc.undo()
        doc.recompute()
        check("which undoes it", bc.EndContinuity == 2, bc.EndContinuity)

        bc.StartContinuity = 7
        doc.recompute()
        gdoc.setEdit(bc, 0)
        settle(0.5)
        cont = child(QtWidgets.QComboBox, "contFirstEdge")
        check(
            "a continuity of 7 is shown as G7",
            cont is not None and cont.currentText() == "G7",
            cont.currentText() if cont else None,
        )
        press(QtWidgets.QDialogButtonBox.Ok)
        check("and left alone", bc.StartContinuity == 7, bc.StartContinuity)
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
