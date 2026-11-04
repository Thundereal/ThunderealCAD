# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's flat (docs/SheetMetal.md sec 12): every feature of a sheet's
# solid lays itself flat when asked, as its element Flat
# (feature.getSubObject("Flat")), and the solid gives its tip's. The sheet
# has one flat object (ruled 2026-11-04), made by the body, hidden: it holds
# the flat's settings (the root by the wall's name, the grain, the tangent
# lines), which every feature's flat follows, and saves its solid's tip's
# flat with what a bill of materials takes of it, laid only while it is
# shown or something uses it (nothing is laid flat in a recompute
# otherwise, ruled 2026-11-03). An L
# of inner legs 19 and 9, 15 wide, 1 thick, the rule's radius 1 and K 0.5:
# 19 + BA + 9 long flat, where BA is pi / 2 * 1.5.

import math
import os
import tempfile
import unittest
import xml.etree.ElementTree as ET

import FreeCAD
import Part
import PartDesign

V = FreeCAD.Vector
BA = math.pi / 2 * 1.5
VALUES = (
    "FlatLength",
    "FlatWidth",
    "FlatArea",
    "Thickness",
    "MaterialName",
    "BendCount",
    "MaxBendAngle",
    "BendDirections",
    "Status",
)
SETTINGS = ("Root", "SecondSkin", "AlignEdge", "TangentLines", "Grain", "GrainAngle")
OUT_OF_DATE = "Out of date: it is laid flat when shown"


def readDXF(path):
    """The entities of a DXF file: (type, layer, {code: [value]})"""
    with open(path) as f:
        lines = [line.strip() for line in f]
    entities, current, inside = [], None, False
    for code, value in zip(lines[0::2], lines[1::2]):
        if code == "2" and value == "ENTITIES":
            inside = True
            continue
        if not inside:
            continue
        if code == "0":
            if current:
                entities.append(current)
            current = None if value in ("ENDSEC", "EOF", "SEQEND") else [value, "", {}]
            if value == "ENDSEC":
                inside = False
        elif current is not None:
            if code == "8":
                current[1] = value
            else:
                current[2].setdefault(code, []).append(value)
    return entities


def readSVG(path):
    """The layers of an SVG: {id: [(tag, attributes, text)]}, and the root's
    attributes"""
    root = ET.parse(path).getroot()
    layers = {}
    for g in root.iter("{http://www.w3.org/2000/svg}g"):
        if g.get("id"):
            layers[g.get("id")] = [(c.tag.split("}")[1], c.attrib, c.text) for c in g]
    return layers, root.attrib


def flatOf(obj):
    # Part.getShape(obj, "Flat", needSubElement=True) would hand out its solid
    # alone, as it does for any compound element
    return obj.getSubObject("Flat")


def flatObjectsOf(doc):
    return [o for o in doc.Objects if o.isDerivedFrom("PartDesign::SheetMetalFlatPattern")]


