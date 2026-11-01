"""Sheet metal's rule panel (docs/SheetMetal.md sec 4.6, 8.1).

  - a double-click on the rule opens its panel: a box for each value, named
    after it, and its card said -- the body's, with a gauge row for the
    thickness;
  - each value the card gives is marked as the card's, its reset button
    off; one typed in the panel is the rule's, marked set by hand, the
    reset button on; reset gives it back to the card;
  - a thickness the card has a gauge row for takes that row's values into
    the boxes; one it has none for is said, the values kept;
  - a card chosen in the card list is the rule's own; the list's first
    entry, the body's material, follows the body again;
  - Cancel puts every value back; OK keeps them in one undo step.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SheetMetalRulePanel"
V = FreeCAD.Vector
state = {"done": False}

CARD_VALUES = ("BendRadius", "KFactor", "MinFlange", "AllowanceMethod")
VALUES = CARD_VALUES + (
    "Thickness",
    "Gap",
    "ReliefShape",
    "ReliefWidth",
    "ReliefDepth",
    "CornerReliefShape",
    "CornerReliefSize",
)


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


def panel():
    """The open task panel's widgets by object name"""
    if not FreeCADGui.Control.activeDialog():
        return None
    view = FreeCADGui.getMainWindow().findChild(QtWidgets.QWidget, "TaskView")
    found = {}
    for w in (view or FreeCADGui.getMainWindow()).findChildren(QtWidgets.QWidget):
        if w.objectName() and w.isVisible():
            found.setdefault(w.objectName(), w)
    return found


def press(which):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            pump(60)
            return True
    return False


def card(gauges, name):
    """A material with the SheetMetal model: gauges as (thickness, radius, K,
    minimum flange) rows"""
    import Materials

    mat = Materials.Material()
    mat.Name = name
    mat.addPhysicalModel(Materials.UUIDs().SheetMetal)
    array = Materials.Array2D()
    array.Columns = 4
    array.Rows = len(gauges)
    for r, (t, radius, k, flange) in enumerate(gauges):
        array.setValue(r, 0, "%g mm" % t)
        array.setValue(r, 1, "%g mm" % radius)
        array.setValue(r, 2, "%g" % k)
        array.setValue(r, 3, "%g mm" % flange)
    mat.setValue("Gauges", array)
    return mat


def setSpin(box, value):
    """A value typed into a quantity box, as a person ends it"""
    box.setFocus()
    box.lineEdit().selectAll()
    box.lineEdit().insert("%g mm" % value)
    QtWidgets.QApplication.sendEvent(
        box,
        QtGui.QKeyEvent(QtCore.QEvent.KeyPress, QtCore.Qt.Key_Return, QtCore.Qt.NoModifier),
    )
    pump(30)


