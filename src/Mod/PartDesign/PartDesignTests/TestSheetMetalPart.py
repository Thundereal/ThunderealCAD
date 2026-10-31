# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal end to end (docs/SheetMetal.md sec 9, M1): a part of base
# wall, flanges and ordinary PartDesign features, its flat, and references
# that survive an edit upstream.

import unittest

import FreeCAD
import Part

V = FreeCAD.Vector


class TestSheetMetalPart(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalPart")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def plate(self, thickness=1.0):
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        plate.Profile = sketch
        self.Doc.recompute()
        plate.Rule.Thickness = thickness
        self.Doc.recompute()
        return plate

    def edge(self, feature, a, b):
        for i, e in enumerate(feature.Shape.Edges):
            ends = [v.Point for v in e.Vertexes]
            if len(ends) == 2 and (
                (ends[0].isEqual(V(*a), 1e-7) and ends[1].isEqual(V(*b), 1e-7))
                or (ends[0].isEqual(V(*b), 1e-7) and ends[1].isEqual(V(*a), 1e-7))
            ):
                return "Edge%d" % (i + 1)
        self.fail("no edge from %s to %s" % (a, b))

    def flange(self, base, a, b, name="Flange", **props):
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", name)
        flange.Edges = (base, [self.edge(base, a, b)])
        for k, v in props.items():
            setattr(flange, k, v)
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        return flange

    def testCountersunkHoleInTheFlat(self):
        """A countersunk hole in a wall, a flange after it: the hole, cone
        and all, is in the flat"""
        plate = self.plate(thickness=3)
        sketch = self.Body.newObject("Sketcher::SketchObject", "HoleSketch")
        sketch.Placement = FreeCAD.Placement(V(0, 0, 3), FreeCAD.Rotation())
        sketch.addGeometry(Part.Circle(V(10, 10, 0), V(0, 0, 1), 2))
        self.Doc.recompute()
        hole = self.Body.newObject("PartDesign::Hole", "Hole")
        hole.Profile = sketch
        hole.Diameter = 4
        hole.ThreadType = 0
        hole.HoleCutType = 2
        hole.HoleCutDiameter = 7
        hole.HoleCutCountersinkAngle = 90
        hole.DepthType = 1
        hole.DrillPoint = 0
        self.Doc.recompute()
        self.assertNotIn("Invalid", hole.State)
        self.assertLess(hole.Shape.Volume, plate.Shape.Volume - 30)
        flange = self.flange(hole, (0, 20, 3), (40, 20, 3))
        self.assertEqual(flange.Rule, plate.Rule)
        cones = [f for f in flange.Shape.Faces if f.Surface.TypeId == "Part::GeomCone"]
        self.assertEqual(len(cones), 1)

        flat = self.Body.newObject("PartDesign::SheetMetalFlatPattern", "FlatPattern")
        flat.Source = flange
        self.Doc.recompute()
        self.assertNotIn("Invalid", flat.State)
        solid = flat.Shape.Solids[0]
        self.assertTrue(solid.isValid())
        # K 0.5 keeps the volume, the hole's included
        self.assertAlmostEqual(solid.Volume, flange.Shape.Volume, places=6)
        cones = [f for f in solid.Faces if f.Surface.TypeId == "Part::GeomCone"]
        self.assertEqual(len(cones), 1)
        # the countersink opens on the flat's top or bottom, as it did on the
        # wall's skin
        self.assertAlmostEqual(cones[0].BoundBox.ZLength, 1.5, places=6)

    def testFlangeEndSurvivesEdit(self):
        """A flange on another's end keeps to that end when the other is
        made longer"""
        plate = self.plate()
        first = self.flange(plate, (0, 20, 1), (40, 20, 1), LengthType="Leg")
        second = self.flange(first, (0, 19, 12), (40, 19, 12), name="Flange2")
        # its bend flush with the first leg's end, cut back R + T
        self.assertAlmostEqual(second.Shape.BoundBox.ZMax, 12, places=6)
        first.Length = 15
        self.Doc.recompute()
        self.assertNotIn("Invalid", second.State)
        self.assertTrue(second.Shape.isValid())
        self.assertAlmostEqual(second.Shape.BoundBox.ZMax, 17, places=6)
        model = second.SheetModel["Elements"]
        bend = [e for e in model if e["Name"] == "Bend2"][0]
        self.assertEqual((bend["Before"], bend["After"]), ("Wall2", "Wall3"))
        flat = self.Body.newObject("PartDesign::SheetMetalFlatPattern", "FlatPattern")
        flat.Source = second
        self.Doc.recompute()
        self.assertNotIn("Invalid", flat.State)
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, second.Shape.Volume, places=6)
