# SPDX-License-Identifier: LGPL-2.1-or-later

# A bend's own K (docs/SheetMetal.md sec 4.6, ruled 2026-11-05): kept on the
# feature that made the bend, in BendKFactors, keyed by the feature's own
# bends counted in the order it makes them -- so the key holds while bends are
# added earlier in the sheet and the bend's name moves on. Collected and
# edited in Check Sheet's bend table, where its source reads "bend". It
# changes that bend's flat and nothing else.
#
# On a plate 40 x 20, 1 thick, a flange 10 high (outer sharp) on its far edge
# and another on its near edge: each bend 2.356 (pi / 2 * 1.5, the radius 1
# and K 0.5) wide flat. A K of 0.3 makes one pi / 2 * 1.3.

import math
import unittest

import FreeCAD
import Part
import PartDesign

from . import SheetMetalNames
from .SheetMetalFlatView import flatObject

V = FreeCAD.Vector
FAR = ((0, 20, 1), (40, 20, 1))
NEAR = ((0, 0, 1), (40, 0, 1))
LEFT = ((0, 0, 1), (0, 20, 1))


class TestSheetMetalBendOverride(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalBendOverride")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    def edge(self, feature, a, b):
        for i, e in enumerate(feature.Shape.Edges):
            ends = [v.Point for v in e.Vertexes]
            if len(ends) == 2 and (
                (ends[0].isEqual(V(*a), 1e-7) and ends[1].isEqual(V(*b), 1e-7))
                or (ends[0].isEqual(V(*b), 1e-7) and ends[1].isEqual(V(*a), 1e-7))
            ):
                return "Edge%d" % (i + 1)
        self.fail("no edge from %s to %s" % (a, b))

    def flange(self, edges, base=None, **props):
        base = base or self.Plate
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (base, [self.edge(base, a, b) for a, b in edges])
        for k, v in props.items():
            setattr(flange, k, v)
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        return flange

    def bends(self, obj):
        return {b["Name"]: b for b in PartDesign.checkSheet(obj)["Bends"]}

    def flatBox(self, feature):
        return feature.getSubObject("Flat").BoundBox

    def twoFlanges(self):
        first = self.flange([FAR])
        second = self.flange([NEAR], base=first)
        return first, second

    def testOneBendsFlat(self):
        """K 0.3 on the near flange's bend: its allowance and the flat's
        extent that way shrink by pi / 2 * 0.2, the far side and the folded
        part stay as they were"""
        first, second = self.twoFlanges()
        volume = second.Shape.Volume
        box = self.flatBox(second)
        table = self.bends(second)
        self.assertEqual(sorted(table), ["Bend1", "Bend2"])
        self.assertEqual(table["Bend2"]["Feature"], second.Name)

        second.setBendKFactor("Bend2", 0.3)
        self.Doc.recompute()
        self.assertNotIn("Invalid", second.State)
        self.assertEqual(second.BendKFactors, {"1": "0.3"})
        self.assertAlmostEqual(second.bendKFactor("Bend2"), 0.3, places=12)
        self.assertIsNone(second.bendKFactor("Bend1"))

        table = self.bends(second)
        self.assertAlmostEqual(table["Bend2"]["KFactor"], 0.3, places=9)
        self.assertEqual(table["Bend2"]["KFactorSource"], "bend")
        self.assertAlmostEqual(table["Bend2"]["Allowance"], math.pi / 2 * 1.3, places=9)
        self.assertAlmostEqual(table["Bend1"]["KFactor"], 0.5, places=9)
        self.assertEqual(table["Bend1"]["KFactorSource"], "rule")

        self.assertAlmostEqual(second.Shape.Volume, volume, places=9)
        after = self.flatBox(second)
        shrink = math.pi / 2 * 0.2
        self.assertAlmostEqual(after.XLength, box.XLength, places=6)
        self.assertAlmostEqual(after.YLength, box.YLength - shrink, places=6)
        # one side moved, the other kept
        self.assertTrue(
            abs(after.YMin - box.YMin) < 1e-6 or abs(after.YMax - box.YMax) < 1e-6,
            (box, after),
        )

        # the sheet's flat object lays it so too
        flat = flatObject(second)
        flat.Visibility = True
        self.Doc.recompute()
        self.assertAlmostEqual(flat.Shape.BoundBox.YLength, box.YLength - shrink, places=6)

    def testCarriedOn(self):
        """A feature after the one that made the bend keeps its K"""
        first, second = self.twoFlanges()
        first.setBendKFactor("Bend1", 0.25)
        self.Doc.recompute()
        table = self.bends(second)
        self.assertAlmostEqual(table["Bend1"]["KFactor"], 0.25, places=9)
        self.assertEqual(table["Bend1"]["KFactorSource"], "bend")
        self.assertEqual(table["Bend1"]["Feature"], first.Name)

    def testKeyHoldsWhenEarlierBendsAreAdded(self):
        """The near flange's bend is renamed Bend3 when the far flange takes
        the left edge too (gaps at its ends, the near edge left whole); its K
        goes with it"""
        first, second = self.twoFlanges()
        second.setBendKFactor("Bend2", 0.3)
        self.Doc.recompute()
        first.Edges = (self.Plate, [self.edge(self.Plate, a, b) for a, b in (FAR, LEFT)])
        first.Gap1 = 2
        first.Gap2 = 2
        self.Doc.recompute()
        self.assertNotIn("Invalid", first.State)
        self.assertNotIn("Invalid", second.State)
        table = self.bends(second)
        self.assertEqual(len(table), 3)
        mine = [b for b in table.values() if b["Feature"] == second.Name]
        self.assertEqual(len(mine), 1)
        self.assertEqual(mine[0]["Name"], "Bend3")
        self.assertAlmostEqual(mine[0]["KFactor"], 0.3, places=9)
        self.assertEqual(mine[0]["KFactorSource"], "bend")
        for b in table.values():
            if b["Feature"] == first.Name:
                self.assertEqual(b["KFactorSource"], "rule")

    def testCleared(self):
        first, second = self.twoFlanges()
        box = self.flatBox(second)
        second.setBendKFactor("Bend2", 0.3)
        self.Doc.recompute()
        second.setBendKFactor("Bend2", None)
        self.Doc.recompute()
        self.assertEqual(second.BendKFactors, {})
        table = self.bends(second)
        self.assertEqual(table["Bend2"]["KFactorSource"], "rule")
        self.assertAlmostEqual(self.flatBox(second).YLength, box.YLength, places=9)

    def testUndo(self):
        first, second = self.twoFlanges()
        self.Doc.UndoMode = 1
        self.Doc.openTransaction("K")
        second.setBendKFactor("Bend2", 0.3)
        self.Doc.recompute()
        self.Doc.commitTransaction()
        self.Doc.undo()
        self.Doc.recompute()
        self.assertEqual(second.BendKFactors, {})
        self.assertEqual(self.bends(second)["Bend2"]["KFactorSource"], "rule")

    def testRefused(self):
        """A K outside 0..1, a bend another feature made, a bend there is none
        of"""
        first, second = self.twoFlanges()
        with self.assertRaises(ValueError):
            second.setBendKFactor("Bend2", 1.5)
        with self.assertRaises(ValueError):
            second.setBendKFactor("Bend2", -0.1)
        with self.assertRaises(ValueError):
            second.setBendKFactor("Bend1", 0.3)
        with self.assertRaises(ValueError):
            second.setBendKFactor("Bend9", 0.3)
        self.assertEqual(second.BendKFactors, {})

    def testBadValueSetByHand(self):
        """The map set by hand: a value that is no K fails the recompute; a
        key past the feature's bends is kept and changes nothing"""
        first, second = self.twoFlanges()
        second.BendKFactors = {"1": "soft"}
        self.Doc.recompute()
        self.assertIn("Invalid", second.State)
        second.BendKFactors = {"1": "2"}
        self.Doc.recompute()
        self.assertIn("Invalid", second.State)
        second.BendKFactors = {"5": "0.3"}
        self.Doc.recompute()
        self.assertNotIn("Invalid", second.State)
        self.assertEqual(self.bends(second)["Bend2"]["KFactorSource"], "rule")

    def testTableMethod(self):
        """With a K the rule's method finds, the bend's own wins all the same"""
        first, second = self.twoFlanges()
        rule = second.Rule
        rule.AllowanceMethod = "KFactorDIN"
        self.Doc.recompute()
        self.assertEqual(self.bends(second)["Bend2"]["KFactorSource"], "KFactorDIN")
        second.setBendKFactor("Bend2", 0.3)
        self.Doc.recompute()
        bend = self.bends(second)["Bend2"]
        self.assertAlmostEqual(bend["KFactor"], 0.3, places=9)
        self.assertEqual(bend["KFactorSource"], "bend")

    def testEveryMaker(self):
        """The base wall's own bends, from an open profile, take theirs too"""
        body = self.Doc.addObject("PartDesign::Body", "Body2")
        sketch = body.newObject("Sketcher::SketchObject", "Open")
        sketch.addGeometry(Part.LineSegment(V(0, 0, 0), V(20, 0, 0)))
        sketch.addGeometry(Part.LineSegment(V(20, 0, 0), V(20, 20, 0)))
        wall = body.newObject("PartDesign::SheetMetalBaseWall", "Profile")
        wall.Profile = sketch
        wall.Length = 10
        self.Doc.recompute()
        self.assertNotIn("Invalid", wall.State)
        table = self.bends(wall)
        self.assertEqual(len(table), 1)
        name = list(table)[0]
        wall.setBendKFactor(name, 0.4)
        self.Doc.recompute()
        bend = self.bends(wall)[name]
        self.assertAlmostEqual(bend["KFactor"], 0.4, places=9)
        self.assertEqual(bend["KFactorSource"], "bend")
