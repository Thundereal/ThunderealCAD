# SPDX-License-Identifier: LGPL-2.1-or-later

# The flat of what is cut in a bend other than square to the sheet
# (docs/SheetMetal.md sec 10 risk 2, built 2026-11-11): a hole drilled
# straight through a bend, a countersink in one, a pocket or a hole across
# the line where a bend meets its wall. A face across a bend's line is laid
# out in the parts the line cuts it in, each by its own wall or bend; a face
# in a bend that does not unroll to a plane is laid out as a surface through
# its outline unrolled and points of it, within the flat's tolerance, and the
# check of the sheet lists it.
#
# A plate 40 x 20, 1 thick, a flange on its left edge, x 0, bent at a radius
# of 5: the bend's axis through (6, y, 6), its radii 5 and 6, so its line on
# the plate is x 6 and its outer skin's middle is 6 - 6 cos 45 from x 0 and
# z 0; its inner skin is 7.85 round, room for a hole 1.6 across and its
# countersink 3 across. At the rule's radius of 1 a hole that size through
# the bend's middle runs across both of the bend's lines.

import math
import unittest

import FreeCAD
import Part
import PartDesign

from . import SheetMetalNames
from .SheetMetalFlatView import Flat, flatObject
from .TestSheetMetalContourFlange import edgeOf

V = FreeCAD.Vector
C = math.cos(math.radians(45))
CIRCLE = [Part.Circle(V(0, 0, 0), V(0, 0, 1), 0.8)]
COUNTERSINK = dict(HoleCutType="Countersink", HoleCutDiameter=3, HoleCutCountersinkAngle=90)


