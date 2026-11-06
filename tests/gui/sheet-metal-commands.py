"""Sheet metal's commands and tool bar in PartDesign (docs/SheetMetal.md sec 8.1).

  - the Part Design menu has a Sheet Metal submenu holding Base Wall, always
    (ruled 2026-10-31);
  - the Sheet Metal tool bar is there but not offered at first;
  - Base Wall on a selected closed sketch makes a plate in the active body,
    the sketch hidden, and with it the rule; its panel opens, and OK closes it;
  - Flange on a selected edge of the wall's skin makes a flange there;
  - Extend Wall on a selected end of a wall carries it out;
  - the sheet has one flat object, claimed by its solid; Flatten shows it in
    place of the folded part, laid flat by its recompute, picked and
    selected by its own element names, and Export Flat is offered with
    nothing selected; shown with the folded part, it is moved off it by its
    placement; Check Sheet opens a panel of its bends and problems, a row
    picked selecting its faces as they are drawn;
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
import numpy as np
import Part
from pivy import coin
from PySide import QtCore, QtGui, QtWidgets

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
        starter = FreeCADGui.Command.get("PartDesign_SheetMetalStarter")
        check(
            "and Starter Shape beside it",
            starter is not None and starter.getAction()[0] in (actions or []),
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

        # Flatten: the sheet's flat object shown in place of the folded part
        # (docs/SheetMetal.md sec 12, ruled 2026-11-04)
        solid = body.getSolidBody(flange)
        flats = [
            o for o in solid.InList
            if o.isDerivedFrom("PartDesign::SheetMetalFlatPattern") and o.Sheet == solid
        ]
        flat = flats[0] if flats else None
        check(
            "the sheet has one flat object, hidden, claimed by its solid after its features",
            len(flats) == 1
            and not flat.Visibility
            and solid.ViewObject.claimChildren()[-1] == flat
            and flat not in body.ViewObject.claimChildren(),
            (flats, flat and solid.ViewObject.claimChildren()[-3:]),
        )
        cmd = FreeCADGui.Command.get("PartDesign_SheetMetalFlatten")
        view.setActiveObject("pdbody", body)
        pump()
        check("Flatten is offered on the active body's sheet", cmd.isActive())
        FreeCADGui.Selection.clearSelection()
        check(
            "and Export Flat, nothing selected",
            FreeCADGui.Command.get("PartDesign_SheetMetalExportDXF").isActive(),
        )
        # Export Every Flat: a file per sheet of the document into a folder
        every = FreeCADGui.Command.get("PartDesign_SheetMetalExportFlats")
        check(
            "and Export Every Flat, the document holding a sheet",
            every is not None and every.isActive(),
        )
        folder = os.path.join(OUT, "every")
        os.makedirs(folder, exist_ok=True)
        asked = []

        def fill():
            # the command's dialog: the folder, SVG, OK
            for w in QtWidgets.QApplication.topLevelWidgets():
                if w.objectName() == "SheetMetalExportFlats" and w.isVisible():
                    chooser = w.findChild(QtWidgets.QWidget, "folder")
                    combo = w.findChild(QtWidgets.QComboBox, "format")
                    asked.append([combo.itemText(i) for i in range(combo.count())])
                    chooser.setProperty("fileName", folder)
                    combo.setCurrentIndex(1)
                    w.accept()
                    return
            QtCore.QTimer.singleShot(50, fill)

        if every is not None:
            QtCore.QTimer.singleShot(0, fill)
            FreeCADGui.runCommand("PartDesign_SheetMetalExportFlats")
            pump()
        written = sorted(os.listdir(folder))
        check(
            "which asks a folder and DXF or SVG, and writes each sheet's flat there",
            asked == [["DXF", "SVG"]] and written == [body.Label + ".svg"],
            (asked, written),
        )
        folded = flange.ViewObject.getBoundingBox()
        FreeCADGui.runCommand("PartDesign_SheetMetalFlatten", 1)
        pump(20)
        thick = flange.Rule.Thickness.Value
        box = flat.ViewObject.getBoundingBox()
        check(
            "Flatten shows the flat object, laid flat by its own recompute, the folded part hidden",
            flat.Visibility
            and flat.Status == ""
            and "Touched" not in flat.State
            and not flange.Visibility,
            (flat.Visibility, flat.Status, flat.State, flange.Visibility),
        )
        check(
            "the flat drawn, one thickness high",
            thick > 0 and abs(box.ZLength - thick) < 1e-3 and folded.ZLength > 2 * thick,
            (box, folded, thick),
        )
        cmd.isActive()
        bodyBox = body.ViewObject.getBoundingBox()
        check(
            "the body's bounds the flat's, not the folded part's (a view fitted to it)",
            abs(bodyBox.ZLength - thick) < 1e-3 and abs(bodyBox.XLength - box.XLength) < 1e-3,
            (bodyBox, box),
        )
        check("its button checked", cmd.getAction()[0].isChecked())
        top = max(flat.Shape.Faces, key=lambda f: f.Area)
        view.fitAll()
        pump()

        def pickOn(obj, faces, move=FreeCAD.Vector()):
            # the first pick on one of the faces naming a face of obj
            last = None
            for face in faces:
                u0, u1, v0, v1 = face.ParameterRange
                for fu in (0.5, 0.3, 0.7, 0.2, 0.8):
                    for fv in (0.5, 0.3, 0.7, 0.2, 0.8):
                        p = face.valueAt(u0 + (u1 - u0) * fu, v0 + (v1 - v0) * fv)
                        if not face.isInside(p, 1e-6, True):
                            continue
                        x, y = view.getPointOnScreen(p + move)
                        info = view.getObjectInfo((int(x), int(y)))
                        if info:
                            last = info
                            if info.get("Object") == obj.Name and str(
                                info.get("Component", "")
                            ).startswith("Face"):
                                return info
            return last

        hit = pickOn(flat, [top])
        check(
            "picked, the flat names its own Face",
            hit is not None
            and hit.get("Object") == flat.Name
            and str(hit.get("Component", "")).startswith("Face"),
            hit,
        )
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, flat.Name, "Face1")
        sel = FreeCADGui.Selection.getSelectionEx("", 0)
        check(
            "Face1 of the flat selected is its face, the tip's FlatFace1",
            len(sel) == 1
            and len(sel[0].SubObjects) == 1
            and sel[0].SubObjects[0].isPartner(flat.Shape.Faces[0])
            and sel[0].SubObjects[0].isPartner(flange.getSubObject("FlatFace1")),
            sel and [(s.ObjectName, s.SubElementNames) for s in sel],
        )
        FreeCADGui.Selection.clearSelection()

        # The markings, each kind drawn in its own style (sec 12): the bend
        # lines dashed, up and down in two colours, as the SVG has them
        fvo = flat.ViewObject
        check(
            "the flat's view styles each kind of marking, the bend lines dashed",
            fvo.BendUpStyle == "Dashed"
            and fvo.BendDownStyle == "Dashed"
            and fvo.TangentLineStyle == "Dotted"
            and fvo.GrainStyle == "Solid"
            and fvo.getEnumerationsOfProperty("BendUpStyle")
            == ["Solid", "Dashed", "Dotted", "Dashdot"]
            and all(fvo.getGroupOfProperty(n) == "Markings" for n in ("ShowBendUp", "GrainWidth"))
            and fvo.BendUpColor[:3] != fvo.BendDownColor[:3],
            [(n, getattr(fvo, n)) for n in ("BendUpStyle", "BendUpColor", "BendDownColor")],
        )
        lineEdges = [
            (i + 1, e)
            for i, e in enumerate(flat.Shape.Edges)
            if flat.Shape.getElementMappedName("Edge%d" % (i + 1)).startswith("Bend")
            and "Tangent" not in flat.Shape.getElementMappedName("Edge%d" % (i + 1))
        ]
        line = max(lineEdges, key=lambda t: t[1].Length) if lineEdges else None

        def pickLine(index, edge):
            # a point along the line, off its ends, picked as a click picks:
            # an edge in a face's plane takes the pick from the face (a bare
            # ray pick, getObjectInfo(), has the nearer of the two)
            last = None
            for f in (0.5, 0.4, 0.6, 0.3, 0.7):
                p = edge.valueAt(edge.FirstParameter + (edge.LastParameter - edge.FirstParameter) * f)
                x, y = view.getPointOnScreen(p)
                res = view.getViewer().getPickedList(
                    pos=(float(x), float(y)), singlePick=True, mapCoords=False, resolve=0
                )
                for obj, sub in res:
                    last = sub
                    if sub.startswith(flat.Name + ".") and sub.split(".")[-1] == "Edge%d" % index:
                        return sub
            return last

        hit = pickLine(*line) if line else None
        check(
            "a bend line picked is the flat's own edge, named after its bend",
            line is not None and hit is not None and hit.split(".")[-1] == "Edge%d" % line[0],
            (line and (line[0], flat.Shape.getElementMappedName("Edge%d" % line[0])), hit),
        )
        FreeCADGui.Selection.clearSelection()
        if line:
            FreeCADGui.Selection.addSelection(doc.Name, flat.Name, "Edge%d" % line[0])
        sel = FreeCADGui.Selection.getSelectionEx("", 0)
        check(
            "and selected by that name, the line",
            line is not None
            and len(sel) == 1
            and len(sel[0].SubObjects) == 1
            and sel[0].SubObjects[0].isPartner(line[1]),
            sel and [(x.ObjectName, x.SubElementNames) for x in sel],
        )
        FreeCADGui.Selection.clearSelection()
        fvo.ShowBendUp = False
        fvo.ShowBendDown = False
        pump()
        hit = pickLine(*line) if line else None
        check(
            "the bend lines hidden, no longer picked",
            line is not None and hit is not None and hit.split(".")[-1] != "Edge%d" % line[0],
            hit,
        )
        fvo.ShowBendUp = True
        fvo.ShowBendDown = True
        pump()

        # Laid by its second skin the bends go down: a down bend's line lies
        # on the blank's top, as every marking does, where one looking down
        # sees it
        camera = view.getCamera()
        flat.SecondSkin = True
        doc.recompute()
        pump(20)
        view.viewTop()
        view.fitAll()
        pump()
        def bendOf(i):
            # Bend1, or Bend1Part2: the bend a marking is of, or None
            name = flat.Shape.getElementMappedName("Edge%d" % i).split(";")[0]
            if not name.startswith("Bend") or "Tangent" in name:
                return None
            return name.split("Part")[0]

        downs = [
            (i + 1, e)
            for i, e in enumerate(flat.Shape.Edges)
            if flat.BendDirections.get(bendOf(i + 1)) == "Down"
        ]
        down = max(downs, key=lambda t: t[1].Length) if downs else None
        hit = pickLine(*down) if down else None
        check(
            "a down bend's line, on the blank's top, is drawn and picked from above",
            down is not None
            and abs(down[1].BoundBox.ZMin - flat.Shape.Solids[0].BoundBox.ZMax) < 1e-6
            and hit is not None
            and hit.split(".")[-1] == "Edge%d" % down[0],
            (down and (down[0], down[1].BoundBox), flat.BendDirections, hit),
        )
        flat.SecondSkin = False
        doc.recompute()
        view.setCamera(camera)
        pump(20)

        # Both: the folded part shown too, the flat moved off it by its own
        # placement
        flatBox = box
        shift = folded.XMax - flatBox.XMin + 10
        flat.Placement.Base = FreeCAD.Vector(shift, 0, 0)
        doc.recompute()
        view.fitAll()
        pump(20)
        # (at the scale the line was picked at before, the flat alone shown)
        moving = max(lineEdges, key=lambda t: t[1].Length) if lineEdges else None
        hitLine = (
            pickLine(moving[0], moving[1].translated(FreeCAD.Vector(shift, 0, 0)))
            if moving
            else None
        )
        check(
            "its markings moved with it: a bend line picked where it now lies",
            moving is not None
            and hitLine is not None
            and hitLine.split(".")[-1] == "Edge%d" % moving[0],
            hitLine,
        )
        flange.Visibility = True
        pump(20)
        moved = FreeCAD.BoundBox(flatBox)
        moved.move(FreeCAD.Vector(shift, 0, 0))
        box = flat.ViewObject.getBoundingBox()
        both = FreeCAD.BoundBox(folded)
        both.add(moved)
        bodyBox = body.ViewObject.getBoundingBox()
        check(
            "the flat moved by its placement, and the body's bounds both",
            all(
                abs(getattr(box, k) - getattr(moved, k)) < 1e-3
                and abs(getattr(bodyBox, k) - getattr(both, k)) < 1e-3
                for k in ("XMin", "XMax", "YMin", "YMax", "ZMin", "ZMax")
            ),
            (box, moved, bodyBox, both),
        )
        # the tip alone of the folded part shown while picked
        others = [
            o for o in body.Group
            if o not in (flange, flat) and o.ViewObject and o.ViewObject.Visibility
        ]
        for o in others:
            o.ViewObject.Visibility = False
        view.fitAll()
        pump()
        hit = pickOn(flat, [top], FreeCAD.Vector(shift, 0, 0))
        check(
            "the moved flat picked names its Face",
            hit is not None
            and hit.get("Object") == flat.Name
            and str(hit.get("Component", "")).startswith("Face"),
            hit,
        )
        hit = pickOn(flange, sorted(flange.Shape.Faces, key=lambda f: -f.Area))
        check(
            "and the folded part picked names its Face",
            hit is not None
            and hit.get("Object") == flange.Name
            and str(hit.get("Component", "")).startswith("Face"),
            hit,
        )
        # (the tip last: a feature shown may hide the others of its solid)
        for o in others:
            o.ViewObject.Visibility = True
        flange.ViewObject.Visibility = True
        flat.Placement.Base = FreeCAD.Vector()
        doc.recompute()
        flange.Visibility = False
        pump()

        # Check Sheet: the bend table and the problems found, on demand
        # (docs/SheetMetal.md sec 7); a least flange no wall reaches lists
        # each wall past each bend
        import PartDesign

        checkCmd = FreeCADGui.Command.get("PartDesign_SheetMetalCheck")
        check(
            "Check Sheet is on the tool bar, offered with nothing selected",
            checkCmd is not None
            and toolbar() is not None
            and checkCmd.getAction()[0] in toolbar().actions()
            and checkCmd.isActive(),
        )
        rule = flange.Rule
        rule.MinFlange = 1000
        doc.recompute()
        pump()
        found = PartDesign.checkSheet(flange)
        FreeCADGui.runCommand("PartDesign_SheetMetalCheck")
        pump()
        mw = FreeCADGui.getMainWindow()
        table = mw.findChild(QtWidgets.QTableWidget, "tableBends")
        listed = mw.findChild(QtWidgets.QListWidget, "listProblems")
        check(
            "its panel lists each bend and each problem checkSheet() finds",
            table is not None
            and listed is not None
            and table.rowCount() == len(found["Bends"]) > 0
            and listed.count() == len(found["Problems"]) > 0
            and listed.item(0).text() == found["Problems"][0]["Message"],
            table and (table.rowCount(), listed.count(), len(found["Bends"]), len(found["Problems"])),
        )
        check(
            "and Check Sheet is not offered while it is open",
            not checkCmd.isActive(),
        )
        first = found["Problems"][0]
        FreeCADGui.Selection.clearSelection()
        listed.setCurrentRow(0)
        pump()
        sel = FreeCADGui.Selection.getSelectionEx("", 0)
        # a path through the body to the flat: Flat.<its mapped name>.Face1
        picked = sorted(n.split(".")[-1] for s in sel for n in s.SubElementNames)
        onFlat = all(n.startswith(flat.Name + ".") for s in sel for n in s.SubElementNames)
        check(
            "a problem picked selects its faces on the flat object, as they are named there",
            onFlat
            and picked == sorted(n[len("Flat"):] for n in first["FlatElements"])
            and picked,
            ([n for s in sel for n in s.SubElementNames][:2], picked, first["FlatElements"]),
        )
        FreeCADGui.runCommand("PartDesign_SheetMetalFlatten", 0)
        pump(20)
        listed.setCurrentRow(-1)
        listed.setCurrentRow(0)
        pump()
        sel = FreeCADGui.Selection.getSelectionEx("", 0)
        names = sorted(n.split(".")[-1] for s in sel for n in s.SubElementNames)
        check(
            "folded back, its faces on the folded part",
            names == sorted(first["Elements"]) and names,
            (names, first["Elements"]),
        )
        table.selectRow(0)
        pump()
        sel = FreeCADGui.Selection.getSelectionEx("", 0)
        names = sorted(n.split(".")[-1] for s in sel for n in s.SubElementNames)
        check(
            "a bend picked selects its faces",
            names == sorted(found["Bends"][0]["Elements"]) and names,
            (names, found["Bends"][0]["Elements"]),
        )
        rule.MinFlange = 0
        doc.recompute()
        mw.findChild(QtWidgets.QPushButton, "buttonCheckAgain").click()
        pump()
        check(
            "Check Again: the least flange put back, those problems gone",
            listed.count() == len(PartDesign.checkSheet(flange)["Problems"]),
            listed.count(),
        )

        # a bend's own K, typed in the table (ruled 2026-11-05: kept on the
        # feature that made the bend), an undoable step each
        def typeInto(column, text):
            table.setCurrentCell(0, column)
            table.editItem(table.item(0, column))
            pump(5)
            edit = table.findChild(QtWidgets.QLineEdit, "editBendValue")
            if edit is None:
                return False
            edit.setText(text)
            # Return commits and closes the editor, which is then gone
            QtWidgets.QApplication.sendEvent(
                edit, QtGui.QKeyEvent(QtCore.QEvent.KeyPress, QtCore.Qt.Key_Return, QtCore.Qt.NoModifier)
            )
            pump(10)
            return True

        bend0 = found["Bends"][0]
        maker = doc.getObject(bend0["Feature"])
        undos = doc.UndoCount
        typed = typeInto(4, "0.3")
        check(
            "a K typed in the bend table is the bend's own, kept on its feature",
            typed
            and maker.bendKFactor(bend0["Name"]) == 0.3
            and table.item(0, 4).text().endswith("(bend)")
            and doc.UndoCount == undos + 1
            and doc.UndoNames[0] == "Bend K factor",
            (typed, maker.bendKFactor(bend0["Name"]), table.item(0, 4).text(), doc.UndoNames[:1]),
        )
        T = maker.Rule.Thickness.Value
        allowance = math.radians(bend0["Angle"]) * (bend0["Radius"] + 0.25 * T)
        typed = typeInto(5, "%.9f" % allowance)
        k = maker.bendKFactor(bend0["Name"])
        check(
            "an allowance typed is the K that makes it",
            typed and k is not None and abs(k - 0.25) < 1e-6,
            (typed, k),
        )
        typed = typeInto(4, "")
        check(
            "cleared, the bend takes the rule's again",
            typed
            and maker.bendKFactor(bend0["Name"]) is None
            and not table.item(0, 4).text().endswith("(bend)"),
            (typed, maker.bendKFactor(bend0["Name"]), table.item(0, 4).text()),
        )
        doc.undo()
        doc.recompute()
        k = maker.bendKFactor(bend0["Name"])
        check("and that is undone as a step", k is not None and abs(k - 0.25) < 1e-6, k)
        maker.setBendKFactor(bend0["Name"], None)
        doc.recompute()

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Control.closeDialog()
        pump()
        # the button follows the flat where the command is asked, as the
        # tool bar's update does
        cmd.isActive()
        box = flange.ViewObject.getBoundingBox()
        # hidden, the flat draws nothing: not its blank, nor its lines
        for o in body.Group:
            if o != flat and o.ViewObject and o.ViewObject.Visibility:
                o.ViewObject.Visibility = False
        view.fitAll()
        pump(10)
        stray = pickLine(*line) if line else None
        for o in body.Group:
            if o == flange:
                o.ViewObject.Visibility = True
        pump(10)
        check(
            "hidden, the flat's lines are not drawn: nothing picked where one lay",
            line is not None and not (stray and stray.startswith(flat.Name + ".")),
            stray,
        )
        check(
            "folded back: the flat hidden, the tip shown as it is, unchecked",
            not flat.Visibility
            and flange.Visibility
            and abs(box.ZLength - folded.ZLength) < 1e-3
            and not cmd.getAction()[0].isChecked(),
            (flat.Visibility, flange.Visibility, box, folded),
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

        # A drawing of the sheet's flat (docs/SheetMetal.md sec 5.6): TechDraw
        # asks the source's view how to draw an edge, by its source name, and
        # the flat's bend lines come out in the flat's own colours
        def pens(page):
            """The colours the page's scene is drawn in, each of 20 pixels
            or more: its edges are items Python cannot ask for their pens"""
            counts = {}
            for v in mw.findChildren(QtWidgets.QGraphicsView):
                scene = v.scene()
                if scene is None or not scene.items():
                    continue
                r = scene.itemsBoundingRect()
                img = QtGui.QImage(int(r.width()) + 1, int(r.height()) + 1, QtGui.QImage.Format_RGB32)
                img.fill(QtGui.QColor(255, 255, 255))
                painter = QtGui.QPainter(img)
                scene.render(painter, QtCore.QRectF(img.rect()), r)
                painter.end()
                for y in range(img.height()):
                    for x in range(img.width()):
                        c = img.pixelColor(x, y)
                        key = (c.red(), c.green(), c.blue())
                        counts[key] = counts.get(key, 0) + 1
            return {c for c, n in counts.items() if n >= 20}

        def rgb(prop):
            return tuple(int(round(x * 255)) for x in prop[:3])

        def drawnIn(page, colour, turns=5):
            """Whether the page draws in colour, given it a few turns to draw:
            a page drawn once only shows its frame"""
            for _ in range(turns):
                found = pens(page)
                if colour in found:
                    return True, found
                pump(60)
            return False, found

        ups = [b for b, d in flat.BendDirections.items() if d == "Up"]
        downs = [b for b, d in flat.BendDirections.items() if d == "Down"]
        page = doc.addObject("TechDraw::DrawPage", "Page")
        template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        page.Template = template
        drawing = doc.addObject("TechDraw::DrawViewPart", "FlatDrawing")
        page.addView(drawing)
        drawing.Source = [flat]
        drawing.Direction = FreeCAD.Vector(0, 0, 1)
        flat.Visibility = True
        doc.recompute()
        page.ViewObject.show()
        pump(60)
        drawing.recompute()
        pump(60)
        seen = pens(page)
        upColour = rgb(flat.ViewObject.BendUpColor)
        downColour = rgb(flat.ViewObject.BendDownColor)
        check(
            "a drawing of the flat draws its up and down bend lines in the flat's colours",
            bool(ups) and bool(downs) and upColour in seen and downColour in seen,
            (ups, downs, upColour, downColour, sorted(seen)[:8]),
        )
        flat.ViewObject.BendDownColor = (0.0, 0.6, 0.0)
        drawing.touch()
        doc.recompute()
        pump(60)
        check(
            "and follows a colour changed on the flat",
            (0, 153, 0) in pens(page),
            sorted(pens(page))[:8],
        )

        # Each edge's info, a bend line's note its label, from Python too;
        # Import Edge Labels makes a balloon of each, to edit
        downBend = downs[0]
        info = getattr(flat.ViewObject, "getEdgeInfo", lambda name: None)(downBend)
        bendRow = [b for b in PartDesign.checkSheet(flat)["Bends"] if b["Name"] == downBend][0]
        bendNote = "DOWN %g\u00b0 R%g" % (round(bendRow["Angle"], 2), round(bendRow["Radius"], 3))
        check(
            "the flat's view tells a drawing a bend line's style and its note",
            info is not None
            and info["Label"] == bendNote
            and abs(info["Color"][0] - flat.ViewObject.BendDownColor[0]) < 1e-6,
            (info, bendNote),
        )
        check(
            "and nothing of the blank's edges",
            info is not None and flat.ViewObject.getEdgeInfo(
                flat.Shape.getElementMappedName("Edge1")) is None,
        )
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, drawing.Name)
        FreeCADGui.runCommand("TechDraw_ImportEdgeLabels")
        pump(30)
        labels = [o for o in doc.Objects if o.TypeId == "TechDraw::DrawEdgeLabel"]
        byEdge = {o.SourceEdge.split(";")[0]: o for o in labels}
        check(
            "Import Edge Labels makes a label of each bend line on the page, its bendNote",
            len(labels) == len(flat.BendDirections)
            and downBend in byEdge
            and byEdge[downBend].Text == bendNote
            and byEdge[downBend].SourceView == drawing
            and byEdge[downBend] in page.Views,
            sorted((k, o.Text) for k, o in byEdge.items()),
        )
        if downBend in byEdge:
            mine = byEdge[downBend]
            mine.Text = "DOWN, hem side"
            moved = (mine.X.Value - mine.OriginX.Value + 5, mine.Y.Value - mine.OriginY.Value)
            mine.X = mine.OriginX.Value + moved[0]
            before = {k: o.Text for k, o in byEdge.items()}
            # laid by the other skin, every bend goes the other way
            flat.SecondSkin = not flat.SecondSkin
            doc.recompute()
            pump(20)
            FreeCADGui.Selection.clearSelection()
            FreeCADGui.Selection.addSelection(doc.Name, drawing.Name)
            FreeCADGui.runCommand("TechDraw_ImportEdgeLabels")
            pump(30)
            rows = {b["Name"]: b for b in PartDesign.checkSheet(flat)["Bends"]}

            def noteOf(name):
                b = rows[name]
                return "%s %g\u00b0 R%g" % (
                    "DOWN" if flat.BendDirections.get(name) == "Down" else "UP",
                    round(b["Angle"], 2),
                    round(b["Radius"], 3),
                )

            stale = [
                (k, o.Text, noteOf(k))
                for k, o in byEdge.items()
                if o is not mine and o.Text != noteOf(k)
            ]
            check(
                "imported again: a text the user wrote kept, the others the new notes, "
                "a label moved by hand kept where it was against its edge",
                mine.Text == "DOWN, hem side"
                and not stale
                and all(o.Text != before[k] for k, o in byEdge.items() if o is not mine)
                and abs(mine.X.Value - mine.OriginX.Value - moved[0]) < 1e-6,
                (mine.Text, stale, mine.X.Value - mine.OriginX.Value, moved),
            )
            flat.SecondSkin = not flat.SecondSkin
            doc.recompute()

        # Flat-only markings: a sketch given to the flat by Flat Markings is
        # drawn on the blank in its own colour, and on the drawing; the folded
        # part knows nothing of it
        markCmd = FreeCADGui.Command.get("PartDesign_SheetMetalFlatMarkings")
        check(
            "Flat Markings is on the tool bar",
            markCmd is not None
            and toolbar() is not None
            and markCmd.getAction()[0] in toolbar().actions(),
        )
        centre = flat.Placement.inverse().multVec(flat.Shape.Solids[0].BoundBox.Center)
        mark = body.newObject("Sketcher::SketchObject", "Mark")
        mark.addGeometry(Part.Circle(V(centre.x, centre.y, 0), V(0, 0, 1), 5))
        doc.recompute()
        folded = body.Tip.Shape.Volume
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, mark.Name)
        check("offered with a sketch selected", markCmd is not None and markCmd.isActive())
        FreeCADGui.runCommand("PartDesign_SheetMetalFlatMarkings")
        doc.recompute()
        pump(20)
        names = [
            flat.Shape.getElementMappedName("Edge%d" % (i + 1)) or ""
            for i in range(len(flat.Shape.Edges))
        ]
        marks = [n for n in names if n.startswith("Marking")]
        check(
            "run, the sketch is the flat's marking, drawn on the blank, hidden itself",
            flat.Markings == [mark]
            and flat.Status == ""
            and len(marks) == 1
            and not mark.Visibility
            and abs(body.Tip.Shape.Volume - folded) < 1e-9,
            (flat.Markings, flat.Status, marks, mark.Visibility),
        )
        markInfo = flat.ViewObject.getEdgeInfo(marks[0]) if marks else None
        check(
            "in the markings' own style, which a drawing takes",
            markInfo is not None
            and markInfo["Style"] == 1
            and abs(markInfo["Color"][0] - flat.ViewObject.MarkingColor[0]) < 1e-6,
            markInfo,
        )
        drawing.touch()
        doc.recompute()
        pump(60)
        check(
            "and the drawing of the flat draws it in that colour",
            drawnIn(page, rgb(flat.ViewObject.MarkingColor))[0],
            (rgb(flat.ViewObject.MarkingColor), sorted(pens(page))[:10]),
        )
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, mark.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalFlatMarkings")
        doc.recompute()
        pump(20)
        check(
            "run again on it, it is taken off and shown",
            flat.Markings == []
            and mark.Visibility
            and not [
                i
                for i in range(len(flat.Shape.Edges))
                if (flat.Shape.getElementMappedName("Edge%d" % (i + 1)) or "").startswith(
                    "Marking"
                )
            ],
            (flat.Markings, mark.Visibility),
        )
        doc.removeObject(mark.Name)
        doc.recompute()

        # Cross break: a wall's face picked on the tip; the solid as it was,
        # the X drawn on that face in 3D, the diagonals in the flat with the
        # note on the first, and on the drawing
        crossCmd = FreeCADGui.Command.get("PartDesign_SheetMetalCrossBreak")
        check(
            "Cross Break is on the tool bar",
            crossCmd is not None
            and toolbar() is not None
            and crossCmd.getAction()[0] in toolbar().actions(),
        )
        check(
            "Starter Shape is on the tool bar, first",
            starter is not None
            and toolbar() is not None
            and toolbar().actions()[:1] == [starter.getAction()[0]],
        )
        tip = body.Tip
        walls = [
            ("Face%d" % (i + 1), f)
            for i, f in enumerate(tip.Shape.Faces)
            if isinstance(f.Surface, Part.Plane)
            and (tip.sheetElementOf("Face%d" % (i + 1)) or "").startswith("Wall")
        ]
        faceName, _ = max(walls, key=lambda w: w[1].Area)
        wallName = tip.sheetElementOf(faceName)
        folded = tip.Shape.Volume
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.runCommand("PartDesign_SheetMetalCrossBreak")
        pump(20)
        check(
            "with no face selected it makes nothing",
            [o.Name for o in doc.Objects] == before,
        )
        FreeCADGui.Selection.addSelection(doc.Name, tip.Name, faceName)
        FreeCADGui.runCommand("PartDesign_SheetMetalCrossBreak")
        pump(20)
        crosses = [
            o
            for o in doc.Objects
            if o.Name not in before and o.TypeId == "PartDesign::SheetMetalCrossBreak"
        ]
        cross = crosses[0] if crosses else None

        def drawnX(obj):
            """Whether obj's view draws the X of its crossBreakLines(), as its
            switches stand"""
            ends = [v.Point for e in obj.crossBreakLines() for v in e.Vertexes]
            search = coin.SoSearchAction()
            search.setType(coin.SoCoordinate3.getClassTypeId())
            search.setInterest(coin.SoSearchAction.ALL)
            search.apply(obj.ViewObject.RootNode)
            for path in search.getPaths():
                pts = [V(*p.getValue()) for p in path.getTail().point.getValues()]
                if len(pts) == 4 and all(any(p.isEqual(q, 1e-6) for q in ends) for p in pts):
                    return True
            return False

        editing = FreeCADGui.Control.activeDialog()
        if cross is not None:
            doc.recompute()
            pump(20)
        drawnEditing = cross is not None and drawnX(cross)
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
            pump()
        # as OK does: the edit ended, not only the panel
        if FreeCADGui.ActiveDocument.getInEdit():
            FreeCADGui.ActiveDocument.resetEdit()
            pump()
        doc.recompute()
        pump(20)
        broken = cross and [e["Name"] for e in cross.SheetModel["Elements"] if "CrossBreak" in e]
        check(
            "with a wall's face selected it breaks that wall, the solid as it was",
            cross is not None
            and "Invalid" not in cross.State
            and body.Tip == cross
            and broken == [wallName]
            and abs(cross.Shape.Volume - folded) < 1e-9,
            cross and (cross.State, cross.getStatusString(), broken),
        )
        lines = cross.crossBreakLines() if cross else []
        check(
            "while its panel is open its view draws the X on the face picked",
            len(lines) == 2 and editing is not None and drawnEditing,
            (len(lines), editing is not None, drawnEditing),
        )
        check(
            "the panel closed, it does not",
            cross is not None and not drawnX(cross),
        )

        # The break as a surface finish the feature implies (ruled
        # 2026-11-06): App::SurfaceFinish CrossBreak (8) or CrossBreakReversed
        # (9) on both skins of the wall, carried on by every feature after it
        # and by the body, gone while it is suppressed
        def renderMaterial(vo):
            def walk(node):
                if node.getTypeId().getName() == "SoFCRenderMaterial":
                    return node
                children = getattr(node, "getChildren", None)
                kids = children() if children else None
                for i in range(kids.getLength() if kids else 0):
                    found = walk(kids[i])
                    if found:
                        return found
                return None

            return walk(vo.RootNode)

        def crossFinishes(vo):
            """{0-based face: palette entry} of the faces vo's render
            material gives a cross break finish"""
            node = renderMaterial(vo)
            if node is None:
                return {}
            palette = node.getField("finishPalette")
            indices = node.getField("finishIndices")
            res = {}
            for i in range(indices.getNum()):
                k = indices[i]
                if 0 <= k < palette.getNum() and round(palette[k].getValue()[0]) in (8, 9):
                    res[i] = k
            return res

        def skins(obj):
            return sorted(int(f["Face"][4:]) - 1 for f in PartDesign.crossBreakFaces(obj))

        own = crossFinishes(cross.ViewObject) if cross else {}
        check(
            "the wall's two skins carry the cross break as a finish, no other face",
            cross is not None
            and len(skins(cross)) == 2
            and sorted(own) == skins(cross)
            and len(set(own.values())) == 1,
            (own, cross and skins(cross)),
        )
        # the body draws the tip itself in its Tip mode only
        mode = body.ViewObject.DisplayModeBody
        body.ViewObject.DisplayModeBody = "Tip"
        body.touch()
        doc.recompute()
        pump(20)
        shown = crossFinishes(body.ViewObject)
        body.ViewObject.DisplayModeBody = mode
        pump(20)
        check(
            "and so does the body drawing its tip",
            len(skins(body)) == 2 and sorted(shown) == skins(body),
            (shown, skins(body)),
        )
        other = [
            ("Face%d" % (i + 1), f)
            for i, f in enumerate(cross.Shape.Faces if cross else [])
            if isinstance(f.Surface, Part.Plane)
            and (cross.sheetElementOf("Face%d" % (i + 1)) or "").startswith("Wall")
            and cross.sheetElementOf("Face%d" % (i + 1)) != wallName
        ]
        later = None
        if other:
            before = [o.Name for o in doc.Objects]
            FreeCADGui.Selection.clearSelection()
            FreeCADGui.Selection.addSelection(
                doc.Name, cross.Name, max(other, key=lambda w: w[1].Area)[0]
            )
            FreeCADGui.runCommand("PartDesign_SheetMetalCrossBreak")
            pump(20)
            if FreeCADGui.Control.activeDialog():
                FreeCADGui.Control.closeDialog()
                pump()
            doc.recompute()
            pump(20)
            found = [
                o
                for o in doc.Objects
                if o.Name not in before and o.TypeId == "PartDesign::SheetMetalCrossBreak"
            ]
            later = found[0] if found else None
        carried = crossFinishes(later.ViewObject) if later else {}
        check(
            "a cross break after it carries the first on beside its own",
            later is not None
            and len(skins(later)) == 4
            and sorted(carried) == skins(later)
            and len(set(carried.values())) == 2,
            (carried, later and skins(later)),
        )
        if later is not None:
            # the first suppressed: the later one's shape is what it was, and
            # its walls lose the first's break all the same
            cross.Suppress = True
            doc.recompute()
            pump(20)
            alone = crossFinishes(later.ViewObject)
            check(
                "the first suppressed, the one after it keeps its own wall's only",
                sorted(alone) == skins(later) and len(skins(later)) == 2,
                (alone, skins(later)),
            )
            cross.Suppress = False
            doc.recompute()
            pump(20)
            body.removeObject(later)
            doc.removeObject(later.Name)
            doc.recompute()
            pump(20)

        def picture(tag):
            view.redraw()
            pump(60)
            path = os.path.join(OUT, "cross-break-%s.png" % tag)
            view.saveRenderDump(path, metadata=False)
            image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB888)
            w, h = image.width(), image.height()
            raw = np.frombuffer(image.constBits(), dtype=np.uint8, count=h * image.bytesPerLine())
            return raw.reshape(h, image.bytesPerLine())[:, : w * 3].astype(np.int32)

        def differing(a, b):
            """How many pixels differ by more than a few levels"""
            if a.shape != b.shape:
                return -1
            return int((np.abs(a - b).reshape(a.shape[0], -1, 3).max(axis=2) > 8).sum())

        view.viewIsometric()
        view.fitAll()
        pump(40)
        try:
            broken = picture("on")
        except Exception as e:
            broken = None
            note("NOTE no picture: %s" % e)
        if cross is not None:
            cross.Suppress = True
            doc.recompute()
            pump(20)
        check(
            "suppressed, neither it nor the body carries the finish",
            cross is not None
            and crossFinishes(cross.ViewObject) == {}
            and crossFinishes(body.ViewObject) == {},
            cross and (crossFinishes(cross.ViewObject), crossFinishes(body.ViewObject)),
        )
        plain = picture("suppressed") if broken is not None else None
        count = differing(broken, plain) if plain is not None else -1
        check(
            "the renderer draws it: the picture differs from the one suppressed",
            count > 200,
            "%d pixels" % count,
        )
        if cross is not None:
            cross.Suppress = False
            doc.recompute()
            pump(20)
        doc.recompute()
        pump(20)
        crossNames = [
            n
            for n in (
                flat.Shape.getElementMappedName("Edge%d" % (i + 1)) or ""
                for i in range(len(flat.Shape.Edges))
            )
            if "CrossBreak" in n
        ]
        first = [n for n in crossNames if n.startswith(wallName + "CrossBreak1")]
        crossInfo = flat.ViewObject.getEdgeInfo(first[0]) if first else None
        check(
            "the flat has the diagonals, dash-dotted in their colour, the first with the note",
            len(crossNames) == 2
            and crossInfo is not None
            and crossInfo["Style"] == 4
            and abs(crossInfo["Color"][0] - flat.ViewObject.CrossBreakColor[0]) < 1e-6
            and crossInfo["Label"]
            == "CROSS BREAK %s 3\u00b0" % flat.CrossBreaks[wallName].upper(),
            (crossNames, crossInfo, dict(flat.CrossBreaks)),
        )
        drawing.touch()
        doc.recompute()
        pump(60)
        check(
            "and the drawing of the flat draws them in that colour",
            drawnIn(page, rgb(flat.ViewObject.CrossBreakColor))[0],
            (rgb(flat.ViewObject.CrossBreakColor), sorted(pens(page))[:10]),
        )
        if cross is not None:
            body.removeObject(cross)
            doc.removeObject(cross.Name)
            doc.recompute()
            pump(20)
        for o in labels:
            if doc.getObject(o.Name) is not None:
                doc.removeObject(o.Name)
        for name in [o.Name for o in (drawing, page, template)]:
            if doc.getObject(name) is not None:
                doc.removeObject(name)
        pump(20)

        # Form: a face of a wall on the tip and a sketch of points on it make a
        # dimple at each point (M5, ruled 2026-11-06)
        formCmd = FreeCADGui.Command.get("PartDesign_SheetMetalForm")
        check(
            "Form is on the tool bar",
            formCmd is not None
            and toolbar() is not None
            and formCmd.getAction()[0] in toolbar().actions(),
        )
        tip = body.Tip
        walls = [
            ("Face%d" % (i + 1), f)
            for i, f in enumerate(tip.Shape.Faces)
            if isinstance(f.Surface, Part.Plane)
            and (tip.sheetElementOf("Face%d" % (i + 1)) or "").startswith("Wall")
        ]
        faceName, wallFace = max(walls, key=lambda w: w[1].Area)
        centre = wallFace.CenterOfMass
        normal = wallFace.normalAt(*wallFace.Surface.parameter(centre))
        points = body.newObject("Sketcher::SketchObject", "FormPoints")
        points.Placement = FreeCAD.Placement(centre, FreeCAD.Rotation(V(0, 0, 1), normal))
        points.addGeometry(Part.Point(V(0, 0, 0)))
        doc.recompute()
        body.Tip = tip
        before = [o.Name for o in doc.Objects]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, tip.Name, faceName)
        FreeCADGui.Selection.addSelection(doc.Name, points.Name)
        FreeCADGui.runCommand("PartDesign_SheetMetalForm")
        pump(20)
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
            pump()
        if FreeCADGui.ActiveDocument.getInEdit():
            FreeCADGui.ActiveDocument.resetEdit()
            pump()
        doc.recompute()
        pump(20)
        forms = [
            o
            for o in doc.Objects
            if o.Name not in before and o.TypeId == "PartDesign::SheetMetalForm"
        ]
        form = forms[0] if forms else None
        if form is not None:
            # small enough for whatever wall is the largest here
            form.Diameter, form.Depth, form.Radius = 6, 2, 0.5
            doc.recompute()
            pump(20)
        check(
            "with a wall's face and a sketch of points selected it presses a dimple there",
            form is not None
            and "Invalid" not in form.State
            and form.Kind == "Dimple"
            and body.Tip == form
            and [f["Name"] for f in form.SheetModel.get("Forms", [])] == ["Form1"]
            and form.Shape.isValid(),
            form and (form.State, form.getStatusString()),
        )
        check(
            "and its view is the form's, the sketch hidden",
            form is not None
            and form.ViewObject.TypeId == "PartDesignGui::ViewProviderSheetMetalForm"
            and not points.Visibility,
            form and form.ViewObject.TypeId,
        )
        if form is not None:
            body.removeObject(form)
            doc.removeObject(form.Name)
        body.removeObject(points)
        doc.removeObject(points.Name)
        doc.recompute()
        pump(20)

        # Rip: a tube, which does not lie flat, cut open at a bend picked
        # (docs/SheetMetal.md sec 12, ruled 2026-11-05)
        ripCmd = FreeCADGui.Command.get("PartDesign_SheetMetalRip")
        check(
            "Rip is on the tool bar",
            ripCmd is not None
            and toolbar() is not None
            and ripCmd.getAction()[0] in toolbar().actions(),
        )
        duct = doc.addObject("PartDesign::Body", "Duct")
        view.setActiveObject("pdbody", duct)
        square = duct.newObject("Sketcher::SketchObject", "Square")
        pts = [(0, 0), (20, 0), (20, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            square.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        tube = duct.newObject("PartDesign::SheetMetalBaseWall", "Tube")
        tube.Profile = square
        tube.ClosedProfile = "Tube"
        tube.Length = 30
        doc.recompute()
        pump()
        bendFace = [
            "Face%d" % (i + 1)
            for i in range(len(tube.Shape.Faces))
            if tube.sheetElementOf("Face%d" % (i + 1)) == "Bend1"
        ][:1]
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, tube.Name, bendFace[0] if bendFace else "")
        before = [o.Name for o in doc.Objects]
        FreeCADGui.runCommand("PartDesign_SheetMetalRip")
        pump(20)
        rips = [
            o
            for o in doc.Objects
            if o.Name not in before and o.TypeId == "PartDesign::SheetMetalRip"
        ]
        rip = rips[0] if rips else None
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
            pump()
        doc.recompute()
        bends = rip and [e["Name"] for e in rip.SheetModel["Elements"] if e["Kind"] == "Bend"]
        check(
            "with a bend's face selected it rips the tube there, the body's tip",
            rip is not None
            and "Invalid" not in rip.State
            and duct.Tip == rip
            and rip.Faces[0] == tube
            and bends is not None
            and "Bend1" not in bends
            and len(bends) == 3,
            rip and (rip.State, rip.getStatusString(), bends),
        )
        check(
            "and the tube that would not lie flat does",
            rip is not None
            and rip.getSubObject("Flat") is not None
            and not [p for p in PartDesign.checkSheet(rip)["Problems"] if p["Kind"] == "NoFlat"],
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