def run():
    try:
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        doc = FreeCAD.newDocument(DOC)
        view = FreeCADGui.ActiveDocument.ActiveView
        body = doc.addObject("PartDesign::Body", "Body")
        body.ShapeMaterial = card([(1, 1.5, 0.42, 6), (2, 2.5, 0.45, 10)], "Panel steel")
        view.setActiveObject("pdbody", body)
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        wall = body.newObject("PartDesign::SheetMetalBaseWall", "Wall")
        wall.Profile = sketch
        doc.recompute()
        rule = wall.Rule
        pump()
        check(
            "the rule follows the body's card",
            rule is not None and rule.ShapeMaterial.UUID == body.ShapeMaterial.UUID
            and abs(rule.BendRadius.Value - 1.5) < 1e-9,
            rule and (rule.ShapeMaterial.Name, rule.BendRadius),
        )

        rule.ViewObject.doubleClicked()
        pump(30)
        w = panel()
        check("a double-click on the rule opens its panel", w is not None and "TaskSheetMetalRule" in w)
        w = w or {}
        check(
            "a box for each value, named after it",
            all(n in w for n in VALUES),
            [n for n in VALUES if n not in w],
        )
        cardLabel = w.get("labelCard")
        text = cardLabel.text() if cardLabel else ""
        check(
            "its card said: the body's, with a gauge row for the thickness",
            "Panel steel" in text and "body" in text and "gauge" in text and "no gauge" not in text,
            text,
        )
        check(
            "the card's values marked as the card's, their reset off",
            all(
                "source" + n in w and "card" in w["source" + n].text()
                and "reset" + n in w and not w["reset" + n].isEnabled()
                for n in CARD_VALUES
            ),
            [(w["source" + n].text(), w["reset" + n].isEnabled()) for n in CARD_VALUES
             if "source" + n in w and "reset" + n in w],
        )
        check(
            "the boxes show the card's values",
            abs(w["BendRadius"].property("rawValue") - 1.5) < 1e-9
            and abs(w["KFactor"].value() - 0.42) < 1e-9,
            (w["BendRadius"].property("rawValue"), w["KFactor"].value()),
        )

        setSpin(w["BendRadius"], 3)
        check(
            "a value typed in the panel is the rule's, set by hand",
            abs(rule.BendRadius.Value - 3) < 1e-9 and "BendRadius" in rule.Overridden
            and "hand" in w["sourceBendRadius"].text() and w["resetBendRadius"].isEnabled(),
            (rule.BendRadius, rule.Overridden, w["sourceBendRadius"].text()),
        )
        check(
            "the features following it recomputed",
            "Touched" not in wall.State and "Invalid" not in wall.State,
            wall.State,
        )
        w["resetBendRadius"].click()
        pump(30)
        check(
            "reset gives it back to the card",
            abs(rule.BendRadius.Value - 1.5) < 1e-9 and "BendRadius" not in rule.Overridden
            and abs(w["BendRadius"].property("rawValue") - 1.5) < 1e-9
            and "card" in w["sourceBendRadius"].text() and not w["resetBendRadius"].isEnabled(),
            (rule.BendRadius, rule.Overridden, w["BendRadius"].property("rawValue")),
        )

        setSpin(w["Thickness"], 2)
        check(
            "a thickness with a gauge row takes that row into the boxes",
            abs(rule.BendRadius.Value - 2.5) < 1e-9
            and abs(w["BendRadius"].property("rawValue") - 2.5) < 1e-9
            and abs(w["KFactor"].value() - 0.45) < 1e-9,
            (rule.BendRadius, w["BendRadius"].property("rawValue"), w["KFactor"].value()),
        )
        setSpin(w["Thickness"], 3)
        text = w["labelCard"].text()
        check(
            "one with no gauge row is said, the values kept",
            "no gauge" in text and "3 mm" in text and abs(rule.BendRadius.Value - 2.5) < 1e-9,
            (text, rule.BendRadius),
        )

        tree = w.get("materialCard")
        check("a card list", tree is not None)
        import Materials

        manager = Materials.MaterialManager()
        steel = [
            uuid for uuid, mat in manager.Materials.items() if mat.Name.startswith("Steel")
        ]
        if tree is not None and steel:
            QtCore.QMetaObject.invokeMethod(tree, "onMaterial", QtCore.Q_ARG(str, steel[0]))
            pump(30)
        check(
            "a card chosen there is the rule's own",
            steel and rule.ShapeMaterial.UUID == steel[0]
            and body.ShapeMaterial.Name == "Panel steel"
            and "own" in w["labelCard"].text(),
            (rule.ShapeMaterial.Name, w["labelCard"].text()),
        )
        if tree is not None:
            QtCore.QMetaObject.invokeMethod(tree, "leadingEntrySelected")
            pump(30)
        check(
            "the list's first entry follows the body again",
            rule.ShapeMaterial.UUID == body.ShapeMaterial.UUID and "body" in w["labelCard"].text(),
            (rule.ShapeMaterial.Name, w["labelCard"].text()),
        )

        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(30)
        check(
            "Cancel puts every value back",
            not FreeCADGui.Control.activeDialog() and abs(rule.Thickness.Value - 1) < 1e-9
            and abs(rule.BendRadius.Value - 1.5) < 1e-9 and list(rule.Overridden) == []
            and rule.ShapeMaterial.UUID == body.ShapeMaterial.UUID
            and abs(wall.Shape.BoundBox.ZLength - 1) < 1e-6,
            (rule.Thickness, rule.BendRadius, rule.Overridden, wall.Shape.BoundBox.ZLength),
        )

        rule.ViewObject.doubleClicked()
        pump(30)
        w = panel() or {}
        setSpin(w["Thickness"], 2)
        setSpin(w["Gap"], 0.3)
        press(QtWidgets.QDialogButtonBox.Ok)
        pump(30)
        check(
            "OK keeps them, the wall rebuilt",
            not FreeCADGui.Control.activeDialog() and abs(rule.Thickness.Value - 2) < 1e-9
            and abs(rule.Gap.Value - 0.3) < 1e-9 and abs(wall.Shape.BoundBox.ZLength - 2) < 1e-6
            and "Touched" not in wall.State and "Touched" not in rule.State,
            (rule.Thickness, rule.Gap, wall.Shape.BoundBox.ZLength, wall.State, rule.State),
        )
        doc.undo()
        doc.recompute()
        pump(10)
        check(
            "in one undo step",
            abs(rule.Thickness.Value - 1) < 1e-9 and abs(rule.Gap.Value - 0.1) < 1e-9,
            (rule.Thickness, rule.Gap),
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