class TestSheetMetalFlat(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalFlat")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def sketch(self, points, closed=False, name="Sketch"):
        sketch = self.Body.newObject("Sketcher::SketchObject", name)
        n = len(points) if closed else len(points) - 1
        for i in range(n):
            a, b = points[i], points[(i + 1) % len(points)]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        return sketch

    def wall(self, points, closed=False, **props):
        wall = self.Body.newObject("PartDesign::SheetMetalBaseWall", "BaseWall")
        wall.Profile = self.sketch(points, closed)
        for k, v in props.items():
            setattr(wall, k, v)
        self.Doc.recompute()
        return wall

    def L(self):
        return self.wall([(0, 0), (20, 0), (20, 10)], Length=15)

    def flatObject(self, feature):
        """The flat object of a feature's sheet"""
        sheet = self.Body.getSolidBody(feature)
        found = [o for o in flatObjectsOf(self.Doc) if o.Sheet == sheet]
        self.assertEqual(len(found), 1)
        return found[0]

    def show(self, flat):
        flat.Visibility = True
        self.Doc.recompute()

    def solidOf(self, flat):
        solids = flat.SubShapes[0].Solids
        self.assertEqual(len(solids), 1)
        self.assertTrue(solids[0].isValid())
        return solids[0]

    def checkFlat(self, flat, length, width):
        solid = self.solidOf(flat)
        box = solid.BoundBox
        # on XY, the material above
        self.assertAlmostEqual(box.ZMin, 0, places=6)
        self.assertAlmostEqual(box.ZMax, 1, places=6)
        for a, b in zip(sorted([box.XLength, box.YLength]), sorted([length, width])):
            self.assertAlmostEqual(a, b, places=6)
        return solid

    def testFeatureFlat(self):
        """A feature's flat, asked for: the solid laid on XY, its bend line"""
        wall = self.L()
        flat = flatOf(wall)
        solid = self.checkFlat(flat, 19 + BA + 9, 15)
        self.assertAlmostEqual(solid.Volume, wall.Shape.Volume, places=6)
        lines = flat.SubShapes[1].Edges
        self.assertEqual(len(lines), 1)
        self.assertAlmostEqual(lines[0].Length, 15, places=6)

    def testSolidFlat(self):
        """The solid's flat is its tip's, and follows the tip: a plate 40 x
        20, then a flange on its far edge, 18 + BA + 8 by 40 flat"""
        plate = self.wall([(0, 0), (40, 0), (40, 20), (0, 20)], closed=True)
        sheet = self.Body.getSolidBody(plate)
        self.assertIsNotNone(sheet)
        flat = self.flatObject(plate)
        self.show(flat)
        self.checkFlat(flatOf(sheet), 40, 20)
        self.assertEqual(flat.BendCount, 0)
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        edge = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(plate.Shape.Edges)
            if all(abs(v.Point.y - 20) < 1e-7 and abs(v.Point.z - 1) < 1e-7 for v in e.Vertexes)
        ]
        self.assertEqual(len(edge), 1)
        flange.Edges = (plate, edge)
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        self.assertEqual(self.Body.getSolidBody(flange), sheet)
        solid = self.checkFlat(flatOf(sheet), 18 + BA + 8, 40)
        self.assertAlmostEqual(solid.Volume, flange.Shape.Volume, places=6)
        self.assertEqual(flat.BendCount, 1)
        self.assertEqual(flat.Status, "")
        self.checkFlat(flat.Shape, 18 + BA + 8, 40)

    def testValues(self):
        """What a bill of materials takes, read only on the flat object, of its
        solid's tip; following an edit of the tip"""
        import Materials

        wall = self.L()
        card = Materials.Material()
        card.Name = "Sheet test"
        wall.Rule.ShapeMaterial = card
        flat = self.flatObject(wall)
        self.show(flat)
        self.assertAlmostEqual(flat.FlatLength.Value, 19 + BA + 9, places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, 15, places=6)
        self.assertAlmostEqual(flat.FlatArea.Value, (19 + BA + 9) * 15, places=6)
        self.assertAlmostEqual(flat.Thickness.Value, 1)
        self.assertEqual(flat.MaterialName, "Sheet test")
        self.assertEqual(flat.BendCount, 1)
        self.assertAlmostEqual(flat.MaxBendAngle.Value, 90)
        self.assertEqual(flat.Status, "")
        for name in VALUES:
            self.assertIn("ReadOnly", flat.getEditorMode(name), name)
        wall.Length = 12
        self.Doc.recompute()
        self.assertAlmostEqual(flat.FlatWidth.Value, 12, places=6)

    def testFlatObject(self):
        """One flat object per sheet, made by the body with the first sheet
        metal feature: hidden, its solid's, not in the solid's chain nor the
        body's tip; a second feature makes no other; the solid has no
        settings of its own"""
        wall = self.L()
        flats = flatObjectsOf(self.Doc)
        self.assertEqual(len(flats), 1)
        flat = flats[0]
        sheet = self.Body.getSolidBody(wall)
        self.assertEqual(flat.Sheet, sheet)
        self.assertFalse(flat.Visibility)
        self.assertEqual(flat.Label, "Flat")
        self.assertIn(flat, self.Body.Group)
        self.assertNotIn(flat, sheet.Group)
        self.assertEqual(self.Body.Tip, wall)
        for name in ("Flatten", "FlatRoot", "TangentLines", "FlatLength", "FlatStatus"):
            self.assertNotIn(name, sheet.PropertiesList)
        for name in SETTINGS + VALUES:
            self.assertIn(name, flat.PropertiesList)
        hem = self.Body.newObject("PartDesign::SheetMetalHem", "Hem")
        hem.Edges = (wall, [self.endEdge(wall)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", hem.State)
        self.assertEqual(flatObjectsOf(self.Doc), [flat])

    def endEdge(self, wall):
        """An edge where the L's short leg's skin meets its end, at y 10"""
        for i, e in enumerate(wall.Shape.Edges):
            ends = [v.Point for v in e.Vertexes]
            if (
                len(ends) == 2
                and all(abs(p.y - 10) < 1e-7 for p in ends)
                and abs(ends[0].x - ends[1].x) < 1e-7
            ):
                return "Edge%d" % (i + 1)
        self.fail("no end edge")

    def testLaidWhenShown(self):
        """Hidden, nothing is laid flat in a recompute: no shape, out of date.
        Shown, it asks for a recompute and holds the tip's flat; it follows an
        edit; hidden again, an edit leaves the flat as it was, out of date,
        until it is shown"""
        wall = self.L()
        flat = self.flatObject(wall)
        self.Doc.recompute()
        self.assertTrue(flat.Shape.isNull())
        self.assertEqual(flat.Status, OUT_OF_DATE)
        self.assertEqual(flat.BendCount, 0)
        flat.Visibility = True
        self.assertIn("Touched", flat.State)
        self.Doc.recompute()
        self.assertEqual(flat.Status, "")
        self.checkFlat(flat.Shape, 19 + BA + 9, 15)
        wall.Length = 12
        self.Doc.recompute()
        self.checkFlat(flat.Shape, 19 + BA + 9, 12)
        flat.Visibility = False
        wall.Length = 10
        self.Doc.recompute()
        self.assertEqual(flat.Status, OUT_OF_DATE)
        self.checkFlat(flat.Shape, 19 + BA + 9, 12)
        self.assertAlmostEqual(flat.FlatWidth.Value, 12, places=6)
        self.show(flat)
        self.checkFlat(flat.Shape, 19 + BA + 9, 10)
        self.assertAlmostEqual(flat.FlatWidth.Value, 10, places=6)

    def testLaidWhenUsed(self):
        """Hidden but used, by a link here: laid flat as if shown"""
        wall = self.L()
        flat = self.flatObject(wall)
        link = self.Doc.addObject("App::Link", "FlatLink")
        link.LinkedObject = flat
        self.Doc.recompute()
        self.assertFalse(flat.Visibility)
        self.assertEqual(flat.Status, "")
        self.checkFlat(flat.Shape, 19 + BA + 9, 15)

    def testFlatPlaced(self):
        """Its placement moves its shape, the flat drawn off the folded part"""
        wall = self.L()
        flat = self.flatObject(wall)
        self.show(flat)
        before = flat.Shape.BoundBox
        flat.Placement.Base = V(100, 0, 0)
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.BoundBox.XMin, before.XMin + 100, places=6)
        wall.Length = 12
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.BoundBox.XMin, before.XMin + 100, places=6)

    def testSaved(self):
        """Its shape and values are saved: opened again, the flat is there
        without a recompute, its faces named after the folded ones"""
        wall = self.L()
        flat = self.flatObject(wall)
        self.show(flat)
        names = flat.Shape.ElementMapSize
        path = os.path.join(tempfile.mkdtemp(), "flat.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        flat = flatObjectsOf(self.Doc)[0]
        self.assertNotIn("Touched", flat.State)
        self.assertEqual(flat.Status, "")
        self.checkFlat(flat.Shape, 19 + BA + 9, 15)
        self.assertEqual(flat.Shape.ElementMapSize, names)
        self.assertAlmostEqual(flat.FlatWidth.Value, 15, places=6)

    def testNotLaying(self):
        """Shown with a root no wall has: why, the last flat kept"""
        wall = self.L()
        flat = self.flatObject(wall)
        self.show(flat)
        flat.Root = "Wall9"
        self.Doc.recompute()
        self.assertIn("Wall9", flat.Status)
        self.checkFlat(flat.Shape, 19 + BA + 9, 15)

    def testUndoneWithItsSheet(self):
        """Made in the same undo step as the first sheet metal feature, and
        undone with it"""
        self.Doc.UndoMode = 1
        self.Doc.openTransaction("Wall")
        self.L()
        self.Doc.commitTransaction()
        self.assertEqual(len(flatObjectsOf(self.Doc)), 1)
        self.Doc.undo()
        self.assertEqual(flatObjectsOf(self.Doc), [])

    def testRootByWall(self):
        """Laid from the second wall by its name: the same flat moved; a name
        no wall has, no flat"""
        wall = self.L()
        flat = self.flatObject(wall)
        first = self.solidOf(flatOf(wall)).CenterOfMass
        flat.Root = "Wall2"
        self.Doc.recompute()
        second = self.checkFlat(flatOf(wall), 19 + BA + 9, 15).CenterOfMass
        self.assertGreater((first - second).Length, 1)
        flat.Root = "Wall9"
        self.Doc.recompute()
        self.assertIsNone(flatOf(wall))

    def testSecondSkin(self):
        """The root wall's second skin on XY instead: the same flat turned
        over"""
        wall = self.L()
        self.flatObject(wall).SecondSkin = True
        self.Doc.recompute()
        self.checkFlat(flatOf(wall), 19 + BA + 9, 15)

    def testAfterSheetFeature(self):
        """A pocket after the sheet, no sheet metal feature: its flat laid by
        the sheet's model, the hole in it; the solid's values its"""
        wall = self.L()
        sketch = self.Body.newObject("Sketcher::SketchObject", "HoleSketch")
        sketch.Placement = FreeCAD.Placement(V(0, 0, 7.5), FreeCAD.Rotation(V(1, 0, 0), 90))
        sketch.addGeometry(Part.Circle(V(8, 0, 0), V(0, 0, 1), 2))
        pocket = self.Body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = sketch
        pocket.Type = "ThroughAll"
        pocket.Midplane = True
        self.Doc.recompute()
        self.assertNotIn("Invalid", pocket.State)
        solid = self.checkFlat(flatOf(pocket), 19 + BA + 9, 15)
        self.assertAlmostEqual(solid.Volume, pocket.Shape.Volume, places=6)
        flat = self.flatObject(pocket)
        self.show(flat)
        self.assertAlmostEqual(flat.FlatArea.Value, pocket.Shape.Volume, places=6)

    def testCached(self):
        """Asked again with nothing changed, the same flat; after an edit, a
        new one"""
        wall = self.L()
        a = wall.getSubObject("Flat")
        self.assertTrue(a.isSame(wall.getSubObject("Flat")))
        wall.Length = 12
        self.Doc.recompute()
        b = wall.getSubObject("Flat")
        self.assertFalse(a.isSame(b))
        self.checkFlat(b, 19 + BA + 9, 12)

    def testElementNames(self):
        """Named as a sketch's InternalShape is: Flat the whole, FlatFace1 an
        element of it, by Part.getShape() too; Flat. names an object, the
        flat object"""
        wall = self.L()
        flat = flatOf(wall)
        face = wall.getSubObject("FlatFace1")
        self.assertTrue(face.isSame(flat.Faces[0]))
        face = Part.getShape(wall, "FlatFace1", needSubElement=True)
        self.assertEqual(face.ShapeType, "Face")
        self.assertTrue(face.isPartner(flat.Faces[0]))
        edge = wall.getSubObject("FlatEdge%d" % len(flat.Edges))
        self.assertTrue(edge.isSame(flat.Edges[-1]))
        self.assertIsNone(wall.getSubObject("FlatFace%d" % (len(flat.Faces) + 1)))
        # "Flat." names an object: the sheet's flat object, a member of the body
        self.assertEqual(wall.getSubObject("Flat.", retType=1), self.flatObject(wall))
        # the folded shape's own elements as they were
        self.assertTrue(wall.getSubObject("Face1").isSame(wall.Shape.Faces[0]))
        sheet = self.Body.getSolidBody(wall)
        self.assertTrue(sheet.getSubObject("FlatFace1").isPartner(flat.Faces[0]))

    def exported(self, obj, suffix):
        path = os.path.join(tempfile.mkdtemp(), "flat" + suffix)
        PartDesign.exportFlat(obj, path)
        return path

    def testExportDXF(self):
        """A feature's flat, the solid's or the flat object's (its tip's),
        written as DXF, by the file's suffix: the outline as large as the
        flat, the bend line on its direction's layer, labelled; the sheet's
        tangent lines with it"""
        wall = self.L()
        sheet = self.Body.getSolidBody(wall)
        for obj in (wall, sheet, self.flatObject(wall)):
            ents = readDXF(self.exported(obj, ".dxf"))
            outline = [e for e in ents if e[1] == "OUTLINE"]
            self.assertEqual({e[0] for e in outline}, {"LINE"})
            self.assertEqual(len(outline), 4)
            xs = [float(x) for e in outline for x in e[2]["10"] + e[2]["11"]]
            ys = [float(y) for e in outline for y in e[2]["20"] + e[2]["21"]]
            size = sorted([max(xs) - min(xs), max(ys) - min(ys)])
            self.assertAlmostEqual(size[0], 15, places=6)
            self.assertAlmostEqual(size[1], 19 + BA + 9, places=6)
            bends = [e for e in ents if e[1] in ("BEND_UP", "BEND_DOWN") and e[0] == "LINE"]
            self.assertEqual(len(bends), 1)
            labels = [e[2]["1"][0] for e in ents if e[1] == "LABELS" and e[0] == "TEXT"]
            self.assertEqual(labels, ["Bend1"])
            self.assertEqual([e for e in ents if e[1] == "BEND_TANGENT"], [])
        self.flatObject(wall).TangentLines = True
        self.Doc.recompute()
        ents = readDXF(self.exported(wall, ".DXF"))
        self.assertEqual(len([e for e in ents if e[1] == "BEND_TANGENT"]), 2)

    def testExportSVG(self):
        """The same as SVG: the layers as groups, the drawing as large as the
        flat, in mm"""
        wall = self.L()
        sheet = self.Body.getSolidBody(wall)
        layers, root = readSVG(self.exported(sheet, ".svg"))
        self.assertEqual([e[0] for e in layers["OUTLINE"]], ["line"] * 4)
        self.assertEqual(len(layers.get("BEND_UP", []) + layers.get("BEND_DOWN", [])), 1)
        size = sorted(float(v) for v in root["viewBox"].split()[2:])
        self.assertAlmostEqual(size[0], 15, places=6)
        self.assertAlmostEqual(size[1], 19 + BA + 9, places=6)
        self.assertTrue(root["width"].endswith("mm"), root)

    def testExportRefused(self):
        """Refused, saying why: a file neither .dxf nor .svg; a feature that
        does not lie flat; one of no sheet. Nothing written"""
        wall = self.L()
        folder = tempfile.mkdtemp()
        with self.assertRaisesRegex(ValueError, "dxf or .svg"):
            PartDesign.exportFlat(wall, os.path.join(folder, "flat.txt"))
        self.flatObject(wall).Root = "Wall9"
        self.Doc.recompute()
        with self.assertRaisesRegex(ValueError, "Wall9"):
            PartDesign.exportFlat(wall, os.path.join(folder, "flat.dxf"))
        other = self.Doc.addObject("PartDesign::Body", "Other")
        sketch = other.newObject("Sketcher::SketchObject", "Square")
        for a, b in (((0, 0), (5, 0)), ((5, 0), (5, 5)), ((5, 5), (0, 5)), ((0, 5), (0, 0))):
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        pad = other.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        self.Doc.recompute()
        with self.assertRaisesRegex(ValueError, "sheet"):
            PartDesign.exportFlat(pad, os.path.join(folder, "flat.dxf"))
        self.assertEqual(os.listdir(folder), [])

    # Every sheet's flat at once, one file each (sec 5.5's catalogue)

    def plate(self, body, label, size, **props):
        """A plate in body, labelled; its sketch a size[0] x size[1] rectangle"""
        body.Label = label
        sketch = body.newObject("Sketcher::SketchObject", "Plate" + body.Name)
        x, y = size
        pts = [(0, 0), (x, 0), (x, y), (0, y)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        wall = body.newObject("PartDesign::SheetMetalBaseWall", "Wall" + body.Name)
        wall.Profile = sketch
        for k, v in props.items():
            setattr(wall, k, v)
        self.Doc.recompute()
        self.assertNotIn("Invalid", wall.State)
        return wall

    def outlineSize(self, path):
        ents = readDXF(path)
        xs = [float(x) for e in ents if e[1] == "OUTLINE" for x in e[2]["10"] + e[2]["11"]]
        ys = [float(y) for e in ents if e[1] == "OUTLINE" for y in e[2]["20"] + e[2]["21"]]
        return sorted([round(max(xs) - min(xs), 6), round(max(ys) - min(ys), 6)])

    def testExportEvery(self):
        """Every sheet of the document, one file each in the folder, named
        after its body, as DXF or SVG; a body of no sheet is passed over"""
        self.L()
        self.plate(self.Doc.addObject("PartDesign::Body", "Other"), "Bracket", (30, 20))
        padded = self.Doc.addObject("PartDesign::Body", "Padded")
        sketch = padded.newObject("Sketcher::SketchObject", "Square")
        for a, b in (((0, 0), (5, 0)), ((5, 0), (5, 5)), ((5, 5), (0, 5)), ((0, 5), (0, 0))):
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        padded.newObject("PartDesign::Pad", "Pad").Profile = sketch
        self.Doc.recompute()
        for suffix in ("dxf", "svg"):
            folder = tempfile.mkdtemp()
            res = PartDesign.exportFlats(self.Doc, folder, suffix)
            self.assertEqual([r["Error"] for r in res], ["", ""])
            self.assertEqual(
                [os.path.basename(r["File"]) for r in res],
                ["Body." + suffix, "Bracket." + suffix],
            )
            self.assertEqual(sorted(os.listdir(folder)), ["Body." + suffix, "Bracket." + suffix])
            self.assertEqual(
                [r["Sheet"] for r in res],
                [self.Body.SolidBodies[0], self.Doc.Other.SolidBodies[0]],
            )
        folder = tempfile.mkdtemp()
        PartDesign.exportFlats(self.Doc, folder, ".DXF")
        self.assertEqual(
            self.outlineSize(os.path.join(folder, "Body.dxf")), [15, round(19 + BA + 9, 6)]
        )
        self.assertEqual(self.outlineSize(os.path.join(folder, "Bracket.dxf")), [20, 30])

    def testExportEveryNames(self):
        """A body of two sheets names each file after the body and the
        sheet's solid; a label is made a file name, and a name taken twice
        is numbered"""
        first = self.plate(self.Body, "Box/Lid", (10, 10))
        self.plate(self.Body, "Box/Lid", (20, 10), NewSolid=True)
        self.assertEqual(len(self.Body.SolidBodies), 2)
        other = self.Doc.addObject("PartDesign::Body", "Other")
        self.plate(other, "Box?Lid-" + self.Body.SolidBodies[0].Label, (5, 5))
        folder = tempfile.mkdtemp()
        res = PartDesign.exportFlats(self.Doc, folder, "dxf")
        a, b = (s.Label for s in self.Body.SolidBodies)
        self.assertEqual(
            [os.path.basename(r["File"]) for r in res],
            ["Box_Lid-%s.dxf" % a, "Box_Lid-%s.dxf" % b, "Box_Lid-%s-2.dxf" % a],
        )
        self.assertIn(first, self.Body.SolidBodies[0].Group)
        self.assertEqual(len(os.listdir(folder)), 3)

    def testExportEveryFailing(self):
        """A sheet that does not lie flat is told, with why, and written not;
        the others are. Refused before anything is written: a suffix neither
        dxf nor svg, a folder that is not there"""
        wall = self.L()
        self.plate(self.Doc.addObject("PartDesign::Body", "Other"), "Bracket", (30, 20))
        self.flatObject(wall).Root = "Wall9"
        self.Doc.recompute()
        folder = tempfile.mkdtemp()
        res = PartDesign.exportFlats(self.Doc, folder, "dxf")
        self.assertEqual(res[0]["File"], "")
        self.assertIn("Wall9", res[0]["Error"])
        self.assertEqual(res[1]["Error"], "")
        self.assertEqual(os.listdir(folder), ["Bracket.dxf"])
        empty = tempfile.mkdtemp()
        with self.assertRaisesRegex(ValueError, "dxf or svg"):
            PartDesign.exportFlats(self.Doc, empty, "txt")
        with self.assertRaisesRegex(ValueError, "folder"):
            PartDesign.exportFlats(self.Doc, os.path.join(empty, "none"), "dxf")
        self.assertEqual(os.listdir(empty), [])

    def testSettings(self):
        """The flat object's grain and tangent lines are every feature's flat's"""
        wall = self.L()
        flat = self.flatObject(wall)
        self.assertEqual(len(flatOf(wall).SubShapes), 2)
        flat.TangentLines = True
        flat.Grain = True
        self.Doc.recompute()
        flat = flatOf(wall)
        self.assertEqual(len(flat.SubShapes), 4)
        self.assertEqual(len(flat.SubShapes[2].Edges), 2)
        self.assertEqual(len(flat.SubShapes[3].Edges), 3)

    # A feature's flat stored with it while something refers to it -- a
    # sketch's external geometry -- so the file holds the flat the reference
    # was made on; laid on demand otherwise (docs/SheetMetal.md sec 12)

    def referenced(self):
        """The L, and a sketch after it taking a straight edge of its flat's
        top as external geometry: the wall, the sketch, the element's name"""
        wall = self.L()
        flat = flatOf(wall)
        names = [
            "FlatEdge%d" % (i + 1)
            for i, e in enumerate(flat.Edges)
            if isinstance(e.Curve, Part.Line) and e.BoundBox.ZMin > 1 - 1e-7 and e.Length > 10
        ]
        self.assertTrue(names)
        sketch = self.Body.newObject("Sketcher::SketchObject", "Ref")
        sketch.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        sketch.addExternal(wall.Name, names[0])
        self.Doc.recompute()
        self.assertNotIn("Invalid", sketch.State, sketch.getStatusString())
        return wall, sketch, names[0]

    def saved(self):
        path = os.path.join(tempfile.mkdtemp(), "stored.FCStd")
        self.Doc.saveAs(path)
        return path

    def reopened(self, path):
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        self.Body = self.Doc.getObject("Body")
        return self.Doc

    def testStoredFlatWhileReferenced(self):
        """Saved, the referred flat is stored with the feature; opened again,
        the reference finds the element it was made on, before any
        recompute"""
        wall, sketch, name = self.referenced()
        edge = wall.getSubObject(name)
        path = self.saved()
        self.assertIn("StoredFlat", wall.PropertiesList)
        self.assertEqual(len(wall.StoredFlat.Edges), len(flatOf(wall).Edges))
        names = wall.Name, sketch.Name
        doc = self.reopened(path)
        wall = doc.getObject(names[0])
        self.assertIn("StoredFlat", wall.PropertiesList)
        again = wall.getSubObject(name)
        self.assertIsNotNone(again)
        for a, b in (
            (again.Vertexes[0].Point, edge.Vertexes[0].Point),
            (again.Vertexes[-1].Point, edge.Vertexes[-1].Point),
        ):
            self.assertTrue(a.isEqual(b, 1e-9), (a, b))
        self.assertNotIn("Invalid", doc.getObject(names[1]).State)

    def testNoStoredFlatUnreferenced(self):
        """Nothing referring to its flat, a feature stores none"""
        wall = self.L()
        flatOf(wall)
        self.saved()
        self.assertNotIn("StoredFlat", wall.PropertiesList)

    def testStoredFlatDropped(self):
        """The reference gone, the next save drops the stored flat"""
        wall, sketch, name = self.referenced()
        self.saved()
        self.assertIn("StoredFlat", wall.PropertiesList)
        self.Doc.removeObject(sketch.Name)
        self.Doc.recompute()
        self.saved()
        self.assertNotIn("StoredFlat", wall.PropertiesList)

    def testStoredFlatChanged(self):
        """The feature changed, its flat is laid anew, not the stored one,
        and the next save stores the new one"""
        wall, sketch, name = self.referenced()
        path = self.saved()
        named = wall.Name
        doc = self.reopened(path)
        wall = doc.getObject(named)
        before = flatOf(wall).BoundBox
        wall.Length = 20
        doc.recompute()
        after = flatOf(wall).BoundBox
        self.assertAlmostEqual(
            max(after.XLength, after.YLength) - max(before.XLength, before.YLength), 0, places=6
        )
        self.assertAlmostEqual(
            min(after.XLength, after.YLength) - min(before.XLength, before.YLength), 5, places=6
        )
        self.saved()
        stored = wall.StoredFlat.BoundBox
        self.assertAlmostEqual(
            min(stored.XLength, stored.YLength), min(after.XLength, after.YLength), places=6
        )

    def testStoredFlatNoUndoStep(self):
        """Storing the flat at a save is no undo step"""
        self.Doc.UndoMode = 1
        wall, sketch, name = self.referenced()
        count = self.Doc.UndoCount
        self.saved()
        self.assertIn("StoredFlat", wall.PropertiesList)
        self.assertEqual(self.Doc.UndoCount, count)

    def testNotSheet(self):
        """A body of no sheet metal: no flat, no flat object"""
        sketch = self.sketch([(0, 0), (10, 0), (10, 10), (0, 10)], closed=True)
        pad = self.Body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 2
        self.Doc.recompute()
        self.assertIsNone(flatOf(pad))
        self.assertEqual(flatObjectsOf(self.Doc), [])
