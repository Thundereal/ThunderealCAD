"""Sheet metal's commands and tool bar in PartDesign (docs/SheetMetal.md sec 8.1).

  - the Part Design menu has a Sheet Metal submenu holding Base Wall, always
    (ruled 2026-10-31);
  - the Sheet Metal tool bar is there but not offered at first;
  - Base Wall on a selected closed sketch makes a plate in the active body,
    the sketch hidden, and with it the rule; its panel opens, and OK closes it;
  - Flange on a selected edge of the wall's skin makes a flange there;
  - Extend Wall on a selected end of a wall carries it out;
  - Flat Pattern makes the flat of the body's tip, not the body's tip itself;
  - the solid body holding the sheet has the sheet metal overlay on its icon;
  - a double-click on it makes it active, which offers the tool bar, and a
    second lets it go and takes the tool bar away;
  - a body with no sheet metal: its solid body, if any, offers nothing.

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
DOC = "SheetMetalCommands"
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


def hidden():
    tb = toolbar()
    return tb is not None and not tb.isVisible() and not tb.toggleViewAction().isVisible()


def image(icon):
    return icon.pixmap(32, 32).toImage()


def newest(doc, before, typeId):
    made = [o for o in doc.Objects if o.Name not in before and o.TypeId == typeId]
    return made[0] if made else None


def ok():
    """The open panel's OK, as the user presses it"""
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(QtWidgets.QDialogButtonBox.Ok)
        if b is not None and b.isVisible():
            b.click()
            pump(60)
            return True
    return False


def menuActions(title):
    """The actions of the Part Design menu's submenu of that title"""
    bar = FreeCADGui.getMainWindow().menuBar()
    for action in bar.actions():
        menu = action.menu()
        if not menu or action.text().replace("&", "") != "Part Design":
            continue
        for sub in menu.actions():
            if sub.menu() and sub.text().replace("&", "") == title:
                return sub.menu().actions()
    return None


