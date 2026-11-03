# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's checks (docs/SheetMetal.md sec 7, ruled 2026-11-03: run on
# demand, as Flatten is, never in a recompute): PartDesign.checkSheet(obj)
# reads a feature of a sheet's solid, or the solid (its tip), and gives its
# bend table, each value with where it comes from, and what is wrong with
# its flat: material overlapping itself, a relief smaller than the rule's, a
# flange shorter than the rule's minimum, a hole too close to a bend line.
#
# On a plate 40 x 20, 1 thick, a flange 10 high (outer sharp) on its far
# edge, its material inside: the plate is cut back to 18, its bend 2.356
# (pi / 2 * 1.5, the radius 1 and K 0.5) wide flat, the flange's wall 8
# long flat past it. The bend line is the bend's middle, 1.178 past the
# plate's end.

import math
import unittest

import FreeCAD
import Part
import PartDesign

V = FreeCAD.Vector
BA = math.pi / 2 * 1.5


class TestSheetMetalCheck(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalCheck")
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
        FreeCAD.closeDocument(self.Doc.Name)

    def edge(self, feature, a, b):
        for i, e in enumerate(feature.Shape.Edges):
            ends = [v.Point for v in e.Vertexes]
            if len(ends) == 2 and (
                (ends[0].isEqual(V(*a), 1e-7) and ends[1].isEqual(V(*b), 1e-7))
                or (ends[0].isEqual(V(*b), 1e-7) and ends[1].isEqual(V(*a), 1e-7))
            ):
                return "Edge%d" % (i + 1)
        self.fail("no edge from %s to %s" % (a, b))

    def flange(self, edges=(((0, 20, 1), (40, 20, 1)),), base=None, **props):
        base = base or self.Plate
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (base, [self.edge(base, a, b) for a, b in edges])
        for k, v in props.items():
            setattr(flange, k, v)
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        return flange

    def hole(self, x, y, r=2):
        """A hole through the plate, before the flange"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "HoleSketch")
        sketch.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        sketch.addGeometry(Part.Circle(V(x, y, 0), V(0, 0, 1), r))
        pocket = self.Body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = sketch
        pocket.Type = "ThroughAll"
        self.Doc.recompute()
        self.assertNotIn("Invalid", pocket.State)
        return pocket

    def problems(self, obj, kind=None):
        return [p for p in PartDesign.checkSheet(obj)["Problems"] if not kind or p["Kind"] == kind]

    def checkElements(self, feature, found, owner=None):
        """Folded faces of the feature, each of the element given, and as
        many faces of its flat"""
        self.assertTrue(found["Elements"])
        for name in found["Elements"]:
            self.assertTrue(name.startswith("Face"), name)
            if owner:
                self.assertEqual(feature.sheetElementOf(name), owner, name)
        self.assertEqual(len(found["FlatElements"]), len(found["Elements"]))
        flat = feature.getSubObject("Flat")
        for name in found["FlatElements"]:
            self.assertTrue(name.startswith("FlatFace"), name)
            self.assertIsNotNone(feature.getSubObject(name), name)
        self.assertIsNotNone(flat)

    def testBendTable(self):
        """The flange's bend: its angle, the rule's radius and K, the
        allowance they make, which way it bends flat, its faces folded and
        flat; each value from the rule, set as it starts"""
        flange = self.flange()
        found = PartDesign.checkSheet(flange)
        self.assertEqual(len(found["Bends"]), 1)
        bend = found["Bends"][0]
        self.assertEqual(bend["Feature"], flange.Name)
        self.assertAlmostEqual(bend["Angle"], 90, places=6)
        self.assertAlmostEqual(bend["Radius"], 1, places=6)
        self.assertEqual(bend["RadiusSource"], "rule")
        self.assertAlmostEqual(bend["KFactor"], 0.5, places=6)
        self.assertEqual(bend["KFactorSource"], "rule")
        self.assertEqual(bend["Method"], "KFactor")
        self.assertEqual(bend["MethodSource"], "rule")
        self.assertAlmostEqual(bend["Allowance"], BA, places=6)
        self.assertIn(bend["Direction"], ("Up", "Down"))
        self.checkElements(flange, bend, bend["Name"])
        self.assertEqual(found["Problems"], [])

    def testSources(self):
        """From a card's gauge row, the card's; set on the rule by hand, the
        rule's; the flange's own radius, the feature's"""
        import Materials

        card = Materials.Material()
        card.Name = "Gauged"
        card.addPhysicalModel(Materials.UUIDs().SheetMetal)
        gauges = Materials.Array2D()
        gauges.Columns = 4
        gauges.Rows = 1
        for c, v in enumerate(("1 mm", "1.5 mm", "0.42", "6 mm")):
            gauges.setValue(0, c, v)
        card.setValue("Gauges", gauges)
        self.Plate.Rule.ShapeMaterial = card
        flange = self.flange()
        bend = PartDesign.checkSheet(flange)["Bends"][0]
        self.assertAlmostEqual(bend["Radius"], 1.5, places=6)
        self.assertEqual(bend["RadiusSource"], "card")
        self.assertAlmostEqual(bend["KFactor"], 0.42, places=6)
        self.assertEqual(bend["KFactorSource"], "card")
        self.assertEqual(bend["MethodSource"], "card")
        self.assertAlmostEqual(bend["Allowance"], math.pi / 2 * (1.5 + 0.42), places=6)

        self.Plate.Rule.KFactor = 0.4
        self.Doc.recompute()
        bend = PartDesign.checkSheet(flange)["Bends"][0]
        self.assertAlmostEqual(bend["KFactor"], 0.4, places=6)
        self.assertEqual(bend["KFactorSource"], "rule")
        self.assertEqual(bend["RadiusSource"], "card")

        flange.UseRuleRadius = False
        flange.BendRadius = 3
        self.Doc.recompute()
        bend = PartDesign.checkSheet(flange)["Bends"][0]
        self.assertAlmostEqual(bend["Radius"], 3, places=6)
        self.assertEqual(bend["RadiusSource"], "feature")

    def testSolid(self):
        """The solid is checked as its tip"""
        flange = self.flange()
        solid = self.Body.getSolidBody(flange)
        self.assertEqual(PartDesign.checkSheet(solid), PartDesign.checkSheet(flange))

    def testShortFlange(self):
        """The flange's wall lies 8 long flat past its bend: shorter than the
        rule's minimum 9, it is listed with its faces; 20 lists the plate's 18
        too; 0, the rule's own, checks nothing"""
        flange = self.flange()
        rule = flange.Rule
        self.assertAlmostEqual(rule.MinFlange.Value, 0)
        self.assertEqual(self.problems(flange), [])
        rule.MinFlange = 9
        self.Doc.recompute()
        found = self.problems(flange)
        self.assertEqual(len(found), 1, found)
        short = found[0]
        self.assertEqual(short["Kind"], "ShortFlange")
        self.assertEqual(short["Feature"], flange.Name)
        self.assertAlmostEqual(short["Value"], 8, places=6)
        self.assertAlmostEqual(short["Limit"], 9, places=6)
        walls = {flange.sheetElementOf(name) for name in short["Elements"]}
        self.assertEqual(len(walls), 1)
        self.assertNotEqual(walls, {flange.sheetElementOf("Face1")} - walls)
        self.checkElements(flange, short)
        rule.MinFlange = 20
        self.Doc.recompute()
        values = sorted(p["Value"] for p in self.problems(flange, "ShortFlange"))
        self.assertEqual(len(values), 2)
        self.assertAlmostEqual(values[0], 8, places=6)
        self.assertAlmostEqual(values[1], 18, places=6)

    def testHoleNearBend(self):
        """A hole 2 across at 15 on the plate: its edge 1 short of the plate's
        end, 1 + 1.178 from the bend line, nearer than 2.5 x the thickness
        plus the radius, the rule's MinHoleToBend 0 asking for that; listed
        with its faces. MinHoleToBend 2 lets it be"""
        pocket = self.hole(20, 15)
        flange = self.flange(base=pocket)
        rule = flange.Rule
        self.assertAlmostEqual(rule.MinHoleToBend.Value, 0)
        found = self.problems(flange)
        self.assertEqual(len(found), 1, found)
        near = found[0]
        self.assertEqual(near["Kind"], "HoleNearBend")
        self.assertAlmostEqual(near["Value"], 1 + BA / 2, places=6)
        self.assertAlmostEqual(near["Limit"], 2.5 * 1 + 1, places=6)
        bend = PartDesign.checkSheet(flange)["Bends"][0]["Name"]
        self.assertIn(bend, near["Message"])
        self.checkElements(flange, near)
        faces = [flange.Shape.getElement(name) for name in near["Elements"]]
        self.assertTrue(
            any(isinstance(f.Surface, Part.Cylinder) and abs(f.Surface.Radius - 2) < 1e-7
                for f in faces),
            near["Elements"],
        )
        rule.MinHoleToBend = 2
        self.Doc.recompute()
        self.assertEqual(self.problems(flange), [])

    def testHoleClear(self):
        """A hole at 8 on the plate, 7.178 from the bend line: nothing"""
        flange = self.flange(base=self.hole(20, 8))
        self.assertEqual(self.problems(flange), [])

    def testOverlap(self):
        """Flanges on two edges of a corner, mitered, with no corner relief:
        their bends' flats overlap a little at the corner (0.35 mm^2), listed
        with the faces of each. The rule's circle, covering the bends'
        crossing (CornerReliefCover 1), cuts it away; 2 across alone (cover 0)
        leaves 0.0175 mm^2 of it, the strips being 2.356 wide where they
        cross"""
        flange = self.flange([((0, 20, 1), (40, 20, 1)), ((40, 0, 1), (40, 20, 1))])
        rule = flange.Rule
        self.assertEqual(rule.CornerReliefShape, "Circle")
        self.assertAlmostEqual(rule.CornerReliefCover, 1)
        self.assertEqual(self.problems(flange), [])
        rule.CornerReliefCover = 0
        self.Doc.recompute()
        found = self.problems(flange)
        self.assertEqual([p["Kind"] for p in found], ["Overlap"])
        self.assertAlmostEqual(found[0]["Value"], 0.0175, delta=5e-4)
        rule.CornerReliefShape = "None"
        self.Doc.recompute()
        found = self.problems(flange)
        self.assertEqual(len(found), 1, found)
        overlap = found[0]
        self.assertEqual(overlap["Kind"], "Overlap")
        self.assertGreater(overlap["Value"], 1e-6)
        self.checkElements(flange, overlap)
        owners = {flange.sheetElementOf(name) for name in overlap["Elements"]}
        self.assertEqual(len(owners), 2, owners)
        for owner in owners:
            self.assertIn(owner, overlap["Message"])

    def testRelief(self):
        """A flange's own relief narrower or shallower than the rule's is
        listed by the feature; the rule's, or a larger, is not"""
        flange = self.flange(Gap1=5, Gap2=5)
        self.assertEqual(self.problems(flange), [])
        flange.ReliefShape = "Rectangle"
        flange.ReliefWidth = 0.5
        self.Doc.recompute()
        found = self.problems(flange)
        self.assertEqual(len(found), 1, found)
        relief = found[0]
        self.assertEqual(relief["Kind"], "Relief")
        self.assertEqual(relief["Feature"], flange.Name)
        self.assertAlmostEqual(relief["Value"], 0.5, places=6)
        self.assertAlmostEqual(relief["Limit"], 1, places=6)
        flange.ReliefWidth = 2
        flange.ReliefDepth = 0.25
        self.Doc.recompute()
        found = self.problems(flange)
        self.assertEqual([(p["Kind"], p["Value"]) for p in found], [("Relief", 0.25)])
        flange.ReliefDepth = 2
        self.Doc.recompute()
        self.assertEqual(self.problems(flange), [])

    def testNoFlat(self):
        """A flat that cannot be laid is listed, why with it; the bends still
        are"""
        flange = self.flange()
        self.Body.getSolidBody(flange).FlatRoot = "Wall9"
        self.Doc.recompute()
        found = PartDesign.checkSheet(flange)
        self.assertEqual(len(found["Bends"]), 1)
        self.assertEqual([p["Kind"] for p in found["Problems"]], ["NoFlat"])
        self.assertIn("Wall9", found["Problems"][0]["Message"])

    def testNotSheet(self):
        """Refused for an object of no sheet"""
        box = self.Doc.addObject("Part::Box", "Box")
        self.Doc.recompute()
        with self.assertRaises(ValueError):
            PartDesign.checkSheet(box)

    def testNothingStored(self):
        """Asked for, not kept: the document is not touched"""
        flange = self.flange()
        self.Doc.recompute()
        PartDesign.checkSheet(flange)
        self.assertFalse(any("Touched" in o.State for o in self.Doc.Objects))
