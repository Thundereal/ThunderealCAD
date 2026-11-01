# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's flange, PartDesign::SheetMetalFlange (docs/SheetMetal.md sec
# 5.1): a bend and a leg on one or more edges of a wall, the bend toward the
# side of the skin the edge is on. Every volume below is worked by hand on a
# plate 40 x 20, 1 thick (z 0..1), with the rule's 1 mm inner radius: a 90 deg
# bend is a quarter ring, (pi / 4) (2^2 - 1^2) = 3 pi / 4 in section.

import math
import os
import tempfile
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector
RING = 3 * math.pi / 4


class TestSheetMetalFlange(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalFlange")
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
        """The name of the edge of a feature's shape from a to b"""
        for i, e in enumerate(feature.Shape.Edges):
            ends = [v.Point for v in e.Vertexes]
            if len(ends) == 2 and (
                (ends[0].isEqual(V(*a), 1e-7) and ends[1].isEqual(V(*b), 1e-7))
                or (ends[0].isEqual(V(*b), 1e-7) and ends[1].isEqual(V(*a), 1e-7))
            ):
                return "Edge%d" % (i + 1)
        self.fail("no edge from %s to %s" % (a, b))

    def flange(self, edges, base=None, name="Flange", **props):
        base = base or self.Plate
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", name)
        flange.Edges = (base, [self.edge(base, a, b) for a, b in edges])
        for k, v in props.items():
            setattr(flange, k, v)
        self.Doc.recompute()
        return flange

    def far(self, **props):
        """A flange on the plate's far edge on its top skin"""
        return self.flange([((0, 20, 1), (40, 20, 1))], **props)

    def checkValid(self, flange):
        self.assertNotIn("Invalid", flange.State)
        self.assertTrue(flange.Shape.isValid())
        self.assertEqual(len(flange.Shape.Solids), 1)

    def owners(self, feature):
        return [feature.sheetElementOf("Face%d" % (i + 1)) for i in range(len(feature.Shape.Faces))]

    def testMaterialInside(self):
        """The default: the leg's outer skin where the wall's end was, the
        wall cut back R + T under it; OuterSharp length 10, so the leg's
        straight part is 10 - (R + T)"""
        flange = self.far()
        self.checkValid(flange)
        self.assertEqual(flange.Rule, self.Plate.Rule)
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 18 + 40 * RING + 40 * 8, places=6)
        box = flange.Shape.BoundBox
        self.assertAlmostEqual(box.YMax, 20)
        self.assertAlmostEqual(box.ZMax, 10)
        model = flange.SheetModel["Elements"]
        self.assertEqual([e["Name"] for e in model], ["Wall1", "Wall2", "Bend1"])
        self.assertEqual((model[2]["Before"], model[2]["After"]), ("Wall1", "Wall2"))
        self.assertAlmostEqual(model[2]["Angle"], 90)
        self.assertNotIn(None, self.owners(flange))
        self.assertEqual(self.Body.Tip, flange)

    def testMaterialOutside(self):
        """The leg's inner skin where the wall's end was: cut back R"""
        flange = self.far(BendPosition="MaterialOutside")
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 19 + 40 * RING + 40 * 8, places=6)
        self.assertAlmostEqual(flange.Shape.BoundBox.YMax, 21)

    def testBendOutside(self):
        """The bend starting at the wall's end, nothing cut; Offset moves it
        out, the wall carried on to it"""
        flange = self.far(BendPosition="BendOutside")
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 20 + 40 * RING + 40 * 8, places=6)
        self.assertAlmostEqual(flange.Shape.BoundBox.YMax, 22)
        flange.Offset = 3
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 23 + 40 * RING + 40 * 8, places=6)
        self.assertAlmostEqual(flange.Shape.BoundBox.YMax, 25)
        self.assertNotIn(None, self.owners(flange))

    def testLengthTypes(self):
        """Leg is the straight part; InnerSharp from where the inner skins
        meet, OuterSharp from where the outer ones do"""
        flange = self.far(LengthType="Leg")
        self.assertAlmostEqual(flange.Shape.BoundBox.ZMax, 12)
        flange.LengthType = "InnerSharp"
        self.Doc.recompute()
        self.assertAlmostEqual(flange.Shape.BoundBox.ZMax, 11)
        flange.LengthType = "OuterSharp"
        self.Doc.recompute()
        self.assertAlmostEqual(flange.Shape.BoundBox.ZMax, 10)

    def testAngle(self):
        """45 deg: the cut back and the length from the virtual sharps, by
        tan(22.5 deg)"""
        flange = self.far(Angle=45)
        self.checkValid(flange)
        t = math.tan(math.radians(22.5))
        back = 2 * t
        leg = 10 - 2 * t
        ring = (math.pi / 8) * 3
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * (20 - back) + 40 * ring + 40 * leg, places=6
        )
        bend = flange.SheetModel["Elements"][2]
        self.assertAlmostEqual(bend["Angle"], 45)

    def testReversed(self):
        """Bent down, from the top skin's edge: the bottom skin is the inner
        one, and the outer sharp is on the top skin"""
        flange = self.far(Reversed=True)
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 18 + 40 * RING + 40 * 8, places=6)
        self.assertAlmostEqual(flange.Shape.BoundBox.ZMin, -9)
        self.assertAlmostEqual(flange.Shape.BoundBox.ZMax, 1)

    def testFromTheOtherSkin(self):
        """An edge on the bottom skin bends down"""
        flange = self.flange([((0, 20, 0), (40, 20, 0))])
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.BoundBox.ZMin, -9)

    def testGaps(self):
        """Gaps at the ends make the flange narrower; the wall is cut back
        under it only"""
        flange = self.far(Gap1=5, Gap2=5)
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 + 30 * RING + 30 * 8, places=6
        )
        box = flange.Shape.BoundBox
        self.assertAlmostEqual(box.XMin, 0)
        self.assertAlmostEqual(box.XMax, 40)

    def testTwoEdges(self):
        """Two edges in one feature, one join"""
        flange = self.flange([((0, 20, 1), (40, 20, 1)), ((0, 0, 1), (40, 0, 1))])
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 16 + 2 * (40 * RING + 40 * 8), places=6
        )
        names = [e["Name"] for e in flange.SheetModel["Elements"]]
        self.assertEqual(sorted(names), ["Bend1", "Bend2", "Wall1", "Wall2", "Wall3"])
        self.assertNotIn(None, self.owners(flange))

    def flat(self, source):
        flat = self.Body.newObject("PartDesign::SheetMetalFlatPattern", "Flat")
        flat.Source = source
        self.Doc.recompute()
        return flat

    def testMiter(self):
        """Two edges of one flange meeting at the plate's corner: each leg
        and bend stops at the plane halving the corner, the rule's gap
        between them, and the flat lays out"""
        flange = self.flange([((0, 20, 1), (40, 20, 1)), ((40, 0, 1), (40, 20, 1))])
        self.checkValid(flange)
        gap = flange.Rule.Gap.Value
        self.assertAlmostEqual(gap, 0.1)
        shape = flange.Shape
        # the legs: the far one's at y 19..20, the right one's at x 39..40;
        # the corner's plane x - y = 20 between them, z 5 halfway up
        self.assertTrue(shape.isInside(V(38.5, 19.5, 5), 1e-9, True))
        self.assertTrue(shape.isInside(V(39.5, 18.5, 5), 1e-9, True))
        for x in (38.6, 39.0, 39.5, 39.96):
            self.assertFalse(shape.isInside(V(x, x - 20, 5), 1e-9, True), x)
        # just outside the gap on either side, just inside it
        off = 0.06 * math.sqrt(2)
        self.assertTrue(shape.isInside(V(39.5 - off / 2, 19.5 + off / 2, 5), 1e-9, True))
        self.assertTrue(shape.isInside(V(39.5 + off / 2, 19.5 - off / 2, 5), 1e-9, True))
        near = 0.04 * math.sqrt(2)
        self.assertFalse(shape.isInside(V(39.5 - near / 2, 19.5 + near / 2, 5), 1e-9, True))
        # the bends at the corner apart too, halfway round
        self.assertFalse(shape.isInside(V(39.3, 19.3, 1.5), 1e-9, True))
        names = sorted(e["Name"] for e in flange.SheetModel["Elements"])
        self.assertEqual(names, ["Bend1", "Bend2", "Wall1", "Wall2", "Wall3"])
        self.assertNotIn(None, self.owners(flange))
        flat = self.flat(flange)
        self.assertNotIn("Invalid", flat.State)
        solid = flat.Shape.Solids[0]
        self.assertTrue(solid.isValid())
        self.assertEqual(flat.BendCount, 2)
        # unrolling keeps a bend's volume only where it is cut square across:
        # cut at a slant, its inner and outer fibres stretch differently, by
        # the integral of (r - (R + K T)) r cos(theta), 1/12 each bend here,
        # 0.19 measured with the ruled face across
        self.assertAlmostEqual(solid.Volume, flange.Shape.Volume, delta=0.25)
        # the miter's limits saved with the model
        limits = [e for e in flange.SheetModel["Elements"] if "Limits" in e]
        self.assertEqual(sorted(e["Name"] for e in limits), ["Bend1", "Bend2", "Wall2", "Wall3"])
        path = os.path.join(tempfile.mkdtemp(), "miter.FCStd")
        names = flange.Name, flat.Name
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        flange, flat = (self.Doc.getObject(n) for n in names)
        self.assertEqual(len([e for e in flange.SheetModel["Elements"] if "Limits" in e]), 4)
        flat.touch()
        self.Doc.recompute()
        self.assertNotIn("Invalid", flat.State)

    def testMiterTray(self):
        """All four edges in one flange: four miters, a closed tray whose flat
        is a cross"""
        flange = self.flange(
            [
                ((0, 0, 1), (40, 0, 1)),
                ((40, 0, 1), (40, 20, 1)),
                ((0, 20, 1), (40, 20, 1)),
                ((0, 0, 1), (0, 20, 1)),
            ]
        )
        self.checkValid(flange)
        for x, y in ((0, 0), (40, 0), (40, 20), (0, 20)):
            sx, sy = (1 if x else -1), (1 if y else -1)
            # on the corner's diagonal, 0.5 in from the outside, halfway up
            p = V(x - 0.5 * sx, y - 0.5 * sy, 5)
            self.assertFalse(flange.Shape.isInside(p, 1e-9, True), (x, y))
        flat = self.flat(flange)
        self.assertNotIn("Invalid", flat.State)
        self.assertEqual(flat.BendCount, 4)
        self.assertAlmostEqual(flat.Shape.Solids[0].Volume, flange.Shape.Volume, delta=1.0)

    def testNoMiterApart(self):
        """Edges that do not meet, or flanges bent to opposite sides, are
        left whole"""
        flange = self.flange([((0, 20, 1), (40, 20, 1)), ((0, 0, 1), (40, 0, 1))])
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 16 + 2 * (40 * RING + 40 * 8), places=6
        )

    def testFlangeOnFlange(self):
        """A flange on the first one's end: its leg is the wall it starts
        from"""
        first = self.far(LengthType="Leg")
        # the first leg's end, z = 12, its outer skin y = 20, inner y = 19
        second = self.flange([((0, 19, 12), (40, 19, 12))], base=first, name="Flange2")
        self.checkValid(second)
        model = second.SheetModel["Elements"]
        bend = [e for e in model if e["Name"] == "Bend2"][0]
        self.assertEqual((bend["Before"], bend["After"]), ("Wall2", "Wall3"))
        self.assertEqual(second.Rule, self.Plate.Rule)
        self.assertNotIn(None, self.owners(second))

    def testRuleFollowed(self):
        """A rule value changed recomputes the flange"""
        flange = self.far()
        self.Plate.Rule.BendRadius = 2
        self.Doc.recompute()
        self.checkValid(flange)
        ring = math.pi / 4 * (9 - 4)
        # cut back R + T = 3, leg 10 - 3
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 17 + 40 * ring + 40 * 7, places=6)

    def testTangent(self):
        """Tangent: from the plane across the leg that touches the outside of
        the bend -- at 90 deg the outer sharp, at 135 the bend's own outside"""
        flange = self.far(LengthType="Tangent")
        self.assertAlmostEqual(flange.Shape.BoundBox.ZMax, 10)
        flange.Angle = 135
        self.Doc.recompute()
        self.checkValid(flange)
        # the straight part is L - (R + T)
        ring = (math.pi * 135 / 180) / 2 * (4 - 1)
        cut = 2 * math.tan(math.radians(67.5))
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * (20 - cut) + 40 * ring + 40 * (10 - 2), places=6
        )
        flange.Angle = 45
        self.Doc.recompute()
        # below 90, L - (R + T) sin A
        straight = 10 - 2 * math.sin(math.radians(45))
        cut = 2 * math.tan(math.radians(22.5))
        ring = (math.pi / 8) * 3
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * (20 - cut) + 40 * ring + 40 * straight, places=6
        )

    def testExtents(self):
        """A negative gap carries the flange past the edge's end"""
        flange = self.far(BendPosition="BendOutside", Gap1=-5)
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 + 45 * RING + 45 * 8, places=6
        )
        self.assertAlmostEqual(flange.Shape.BoundBox.XLength, 45)

    def testParallelTo(self):
        """The angle from a face the leg runs parallel to"""
        plane = self.Body.newObject("PartDesign::Plane", "Plane")
        plane.MapMode = "Deactivated"
        # its normal halfway between +Y and +Z: a leg parallel to it goes up
        # and back over the plate, 135 deg from it
        plane.Placement = FreeCAD.Placement(V(0, 0, 0), FreeCAD.Rotation(V(1, 0, 0), -45))
        self.Doc.recompute()
        flange = self.far(ParallelTo=(plane, [""]))
        self.checkValid(flange)
        bend = [e for e in flange.SheetModel["Elements"] if e["Kind"] == "Bend"][0]
        self.assertAlmostEqual(bend["Angle"], 135, places=6)

    def testUpToFace(self):
        """The length from a face the leg runs up to"""
        plane = self.Body.newObject("PartDesign::Plane", "Plane")
        plane.MapMode = "Deactivated"
        plane.Placement = FreeCAD.Placement(V(0, 0, 15), FreeCAD.Rotation())
        self.Doc.recompute()
        flange = self.far(UpToFace=(plane, [""]))
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.BoundBox.ZMax, 15)

    def testErrors(self):
        """An edge that is not on a wall's skin, a leg left with no length"""
        flange = self.flange([((40, 0, 0), (40, 0, 1))])
        self.assertIn("Invalid", flange.State)
        flange.Edges = (self.Plate, [self.edge(self.Plate, (0, 20, 1), (40, 20, 1))])
        flange.Length = 1.5
        self.Doc.recompute()
        self.assertIn("Invalid", flange.State)
        flange.Length = 10
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)

    def testSaveAndRestore(self):
        flange = self.far()
        model = flange.SheetModel
        volume = flange.Shape.Volume
        path = os.path.join(tempfile.mkdtemp(), "flange.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        flange = self.Doc.getObject("Flange")
        self.assertEqual(flange.SheetModel, model)
        flange.touch()
        self.Doc.recompute()
        self.assertAlmostEqual(flange.Shape.Volume, volume)
        self.assertEqual(flange.SheetModel, model)
