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
        own = w.get("UseRuleRadius")
        check(
            "the radius the rule's, its own greyed out",
            own is not None and own.isChecked() and "BendRadius" in w
            and not w["BendRadius"].isEnabled(),
            [(n, n in w and w[n].isEnabled()) for n in ("UseRuleRadius", "BendRadius")],
        )
        if own is not None:
            own.click()
            pump(20)
            doc.recompute()
            pump(10)
            check(
                "a radius of its own: the flange's, from the rule's, open",
                not flange.UseRuleRadius and w["BendRadius"].isEnabled()
                and abs(w["BendRadius"].property("rawValue") - flange.Rule.BendRadius.Value)
                < 1e-9,
                (flange.UseRuleRadius, w["BendRadius"].isEnabled(), flange.BendRadius),
            )
            own.click()
            pump(20)
            doc.recompute()
            pump(10)

        # View: the feature folded, laid flat, or both (docs/SheetMetal.md
        # sec 12, 2026-11-04), while the panel is open. In an edit session
        # the flat is a node of the session's and the rest is hidden by its
        # views' table, out of reach here: the panel says what it draws
        vo = flange.ViewObject

        def field(name):
            value = vo.SwitchNode.getField(name).get()
            return int(value.getString() if hasattr(value, "getString") else str(value))

        def drawn():
            return (bool(combo.property("flatDrawn")), bool(combo.property("drawnAlone")))

        folded = (vo.RootNode.getNumChildren(), field("whichChild"))
        combo = w.get("comboSheetView")
        check(
            "the panel offers View: Folded, Flat, Both, Folded at first",
            combo is not None
            and [combo.itemText(i) for i in range(combo.count())] == ["Folded", "Flat", "Both"]
            and combo.currentIndex() == 0,
            combo and [combo.itemText(i) for i in range(combo.count())],
        )
        if combo is not None:
            combo.setCurrentIndex(1)
            pump(20)
            check("Flat: the flange's flat drawn, nothing else", drawn() == (True, True), drawn())
            combo.setCurrentIndex(2)
            pump(20)
            check("Both: the flat and the folded part", drawn() == (True, False), drawn())
            combo.setCurrentIndex(0)
            pump(20)
            check(
                "Folded: as it was",
                drawn() == (False, False)
                and (vo.RootNode.getNumChildren(), field("whichChild")) == folded,
                (drawn(), vo.RootNode.getNumChildren(), field("whichChild"), folded),
            )
            solid = body.getSolidBody(flange)
            flatObj = [
                o for o in solid.InList
                if o.isDerivedFrom("PartDesign::SheetMetalFlatPattern") and o.Sheet == solid
            ][0]
            flatObj.Root = "Wall9"
            doc.recompute()
            combo.setCurrentIndex(1)
            pump(20)
            said = w.get("labelFlatStatus")
            check(
                "a flange that does not lie flat says why, and is drawn folded",
                said is not None
                and "Wall9" in said.text()
                and drawn() == (False, False)
                and (vo.RootNode.getNumChildren(), field("whichChild")) == folded,
                said and (said.text(), drawn()),
            )
            flatObj.Root = ""
            doc.recompute()
            pump(20)
            check(
                "laid again after a recompute",
                said is not None and said.text() == "" and drawn() == (True, True),
                said and (said.text(), drawn()),
            )
        accept()
        pump(20)
        check(
            "the panel closed on Flat: the flange drawn folded again",
            (vo.RootNode.getNumChildren(), field("whichChild")) == folded,
            (vo.RootNode.getNumChildren(), field("whichChild"), folded),
        )

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
            all(n in w for n in ("Profile", "Wrap", "Reversed", "UpToFirst")),
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
        wrap = w.get("Wrap")
        check("and Wrap, the sketch as if drawn on the flat", wrap is not None)
        if wrap is not None:
            wrap.click()
            pump(20)
            doc.recompute()
            pump(20)
            check(
                "Wrap set there is the feature's, the way and the first only then not",
                cutout.Wrap and first is not None and not first.isEnabled()
                and not w["Reversed"].isEnabled(),
                (cutout.Wrap, first and first.isEnabled()),
            )
        accept()
        pump(20)
        check("OK keeps a good cutout", "Invalid" not in cutout.State, cutout.State)

        # -- the fold's panel: its line, its values, the face on the side that moves
        line = body.newObject("Sketcher::SketchObject", "FoldLine")
        line.MapMode = "Deactivated"
        line.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        line.addGeometry(Part.LineSegment(V(-5, 23, 0), V(45, 23, 0)))
        doc.recompute()
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, line.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalFold")
        pump()
        fold = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalFold"][0]
        w = panel() or {}
        check(
            "Fold opens its panel with its line, angle, line position and way",
            all(n in w for n in ("Profile", "Angle", "LinePosition", "Reversed")),
            sorted(n for n in w if n[0].isupper()),
        )
        moving = w.get("listMoving")
        check(
            "and a list of the face on the side that moves, none picked",
            moving is not None and moving.count() == 0,
            moving and moving.count(),
        )
        if "Angle" in w:
            w["Angle"].setProperty("rawValue", 60.0)
            pump(20)
            doc.recompute()
            check("the angle set there is the fold's", abs(fold.Angle.Value - 60) < 1e-9,
                  fold.Angle.Value)
        accept()
        pump(20)
        check("OK keeps a good fold", "Invalid" not in fold.State, fold.State)

        # -- the jog's panel: its line, its values, the face on the side that moves
        line = body.newObject("Sketcher::SketchObject", "JogLine")
        line.MapMode = "Deactivated"
        line.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        line.addGeometry(Part.LineSegment(V(-5, 16, 0), V(45, 16, 0)))
        doc.recompute()
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, line.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalJog")
        pump()
        jog = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalJog"][0]
        w = panel() or {}
        names = ("Profile", "Offset", "OffsetMeasure", "Angle", "LinePosition",
                 "FixProjectedLength", "Reversed")
        check(
            "Jog opens its panel with its line, offset and its measure, angle, line "
            "position, the end kept and way",
            all(n in w for n in names),
            sorted(n for n in w if n[0].isupper()),
        )
        moving = w.get("listMoving")
        check(
            "and a list of the face on the side that moves, none picked",
            moving is not None and moving.count() == 0,
            moving and moving.count(),
        )
        if "Offset" in w:
            w["Offset"].setProperty("rawValue", 4.0)
            pump(20)
            doc.recompute()
            check("the offset set there is the jog's", abs(jog.Offset.Value - 4) < 1e-9,
                  jog.Offset.Value)
        accept()
        pump(20)
        check("OK keeps a good jog", "Invalid" not in jog.State, jog.State)

        # -- the tab's panel: its sketch
        sketch = body.newObject("Sketcher::SketchObject", "TabSketch")
        sketch.MapMode = "Deactivated"
        sketch.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        pts = [(-8, 6), (0, 6), (0, 12), (-8, 12)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        doc.recompute()
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, sketch.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalTab")
        pump()
        tab = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalTab"][0]
        w = panel() or {}
        profile = w.get("Profile")
        check(
            "Tab opens its panel with its sketch, the one picked",
            profile is not None and profile.currentText() == sketch.Label,
            profile and profile.currentText(),
        )
        accept()
        pump(20)
        check("OK keeps a good tab", "Invalid" not in tab.State, tab.State)

        # -- the contour flange's panel: its edges, sketch, gaps and relief
        top = max(v.Point.z for v in tab.Shape.Vertexes if abs(v.Point.x + 8) < 1e-7)
        edge = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(tab.Shape.Edges)
            if all(abs(v.Point.x + 8) < 1e-7 and abs(v.Point.z - top) < 1e-7 for v in e.Vertexes)
        ]
        sketch = body.newObject("Sketcher::SketchObject", "ContourSketch")
        sketch.MapMode = "Deactivated"
        sketch.Placement = FreeCAD.Placement(
            V(0, 9, 0), FreeCAD.Rotation(V(1, 0, 0), V(0, 0, 1), V(0, -1, 0), "ZXY")
        )
        sketch.addGeometry(Part.LineSegment(V(-8, top, 0), V(-8, top + 5, 0)))
        doc.recompute()
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, tab.Name, edge[0])
        FreeCADGui.Selection.addSelection(doc.Name, sketch.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalContourFlange")
        pump()
        contour = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalContourFlange"][0]
        w = panel() or {}
        names = ("Profile", "Gap1", "Gap2", "ReliefShape", "ReliefWidth", "ReliefDepth")
        edges = w.get("listEdges")
        check(
            "Contour Flange opens its panel with its sketch, gaps and relief, its edge listed",
            all(n in w for n in names) and edges is not None and edges.count() == 1,
            (sorted(n for n in w if n[0].isupper()), edges and edges.count()),
        )
        check(
            "and none of the flange's angle, length or bend position",
            not any(n in w for n in ("Angle", "Length", "LengthType", "BendPosition")),
            sorted(n for n in w if n[0].isupper()),
        )
        check(
            "its gaps open to edit",
            "Gap1" in w and w["Gap1"].isEnabled() and not w["Gap1"].isReadOnly(),
            "Gap1" in w and (w["Gap1"].isEnabled(), w["Gap1"].isReadOnly()),
        )
        if "Gap1" in w:
            w["Gap1"].setProperty("rawValue", 1.0)
            pump(20)
            doc.recompute()
            check("the gap set there is the contour flange's", abs(contour.Gap1.Value - 1) < 1e-9,
                  contour.Gap1.Value)
        accept()
        pump(20)
        check("OK keeps a good contour flange", "Invalid" not in contour.State, contour.State)

        # -- the join by bend's panel: its two edges and relief; a plate 10
        # above the contour's leg and 10 beyond it
        pieces = contour.AddSubShape
        legTop = max(v.Point.z for v in pieces.Vertexes)
        top = [v.Point for v in pieces.Vertexes if abs(v.Point.z - legTop) < 1e-7]
        legX = max(p.x for p in top)
        y0, y1 = min(p.y for p in top), max(p.y for p in top)
        x0, z0 = legX + 10, legTop + 10
        plateSketch = body.newObject("Sketcher::SketchObject", "JoinSketch")
        plateSketch.MapMode = "Deactivated"
        plateSketch.Placement = FreeCAD.Placement(V(x0, y0, z0), FreeCAD.Rotation())
        pts = [(0, 0), (20, 0), (20, y1 - y0), (0, y1 - y0)]
        for i in range(4):
            p, q = pts[i], pts[(i + 1) % 4]
            plateSketch.addGeometry(Part.LineSegment(V(p[0], p[1], 0), V(q[0], q[1], 0)))
        plate = body.newObject("PartDesign::SheetMetalBaseWall", "JoinPlate")
        plate.Profile = plateSketch
        doc.recompute()
        pump()

        def along_y(shape, x, z):
            return [
                "Edge%d" % (i + 1)
                for i, e in enumerate(shape.Edges)
                if all(abs(v.Point.x - x) < 1e-7 and abs(v.Point.z - z) < 1e-7 for v in e.Vertexes)
            ]

        ends = along_y(plate.Shape, legX, legTop)[:1] + along_y(plate.Shape, x0, z0)[:1]
        if not check("two edges to join, the leg's top end and the plate's", len(ends) == 2, ends):
            raise RuntimeError("no edges to join")
        FreeCADGui.Selection.clearSelection()
        for sub in ends:
            FreeCADGui.Selection.addSelection(doc.Name, plate.Name, sub)
        FreeCADGui.runCommand("PartDesign_SheetMetalJoin")
        pump()
        join = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalJoin"][0]
        w = panel() or {}
        edges = w.get("listEdges")
        check(
            "Join by Bend opens its panel with its relief, its two edges listed",
            all(n in w for n in ("ReliefShape", "ReliefWidth", "ReliefDepth"))
            and edges is not None
            and edges.count() == 2,
            (sorted(n for n in w if n[0].isupper()), edges and edges.count()),
        )
        accept()
        pump(20)
        check(
            "OK keeps a good join, one solid",
            "Invalid" not in join.State and len(join.Shape.Solids) == 1,
            (join.State, join.getStatusString()),
        )

        # -- the bend cuts' panel: the bend listed, what the pattern takes -----
        bendFace = [
            "Face%d" % (i + 1) for i in range(len(join.Shape.Faces))
            if (join.sheetElementOf("Face%d" % (i + 1)) or "").startswith("Bend")
        ][:1]
        FreeCADGui.Selection.clearSelection()
        for sub in bendFace:
            FreeCADGui.Selection.addSelection(doc.Name, join.Name, sub)
        FreeCADGui.runCommand("PartDesign_SheetMetalBendCuts")
        pump()
        cuts = [o for o in doc.Objects if o.TypeId == "PartDesign::SheetMetalBendCuts"][0]
        w = panel() or {}
        bends = w.get("listBends")
        names = ("Pattern", "SlotLength", "BridgeLength", "EndMargin", "SlotWidth", "Rows", "Tile")
        check(
            "Bend Cuts opens its panel with its pattern and sizes, its bend listed",
            all(n in w for n in names) and bends is not None and bends.count() == 1,
            (sorted(n for n in w if n[0].isupper()), bends and bends.count()),
        )
        check(
            "Slots: the rows and the tile greyed",
            "Rows" in w and not w["Rows"].isEnabled() and "Tile" in w
            and not w["Tile"].isEnabled() and w["SlotLength"].isEnabled(),
        )
        pattern = w.get("Pattern")
        if pattern is not None:
            pattern.setCurrentIndex(1)
            pump(20)
            doc.recompute()
            pump(20)
            check(
                "Staggered set there is the feature's, its rows then not greyed",
                cuts.Pattern == "Staggered" and w["Rows"].isEnabled(),
                (cuts.Pattern, w["Rows"].isEnabled()),
            )
            w["Rows"].setValue(2)
            pump(20)
            doc.recompute()
            check("the rows set there are the feature's", cuts.Rows == 2, cuts.Rows)
        accept()
        pump(20)
        check(
            "OK keeps good bend cuts, one solid",
            "Invalid" not in cuts.State and len(cuts.Shape.Solids) == 1,
            (cuts.State, cuts.getStatusString()),
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

        # A flange's panel says what it did of its own accord: on an L's leg,
        # up to the L's bend, kept off that bend by the relief's width
        # (ruled 2026-11-04)
        lbody = doc.addObject("PartDesign::Body", "LBody")
        lsketch = lbody.newObject("Sketcher::SketchObject", "LSketch")
        for a, b in (((0, 0), (20, 0)), ((20, 0), (20, 10))):
            lsketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        lwall = lbody.newObject("PartDesign::SheetMetalBaseWall", "L")
        lwall.Profile = lsketch
        lwall.Length = 15
        doc.recompute()
        top = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(lwall.Shape.Edges)
            if len(e.Vertexes) == 2
            and all(abs(v.Point.z - 15) < 1e-7 and abs(v.Point.x - 20) < 1e-7 for v in e.Vertexes)
            and abs(e.Vertexes[0].Point.y - e.Vertexes[1].Point.y) > 1
        ]
        lflange = lbody.newObject("PartDesign::SheetMetalFlange", "LFlange")
        lflange.Edges = (lwall, top[:1])
        lflange.Length = 8
        doc.recompute()
        FreeCADGui.ActiveDocument.setEdit(lflange.Name)
        pump(30)
        w = panel() or {}
        said = w.get("labelNotice")
        check(
            "a flange kept off its wall's own bend says so in its panel",
            said is not None and "Bend1" in said.text() and "kept 1 mm" in said.text(),
            said and said.text(),
        )
        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(20)
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
