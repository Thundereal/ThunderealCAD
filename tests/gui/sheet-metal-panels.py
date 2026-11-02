"""Sheet metal's task panels (docs/SheetMetal.md sec 5.1, 8.1).

  - Base Wall opens its panel: a box for each value, named after it, the
    rule's thickness and radius said, no status while it is good;
  - a value set in the panel is the feature's, recomputed;
  - while the panel is open the Sheet Metal tool bar is offered, and OK
    takes it away again;
  - Flange opens its panel with its edge listed; the angle set there is the
    flange's; the edge removed leaves it saying why it is invalid; another
    edge picked with Add is listed and the flange good again; its profile
    offered from the body's sketches, one chosen greying out Length and the
    gaps, a sketch not parallel to the leg said in its status, None taking it
    away;
  - Extend Wall's panel lists its face, has its gaps, its Add says it picks
    faces, its length set there;
  - the flat pattern's panel, on a double-click: what it lays flat, its
    manufacturing values, the root face picked in the view (the flat
    recomputed on it), Cancel putting it back, OK keeping it;
  - Cancel on a new base wall takes it away;
  - the panel says its values in words (Material side, Outer sharp), a
    plate's Length, Reversed and Midplane greyed out, as the plate does not
    use them, and a value set from Python while it is open shown in its box.

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


def labels():
    view = FreeCADGui.getMainWindow().findChild(QtWidgets.QWidget, "TaskView")
    return [w.text() for w in (view or FreeCADGui.getMainWindow()).findChildren(QtWidgets.QLabel)
            if w.isVisible()]


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
        check("the panel says its values in words", "Material side" in labels(), labels())
        check(
            "a plate's Length, Reversed and Midplane greyed out",
            all(n in w and not w[n].isEnabled() for n in ("Length", "Reversed", "Midplane"))
            and w["MaterialSide"].isEnabled(),
            [(n, w[n].isEnabled()) for n in ("Length", "Reversed", "Midplane") if n in w],
        )

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
        kinds = w.get("LengthType")
        items = [kinds.itemText(i) for i in range(kinds.count())] if kinds else []
        check("the choices in words too", "Outer sharp" in items, items)
        flange.Length = 17
        pump(10)
        check(
            "a value set from Python shows in its box",
            abs(w["Length"].property("rawValue") - 17) < 1e-9,
            w["Length"].property("rawValue"),
        )
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
        profile = w.get("Profile")
        items = [profile.itemText(i) for i in range(profile.count())] if profile else []
        check("a profile offered from the body's sketches", items == ["None", "Sketch"], items)
        profile.setCurrentIndex(1)
        pump(20)
        doc.recompute()
        pump(10)
        check(
            "a profile chosen is the flange's, Length and the gaps greyed out",
            flange.Profile and flange.Profile[0] == sketch
            and all(not w[n].isEnabled() for n in ("Length", "LengthType", "Gap1", "Gap2"))
            and w["Angle"].isEnabled(),
            (flange.Profile, [(n, w[n].isEnabled()) for n in ("Length", "Gap1")]),
        )
        check(
            "the base wall's sketch is no profile for it: it says why",
            "parallel to the leg" in w["labelStatus"].text(),
            w["labelStatus"].text(),
        )
        profile.setCurrentIndex(0)
        pump(20)
        doc.recompute()
        pump(10)
        check(
            "None takes it away again",
            flange.Profile is None
            and w["Length"].isEnabled()
            and "Invalid" not in flange.State,
            (flange.Profile, flange.State),
        )
        relief = w.get("ReliefShape")
        check(
            "the relief follows the rule, its width and depth greyed out",
            relief is not None
            and flange.ReliefShape == "Rule"
            and all(n in w and not w[n].isEnabled() for n in ("ReliefWidth", "ReliefDepth")),
            [(n, n in w and w[n].isEnabled()) for n in ("ReliefShape", "ReliefWidth")],
        )
        if relief is not None:
            relief.setCurrentIndex(1)
            pump(20)
            doc.recompute()
            pump(10)
            check(
                "a shape of its own: the flange's, width and depth from the rule's, open",
                flange.ReliefShape == "Rectangle"
                and all(w[n].isEnabled() for n in ("ReliefWidth", "ReliefDepth"))
                and abs(w["ReliefWidth"].property("rawValue") - 1) < 1e-9,
                (flange.ReliefShape, w["ReliefWidth"].isEnabled(), flange.ReliefWidth),
            )
            relief.setCurrentIndex(0)
            pump(20)
            doc.recompute()
            pump(10)
        accept()
        pump(20)

        # the flange went to the near edge in the end: the far end is plain
        far = [
            "Face%d" % (i + 1)
            for i, f in enumerate(flange.Shape.Faces)
            if abs(f.BoundBox.YMin - 20) < 1e-7 and abs(f.BoundBox.YMax - 20) < 1e-7
        ]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, flange.Name, far[0])
        FreeCADGui.runCommand("PartDesign_SheetMetalExtendWall")
        pump()
        ext = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalExtendWall"][0]
        w = panel() or {}
        faces = w.get("listFaces")
        check(
            "Extend Wall opens its panel with its face listed and its length",
            faces is not None and faces.count() == 1 and "Length" in w,
            (faces and faces.count(), "Length" in w),
        )
        check("with its gaps", "Gap1" in w and "Gap2" in w)
        add = w.get("buttonAdd")
        check(
            "its Add picks faces, and says so",
            add is not None and "faces" in add.toolTip(),
            add and add.toolTip(),
        )
        w["Length"].setProperty("rawValue", 7.0)
        pump(20)
        doc.recompute()
        check("the length set there is the feature's", abs(ext.Length.Value - 7) < 1e-9,
              ext.Length.Value)
        accept()
        pump(20)

        # -- the cutout's panel: its sketch, its way and how far ----------------
        hole = body.newObject("Sketcher::SketchObject", "Hole")
        hole.MapMode = "Deactivated"
        hole.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        hole.addGeometry(Part.Circle(V(20, 10, 0), V(0, 0, 1), 3))
        doc.recompute()
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, hole.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalCutout")
        pump()
        cutout = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalCutout"][0]
        w = panel() or {}
        check(
            "Cutout opens its panel with its sketch, its way and how far",
            all(n in w for n in ("Profile", "Reversed", "UpToFirst")),
            sorted(n for n in w if n[0].isupper()),
        )
        profile = w.get("Profile")
        check(
            "its sketch the one picked",
            profile is not None and profile.currentText() == hole.Label,
            profile and profile.currentText(),
        )
        first = w.get("UpToFirst")
        if first is not None:
            first.click()
            pump(20)
            doc.recompute()
            check("the first only, set there, is the feature's", cutout.UpToFirst,
                  cutout.UpToFirst)
        accept()
        pump(20)
        check("OK keeps a good cutout", "Invalid" not in cutout.State, cutout.State)

        # -- the flat pattern's panel ------------------------------------------
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.runCommand("PartDesign_SheetMetalFlatPattern")
        pump()
        doc.recompute()
        flat = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalFlatPattern"][0]
        flat.ViewObject.doubleClicked()
        pump(30)
        w = panel() or {}
        source = w.get("labelSource")
        check(
            "a double-click on the flat opens its panel, saying what it lays flat",
            source is not None and flat.Source.Label in source.text(),
            source and source.text(),
        )
        length = w.get("FlatLength")
        check(
            "with its manufacturing values",
            length is not None and "mm" in length.text() and w.get("BendCount") is not None
            and w["BendCount"].text() == str(flat.BendCount),
            (length and length.text(), w.get("BendCount") and w["BendCount"].text()),
        )
        roots = w.get("listRootFace")
        check("no root face picked: the first wall's", roots is not None and roots.count() == 0)
        skin = [
            "Face%d" % (i + 1)
            for i, f in enumerate(flat.Source.Shape.Faces)
            if abs(f.BoundBox.ZMin - 0.5) < 1e-7 and abs(f.BoundBox.ZMax - 0.5) < 1e-7
        ]
        w["buttonPick"].setChecked(True)
        pump(10)
        FreeCADGui.Selection.addSelection(doc.Name, flat.Source.Name, skin[0])
        pump(30)
        check(
            "a face picked is the root, the flat recomputed on it",
            roots.count() == 1 and flat.RootFace[1] == [skin[0]] and "Invalid" not in flat.State
            and not w["buttonPick"].isChecked(),
            (roots.count(), flat.RootFace, flat.State),
        )
        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(30)
        check(
            "Cancel puts it back",
            not FreeCADGui.Control.activeDialog() and flat.RootFace is None,
            flat.RootFace,
        )
        flat.ViewObject.doubleClicked()
        pump(30)
        w = panel() or {}
        w["buttonPick"].setChecked(True)
        pump(10)
        FreeCADGui.Selection.addSelection(doc.Name, flat.Source.Name, skin[0])
        pump(30)
        accept()
        pump(20)
        check(
            "OK keeps it, the flat built on it",
            not FreeCADGui.Control.activeDialog() and flat.RootFace[1] == [skin[0]]
            and flat.State == ["Up-to-date"],
            (flat.RootFace, flat.State),
        )

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