def run():
    try:
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        pump()
        actions = menuActions("Sheet Metal")
        base = FreeCADGui.Command.get("PartDesign_SheetMetalBaseWall")
        check(
            "the Part Design menu has a Sheet Metal submenu holding Base Wall",
            actions is not None and base is not None and base.getAction()[0] in actions,
            [a.text() for a in actions or []],
        )
        check("the Sheet Metal tool bar is there and not offered", hidden())

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

        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, sketch.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalBaseWall")
        pump()
        check("Base Wall opens its panel, OK closes it", ok())
        doc.recompute()
        wall = newest(doc, before, "PartDesign::SheetMetalBaseWall")
        check(
            "Base Wall on a closed sketch makes a plate",
            wall is not None and wall.Profile[0] == sketch and abs(wall.Shape.Volume - 800) < 1e-6,
            wall and wall.Shape.Volume,
        )
        check("in the active body, with its rule", wall in body.Group and wall.Rule in body.Group)
        check("the sketch hidden", not sketch.Visibility)

        edge = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(wall.Shape.Edges)
            if all(abs(v.Point.y - 20) < 1e-7 and abs(v.Point.z - 1) < 1e-7 for v in e.Vertexes)
        ]
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, wall.Name, edge[0])
        FreeCADGui.runCommand("PartDesign_SheetMetalFlange")
        pump()
        check("Flange opens its panel, OK closes it", ok())
        doc.recompute()
        flange = newest(doc, before, "PartDesign::SheetMetalFlange")
        check(
            "Flange on a selected edge makes a flange there",
            flange is not None and "Invalid" not in flange.State and flange.Edges[1] == edge,
            flange and (flange.State, flange.Edges),
        )
        check("and it is the body's tip", body.Tip == flange)

        near = [
            "Face%d" % (i + 1)
            for i, f in enumerate(flange.Shape.Faces)
            if abs(f.BoundBox.YMin) < 1e-7 and abs(f.BoundBox.YMax) < 1e-7
        ]
        volume = flange.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, flange.Name, near[0])
        FreeCADGui.runCommand("PartDesign_SheetMetalExtendWall")
        pump()
        check("Extend Wall opens its panel, OK closes it", ok())
        doc.recompute()
        ext = newest(doc, before, "PartDesign::SheetMetalExtendWall")
        check(
            "Extend Wall on a selected end carries it out",
            ext is not None
            and "Invalid" not in ext.State
            and abs(ext.Shape.Volume - volume - 40 * ext.Length.Value) < 1e-6,
            ext and (ext.State, ext.Shape.Volume, volume),
        )
        flange = ext

        # a hem on the near end's top edge, where the extend wall left it
        ymin = flange.Shape.BoundBox.YMin
        edge = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(flange.Shape.Edges)
            if all(abs(v.Point.y - ymin) < 1e-7 and abs(v.Point.z - 1) < 1e-7 for v in e.Vertexes)
        ]
        volume = flange.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, flange.Name, edge[0])
        FreeCADGui.runCommand("PartDesign_SheetMetalHem")
        pump()
        check("Hem opens its panel, OK closes it", ok())
        doc.recompute()
        hem = newest(doc, before, "PartDesign::SheetMetalHem")
        check(
            "Hem on a selected edge folds the end back over the skin",
            hem is not None
            and "Invalid" not in hem.State
            and hem.Edges[1] == edge
            and hem.Shape.isInside(
                FreeCAD.Vector(hem.Shape.BoundBox.Center.x, ymin + 3, 1.6), 1e-9, True
            )
            and hem.Shape.Volume > volume,
            hem and (hem.State, hem.Edges, hem.Shape.Volume, volume),
        )
        flange = hem

        # a cutout: a circle drawn on the plate's top, through it
        hole = body.newObject("Sketcher::SketchObject", "Hole")
        hole.MapMode = "Deactivated"
        hole.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        hole.addGeometry(Part.Circle(V(20, 10, 0), V(0, 0, 1), 3))
        doc.recompute()
        pump()
        volume = flange.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, hole.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalCutout")
        pump()
        check("Cutout opens its panel, OK closes it", ok())
        doc.recompute()
        cutout = newest(doc, before, "PartDesign::SheetMetalCutout")
        check(
            "Cutout on a selected sketch cuts it through the sheet",
            cutout is not None
            and "Invalid" not in cutout.State
            and cutout.Profile[0] == hole
            and abs(cutout.Shape.Volume - (volume - 9 * math.pi)) < 1e-6,
            cutout and (cutout.State, cutout.Shape.Volume, volume),
        )
        check("and it is the body's tip, the sketch hidden", body.Tip == cutout
              and not hole.Visibility)
        flange = cutout

        # a fold: a line across the plate at y 4, the smaller side, with the
        # hem on it, turned up
        line = body.newObject("Sketcher::SketchObject", "FoldLine")
        line.MapMode = "Deactivated"
        line.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        line.addGeometry(Part.LineSegment(V(-5, 4, 0), V(45, 4, 0)))
        doc.recompute()
        pump()
        volume = flange.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, line.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalFold")
        pump()
        check("Fold opens its panel, OK closes it", ok())
        doc.recompute()
        fold = newest(doc, before, "PartDesign::SheetMetalFold")
        check(
            "Fold on a selected line bends the plate there, its volume kept",
            fold is not None
            and "Invalid" not in fold.State
            and fold.Profile[0] == line
            and abs(fold.Shape.Volume - volume) < 1e-6
            and fold.Shape.BoundBox.ZMax > flange.Shape.BoundBox.ZMax - 1e-6,
            fold and (fold.State, fold.Shape.Volume, volume),
        )
        flange = fold

        # a jog: a line across the plate at y 15.5, the smaller side stepped
        # up, its end kept, so the jog adds material
        line = body.newObject("Sketcher::SketchObject", "JogLine")
        line.MapMode = "Deactivated"
        line.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        line.addGeometry(Part.LineSegment(V(-5, 15.5, 0), V(45, 15.5, 0)))
        doc.recompute()
        pump()
        volume = flange.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, line.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalJog")
        pump()
        check("Jog opens its panel, OK closes it", ok())
        doc.recompute()
        jog = newest(doc, before, "PartDesign::SheetMetalJog")
        check(
            "Jog on a selected line steps the plate there, adding what it takes",
            jog is not None
            and "Invalid" not in jog.State
            and jog.Profile[0] == line
            and len(jog.Shape.Solids) == 1
            and jog.Shape.Volume > volume + 1e-6,
            jog and (jog.State, jog.Shape.Volume, volume),
        )
        flange = jog

        # a tab: an 8 x 6 rectangle on the plate's left edge
        sketch = body.newObject("Sketcher::SketchObject", "TabSketch")
        sketch.MapMode = "Deactivated"
        sketch.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        pts = [(-8, 6), (0, 6), (0, 12), (-8, 12)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        doc.recompute()
        pump()
        volume = flange.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, sketch.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalTab")
        pump()
        check("Tab opens its panel, OK closes it", ok())
        doc.recompute()
        tab = newest(doc, before, "PartDesign::SheetMetalTab")
        check(
            "Tab on a selected sketch adds it to the wall it lies on",
            tab is not None
            and "Invalid" not in tab.State
            and tab.Profile[0] == sketch
            and len(tab.Shape.Solids) == 1
            and abs(tab.Shape.Volume - volume - 48) < 1e-6,
            tab and (tab.State, tab.Shape.Volume, volume),
        )
        flange = tab

        # a contour flange on the tab's end, up 5 from its top edge
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
        pump()
        volume = flange.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, tab.Name, edge[0])
        FreeCADGui.Selection.addSelection(doc.Name, sketch.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalContourFlange")
        pump()
        check("Contour Flange opens its panel, OK closes it", ok())
        doc.recompute()
        contour = newest(doc, before, "PartDesign::SheetMetalContourFlange")
        check(
            "Contour Flange on a selected edge and sketch bends the sketch's lines from there",
            contour is not None
            and "Invalid" not in contour.State
            and contour.Profile[0] == sketch
            and contour.Edges[1] == edge[:1]
            and len(contour.Shape.Solids) == 1
            and contour.Shape.Volume > volume + 1e-6,
            contour and (contour.State, contour.Edges, contour.Shape.Volume, volume),
        )
        check("the sketch hidden", not sketch.Visibility)
        flange = contour

        # a plate of its own, 10 above the contour's leg and 10 beyond it,
        # joined to the leg's top end by a bend
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
        volume = plate.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        if not check("two edges to join, the leg's top end and the plate's", len(ends) == 2, ends):
            raise RuntimeError("no edges to join")
        for sub in ends:
            FreeCADGui.Selection.addSelection(doc.Name, plate.Name, sub)
        FreeCADGui.runCommand("PartDesign_SheetMetalJoin")
        pump()
        check("Join by Bend opens its panel, OK closes it", ok())
        doc.recompute()
        join = newest(doc, before, "PartDesign::SheetMetalJoin")
        check(
            "Join by Bend on two selected edges joins their walls by a bend, one solid",
            join is not None
            and "Invalid" not in join.State
            and join.Edges[1] == ends
            and len(join.Shape.Solids) == 1
            and join.Shape.Volume > volume + 1e-6,
            join and (join.State, join.getStatusString(), ends, join.Shape.Volume, volume),
        )
        flange = join

        # slots across the join's bend
        bendFace = [
            "Face%d" % (i + 1) for i in range(len(join.Shape.Faces))
            if (join.sheetElementOf("Face%d" % (i + 1)) or "").startswith("Bend")
        ][:1]
        volume = join.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        for sub in bendFace:
            FreeCADGui.Selection.addSelection(doc.Name, join.Name, sub)
        FreeCADGui.runCommand("PartDesign_SheetMetalBendCuts")
        pump()
        check("Bend Cuts opens its panel, OK closes it", ok())
        doc.recompute()
        cuts = newest(doc, before, "PartDesign::SheetMetalBendCuts")
        check(
            "Bend Cuts on a selected bend face cuts slots across that bend, one solid",
            cuts is not None
            and "Invalid" not in cuts.State
            and cuts.Bends[1] == bendFace
            and len(cuts.Shape.Solids) == 1
            and cuts.Shape.Volume < volume - 1e-6,
            cuts and (cuts.State, cuts.getStatusString(), bendFace, cuts.Shape.Volume, volume),
        )
        flange = cuts

        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.runCommand("PartDesign_SheetMetalFlatPattern")
        pump()
        doc.recompute()
        flat = newest(doc, before, "PartDesign::SheetMetalFlatPattern")
        check(
            "Flat Pattern makes the flat of the body's tip",
            flat is not None and flat.Source == flange and "Invalid" not in flat.State,
            flat and (flat.Source, flat.State),
        )
        check("and does not become the tip", body.Tip == flange)

        # Flatten: the active body's sheet drawn laid flat while it is on
        # (docs/SheetMetal.md sec 12)
        solid = body.getSolidBody(flange)
        flat.ViewObject.Visibility = False
        cmd = FreeCADGui.Command.get("PartDesign_SheetMetalFlatten")
        view.setActiveObject("pdbody", body)
        pump()
        check("Flatten is offered on the active body's sheet", cmd.isActive())
        folded = flange.ViewObject.getBoundingBox()
        FreeCADGui.runCommand("PartDesign_SheetMetalFlatten", 1)
        pump()
        thick = solid.Thickness.Value
        box = flange.ViewObject.getBoundingBox()
        check(
            "Flatten turns the solid's Flatten on, its values filled",
            solid.Flatten and solid.FlatArea.Value > 0 and solid.FlatStatus == "",
            (solid.Flatten, solid.FlatArea, solid.FlatStatus),
        )
        check(
            "the tip drawn laid flat, one thickness high",
            thick > 0 and abs(box.ZLength - thick) < 1e-3 and folded.ZLength > 2 * thick,
            (box, folded, thick),
        )
        cmd.isActive()
        check("its button checked", cmd.getAction()[0].isChecked())
        laid = flange.getSubObject("Flat")
        top = max(laid.Faces, key=lambda f: f.Area)
        view.fitAll()
        pump()
        hit = None
        u0, u1, v0, v1 = top.ParameterRange
        for fu in (0.5, 0.3, 0.7, 0.2, 0.8):
            for fv in (0.5, 0.3, 0.7, 0.2, 0.8):
                p = top.valueAt(u0 + (u1 - u0) * fu, v0 + (v1 - v0) * fv)
                if hit is None and top.isInside(p, 1e-6, True):
                    x, y = view.getPointOnScreen(p)
                    hit = view.getObjectInfo((int(x), int(y)))
        check(
            "picked, the flat names its element FlatFace",
            hit is not None and "FlatFace" in str(hit.get("Component", "")),
            hit,
        )
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, flange.Name, "FlatFace1")
        sel = FreeCADGui.Selection.getSelectionEx("", 0)
        check(
            "FlatFace1 selected is the flat's face",
            len(sel) == 1
            and len(sel[0].SubObjects) == 1
            and sel[0].SubObjects[0].isPartner(laid.Faces[0]),
            sel and [(s.ObjectName, s.SubElementNames) for s in sel],
        )
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.runCommand("PartDesign_SheetMetalFlatten", 0)
        pump()
        # the button follows the solid where the command is asked, as the
        # tool bar's update does
        cmd.isActive()
        box = flange.ViewObject.getBoundingBox()
        check(
            "again, folded back: Flatten off, the tip drawn as it is, unchecked",
            not solid.Flatten
            and abs(box.ZLength - folded.ZLength) < 1e-3
            and not cmd.getAction()[0].isChecked(),
            (solid.Flatten, box, folded),
        )

        solid = body.getSolidBody(flange)
        check("the sheet's solid", solid is not None)
        icon = image(solid.ViewObject.Icon)
        check(
            "has the sheet metal overlay on its icon",
            icon != image(FreeCADGui.getIcon("PartDesign_SolidBody.svg")),
        )
        solid.ViewObject.doubleClicked()
        pump(20)
        check(
            "a double-click makes it active and offers the tool bar",
            view.getActiveObject("pdsheetmetal") == solid and offered(),
        )
        solid.ViewObject.doubleClicked()
        pump(20)
        check(
            "a second lets it go and takes the tool bar away",
            view.getActiveObject("pdsheetmetal") is None and hidden(),
        )

        plain = doc.addObject("PartDesign::Body", "Plain")
        a = plain.newObject("PartDesign::AdditiveBox", "Box")
        b = plain.newObject("PartDesign::AdditiveBox", "Box001")
        b.NewSolid = True
        b.Placement.Base = V(50, 0, 0)
        doc.recompute()
        pump()
        other = plain.getSolidBody(a)
        if check("a body of two plain solids has solid bodies", other is not None):
            check(
                "whose icon has no overlay",
                image(other.ViewObject.Icon) == image(FreeCADGui.getIcon("PartDesign_SolidBody.svg")),
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
