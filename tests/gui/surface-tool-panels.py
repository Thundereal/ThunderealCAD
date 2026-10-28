"""The task panels of the blend surface, the two-rail sweep and Pipeshell
(docs/Surface.md sec 8, U3).

  - editing each opens its panel;
  - the blend's side conditions set in it are the feature's, recomputed;
    OK keeps them as one undo step;
  - a section picked into the sweep's list is the feature's; its stations
    set; Cancel puts both back;
  - the pipe's mode and direction set in it are the feature's;
  - why a feature is not built is said in its panel, never in a dialog.

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
DOC = "SurfaceToolPanels"
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


def press(which):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            settle(0.4)
            return True
    return False


def panel(name):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget, name):
        if w.isVisible():
            return w
    return None


def edgeAtX(shape, x):
    for i, e in enumerate(shape.Edges):
        bb = e.BoundBox
        if abs(bb.XMin - x) < 1e-6 and abs(bb.XMax - x) < 1e-6:
            return "Edge%d" % (i + 1)
    return None


def run():
    try:
        V = FreeCAD.Vector
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        settle(0.5)
        doc = FreeCAD.newDocument(DOC)
        gdoc = FreeCADGui.getDocument(DOC)

        low = doc.addObject("Part::Plane", "Low")
        low.Length = low.Width = 10
        high = doc.addObject("Part::Plane", "High")
        high.Length = high.Width = 10
        high.Placement = FreeCAD.Placement(V(15, 0, 3), FreeCAD.Rotation(V(0, 1, 0), -16.7))
        r1 = doc.addObject("Part::Line", "Rail1")
        r1.X1, r1.Y1, r1.Z1, r1.X2, r1.Y2, r1.Z2 = 0, 30, 0, 20, 30, 0
        r2 = doc.addObject("Part::Line", "Rail2")
        r2.X1, r2.Y1, r2.Z1, r2.X2, r2.Y2, r2.Z2 = 0, 40, 0, 20, 46, 0
        s1 = doc.addObject("Part::Feature", "S1")
        s1.Shape = Part.Arc(V(0, 30, 0), V(0, 35, 5), V(0, 40, 0)).toShape()
        s2 = doc.addObject("Part::Feature", "S2")
        s2.Shape = Part.Arc(V(20, 30, 0), V(20, 38, 3), V(20, 46, 0)).toShape()
        doc.recompute()

        b = doc.addObject("Surface::BlendSurface", "Blend")
        b.Edge1 = (low, edgeAtX(low.Shape, 10))
        b.Edge2 = (high, edgeAtX(high.Shape, 15))
        sw = doc.addObject("Surface::TwoRailSweep", "Sweep")
        sw.Rail1, sw.Rail2 = (r1, "Edge1"), (r2, "Edge1")
        sw.Sections = [(s1, "Edge1")]
        helix = doc.addObject("Part::Helix", "Helix")
        helix.Pitch, helix.Height, helix.Radius = 4, 12, 5
        helix.Placement.Base = V(60, 0, 0)
        doc.recompute()
        e = helix.Shape.Edges[0]
        circle = doc.addObject("Part::Feature", "Circle")
        circle.Shape = Part.Wire(
            Part.makeCircle(0.5, e.valueAt(e.FirstParameter), e.tangentAt(e.FirstParameter))
        )
        pipe = doc.addObject("Surface::PipeShell", "Pipe")
        pipe.Spine = [(helix, "")]
        pipe.Profiles = [(circle, "")]
        doc.recompute()
        undos = len(doc.UndoNames)

        # the blend
        gdoc.setEdit(b, 0)
        settle(0.5)
        p = panel("SurfaceGui_BlendSurface")
        if not check("editing the blend opens its panel", p is not None, gdoc.getInEdit()):
            return finish()
        p.findChild(QtWidgets.QComboBox, "side1Continuity").setCurrentIndex(2)
        p.findChild(QtWidgets.QDoubleSpinBox, "side1Weight").setValue(1.8)
        settle(0.4)
        check(
            "a side's continuity and weight set in it are the feature's",
            b.Side1Continuity == "G2" and abs(b.Side1Weight - 1.8) < 1e-9 and b.isValid(),
            (b.Side1Continuity, b.Side1Weight, b.getStatusString()),
        )
        check(
            "OK closes it, as one undo step",
            press(QtWidgets.QDialogButtonBox.Ok)
            and gdoc.getInEdit() is None
            and len(doc.UndoNames) == undos + 1,
            doc.UndoNames[:3],
        )

        # the sweep
        gdoc.setEdit(sw, 0)
        settle(0.5)
        p = panel("SurfaceGui_TwoRailSweep")
        if not check("editing the sweep opens its panel", p is not None):
            return finish()
        p.findChild(QtWidgets.QToolButton, "listSectionsAdd").setChecked(True)
        settle(0.2)
        FreeCADGui.Selection.addSelection(s2, "Edge1")
        settle(0.4)
        check(
            "a section picked is the feature's, and listed",
            len(sw.Sections) == 2
            and p.findChild(QtWidgets.QListWidget, "listSections").count() == 2,
            sw.Sections,
        )
        p.findChild(QtWidgets.QSpinBox, "stations").setValue(5)
        settle(0.4)
        check("its stations set", sw.Stations == 5 and sw.isValid(), sw.getStatusString())
        check(
            "Cancel puts both back",
            press(QtWidgets.QDialogButtonBox.Cancel)
            and gdoc.getInEdit() is None
            and len(sw.Sections) == 1
            and sw.Stations == 9,
            (sw.Sections, sw.Stations),
        )

        # the pipe
        gdoc.setEdit(pipe, 0)
        settle(0.5)
        p = panel("SurfaceGui_PipeShell")
        if not check("editing the pipe opens its panel", p is not None):
            return finish()
        check(
            "its spine and profile are listed",
            p.findChild(QtWidgets.QListWidget, "listSpine").count() == 1
            and p.findChild(QtWidgets.QListWidget, "listProfiles").count() == 1,
        )
        p.findChild(QtWidgets.QComboBox, "mode").setCurrentIndex(3)
        p.findChild(QtWidgets.QDoubleSpinBox, "directionZ").setValue(1)
        settle(0.4)
        check(
            "its mode and direction set in it are the feature's",
            pipe.Mode == "Binormal" and pipe.Direction.z == 1,
            (pipe.Mode, pipe.Direction),
        )
        p.findChild(QtWidgets.QComboBox, "mode").setCurrentIndex(6)
        settle(0.4)
        status = p.findChild(QtWidgets.QLabel, "labelStatus")
        check(
            "the Support mode with no support is said in the panel",
            not pipe.isValid() and status.text() != "",
            status.text(),
        )
        check(
            "and OK refused, with no dialog",
            press(QtWidgets.QDialogButtonBox.Ok)
            and gdoc.getInEdit() is not None
            and QtWidgets.QApplication.activeModalWidget() is None,
        )
        press(QtWidgets.QDialogButtonBox.Cancel)
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
