"""Sheet metal's task panels (docs/SheetMetal.md sec 5.1, 8.1).

  - Base Wall opens its panel: a box for each value, named after it, the
    rule's thickness and radius said, no status while it is good;
  - a value set in the panel is the feature's, recomputed;
  - while the panel is open the Sheet Metal tool bar is offered, and OK
    takes it away again;
  - Flange opens its panel with its edge listed; the angle set there is the
    flange's; the edge removed leaves it saying why it is invalid; another
    edge picked with Add is listed and the flange good again;
  - Cancel on a new base wall takes it away.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SheetMetalPanels"
V = FreeCAD.Vector
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()


def toolbar():
    for tb in FreeCADGui.getMainWindow().findChildren(QtWidgets.QToolBar, "Sheet Metal"):
        return tb
    return None


def offered():
    tb = toolbar()
    return tb is not None and tb.isVisible() and tb.toggleViewAction().isVisible()


def panel():
    """The open task panel's widgets by object name"""
    dlg = FreeCADGui.Control.activeDialog()
    if not dlg:
        return None
    view = FreeCADGui.getMainWindow().findChild(QtWidgets.QWidget, "TaskView")
    found = {}
    # the open panel's only: a closed one's widgets may not be gone yet
    for w in (view or FreeCADGui.getMainWindow()).findChildren(QtWidgets.QWidget):
        if w.objectName() and w.isVisible():
            found.setdefault(w.objectName(), w)
    return found


def edgeAt(feature, y, z):
    return [
        "Edge%d" % (i + 1)
        for i, e in enumerate(feature.Shape.Edges)
        if all(abs(v.Point.y - y) < 1e-7 and abs(v.Point.z - z) < 1e-7 for v in e.Vertexes)
    ][0]


def press(which):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            pump(60)
            return True
    return False


def accept():
    return press(QtWidgets.QDialogButtonBox.Ok)


def run():
    try:
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        doc = FreeCAD.newDocument(DOC)
        view = FreeCADGui.ActiveDocument.ActiveView
        body = doc.addObject("PartDesign::Body", "Body")
        view.setActiveObject("pdbody", body)
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        doc.recompute()
        pump()

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, sketch.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalBaseWall")
        pump()
        wall = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalBaseWall"][0]
        w = panel()
        check("Base Wall opens its panel", w is not None)
        w = w or {}
        check(
            "a box for each value, named after it",
            all(n in w for n in ("MaterialSide", "Length", "Reversed", "Midplane")),
            sorted(n for n in w if n[:1].isupper())[:20],
        )
        rule = w.get("labelRule")
        check(
            "the rule's thickness and radius said",
            rule is not None and "1 mm" in rule.text(),
            rule and rule.text(),
        )
        status = w.get("labelStatus")
        check("no status while it is good", status is not None and status.text() == "")
        check("while the panel is open the tool bar is offered", offered())

        side = w.get("MaterialSide")
        side.setCurrentIndex(2)
        pump(20)
        doc.recompute()
        check(
            "a value set in the panel is the feature's, recomputed",
            wall.MaterialSide == "Middle" and abs(wall.Shape.BoundBox.ZMin + 0.5) < 1e-6,
            (wall.MaterialSide, wall.Shape.BoundBox.ZMin),
        )
        accept()
        pump(20)
        check("OK closes it and takes the tool bar away", not FreeCADGui.Control.activeDialog()
              and not offered())

        edge = edgeAt(wall, 20, 0.5)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, wall.Name, edge)
        FreeCADGui.runCommand("PartDesign_SheetMetalFlange")
        pump()
        flange = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalFlange"][0]
        w = panel() or {}
        edges = w.get("listEdges")
        check(
            "Flange opens its panel with its edge listed",
            edges is not None and edges.count() == 1,
            edges and edges.count(),
        )
        angle = w.get("Angle")
        angle.setProperty("rawValue", 45.0)
        pump(20)
        doc.recompute()
        check("the angle set there is the flange's", abs(flange.Angle.Value - 45) < 1e-9,
              flange.Angle.Value)
        edges.setCurrentRow(0)
        w["buttonRemove"].click()
        pump(20)
        doc.recompute()
        pump(10)
        check(
            "the edge removed leaves it saying why",
            edges.count() == 0 and w["labelStatus"].text() != "",
            (edges.count(), flange.State, flange.Edges, w["labelStatus"].text()),
        )
        w["buttonAdd"].setChecked(True)
        pump(10)
        FreeCADGui.Selection.addSelection(doc.Name, wall.Name, edgeAt(wall, 0, 0.5))
        pump(30)
        doc.recompute()
        pump(10)
        check(
            "another edge picked with Add is listed, the flange good again",
            edges.count() == 1 and "Invalid" not in flange.State and w["labelStatus"].text() == "",
            (edges.count(), flange.State, w["labelStatus"].text()),
        )
        accept()
        pump(20)

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, sketch.Name)
        before = [o.Name for o in doc.Objects]
        FreeCADGui.runCommand("PartDesign_SheetMetalBaseWall")
        pump()
        made = [o.Name for o in doc.Objects if o.Name not in before]
        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(20)
        check(
            "Cancel on a new base wall takes it away",
            made and doc.getObject(made[0]) is None and not FreeCADGui.Control.activeDialog(),
            made,
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
