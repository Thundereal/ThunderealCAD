# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's flat as a view, not a feature (docs/SheetMetal.md sec 12,
# ruled 2026-11-03): every feature of a sheet's solid lays itself flat when
# asked, as its element Flat (feature.getSubObject("Flat")), and the
# solid gives its tip's. The flat's settings are the sheet's, on its solid:
# the root by the wall's name, the grain and the tangent lines; what a bill
# of materials takes are read-only values of the solid, from its tip, kept up
# only while the user has the solid's Flatten on (ruled 2026-11-03: nothing
# is laid flat in a recompute otherwise). An L
# of inner legs 19 and 9, 15 wide, 1 thick, the rule's radius 1 and K 0.5:
# 19 + BA + 9 long flat, where BA is pi / 2 * 1.5.

import math
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector
BA = math.pi / 2 * 1.5
VALUES = ("FlatLength", "FlatWidth", "FlatArea", "Thickness", "MaterialName", "BendCount",
          "MaxBendAngle", "BendDirections", "FlatStatus")


def flatOf(obj):
    # Part.getShape(obj, "Flat", needSubElement=True) would hand out its solid
    # alone, as it does for any compound element
    return obj.getSubObject("Flat")


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
        sheet.Flatten = True
        self.Doc.recompute()
        self.checkFlat(flatOf(sheet), 40, 20)
        self.assertEqual(sheet.BendCount, 0)
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
        self.assertEqual(sheet.BendCount, 1)
        self.assertEqual(sheet.FlatStatus, "")

    def testValues(self):
        """What a bill of materials takes, read only on the solid, its tip's;
        following an edit of the tip"""
        import Materials

        wall = self.L()
        card = Materials.Material()
        card.Name = "Sheet test"
        wall.Rule.ShapeMaterial = card
        sheet = self.Body.getSolidBody(wall)
        sheet.Flatten = True
        self.Doc.recompute()
        self.assertAlmostEqual(sheet.FlatLength.Value, 19 + BA + 9, places=6)
        self.assertAlmostEqual(sheet.FlatWidth.Value, 15, places=6)
        self.assertAlmostEqual(sheet.FlatArea.Value, (19 + BA + 9) * 15, places=6)
        self.assertAlmostEqual(sheet.Thickness.Value, 1)
        self.assertEqual(sheet.MaterialName, "Sheet test")
        self.assertEqual(sheet.BendCount, 1)
        self.assertAlmostEqual(sheet.MaxBendAngle.Value, 90)
        self.assertEqual(sheet.FlatStatus, "")
        for name in VALUES:
            self.assertIn("ReadOnly", sheet.getEditorMode(name), name)
        wall.Length = 12
        self.Doc.recompute()
        self.assertAlmostEqual(sheet.FlatWidth.Value, 12, places=6)

    def testFlattenOff(self):
        """Flatten off, as it starts: no flat laid in a recompute, the values
        left as they were; the flat itself still there when asked. On, the
        values the tip's; off again, an edit leaves them as they were"""
        wall = self.L()
        sheet = self.Body.getSolidBody(wall)
        self.assertFalse(sheet.Flatten)
        self.assertAlmostEqual(sheet.FlatArea.Value, 0)
        self.assertEqual(sheet.BendCount, 0)
        self.checkFlat(flatOf(sheet), 19 + BA + 9, 15)
        sheet.Flatten = True
        self.Doc.recompute()
        self.assertAlmostEqual(sheet.FlatWidth.Value, 15, places=6)
        self.assertEqual(sheet.BendCount, 1)
        sheet.Flatten = False
        wall.Length = 12
        self.Doc.recompute()
        self.assertAlmostEqual(sheet.FlatWidth.Value, 15, places=6)
        self.checkFlat(flatOf(sheet), 19 + BA + 9, 12)
        self.assertNotIn("Flat", wall.State)

    def testRootByWall(self):
        """Laid from the second wall by its name: the same flat moved; a name
        no wall has said, the values cleared, the flat empty"""
        wall = self.L()
        sheet = self.Body.getSolidBody(wall)
        sheet.Flatten = True
        first = self.solidOf(flatOf(wall)).CenterOfMass
        sheet.FlatRoot = "Wall2"
        self.Doc.recompute()
        second = self.checkFlat(flatOf(wall), 19 + BA + 9, 15).CenterOfMass
        self.assertGreater((first - second).Length, 1)
        sheet.FlatRoot = "Wall9"
        self.Doc.recompute()
        self.assertIn("Wall9", sheet.FlatStatus)
        self.assertAlmostEqual(sheet.FlatArea.Value, 0)
        self.assertIsNone(flatOf(wall))

    def testSecondSkin(self):
        """The root wall's second skin on XY instead: the same flat turned
        over"""
        wall = self.L()
        sheet = self.Body.getSolidBody(wall)
        sheet.FlatSecondSkin = True
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
        sheet = self.Body.getSolidBody(pocket)
        sheet.Flatten = True
        self.Doc.recompute()
        self.assertAlmostEqual(sheet.FlatArea.Value, pocket.Shape.Volume, places=6)

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
        element of it, by Part.getShape() too; Flat. names an object, and
        there is none"""
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
        self.assertIsNone(wall.getSubObject("Flat."))
        # the folded shape's own elements as they were
        self.assertTrue(wall.getSubObject("Face1").isSame(wall.Shape.Faces[0]))
        sheet = self.Body.getSolidBody(wall)
        self.assertTrue(sheet.getSubObject("FlatFace1").isPartner(flat.Faces[0]))

    def testSettings(self):
        """The solid's grain and tangent lines are every feature's flat's"""
        wall = self.L()
        sheet = self.Body.getSolidBody(wall)
        self.assertEqual(len(flatOf(wall).SubShapes), 2)
        sheet.TangentLines = True
        sheet.Grain = True
        self.Doc.recompute()
        flat = flatOf(wall)
        self.assertEqual(len(flat.SubShapes), 4)
        self.assertEqual(len(flat.SubShapes[2].Edges), 2)
        self.assertEqual(len(flat.SubShapes[3].Edges), 3)

    def testNotSheet(self):
        """A body of no sheet metal: no flat, its solid's values hidden"""
        sketch = self.sketch([(0, 0), (10, 0), (10, 10), (0, 10)], closed=True)
        pad = self.Body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 2
        self.Doc.recompute()
        self.assertIsNone(flatOf(pad))
        sheet = self.Body.getSolidBody(pad)
        if sheet is not None:
            for name in VALUES + ("Flatten", "FlatRoot", "Grain", "TangentLines"):
                self.assertIn("Hidden", sheet.getEditorMode(name), name)
