# SPDX-License-Identifier: LGPL-2.1-or-later

# The Rip of its own (docs/SheetMetal.md sec 12, ruled 2026-11-05): a closed
# base wall, a tube, does not lie flat until it is ripped open. A Rip cuts a
# bend of it open -- the bend gone, its two walls run on to the corner,
# mitred on the plane halving it with the rule's gap between -- or a wall
# down its middle, along its bends, the rule's gap wide.
#
# The tube: a square sketch 20 a side, the material outside it (the sketch
# on the bends' inner skins), 30 long, the rule's 1 thick, radius 1 and K
# 0.5. Each wall is 18 straight, each bend a quarter of a ring 1 to 2 across.
# Ripped at a bend, each wall runs on to the corner: its inner skin 1 and its
# outer 2 past where the bend started, less (gap / 2) x sqrt 2 off each for
# the gap across the mitre.

import math
import unittest

import FreeCAD
import Part
import PartDesign

from . import SheetMetalNames

V = FreeCAD.Vector
L = 30
WALL = 18 * 1 * L
BEND = math.pi / 4 * (2 * 2 - 1 * 1) * L
TUBE = 4 * WALL + 4 * BEND


class TestSheetMetalRip(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalRip")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    def square(self):
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (20, 0), (20, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        return sketch

    def tube(self):
        wall = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Tube")
        wall.Profile = self.square()
        wall.ClosedProfile = "Tube"
        wall.Length = L
        self.Doc.recompute()
        self.assertNotIn("Invalid", wall.State, wall.getStatusString())
        return wall

    def facesOf(self, feature, element):
        return [
            "Face%d" % (i + 1)
            for i in range(len(feature.Shape.Faces))
            if feature.sheetElementOf("Face%d" % (i + 1)) == element
        ]

    def rip(self, base, element, **props):
        rip = self.Body.newObject("PartDesign::SheetMetalRip", "Rip")
        rip.Faces = (base, self.facesOf(base, element)[:1])
        for k, v in props.items():
            setattr(rip, k, v)
        self.Doc.recompute()
        return rip

    def kinds(self, feature):
        model = feature.SheetModel["Elements"]
        return sorted(e["Name"] for e in model if e["Kind"] == "Wall"), sorted(
            e["Name"] for e in model if e["Kind"] == "Bend"
        )

    def noFlat(self, feature):
        return [p for p in PartDesign.checkSheet(feature)["Problems"] if p["Kind"] == "NoFlat"]

    def checkFlat(self, feature):
        """It lies flat, every bit of it: at K 0.5 the flat holds as much as
        the folded part"""
        self.assertEqual(self.noFlat(feature), [])
        flat = feature.getSubObject("Flat")
        self.assertIsNotNone(flat)
        self.assertAlmostEqual(flat.Volume, feature.Shape.Volume, places=6)
        self.assertLess(flat.BoundBox.ZLength, 1 + 1e-6)

    def testPlateStays(self):
        """A closed sketch is a plate as it was, unless asked for a tube"""
        wall = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        wall.Profile = self.square()
        self.Doc.recompute()
        self.assertEqual(wall.ClosedProfile, "Plate")
        self.assertAlmostEqual(wall.Shape.Volume, 400, places=6)

    def testTube(self):
        """Four walls and four bends all round; closed, it does not lie flat"""
        wall = self.tube()
        self.assertTrue(wall.Shape.isValid())
        self.assertAlmostEqual(wall.Shape.Volume, TUBE, places=6)
        walls, bends = self.kinds(wall)
        self.assertEqual(len(walls), 4)
        self.assertEqual(len(bends), 4)
        problems = self.noFlat(wall)
        self.assertEqual(len(problems), 1)
        self.assertIn("closed", problems[0]["Message"])
        self.assertIn("Rip", problems[0]["Message"])

    def testTubeMaterialInside(self):
        wall = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Tube")
        wall.Profile = self.square()
        wall.ClosedProfile = "Tube"
        wall.MaterialSide = 1
        wall.Length = L
        self.Doc.recompute()
        self.assertNotIn("Invalid", wall.State, wall.getStatusString())
        # the sketch on the outer skins: each wall 20 - 2 x 2 straight
        self.assertAlmostEqual(wall.Shape.Volume, 4 * 16 * L + 4 * BEND, places=6)
        self.assertLess(wall.Shape.BoundBox.XLength, 20 + 1e-6)

    def testRipBend(self):
        tube = self.tube()
        rip = self.rip(tube, "Bend1")
        self.assertNotIn("Invalid", rip.State, rip.getStatusString())
        self.assertTrue(rip.Shape.isValid())
        walls, bends = self.kinds(rip)
        self.assertEqual(len(walls), 4)
        self.assertEqual(bends, sorted(set(self.kinds(tube)[1]) - {"Bend1"}))
        gap = rip.Rule.Gap.Value
        leg = 1 * (1.5 - gap / 2 * math.sqrt(2)) * L
        self.assertAlmostEqual(rip.Shape.Volume, TUBE - BEND + 2 * leg, places=6)
        self.checkFlat(rip)
        # every face of it is a wall's or a bend's
        for i in range(len(rip.Shape.Faces)):
            self.assertIsNotNone(rip.sheetElementOf("Face%d" % (i + 1)), i + 1)

    def testRipWall(self):
        tube = self.tube()
        walls = self.kinds(tube)[0]
        rip = self.rip(tube, walls[0])
        self.assertNotIn("Invalid", rip.State, rip.getStatusString())
        self.assertTrue(rip.Shape.isValid())
        after, bends = self.kinds(rip)
        self.assertEqual(len(after), 5)
        self.assertEqual(len(bends), 4)
        gap = rip.Rule.Gap.Value
        self.assertAlmostEqual(rip.Shape.Volume, TUBE - gap * 1 * L, places=6)
        self.assertEqual(len(rip.Shape.Solids), 1)
        self.checkFlat(rip)

    def testGapFollowsRule(self):
        tube = self.tube()
        rip = self.rip(tube, self.kinds(tube)[0][0])
        tube.Rule.Gap = 0.5
        self.Doc.recompute()
        self.assertAlmostEqual(rip.Shape.Volume, TUBE - 0.5 * L, places=6)

    def testTwoRipsCutOff(self):
        """A second rip of the same loop cuts a piece off: refused"""
        tube = self.tube()
        rip = self.rip(tube, "Bend1")
        self.assertNotIn("Invalid", rip.State)
        second = self.rip(rip, "Bend3")
        self.assertIn("Invalid", second.State)
        self.assertIn("off", second.getStatusString())

    def testOpenSheetRefused(self):
        """A rip on a sheet with no loop cuts it in two: refused"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "Open")
        sketch.addGeometry(Part.LineSegment(V(0, 0, 0), V(20, 0, 0)))
        sketch.addGeometry(Part.LineSegment(V(20, 0, 0), V(20, 20, 0)))
        wall = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Profile")
        wall.Profile = sketch
        wall.Length = L
        self.Doc.recompute()
        rip = self.rip(wall, "Bend1")
        self.assertIn("Invalid", rip.State)
        self.assertIn("off", rip.getStatusString())

    def testNotSheetFace(self):
        tube = self.tube()
        rip = self.Body.newObject("PartDesign::SheetMetalRip", "Rip")
        rip.Faces = (tube, [])
        self.Doc.recompute()
        self.assertIn("Invalid", rip.State)