class TestSheetMetalFlatInBend(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalFlatInBend")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()
        self.Flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        self.Flange.Edges = (self.Plate, [edgeOf(self.Plate, (0, 0, 1), (0, 20, 1))])
        self.Flange.UseRuleRadius = False
        self.Flange.BendRadius = 5
        self.Flange.Length = 20
        self.Doc.recompute()
        self.assertNotIn("Invalid", self.Flange.State)
        self.Volume = self.Flange.Shape.Volume

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    def sketch(self, at, normal, edges):
        """A sketch at a point, looking along normal, its X along Y"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "Cut")
        sketch.MapMode = "Deactivated"
        x = V(0, 1, 0)
        sketch.Placement = FreeCAD.Placement(at, FreeCAD.Rotation(x, normal.cross(x), normal, "ZXY"))
        for edge in edges:
            sketch.addGeometry(edge)
        self.Doc.recompute()
        return sketch

    def hole(self, sketch, **props):
        hole = self.Body.newObject("PartDesign::Hole", "Hole")
        hole.Profile = sketch
        hole.Diameter = 1.6
        hole.DepthType = "ThroughAll"
        hole.Threaded = False
        for k, v in props.items():
            setattr(hole, k, v)
        self.Doc.recompute()
        self.assertNotIn("Invalid", hole.State, hole.getStatusString())
        return hole

    def inBend(self, radius=5):
        """On the bend's outer skin at its middle, looking out of it"""
        at = (radius + 1) * (1 - C)
        return self.sketch(V(at, 10, at), V(-C, 0, -C), CIRCLE)

    def acrossLine(self):
        """On the plate's bottom skin, 0.4 from the bend's line"""
        return self.sketch(V(6.4, 10, 0), V(0, 0, -1), CIRCLE)

    def approximated(self, feature):
        return [p for p in PartDesign.checkSheet(feature)["Problems"] if p["Kind"] == "Approximated"]

    def checkFlat(self, feature, least, most, within):
        """feature cut between least and most from the flange, and lies flat
        as one valid solid, its volume within that of the folded part's"""
        cut = self.Volume - feature.Shape.Volume
        self.assertGreater(cut, least)
        self.assertLess(cut, most)
        sheet = flatObject(feature)
        sheet.Visibility = True
        self.Doc.recompute()
        self.assertEqual(sheet.Status, "")
        flat = Flat(feature)
        self.assertNotIn("Invalid", flat.State)
        solids = flat.Shape.SubShapes[0].Solids
        self.assertEqual(len(solids), 1)
        self.assertTrue(solids[0].isValid())
        # the bend's line, in two where the cut is across it
        self.assertIn(flat.BendCount, (1, 2))
        # the cut is in the blank: its volume the folded part's
        self.assertLess(abs(solids[0].Volume - feature.Shape.Volume), within)
        return flat

    def testPlain(self):
        """The flange as it is: nothing approximated"""
        self.assertEqual(self.approximated(self.Flange), [])

    def testHoleInBend(self):
        """A hole drilled straight through the bend's middle"""
        hole = self.hole(self.inBend())
        self.checkFlat(hole, 1.9, 2.3, 0.02)
        listed = self.approximated(hole)
        self.assertEqual(len(listed), 1)
        self.assertGreaterEqual(len(listed[0]["Elements"]), 1)

    def testHoleInBendIsToldAsAcrossItsLine(self):
        """A hole through the bend's middle touches the bend's line: Check
        Sheet says it lies across the line, not a distance of 1e-11 from it"""
        hole = self.hole(self.inBend())
        near = [p for p in PartDesign.checkSheet(hole)["Problems"] if p["Kind"] == "HoleNearBend"]
        self.assertEqual(len(near), 1)
        self.assertEqual(near[0]["Value"], 0)
        self.assertIn("across Bend1's line", near[0]["Message"])
        self.assertNotIn("e-", near[0]["Message"])

    def testCountersinkInBend(self):
        # the cone is where the outer fibres are, laid out 5.5 / 6 as long
        # round the bend: the blank's cut is the smaller by 2 %
        hole = self.hole(self.inBend(), **COUNTERSINK)
        self.checkFlat(hole, 2.8, 3.4, 0.1)
        self.assertEqual(len(self.approximated(hole)), 1)

    def testHoleAcrossLine(self):
        """A hole through the plate, across where the bend starts"""
        hole = self.hole(self.acrossLine())
        self.checkFlat(hole, 1.9, 2.3, 0.02)
        self.assertEqual(len(self.approximated(hole)), 1)

    def testCountersinkAcrossLine(self):
        hole = self.hole(self.acrossLine(), **COUNTERSINK)
        self.checkFlat(hole, 3.0, 3.9, 0.05)
        self.assertEqual(len(self.approximated(hole)), 1)

    def testHoleThroughTightBend(self):
        """The rule's radius, 1: the hole through the bend's middle is wider
        than the bend's inner skin, across both its lines"""
        self.Flange.UseRuleRadius = True
        self.Doc.recompute()
        self.Volume = self.Flange.Shape.Volume
        hole = self.hole(self.inBend(1))
        # round a bend of radius 1 the fibres are laid out from 1.5 times to
        # 0.75 times as long: the blank's cut differs by some 5 %
        self.checkFlat(hole, 1.9, 2.3, 0.15)
        self.assertEqual(len(self.approximated(hole)), 1)

    def pocket(self, taper):
        sketch = self.sketch(V(0, 0, 9), V(0, 0, 1), [Part.Circle(V(10, -7, 0), V(0, 0, 1), 2)])
        pocket = self.Body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = sketch
        pocket.Type = "ThroughAll"
        pocket.TaperAngle = taper
        self.Doc.recompute()
        self.assertNotIn("Invalid", pocket.State, pocket.getStatusString())
        return pocket

    def testPocketAcrossLine(self):
        """A round pocket straight down through the plate and the bend's
        start"""
        pocket = self.pocket(0)
        self.checkFlat(pocket, 12, 13.5, 0.05)

    def testTaperedPocketAcrossLine(self):
        pocket = self.pocket(20)
        self.checkFlat(pocket, 70, 90, 0.5)

    def testInWall(self):
        """A countersink clear of the bend lies flat as it is: exact, and
        not listed"""
        hole = self.hole(self.sketch(V(20, 10, 0), V(0, 0, -1), CIRCLE), **COUNTERSINK)
        flat = self.checkFlat(hole, 3.5, 3.7, 1e-6)
        self.assertEqual(self.approximated(hole), [])
        self.assertAlmostEqual(
            flat.Shape.SubShapes[0].Solids[0].Volume, hole.Shape.Volume, places=6
        )


if __name__ == "__main__":
    unittest.main()
