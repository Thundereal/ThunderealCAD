# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's flat as a view, not a feature (docs/SheetMetal.md sec 12,
# ruled 2026-11-03): every feature of a sheet's solid lays itself flat when
# asked, as its sub-object Flat (Part.getShape(feature, "Flat.")), and the
# solid gives its tip's. The flat's settings are the sheet's, on its solid:
# the root by the wall's name, the grain and the tangent lines; what a bill
# of materials takes are read-only values of the solid, from its tip. An L
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
    return Part.getShape(obj, "Flat.")


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
        """The solid's flat is its tip's, and follows the tip"""
        wall = self.L()
        sheet = self.Body.getSolidBody(wall)
        self.assertIsNotNone(sheet)
        self.checkFlat(flatOf(sheet), 19 + BA + 9, 15)
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        edge = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(wall.Shape.Edges)
            if all(abs(v.Point.z - 15) < 1e-7 and abs(v.Point.y) < 1e-7 for v in e.Vertexes)
            and e.Length > 18
        ]
        self.assertEqual(len(edge), 1)
        flange.Edges = (wall, edge)
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        self.assertEqual(self.Body.getSolidBody(flange), sheet)
        self.assertAlmostEqual(self.solidOf(flatOf(sheet)).Volume, flange.Shape.Volume, places=6)
        self.assertEqual(sheet.BendCount, 2)

    def testValues(self):
        """What a bill of materials takes, read only on the solid, its tip's;
        following an edit of the tip"""
        import Materials

        wall = self.L()
        card = Materials.Material()
        card.Name = "Sheet test"
        wall.Rule.ShapeMaterial = card
        self.Doc.recompute()
        sheet = self.Body.getSolidBody(wall)
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

    def testRootByWall(self):
        """Laid from the second wall by its name: the same flat moved; a name
        no wall has said, the values cleared"""
        wall = self.L()
        sheet = self.Body.getSolidBody(wall)
        first = self.solidOf(flatOf(wall)).BoundBox
        sheet.FlatRoot = "Wall2"
        self.Doc.recompute()
        second = self.checkFlat(flatOf(wall), 19 + BA + 9, 15).BoundBox
        self.assertGreater(
            abs(first.XMin - second.XMin) + abs(first.YMin - second.YMin), 1
        )
        sheet.FlatRoot = "Wall9"
        self.Doc.recompute()
        self.assertIn("Wall9", sheet.FlatStatus)
        self.assertAlmostEqual(sheet.FlatArea.Value, 0)
        self.assertRaises(Exception, flatOf, wall)

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
        self.assertAlmostEqual(sheet.FlatArea.Value, pocket.Shape.Volume, places=6)

    def testCached(self):
        """Asked again with nothing changed, the same flat; after an edit, a
        new one"""
        wall = self.L()
        a = flatOf(wall)
        self.assertTrue(a.isSame(flatOf(wall)))
        wall.Length = 12
        self.Doc.recompute()
        b = flatOf(wall)
        self.assertFalse(a.isSame(b))
        self.checkFlat(b, 19 + BA + 9, 12)

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
        self.assertRaises(Exception, flatOf, pad)
        sheet = self.Body.getSolidBody(pad)
        if sheet is not None:
            for name in VALUES + ("FlatRoot", "Grain", "TangentLines"):
                self.assertIn("Hidden", sheet.getEditorMode(name), name)
