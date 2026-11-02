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
# the rule's bend relief, a rectangle 1 wide from the wall's end to 1 past
# where the bend starts: 2 back at 90 deg (MaterialInside), so 1 x 3 each
RELIEF = 1 * (2 + 1)


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
        under it only, and a relief each side parts it from the bend"""
        flange = self.far(Gap1=5, Gap2=5)
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 - 2 * RELIEF + 30 * RING + 30 * 8, places=6
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

    # The corner relief (sec 5.2, ruled 2026-11-02): where two bends meet at
    # a miter, the rule's CornerReliefShape drawn in the flat, centred where
    # the bends' lines cross, and folded back into the part through the fold
    # maps, so the blank's outline carries exactly the shape asked for. A
    # circle's CornerReliefSize is its diameter, a square's its side, along
    # the bends' axes; None and Tear cut nothing more than the miter's gap.
    # The far and right bends' lines, the middle of each strip, lie
    # (R + K T) pi / 4 = 1.178 out from where each starts: the crossing is
    # that far out each way from the plate's corner the two bends leave,
    # wherever the flat puts the plate.

    def reliefFlats(self, flange, shape, size):
        """The flat with the rule's corner relief, the flat with none, and
        the bends' lines' crossing in them: from the corner of the plate's
        skin (38 x 18 after the cuts, along X and Y) that both bends' strips
        leave, 1.178 out each way"""
        rule = flange.Rule
        rule.CornerReliefShape = "None"
        self.Doc.recompute()
        flat = self.flat(flange)
        self.assertNotIn("Invalid", flat.State)
        plain = flat.Shape.copy()
        skin = [f for f in plain.Faces if abs(f.Area - 38 * 18) < 1e-6][0].BoundBox
        self.assertAlmostEqual(skin.XLength, 38, places=6)
        self.assertAlmostEqual(skin.YLength, 18, places=6)
        box = plain.BoundBox
        sx = 1 if box.XMax > skin.XMax + 1 else -1
        sy = 1 if box.YMax > skin.YMax + 1 else -1
        out = 1.5 * math.pi / 4
        crossing = V(
            (skin.XMax if sx > 0 else skin.XMin) + sx * out,
            (skin.YMax if sy > 0 else skin.YMin) + sy * out,
            0,
        )
        rule.CornerReliefShape = shape
        rule.CornerReliefSize = size
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertNotIn("Invalid", flat.State)
        self.assertNotIn(None, self.owners(flange))
        return flat.Shape.Solids[0], plain.Solids[0], crossing

    def checkCut(self, relieved, plain, tool):
        """On each skin of the flat (z 0 and 1), nothing left inside the
        shape and nothing changed outside it. Measured face by face in the
        plane: the flat with none, mitered, overlaps itself a little at the
        corner (a bend cut at a slant unrolls its skins differently), where
        a boolean of its solid fails"""
        for z in (0, 1):
            # a skin's faces: planes across Z there (a face with a relief's
            # B-spline edge has a loose bounding box)
            on = lambda shape: [
                f
                for f in shape.Faces
                if isinstance(f.Surface, Part.Plane)
                and abs(abs(f.Surface.Axis.z) - 1) < 1e-9
                and abs(f.Surface.Position.z - z) < 1e-6
            ]
            cut = tool.translated(V(0, 0, z))
            # within the relief's outline as the flat carries it, a B-spline
            # through samples of its image
            left = sum(f.common(cut).Area for f in on(relieved))
            self.assertLess(left, 1e-5, z)
            before = sum(f.Area for f in on(plain)) - sum(f.common(cut).Area for f in on(plain))
            self.assertAlmostEqual(sum(f.Area for f in on(relieved)), before, delta=1e-5)
        self.assertLess(relieved.Volume, plain.Volume - 1e-3)

    def testCornerReliefCircle(self):
        """The rule's default, a circle 2 across"""
        flange = self.flange([((0, 20, 1), (40, 20, 1)), ((40, 0, 1), (40, 20, 1))])
        self.assertEqual(flange.Rule.CornerReliefShape, "Circle")
        relieved, plain, c = self.reliefFlats(flange, "Circle", 2)
        self.checkCut(relieved, plain, Part.Face(Part.Wire(Part.makeCircle(1, c))))

    def testCornerReliefSquare(self):
        """A square 2 on a side, its sides along the bends' axes"""
        flange = self.flange([((0, 20, 1), (40, 20, 1)), ((40, 0, 1), (40, 20, 1))])
        relieved, plain, c = self.reliefFlats(flange, "Square", 2)
        square = [c + V(x, y, 0) for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1), (-1, -1))]
        self.checkCut(relieved, plain, Part.Face(Part.makePolygon(square)))

    def testCornerReliefNone(self):
        """None and Tear: the miter's gap, nothing more"""
        flange = self.flange([((0, 20, 1), (40, 20, 1)), ((40, 0, 1), (40, 20, 1))])
        flange.Rule.CornerReliefShape = "None"
        self.Doc.recompute()
        volume = flange.Shape.Volume
        flange.Rule.CornerReliefShape = "Tear"
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, volume, places=9)
        flange.Rule.CornerReliefShape = "Circle"
        self.Doc.recompute()
        self.assertLess(flange.Shape.Volume, volume - 1e-3)

    def testCornerReliefAgainstEarlier(self):
        """A flange mitered against an earlier one gets the relief too"""
        first = self.far()
        second = self.flange([((40, 0, 1), (40, 18, 1))], base=first, name="Flange2")
        relieved, plain, c = self.reliefFlats(second, "Circle", 2)
        self.checkCut(relieved, plain, Part.Face(Part.Wire(Part.makeCircle(1, c))))

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
        # the box's flat, by hand (sec 9, M1): each side the plate cut back
        # R + T at both ends, and two legs of the bend's allowance
        # (R + T / 2) pi / 2 and the straight 8
        leg = 1.5 * math.pi / 2 + 8
        self.assertAlmostEqual(flat.FlatLength.Value, 40 - 2 * 2 + 2 * leg, places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, 20 - 2 * 2 + 2 * leg, places=6)

    # A later flange meeting an earlier one at a corner (ruled 2026-11-02):
    # it miters itself against it, the same plane and the rule's gap as one
    # flange on both edges, so the part and its flat are that flange's

    def reference(self, edges):
        """One flange on the plate's edges, in a document of its own: its
        volume and its flat's length and width"""
        doc = FreeCAD.newDocument("PartDesignTestSheetMetalFlangeReference")
        try:
            body = doc.addObject("PartDesign::Body", "Body")
            sketch = body.newObject("Sketcher::SketchObject", "Sketch")
            pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
            for i in range(4):
                a, b = pts[i], pts[(i + 1) % 4]
                sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
            plate = body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
            plate.Profile = sketch
            doc.recompute()
            flange = body.newObject("PartDesign::SheetMetalFlange", "Flange")
            flange.Edges = (plate, [self.edge(plate, a, b) for a, b in edges])
            doc.recompute()
            flat = body.newObject("PartDesign::SheetMetalFlatPattern", "Flat")
            flat.Source = flange
            doc.recompute()
            self.assertNotIn("Invalid", flat.State)
            return flange.Shape.Volume, flat.FlatLength.Value, flat.FlatWidth.Value
        finally:
            FreeCAD.closeDocument(doc.Name)

    def checkAsReference(self, flange, reference, bends):
        volume, length, width = reference
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, volume, places=6)
        self.assertNotIn(None, self.owners(flange))
        flat = self.flat(flange)
        self.assertNotIn("Invalid", flat.State)
        self.assertEqual(len(flat.Shape.Solids), 1)
        self.assertTrue(flat.Shape.Solids[0].isValid())
        self.assertEqual(flat.BendCount, bends)
        self.assertAlmostEqual(flat.FlatLength.Value, length, places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, width, places=6)

    def testMiterAgainstEarlier(self):
        """The far edge's flange, then one on the right edge, cut short at
        y 18 by the first: the second carried on to the corner and both
        mitered there, as one flange on both edges; and the other way round"""
        far, right = ((0, 20, 1), (40, 20, 1)), ((40, 0, 1), (40, 20, 1))
        reference = self.reference([far, right])
        first = self.far()
        second = self.flange([((40, 0, 1), (40, 18, 1))], base=first, name="Flange2")
        self.checkAsReference(second, reference, 2)
        shape = second.Shape
        # the corner's plane x - y = 20 between the legs, the gap open
        for x in (38.6, 39.0, 39.5, 39.96):
            self.assertFalse(shape.isInside(V(x, x - 20, 5), 1e-9, True), x)
        self.assertTrue(shape.isInside(V(38.5, 19.5, 5), 1e-9, True))
        self.assertTrue(shape.isInside(V(39.5, 18.5, 5), 1e-9, True))
        self.assertFalse(shape.isInside(V(39.3, 19.3, 1.5), 1e-9, True))

        self.Body.removeObject(second)
        self.Body.removeObject(first)
        self.Doc.removeObject(second.Name)
        self.Doc.removeObject(first.Name)
        self.Doc.recompute()
        first = self.flange([right], name="Right")
        second = self.flange([((0, 20, 1), (38, 20, 1))], base=first, name="Far")
        self.checkAsReference(second, reference, 2)

    def testMiterAgainstEarlierTray(self):
        """Four flanges, one edge each, round the plate: the tray of one
        flange on all four edges, and its cross of a flat"""
        reference = self.reference(
            [
                ((0, 0, 1), (40, 0, 1)),
                ((40, 0, 1), (40, 20, 1)),
                ((0, 20, 1), (40, 20, 1)),
                ((0, 0, 1), (0, 20, 1)),
            ]
        )
        far = self.far()
        right = self.flange([((40, 0, 1), (40, 18, 1))], base=far, name="Right")
        near = self.flange([((0, 0, 1), (38, 0, 1))], base=right, name="Near")
        left = self.flange([((0, 2, 1), (0, 18, 1))], base=near, name="Left")
        self.checkAsReference(left, reference, 4)
        leg = 1.5 * math.pi / 2 + 8
        self.assertAlmostEqual(reference[1], 40 - 2 * 2 + 2 * leg, places=6)

    def testNoMiterAgainstEarlierWithGap(self):
        """A gap at the corner's end keeps the later flange short of it, and
        the earlier one whole there"""
        first = self.far()
        second = self.flange(
            [((40, 0, 1), (40, 18, 1))], base=first, name="Flange2", Gap2=3
        )
        self.checkValid(second)
        self.assertTrue(second.Shape.isInside(V(39.9, 19.5, 5), 1e-9, True))
        self.assertFalse(second.Shape.isInside(V(39.5, 16, 5), 1e-9, True))

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

    # The bend relief (sec 5.2, brought into M1 2026-11-01): where a flange
    # ends inside its wall's end, the wall left beside it meets the bend face
    # to face, and only a cut into the wall there parts them so the part lays
    # flat. Automatic, from the rule: ReliefShape, ReliefWidth beside the
    # bend, from the wall's end to ReliefDepth past where the bend starts.

    def checkFlat(self, flange, across, along):
        """The flange's flat lays out in one valid solid of the flange's
        volume (each bend cut square, K 0.5), across x along"""
        flat = self.flat(flange)
        self.assertNotIn("Invalid", flat.State)
        self.assertEqual(len(flat.Shape.Solids), 1)
        solid = flat.Shape.Solids[0]
        self.assertTrue(solid.isValid())
        self.assertEqual(flat.BendCount, 1)
        self.assertAlmostEqual(solid.Volume, flange.Shape.Volume, places=6)
        box = solid.BoundBox
        lengths = sorted([box.XLength, box.YLength])
        self.assertAlmostEqual(lengths[0], min(across, along), places=6)
        self.assertAlmostEqual(lengths[1], max(across, along), places=6)
        return flat

    def testRelief(self):
        """Gaps of 5: a relief x 4..5 and 35..36, y 17..20, through the
        sheet; the flat is 40 across and the wall's 18, the bend's allowance
        (R + T / 2) pi / 2 and the leg's 8 along"""
        flange = self.far(Gap1=5, Gap2=5)
        self.checkValid(flange)
        shape = flange.Shape
        for x in (4.5, 35.5):
            for y in (19.5, 17.1):
                self.assertFalse(shape.isInside(V(x, y, 0.5), 1e-9, True), (x, y))
            self.assertTrue(shape.isInside(V(x, 16.9, 0.5), 1e-9, True), x)
        for x in (3.9, 36.1):
            self.assertTrue(shape.isInside(V(x, 19.5, 0.5), 1e-9, True), x)
        self.assertNotIn(None, self.owners(flange))
        self.checkFlat(flange, 40, 18 + 1.5 * math.pi / 2 + 8)

    def testReliefOneEnd(self):
        """A gap at one end only: one relief, at that end"""
        flange = self.far(Gap2=5)
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 35 * 2 - RELIEF + 35 * RING + 35 * 8, places=6
        )
        self.checkFlat(flange, 40, 18 + 1.5 * math.pi / 2 + 8)

    def testReliefFromRule(self):
        """The rule's width and depth, followed when they change"""
        flange = self.far(Gap1=5, Gap2=5)
        rule = flange.Rule
        rule.ReliefWidth = 2
        rule.ReliefDepth = 0.5
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 - 2 * 2 * 2.5 + 30 * RING + 30 * 8, places=6
        )
        self.assertFalse(flange.Shape.isInside(V(3.5, 17.6, 0.5), 1e-9, True))
        self.assertTrue(flange.Shape.isInside(V(3.5, 17.4, 0.5), 1e-9, True))
        self.checkFlat(flange, 40, 18 + 1.5 * math.pi / 2 + 8)

    def testReliefObround(self):
        """Obround: the rectangle's deep end a half circle of its width, the
        same depth to its tip"""
        flange = self.far(Gap1=5, Gap2=5)
        flange.Rule.ReliefShape = "Obround"
        self.Doc.recompute()
        self.checkValid(flange)
        relief = 1 * 2.5 + math.pi / 8
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 - 2 * relief + 30 * RING + 30 * 8, places=6
        )
        shape = flange.Shape
        # in the half circle about (4.5, 17.5), and in its corner left whole
        self.assertFalse(shape.isInside(V(4.5, 17.1, 0.5), 1e-9, True))
        self.assertTrue(shape.isInside(V(4.05, 17.1, 0.5), 1e-9, True))
        self.assertNotIn(None, self.owners(flange))
        self.checkFlat(flange, 40, 18 + 1.5 * math.pi / 2 + 8)

    def testReliefTear(self):
        """Tear (ruled 2026-11-02): the wall slit along the bend's side, the
        rule's Gap wide (0.1), from the wall's end to where the bend starts,
        x 4.9..5 and 35..35.1, y 18..20; the flat closes as the others'"""
        flange = self.far(Gap1=5, Gap2=5)
        flange.Rule.ReliefShape = "Tear"
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 - 2 * 0.1 * 2 + 30 * RING + 30 * 8, places=6
        )
        shape = flange.Shape
        for x in (4.95, 35.05):
            self.assertFalse(shape.isInside(V(x, 19.5, 0.5), 1e-9, True), x)
            self.assertTrue(shape.isInside(V(x, 17.9, 0.5), 1e-9, True), x)
        for x in (4.85, 35.15):
            self.assertTrue(shape.isInside(V(x, 19.5, 0.5), 1e-9, True), x)
        self.assertNotIn(None, self.owners(flange))
        self.checkFlat(flange, 40, 18 + 1.5 * math.pi / 2 + 8)

    def testReliefTearFromRule(self):
        """The tear follows the rule's Gap, and with no gap cuts nothing"""
        flange = self.far(Gap1=5, Gap2=5)
        rule = flange.Rule
        rule.ReliefShape = "Tear"
        rule.Gap = 0.5
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 - 2 * 0.5 * 2 + 30 * RING + 30 * 8, places=6
        )
        self.assertFalse(flange.Shape.isInside(V(4.6, 19.5, 0.5), 1e-9, True))
        rule.Gap = 0
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 + 30 * RING + 30 * 8, places=6
        )

    # The relief overridden per flange (sec 5.2, M2): ReliefShape "Rule"
    # follows the rule, and ReliefWidth and ReliefDepth are read only then;
    # another shape is the flange's own, its width and depth starting from
    # the rule's and no longer following it. A tear is still the rule's Gap
    # wide.

    def testReliefOverride(self):
        """A rectangle 2 wide, 0.5 deep, the rule's left as it is"""
        flange = self.far(Gap1=5, Gap2=5)
        self.assertEqual(flange.ReliefShape, "Rule")
        self.assertIn("ReadOnly", flange.getPropertyStatus("ReliefWidth"))
        self.assertIn("ReadOnly", flange.getPropertyStatus("ReliefDepth"))
        flange.ReliefShape = "Rectangle"
        self.assertNotIn("ReadOnly", flange.getPropertyStatus("ReliefWidth"))
        self.assertAlmostEqual(flange.ReliefWidth.Value, 1)
        self.assertAlmostEqual(flange.ReliefDepth.Value, 1)
        flange.ReliefWidth = 2
        flange.ReliefDepth = 0.5
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 - 2 * 2 * 2.5 + 30 * RING + 30 * 8, places=6
        )
        self.assertAlmostEqual(flange.Rule.ReliefWidth.Value, 1)
        self.assertAlmostEqual(flange.Rule.ReliefDepth.Value, 1)
        self.checkFlat(flange, 40, 18 + 1.5 * math.pi / 2 + 8)

    def testReliefOverrideIgnoresRule(self):
        """Overridden, the flange's relief no longer follows the rule's
        shape, width or depth; back to Rule, it does again"""
        flange = self.far(Gap1=5, Gap2=5, ReliefShape="Obround")
        rule = flange.Rule
        rule.ReliefShape = "Tear"
        rule.ReliefWidth = 3
        rule.ReliefDepth = 2
        self.Doc.recompute()
        self.checkValid(flange)
        relief = 1 * 2.5 + math.pi / 8
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 - 2 * relief + 30 * RING + 30 * 8, places=6
        )
        flange.ReliefShape = "Rule"
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertIn("ReadOnly", flange.getPropertyStatus("ReliefWidth"))
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 - 2 * 0.1 * 2 + 30 * RING + 30 * 8, places=6
        )

    def testReliefOverrideTear(self):
        """A tear on the flange, the rule's a rectangle: the rule's Gap wide"""
        flange = self.far(Gap1=5, Gap2=5, ReliefShape="Tear")
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 30 * 2 - 2 * 0.1 * 2 + 30 * RING + 30 * 8, places=6
        )
        self.checkFlat(flange, 40, 18 + 1.5 * math.pi / 2 + 8)

    def testReliefOverrideSaved(self):
        """The override survives a save and reopen, read only status and all"""
        flange = self.far(Gap1=5, Gap2=5, ReliefShape="Rectangle")
        flange.ReliefWidth = 2
        self.Doc.recompute()
        volume = flange.Shape.Volume
        path = os.path.join(tempfile.mkdtemp(), "relief.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        flange = self.Doc.getObject("Flange")
        self.assertEqual(flange.ReliefShape, "Rectangle")
        self.assertAlmostEqual(flange.ReliefWidth.Value, 2)
        self.assertNotIn("ReadOnly", flange.getPropertyStatus("ReliefWidth"))
        flange.touch()
        self.Doc.recompute()
        self.assertAlmostEqual(flange.Shape.Volume, volume, places=6)

    def testReliefNotNeeded(self):
        """No relief where nothing meets the bend's side: the bend starting
        at the wall's end (BendOutside), or the flange carried past the
        edge's end (a negative gap)"""
        flange = self.far(BendPosition="BendOutside", Gap1=5, Gap2=5)
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 20 + 30 * RING + 30 * 8, places=6)
        flange.BendPosition = "MaterialInside"
        flange.Gap1 = -5
        flange.Gap2 = 0
        self.Doc.recompute()
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 18 + 45 * RING + 45 * 8, places=6)

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

    # The leg's shape from a closed sketch (sec 5.1): the sketch lies parallel
    # to the leg, its outline standing on the outer virtual sharp. Through the
    # bend it is a rectangle, which sets the bend's width; above the bend it
    # is the leg, holes and all. The far flange's leg stands in y = 20, the
    # sharp at z = 0, the bend reaching z = 2 (R + T at 90 deg).

    def legSketch(self, points, holes=(), plane=None, name="LegSketch"):
        """A closed sketch of points (x, z) in the plane y = 20, circles
        (x, z, r) for holes"""
        sketch = self.Body.newObject("Sketcher::SketchObject", name)
        sketch.Placement = plane or FreeCAD.Placement(
            V(0, 20, 0), FreeCAD.Rotation(V(1, 0, 0), 90)
        )
        for i in range(len(points)):
            a, b = points[i], points[(i + 1) % len(points)]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        for x, z, r in holes:
            sketch.addGeometry(Part.Circle(V(x, z, 0), V(0, 0, 1), r))
        self.Doc.recompute()
        return sketch

    def testProfileRectangle(self):
        """A rectangle as wide as the edge and 10 high is the plain flange,
        OuterSharp 10; the values it takes the place of are read only"""
        sketch = self.legSketch([(0, 0), (40, 0), (40, 10), (0, 10)])
        flange = self.far(Profile=sketch)
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 18 + 40 * RING + 40 * 8, places=6)
        box = flange.Shape.BoundBox
        self.assertAlmostEqual(box.YMax, 20)
        self.assertAlmostEqual(box.ZMax, 10)
        self.assertNotIn(None, self.owners(flange))
        for name in ("Length", "LengthType", "Gap1", "Gap2"):
            self.assertIn("ReadOnly", flange.getPropertyStatus(name), name)
        flange.Profile = None
        self.Doc.recompute()
        for name in ("Length", "LengthType", "Gap1", "Gap2"):
            self.assertNotIn("ReadOnly", flange.getPropertyStatus(name), name)

    def testProfileNarrower(self):
        """A profile narrower than the edge is the flange with gaps, a
        relief each side, and lays flat"""
        sketch = self.legSketch([(10, 0), (30, 0), (30, 10), (10, 10)])
        flange = self.far(Profile=sketch)
        self.checkValid(flange)
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 20 * 2 - 2 * RELIEF + 20 * RING + 20 * 8, places=6
        )
        self.checkFlat(flange, 40, 18 + 1.5 * math.pi / 2 + 8)
        box = flange.Shape.BoundBox
        self.assertAlmostEqual(box.XMin, 0)
        self.assertAlmostEqual(box.XMax, 40)
        # the bend spans the profile only
        bend = [e for e in flange.SheetModel["Elements"] if e["Name"] == "Bend1"]
        self.assertEqual(len(bend), 1)

    def testProfileShape(self):
        """A tapered leg with a hole: the bend the edge's width, the leg a
        trapezoid from z = 2, 40 wide there and 20 at z = 10, a hole of
        radius 2 in it; edited, the flange follows; the flat has the hole"""
        sketch = self.legSketch(
            [(0, 0), (40, 0), (40, 2), (30, 10), (10, 10), (0, 2)], holes=[(20, 6, 2)]
        )
        flange = self.far(Profile=sketch)
        self.checkValid(flange)
        leg = (40 + 20) / 2 * 8 - math.pi * 4
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 18 + 40 * RING + leg, places=6)
        self.assertNotIn(None, self.owners(flange))
        flat = self.flat(flange)
        self.assertNotIn("Invalid", flat.State)
        self.assertTrue(flat.Shape.isValid())
        circles = [
            e
            for e in flat.Shape.Edges
            if isinstance(e.Curve, Part.Circle) and abs(e.Curve.Radius - 2) < 1e-6
        ]
        self.assertTrue(circles)
        # the hole moved up the leg in the sketch moves in the flange
        sketch.delGeometry(6)
        sketch.addGeometry(Part.Circle(V(20, 7, 0), V(0, 0, 1), 1))
        self.Doc.recompute()
        self.checkValid(flange)
        leg = (40 + 20) / 2 * 8 - math.pi
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 18 + 40 * RING + leg, places=6)

    def testProfileWiderLeg(self):
        """A leg wider than its bend above it: the bend x 10..30, the leg
        x 5..35 from z = 4"""
        sketch = self.legSketch(
            [(10, 0), (30, 0), (30, 4), (35, 4), (35, 10), (5, 10), (5, 4), (10, 4)]
        )
        flange = self.far(Profile=sketch)
        self.checkValid(flange)
        leg = 20 * 2 + 30 * 6
        self.assertAlmostEqual(
            flange.Shape.Volume, 40 * 20 - 20 * 2 - 2 * RELIEF + 20 * RING + leg, places=6
        )

    def testProfileOffPlane(self):
        """The sketch may be anywhere parallel to the leg: it is taken onto
        the leg's plane"""
        sketch = self.legSketch(
            [(0, 0), (40, 0), (40, 10), (0, 10)],
            plane=FreeCAD.Placement(V(0, 35, 0), FreeCAD.Rotation(V(1, 0, 0), 90)),
        )
        flange = self.far(Profile=sketch)
        self.checkValid(flange)
        self.assertAlmostEqual(flange.Shape.Volume, 40 * 18 + 40 * RING + 40 * 8, places=6)

    def testProfileErrors(self):
        """Refused: a sketch not parallel to the leg, one not standing on the
        sharp, one reaching past it, one narrowing within the bend, an open
        one, a profile on two edges, a profile with UpToFace"""
        flange = self.far()
        cases = {
            "not parallel": self.legSketch(
                [(0, 0), (40, 0), (40, 10), (0, 10)],
                plane=FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation()),
                name="Flat",
            ),
            "off the sharp": self.legSketch([(0, 1), (40, 1), (40, 10), (0, 10)], name="Up"),
            "past the sharp": self.legSketch([(0, -1), (40, -1), (40, 10), (0, 10)], name="Down"),
            "narrowing": self.legSketch(
                [(0, 0), (40, 0), (40, 1), (30, 1.5), (30, 10), (10, 10), (10, 1.5), (0, 1)],
                name="Narrow",
            ),
            "hole in the bend": self.legSketch(
                [(0, 0), (40, 0), (40, 10), (0, 10)], holes=[(20, 1, 0.5)], name="Holed"
            ),
            "only the bend": self.legSketch([(0, 0), (40, 0), (40, 2), (0, 2)], name="Short"),
        }
        for what, sketch in cases.items():
            flange.Profile = sketch
            self.Doc.recompute()
            self.assertIn("Invalid", flange.State, what)
        opened = self.Body.newObject("Sketcher::SketchObject", "Open")
        opened.Placement = FreeCAD.Placement(V(0, 20, 0), FreeCAD.Rotation(V(1, 0, 0), 90))
        opened.addGeometry(Part.LineSegment(V(0, 0, 0), V(0, 10, 0)))
        opened.addGeometry(Part.LineSegment(V(0, 10, 0), V(40, 10, 0)))
        opened.addGeometry(Part.LineSegment(V(40, 10, 0), V(40, 0, 0)))
        self.Doc.recompute()
        flange.Profile = opened
        self.Doc.recompute()
        self.assertIn("Invalid", flange.State, "open")

        good = self.legSketch([(0, 0), (40, 0), (40, 10), (0, 10)], name="Good")
        flange.Profile = good
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        flange.Edges = (
            self.Plate,
            [
                self.edge(self.Plate, (0, 20, 1), (40, 20, 1)),
                self.edge(self.Plate, (0, 0, 1), (40, 0, 1)),
            ],
        )
        self.Doc.recompute()
        self.assertIn("Invalid", flange.State, "two edges")
        flange.Edges = (self.Plate, [self.edge(self.Plate, (0, 20, 1), (40, 20, 1))])
        flange.UpToFace = (self.Plate, ["Face1"])
        self.Doc.recompute()
        self.assertIn("Invalid", flange.State, "UpToFace")

    def testProfileSaveAndRestore(self):
        sketch = self.legSketch(
            [(0, 0), (40, 0), (40, 2), (30, 10), (10, 10), (0, 2)], holes=[(20, 6, 2)]
        )
        flange = self.far(Profile=sketch)
        volume = flange.Shape.Volume
        path = os.path.join(tempfile.mkdtemp(), "flange-profile.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        flange = self.Doc.getObject("Flange")
        self.assertEqual(flange.Profile[0].Name, "LegSketch")
        self.assertIn("ReadOnly", flange.getPropertyStatus("Length"))
        flange.touch()
        self.Doc.recompute()
        self.assertAlmostEqual(flange.Shape.Volume, volume, places=6)
