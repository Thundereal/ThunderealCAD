"""Sheet metal's Convert, its command and its panel (docs/SheetMetal.md sec 5.3).

  - the Part Design menu's Sheet Metal submenu holds Convert;
  - Convert on a Part box in no body -- its bottom picked first, then its
    top and its four upright edges -- makes a body on it, the box hidden,
    and the convert there, the bottom fixed, the top open, the edges ripped; it is good, a
    solid, the box's size;
  - its panel lists the three, each under its name; Side set to Outside
    there is the convert's, a thickness larger every way;
  - the open face removed closes the bends in a loop: the panel says to rip;
    the face picked again with that list's Add is listed and it is good;
  - one list's Add at a time: another's pressed lets the first up;
  - the fixed face picked again takes the place of the one it held;
  - OK closes the panel, the convert drawn and not the part under it;
    Flatten lays the converted tray flat.

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
DOC = "SheetMetalConvert"
state = {"done": False}
# a 90 deg bend's allowance at R 1, T 1, K 0.5
BA = (1 + 0.5) * math.pi / 2


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


def texts(widget):
    return [widget.item(i).text() for i in range(widget.count())] if widget else None


def ok():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(QtWidgets.QDialogButtonBox.Ok)
        if b is not None and b.isVisible():
            b.click()
            pump(60)
            return True
    return False


def menuActions(title):
    bar = FreeCADGui.getMainWindow().menuBar()
    for action in bar.actions():
        menu = action.menu()
        if not menu or action.text().replace("&", "") != "Part Design":
            continue
        for sub in menu.actions():
            if sub.menu() and sub.text().replace("&", "") == title:
                return sub.menu().actions()
    return None


def faceAt(shape, test):
    return ["Face%d" % (i + 1) for i, f in enumerate(shape.Faces) if test(f.BoundBox)][0]


def settle(doc):
    pump(20)
    doc.recompute()
    pump(20)


def run():
    try:
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        pump()
        actions = menuActions("Sheet Metal")
        cmd = FreeCADGui.Command.get("PartDesign_SheetMetalConvert")
        check(
            "the Sheet Metal submenu holds Convert",
            actions is not None and cmd is not None and cmd.getAction()[0] in actions,
            [a.text() for a in actions or []],
        )

        doc = FreeCAD.newDocument(DOC)
        box = doc.addObject("Part::Feature", "Box")
        box.Shape = Part.makeBox(40, 20, 10)
        doc.recompute()
        pump()
        shape = box.Shape
        bottom = faceAt(shape, lambda b: b.ZMax < 1e-7)
        top = faceAt(shape, lambda b: b.ZMin > 10 - 1e-7)
        front = faceAt(shape, lambda b: b.YMax < 1e-7)
        upright = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(shape.Edges)
            if abs(e.BoundBox.ZLength - 10) < 1e-7
        ]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, box.Name, bottom)
        FreeCADGui.Selection.addSelection(doc.Name, box.Name, top)
        for e in upright:
            FreeCADGui.Selection.addSelection(doc.Name, box.Name, e)
        FreeCADGui.runCommand("PartDesign_SheetMetalConvert")
        pump(60)
        doc.recompute()
        pump(20)
        convert = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalConvert"]
        convert = convert[0] if convert else None
        body = [o for o in doc.Objects if o.TypeId == "PartDesign::Body"]
        body = body[0] if body else None
        check(
            "Convert on a box in no body makes a body on it, the box hidden",
            body is not None and body.BaseFeature == box and not box.Visibility,
            body and (body.BaseFeature, box.Visibility),
        )
        check(
            "and the convert there, bottom fixed, top open, edges ripped",
            convert is not None
            and convert in body.Group
            and convert.FixedFace == (box, [bottom])
            and convert.OpenFaces == (box, [top])
            and sorted(convert.RipEdges[1]) == sorted(upright),
            convert and (convert.FixedFace, convert.OpenFaces, convert.RipEdges),
        )
        bb = convert.Shape.BoundBox
        check(
            "it is good, a solid, the box's size",
            "Invalid" not in convert.State
            and convert.Kind == "Solid"
            and abs(bb.XLength - 40) < 1e-6
            and abs(bb.ZLength - 10) < 1e-6,
            (convert.State, convert.getStatusString(), convert.Kind, bb),
        )

        w = panel() or {}
        check(
            "its panel lists the three",
            texts(w.get("listFixedFace")) == [bottom]
            and texts(w.get("listOpenFaces")) == [top]
            and sorted(texts(w.get("listRipEdges")) or []) == sorted(upright),
            [texts(w.get(n)) for n in ("listFixedFace", "listOpenFaces", "listRipEdges")],
        )
        side = w.get("Side")
        items = [side.itemText(i) for i in range(side.count())] if side else []
        check("Side offers Inside and Outside", items == ["Inside", "Outside"], items)
        side.setCurrentIndex(1)
        settle(doc)
        bb = convert.Shape.BoundBox
        check(
            "Outside set there is the convert's, a thickness larger every way",
            convert.Side == "Outside"
            and abs(bb.XLength - 42) < 1e-6
            and abs(bb.ZLength - 11) < 1e-6,
            (convert.Side, bb),
        )

        opened = w["listOpenFaces"]
        opened.setCurrentRow(0)
        w["buttonRemoveOpenFaces"].click()
        settle(doc)
        check(
            "the open face removed, the bends close a loop: it says to rip",
            opened.count() == 0 and "rip" in w["labelStatus"].text(),
            (opened.count(), w["labelStatus"].text()),
        )
        built = convert.BaseFeature
        w["buttonAddOpenFaces"].setChecked(True)
        pump(10)
        FreeCADGui.Selection.addSelection(doc.Name, built.Name, top)
        pump(30)
        settle(doc)
        check(
            "picked again with that list's Add it is listed, good again",
            texts(opened) == [top]
            and "Invalid" not in convert.State
            and w["labelStatus"].text() == "",
            (texts(opened), convert.State, w["labelStatus"].text()),
        )
        w["buttonAddRipEdges"].setChecked(True)
        pump(10)
        check(
            "one list's Add at a time",
            w["buttonAddRipEdges"].isChecked() and not w["buttonAddOpenFaces"].isChecked(),
        )
        w["buttonAddRipEdges"].setChecked(False)
        pump(10)

        w["buttonAddFixedFace"].setChecked(True)
        pump(10)
        FreeCADGui.Selection.addSelection(doc.Name, built.Name, front)
        pump(30)
        settle(doc)
        check(
            "the fixed face picked again takes the place of the one it held",
            texts(w["listFixedFace"]) == [front] and convert.FixedFace[1] == [front],
            (texts(w["listFixedFace"]), convert.FixedFace),
        )
        FreeCADGui.Selection.addSelection(doc.Name, built.Name, bottom)
        pump(30)
        settle(doc)
        w["buttonAddFixedFace"].setChecked(False)
        pump(10)
        side.setCurrentIndex(0)
        settle(doc)
        check(
            "and back to the bottom, inside, good",
            convert.FixedFace[1] == [bottom]
            and convert.Side == "Inside"
            and "Invalid" not in convert.State,
            (convert.FixedFace, convert.Side, convert.State, convert.getStatusString()),
        )

        check("OK closes the panel", ok() and not FreeCADGui.Control.activeDialog())
        doc.recompute()
        pump(20)
        drawn = [o.Name for o in body.Group if hasattr(o, "Shape") and o.Visibility]
        check(
            "the convert drawn alone of the body's features, not the part under it",
            drawn == [convert.Name] or drawn == [convert.Name, convert.Rule.Name],
            drawn,
        )

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.runCommand("PartDesign_SheetMetalFlatten", 1)
        pump(60)
        solid = body.getSolidBody(convert)
        flat = None
        for o in solid.InList if solid else []:
            if o.isDerivedFrom("PartDesign::SheetMetalFlatPattern"):
                flat = o
        leg = BA + 8
        check(
            "Flatten lays the converted tray flat",
            flat is not None
            and flat.Visibility
            and abs(flat.FlatLength.Value - (36 + 2 * leg)) < 1e-6
            and abs(flat.FlatWidth.Value - (16 + 2 * leg)) < 1e-6
            and flat.BendCount == 4,
            flat and (flat.Status, flat.FlatLength, flat.FlatWidth, flat.BendCount),
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
