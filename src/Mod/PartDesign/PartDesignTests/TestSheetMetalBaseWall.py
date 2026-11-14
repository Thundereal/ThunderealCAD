# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's base wall, PartDesign::SheetMetalBaseWall (docs/SheetMetal.md
# sec 5.1): a closed sketch makes a plate, an open sketch of lines a profile
# wall with a bend of the rule's radius at every corner. Its sheet model
# (sec 4.1) owns every face of what it makes.

import math
import os
import tempfile
import unittest

import FreeCAD
import Part

from . import SheetMetalNames

V = FreeCAD.Vector


class TestSheetMetalBaseWall(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalBaseWall")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    def sketch(self, points, closed=False, name="Sketch"):
        sketch = self.Body.newObject("Sketcher::SketchObject", name)
        n = len(points) if closed else len(points) - 1
        for i in range(n):
            a, b = points[i], points[(i + 1) % len(points)]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        return sketch

    def wall(self, sketch, name="BaseWall", **props):
        wall = self.Body.newObject("PartDesign::SheetMetalBaseWall", name)
        wall.Profile = sketch
        for k, v in props.items():
            setattr(wall, k, v)
        self.Doc.recompute()
        return wall

    def rules(self):
        return [o for o in self.Body.Group if o.isDerivedFrom("PartDesign::SheetMetalRule")]

    def owners(self, wall):
        """How many faces each element owns; None for a face none owns"""
        count = {}
        for i in range(len(wall.Shape.Faces)):
            name = wall.sheetElementOf("Face%d" % (i + 1))
            count[name] = count.get(name, 0) + 1
        return count

    def elements(self, wall):
        return [e["Name"] for e in wall.SheetModel["Elements"]]

    def testPlate(self):
        """A closed sketch: a plate of the rule's thickness on the sketch's
        side, one wall owning every face; the first sheet feature makes the
        rule, in the body"""
        wall = self.wall(self.sketch([(0, 0), (20, 0), (20, 10), (0, 10)], closed=True))
        self.assertNotIn("Invalid", wall.State)
        rules = self.rules()
        self.assertEqual(len(rules), 1)
        self.assertEqual(wall.Rule, rules[0])
        self.assertAlmostEqual(wall.Shape.Volume, 200)
        box = wall.Shape.BoundBox
        self.assertAlmostEqual(box.ZMin, 0)
        self.assertAlmostEqual(box.ZMax, 1)
        self.assertEqual(self.elements(wall), ["Wall1"])
        self.assertAlmostEqual(wall.SheetModel["Thickness"], 1)
        self.assertEqual(self.owners(wall), {"Wall1": 6})
        self.assertEqual(self.Body.Tip, wall)

    def testPlateSides(self):
        """Inside puts the plate under the sketch, Middle across it; the
        rule's thickness is the plate's"""
        sketch = self.sketch([(0, 0), (20, 0), (20, 10), (0, 10)], closed=True)
        wall = self.wall(sketch, MaterialSide="Inside")
        self.assertAlmostEqual(wall.Shape.BoundBox.ZMin, -1)
        self.assertAlmostEqual(wall.Shape.BoundBox.ZMax, 0)
        wall.MaterialSide = "Middle"
        wall.Rule.Thickness = 2
        self.Doc.recompute()
        self.assertAlmostEqual(wall.Shape.BoundBox.ZMin, -1)
        self.assertAlmostEqual(wall.Shape.BoundBox.ZMax, 1)
        self.assertAlmostEqual(wall.Shape.Volume, 400)
        self.assertAlmostEqual(wall.SheetModel["Thickness"], 2)

    def testPlateLeavesProfileValues(self):
        """Length, Reversed and Midplane are a profile wall's: read only on a
        plate, editable again when its sketch opens"""
        sketch = self.sketch([(0, 0), (20, 0), (20, 10), (0, 10)], closed=True)
        wall = self.wall(sketch)
        for name in ("Length", "Reversed", "Midplane"):
            self.assertIn("ReadOnly", wall.getPropertyStatus(name), name)
        self.assertNotIn("ReadOnly", wall.getPropertyStatus("MaterialSide"))
        sketch.delGeometry(3)
        self.Doc.recompute()
        self.assertNotIn("Invalid", wall.State)
        for name in ("Length", "Reversed", "Midplane"):
            self.assertNotIn("ReadOnly", wall.getPropertyStatus(name), name)

    def checkL(self, side, setback):
        """An L of legs 20 and 10, 15 long; setback is where the sketch's
        corner leaves each leg's straight part, by the side"""
        wall = self.wall(self.sketch([(0, 0), (20, 0), (20, 10)]), MaterialSide=side, Length=15)
        self.assertNotIn("Invalid", wall.State)
        R, T = 1, 1
        bend = math.pi / 4 * ((R + T) ** 2 - R**2)
        straight = (20 - setback) + (10 - setback)
        self.assertAlmostEqual(wall.Shape.Volume, 15 * (straight * T + bend), places=6)
        self.assertEqual(sorted(self.elements(wall)), ["Bend1", "Wall1", "Wall2"])
        bend = [e for e in wall.SheetModel["Elements"] if e["Kind"] == "Bend"][0]
        self.assertAlmostEqual(bend["Radius"], R)
        self.assertAlmostEqual(bend["Angle"], 90)
        # K from the rule's allowance, 0.5 by default
        self.assertAlmostEqual(bend["KFactor"], 0.5)
        # every face owned, caps split at the bend lines
        self.assertEqual(self.owners(wall), {"Wall1": 5, "Wall2": 5, "Bend1": 4})
        return wall

    def testProfileL(self):
        """An open sketch: walls along its lines, a bend at the corner, the
        sketch on the inner skin (Outside), the outer one (Inside) or the
        middle"""
        self.checkL("Outside", 1)

    def testProfileInside(self):
        self.checkL("Inside", 2)

    def testProfileMiddle(self):
        self.checkL("Middle", 1.5)

    def testProfileOwnRadius(self):
        """A profile's bends at the wall's own radius, 3: the sketch on the
        inner skin, each leg's straight part 3 short of the corner"""
        wall = self.wall(self.sketch([(0, 0), (20, 0), (20, 10)]), MaterialSide="Outside",
                         Length=15, UseRuleRadius=False, BendRadius=3)
        self.assertNotIn("Invalid", wall.State)
        bend = math.pi / 4 * (4**2 - 3**2)
        self.assertAlmostEqual(wall.Shape.Volume, 15 * ((20 - 3) + (10 - 3) + bend), places=6)
        bend = [e for e in wall.SheetModel["Elements"] if e["Kind"] == "Bend"][0]
        self.assertAlmostEqual(bend["Radius"], 3)

    def testProfileZ(self):
        """Bends turning both ways: the sketch's side holds along the
        profile, so the second bend has it on its outer skin"""
        wall = self.wall(self.sketch([(0, 0), (20, 0), (20, 10), (40, 10)]), Length=15)
        self.assertNotIn("Invalid", wall.State)
        bend = math.pi / 4 * (4 - 1)
        self.assertAlmostEqual(wall.Shape.Volume, 15 * (19 + 7 + 18 + 2 * bend), places=6)
        self.assertEqual(
            self.owners(wall), {"Wall1": 5, "Wall2": 4, "Wall3": 5, "Bend1": 4, "Bend2": 4}
        )

    def testProfileU(self):
        """A U, extruded both ways from the sketch"""
        wall = self.wall(
            self.sketch([(0, 10), (0, 0), (30, 0), (30, 10)]), Length=8, Midplane=True
        )
        box = wall.Shape.BoundBox
        self.assertAlmostEqual(box.ZMin, -4)
        self.assertAlmostEqual(box.ZMax, 4)
        self.assertEqual(len(wall.SheetModel["Elements"]), 5)
        self.assertNotIn(None, self.owners(wall))
        wall.Reversed = True
        wall.Midplane = False
        self.Doc.recompute()
        self.assertAlmostEqual(wall.Shape.BoundBox.ZMin, -8)
        self.assertAlmostEqual(wall.Shape.BoundBox.ZMax, 0)

    def testRuleFollowed(self):
        """A rule value changed recomputes the wall"""
        wall = self.wall(self.sketch([(0, 0), (20, 0), (20, 10)]), Length=15)
        before = wall.Shape.Volume
        wall.Rule.BendRadius = 3
        self.Doc.recompute()
        self.assertNotIn("Touched", wall.State)
        bend = math.pi / 4 * (16 - 9)
        self.assertAlmostEqual(wall.Shape.Volume, 15 * (17 + 7 + bend), places=6)
        self.assertNotAlmostEqual(wall.Shape.Volume, before)

    def testRuleShared(self):
        """A sheet feature built on another takes its rule; the model goes on
        from the one before"""
        first = self.wall(self.sketch([(0, 0), (20, 0), (20, 10), (0, 10)], closed=True))
        second = self.wall(
            self.sketch([(10, 5), (30, 5), (30, 15), (10, 15)], closed=True, name="Sketch2"),
            name="BaseWall2",
        )
        self.assertEqual(second.Rule, first.Rule)
        self.assertEqual(len(self.rules()), 1)
        self.assertEqual(self.elements(second), ["Wall1", "Wall2"])
        self.assertAlmostEqual(second.Shape.Volume, 350)

    def testTurnsSolidBodyOn(self):
        """In a body grouping its solids the old way, the first sheet feature
        turns AddSolidBody on"""
        self.Body.AddSolidBody = False
        self.wall(self.sketch([(0, 0), (20, 0), (20, 10), (0, 10)], closed=True))
        self.assertTrue(self.Body.AddSolidBody)

    def testLoneSolidHasSolidBody(self):
        """A body's only solid has a solid body once it holds sheet metal,
        so the rule has a place in the tree; Misc leaves the rule to it"""
        self.Body.setAutoAuxGroup(True)
        wall = self.wall(self.sketch([(0, 0), (20, 0), (20, 10), (0, 10)], closed=True))
        solids = self.Body.SolidBodies
        self.assertEqual(len(solids), 1)
        self.assertIn(wall, solids[0].Group)
        self.assertIn(wall.Rule, self.Body.Group)
        misc = [o for o in self.Body.Group if o.Name.startswith("Misc")]
        self.assertTrue(misc)
        self.assertNotIn(wall.Rule, misc[0].Group)

    def rolled(self, segments, name="Rolled"):
        """A sketch of lines and arcs: ("line", a, b) or ("arc", centre,
        radius, from, to) in degrees, counterclockwise from the first to the
        second; ("arc", ..., cw=True) the other way"""
        sketch = self.Body.newObject("Sketcher::SketchObject", name)
        for s in segments:
            if s[0] == "line":
                a, b = s[1], s[2]
                sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
            else:
                c, r, a0, a1 = s[1], s[2], math.radians(s[3]), math.radians(s[4])
                if a1 < a0:
                    a0, a1 = a1, a0
                circle = Part.Circle(V(c[0], c[1], 0), V(0, 0, 1), r)
                sketch.addGeometry(Part.ArcOfCircle(circle, a0, a1))
        return sketch

    def checkRolled(self, wall, radii, straight):
        """Valid, its bends of those inner radii and 90 deg each, every face
        owned; it lies flat, as long as its walls' straight parts and each
        bend's arc at its K factor's line"""
        self.assertNotIn("Invalid", wall.State, wall.getStatusString())
        self.assertTrue(wall.Shape.isValid())
        bends = [e for e in wall.SheetModel["Elements"] if e["Kind"] == "Bend"]
        self.assertEqual([round(b["Radius"], 9) for b in bends], radii)
        for b in bends:
            self.assertAlmostEqual(b["Angle"], 90)
        self.assertNotIn(None, self.owners(wall))
        T = 1
        volume = 15 * (sum(straight) * T + sum(math.pi / 4 * ((r + T) ** 2 - r**2) for r in radii))
        self.assertAlmostEqual(wall.Shape.Volume, volume, places=6)
        flat = wall.getSubObject("Flat")
        self.assertIsNotNone(flat)
        self.assertAlmostEqual(flat.Volume, wall.Shape.Volume, places=5)
        developed = sum(straight) + sum(
            (b["Radius"] + b["KFactor"] * T) * math.pi / 2 for b in bends
        )
        box = flat.BoundBox
        self.assertAlmostEqual(max(box.XLength, box.YLength), developed, places=5)

    def testRolledArc(self):
        """The rolled wall's first step (M5, ruled 2026-11-06): an arc in a
        profile, tangent to the lines either side, is a bend of its own
        radius, rolled, the sketch on the inner skin (Outside): 10; it lies
        flat exactly"""
        sketch = self.rolled(
            [
                ("line", (0, 0), (20, 0)),
                ("arc", (20, 10), 10, -90, 0),
                ("line", (30, 10), (30, 30)),
            ]
        )
        wall = self.wall(sketch, MaterialSide="Outside", Length=15)
        self.checkRolled(wall, [10.0], [20, 20])

    def testRolledArcInside(self):
        """The sketch on the outer skin (Inside): the bend's inner radius the
        thickness less, 9, the walls' straight parts as drawn"""
        sketch = self.rolled(
            [
                ("line", (0, 0), (20, 0)),
                ("arc", (20, 10), 10, -90, 0),
                ("line", (30, 10), (30, 30)),
            ]
        )
        wall = self.wall(sketch, MaterialSide="Inside", Length=15)
        self.checkRolled(wall, [9.0], [20, 20])

    def testRolledAndSharp(self):
        """A sharp corner, the rule's radius 1 about it, then an arc turning
        the other way: the sketch on that one's outer skin, its inner radius
        9; the own radius of a base wall is its corners', not its arcs'"""
        sketch = self.rolled(
            [
                ("line", (0, 0), (20, 0)),
                ("line", (20, 0), (20, 20)),
                ("arc", (30, 20), 10, 90, 180),
                ("line", (30, 30), (50, 30)),
            ]
        )
        wall = self.wall(sketch, MaterialSide="Outside", Length=15)
        # the corner's setback 1 off each line, the arc's none
        self.checkRolled(wall, [1.0, 9.0], [19, 19, 20])

    def testRolledRefused(self):
        """An arc not tangent to its lines, one at the chain's end, of 180
        deg, two with no line between, one too tight for the thickness"""
        cases = {
            "not tangent": [
                ("line", (0, 0), (20, 0)),
                ("arc", (20, 10), 10, -90, 0),
                ("line", (30, 10), (40, 20)),
            ],
            "at the end": [("line", (0, 0), (20, 0)), ("arc", (20, 10), 10, -90, 0)],
            "half a turn": [
                ("line", (0, 0), (20, 0)),
                ("arc", (20, 10), 10, -90, 90),
                ("line", (20, 20), (0, 20)),
            ],
            "two in a row": [
                ("line", (0, 0), (20, 0)),
                ("arc", (20, 10), 10, -90, 0),
                ("arc", (40, 10), 10, 90, 180),
                ("line", (40, 20), (60, 20)),
            ],
        }
        for what, segments in cases.items():
            with self.subTest(what=what):
                wall = self.wall(self.rolled(segments, name=what.replace(" ", "")), name="W")
                self.assertIn("Invalid", wall.State)
                self.Body.removeObject(wall)
                self.Doc.removeObject(wall.Name)
        tight = self.rolled(
            [
                ("line", (0, 0), (20, 0)),
                ("arc", (20, 0.5), 0.5, -90, 0),
                ("line", (20.5, 0.5), (20.5, 20)),
            ],
            name="Tight",
        )
        wall = self.wall(tight, MaterialSide="Inside", name="W")
        self.assertIn("Invalid", wall.State)

    def testErrors(self):
        """No rule, a profile ending in an arc, a leg shorter than its bend"""
        sketch = self.sketch([(0, 0), (20, 0), (20, 10)])
        sketch.addGeometry(Part.ArcOfCircle(Part.Circle(V(20, 15, 0), V(0, 0, 1), 5), -1.57, 0))
        wall = self.wall(sketch)
        self.assertIn("Invalid", wall.State)
        sketch.delGeometry(2)
        self.Doc.recompute()
        self.assertNotIn("Invalid", wall.State)
        short = self.wall(self.sketch([(0, 0), (20, 0), (20, 0.5)], name="Short"), name="Short")
        self.assertIn("Invalid", short.State)
        wall.Rule = None
        wall.touch()
        self.Doc.recompute()
        self.assertIn("Invalid", wall.State)

    def testSaveAndRestore(self):
        """The model comes back with the file, and a recompute after makes
        the same"""
        wall = self.wall(self.sketch([(0, 0), (20, 0), (20, 10)]), Length=15)
        model = wall.SheetModel
        owners = self.owners(wall)
        volume = wall.Shape.Volume
        path = os.path.join(tempfile.mkdtemp(), "wall.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        self.Body = self.Doc.getObject("Body")
        wall = self.Doc.getObject("BaseWall")
        self.assertEqual(wall.SheetModel, model)
        self.assertEqual(self.owners(wall), owners)
        self.assertIsNotNone(wall.Rule)
        wall.touch()
        self.Doc.recompute()
        self.assertAlmostEqual(wall.Shape.Volume, volume)
        self.assertEqual(wall.SheetModel, model)
