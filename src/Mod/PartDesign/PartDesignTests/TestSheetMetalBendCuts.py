# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's hinge and perforation cuts, PartDesign::SheetMetalBendCuts
# (docs/SheetMetal.md sec 5.2, ruled 2026-11-03): cuts across picked bends,
# drawn in each bend's strip in the flat and folded back, so the flat shows
# them as cut. Slots: one row of slots across the strip, SlotWidth 0 the
# whole strip, bridges between, EndMargin left whole at each end, the slots
# that fit centred. Staggered: Rows rows across the strip, every other one
# shifted half a slot and a bridge, its end slots cut short by the margins.
# Sketch: the closed shapes of a sketch, X along the bend and Y across it
# from its middle, tiled along it as the slots are. The bend stays a bend.
#
# On a plate 40 x 20, 1 thick, the rule's radius 1 and K 0.5: a flange on
# the far edge bends about the axis y 18, z 2, x 0 to 40; its strip is
# pi / 2 * 1.5 long in the flat, and a piece of it w wide across and l long
# is w * l of the sheet, in the flat and in volume.

import math
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector
STRIP = math.pi / 2 * 1.5
FAR = ((0, 20, 1), (40, 20, 1))
NEAR = ((0, 0, 1), (40, 0, 1))


def edgeOf(feature, a, b):
    """The edge of a feature's shape on the line from a to b, the longest"""
    best, length = None, 0
    pa, pb = V(*a), V(*b)
    along = (pb - pa).normalize()
    for i, e in enumerate(feature.Shape.Edges):
        if len(e.Vertexes) != 2 or not hasattr(e.Curve, "Direction"):
            continue
        p, q = e.Vertexes[0].Point, e.Vertexes[1].Point
        on = all(((v - pa) - along * (v - pa).dot(along)).Length < 1e-7 for v in (p, q))
        if on and e.Length > length:
            best, length = "Edge%d" % (i + 1), e.Length
    if best is None:
        raise ValueError("no edge on %s %s" % (a, b))
    return best


def bendAt(angle, x, y0=18, r=1.5):
    """A point in the far bend's middle fibre, angle from where it starts"""
    return V(x, y0 + r * math.sin(angle), 2 - r * math.cos(angle))


class TestSheetMetalBendCuts(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalBendCuts")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()
        self.Tip = self.Plate

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def flange(self, name, edge):
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", name)
        flange.Edges = (self.Tip, [edgeOf(self.Tip, *edge)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", flange.State)
        self.Tip = flange
        return flange

    def bendFaces(self, feature, *names):
        """A face of each named bend of the feature's sheet"""
        res = []
        for name in names:
            for i in range(len(feature.Shape.Faces)):
                if feature.sheetElementOf("Face%d" % (i + 1)) == name:
                    res.append("Face%d" % (i + 1))
                    break
        self.assertEqual(len(res), len(names))
        return res

    def cuts(self, faces, **props):
        cuts = self.Body.newObject("PartDesign::SheetMetalBendCuts", "BendCuts")
        cuts.Bends = (self.Tip, faces)
        for k, v in props.items():
            setattr(cuts, k, v)
        self.Doc.recompute()
        return cuts

    def checkValid(self, feature):
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isValid())
        self.assertEqual(len(feature.Shape.Solids), 1)
        self.assertNotIn(
            None,
            [feature.sheetElementOf("Face%d" % (i + 1)) for i in range(len(feature.Shape.Faces))],
        )

    def flatArea(self, feature):
        flat = self.Body.newObject("PartDesign::SheetMetalFlatPattern", "Flat")
        flat.Source = feature
        self.Doc.recompute()
        self.assertNotIn("Invalid", flat.State)
        self.assertEqual(len(flat.Shape.Solids), 1)
        self.assertTrue(flat.Shape.Solids[0].isValid())
        return flat.FlatArea.Value

    def checkCut(self, cuts, removed):
        """The cuts valid, the part removed of that much, the flat too"""
        self.checkValid(cuts)
        self.assertAlmostEqual(cuts.Shape.Volume, self.volume - removed, places=5)
        self.assertAlmostEqual(self.flatArea(cuts), self.area - removed, places=4)

    def far(self):
        self.flange("Far", FAR)
        self.volume = self.Tip.Shape.Volume
        self.area = self.flatArea(self.Tip)
        return self.bendFaces(self.Tip, "Bend1")

    def testSlots(self):
        """The defaults, 10 long, 2 between, 2 left at each end: 36 along,
        three slots 34 long centred, from x 3; across the whole strip"""
        cuts = self.cuts(self.far())
        self.assertEqual(cuts.Pattern, "Slots")
        self.checkCut(cuts, 30 * STRIP)
        shape = cuts.Shape
        for x in (4, 20, 36):
            self.assertFalse(shape.isInside(bendAt(math.pi / 4, x), 1e-9, True), x)
        for x in (2.5, 14, 26, 37.5):
            self.assertTrue(shape.isInside(bendAt(math.pi / 4, x), 1e-9, True), x)
        # the walls stay whole
        self.assertTrue(shape.isInside(V(20, 17.5, 0.5), 1e-9, True))
        self.assertTrue(shape.isInside(V(20, 19.5, 2.5), 1e-9, True))

    def testSlotsFit(self):
        """8 long, 2 between, 1 at each end: four fill the 38 exactly; 7
        long, four again, the 4 left over shared between the ends"""
        faces = self.far()
        cuts = self.cuts(faces, SlotLength=8, BridgeLength=2, EndMargin=1)
        self.checkCut(cuts, 32 * STRIP)
        for x in (1.5, 8.5, 31.5):
            self.assertFalse(cuts.Shape.isInside(bendAt(math.pi / 4, x), 1e-9, True), x)
        self.assertTrue(cuts.Shape.isInside(bendAt(math.pi / 4, 10), 1e-9, True))
        cuts.SlotLength = 7
        self.Doc.recompute()
        self.checkCut(cuts, 28 * STRIP)
        for x in (2.5, 11, 37.5):
            self.assertTrue(cuts.Shape.isInside(bendAt(math.pi / 4, x), 1e-9, True), x)
        for x in (3.5, 9.5, 36.5):
            self.assertFalse(cuts.Shape.isInside(bendAt(math.pi / 4, x), 1e-9, True), x)

    def testSlotWidth(self):
        """1 wide across the strip, about its middle: the bend's ends whole"""
        cuts = self.cuts(self.far(), SlotLength=8, BridgeLength=2, EndMargin=1, SlotWidth=1)
        self.checkCut(cuts, 32)
        shape = cuts.Shape
        self.assertFalse(shape.isInside(bendAt(math.pi / 4, 5), 1e-9, True))
        # 0.6 along the strip from either end is outside the slot's 1
        a = 0.6 / 1.5
        self.assertTrue(shape.isInside(bendAt(a, 5), 1e-9, True))
        self.assertTrue(shape.isInside(bendAt(math.pi / 2 - a, 5), 1e-9, True))

    def testStaggered(self):
        """Three rows 0.3 wide across the strip, at a sixth, a half and five
        sixths; the middle one shifted 5, its end slots cut short to 3"""
        cuts = self.cuts(self.far(), Pattern="Staggered", SlotLength=8, BridgeLength=2,
                         EndMargin=1, SlotWidth=0.3, Rows=3)
        self.checkCut(cuts, (32 + 30 + 32) * 0.3)
        shape = cuts.Shape
        row = [(i + 0.5) / 3 * math.pi / 2 for i in range(3)]
        # the outer rows cut at x 5, the middle one bridged there
        self.assertFalse(shape.isInside(bendAt(row[0], 5), 1e-9, True))
        self.assertFalse(shape.isInside(bendAt(row[2], 5), 1e-9, True))
        self.assertTrue(shape.isInside(bendAt(row[1], 5), 1e-9, True))
        # at x 10 the other way round; the middle row's end slots 1 to 4
        self.assertTrue(shape.isInside(bendAt(row[0], 10), 1e-9, True))
        self.assertFalse(shape.isInside(bendAt(row[1], 10), 1e-9, True))
        self.assertFalse(shape.isInside(bendAt(row[1], 2), 1e-9, True))
        self.assertTrue(shape.isInside(bendAt(row[1], 4.5), 1e-9, True))
        # between the rows the bend is whole
        self.assertTrue(shape.isInside(bendAt(math.pi / 6, 5), 1e-9, True))

    def testStaggeredWidth(self):
        """No width given: half of each row's share of the strip"""
        cuts = self.cuts(self.far(), Pattern="Staggered", SlotLength=8, BridgeLength=2,
                         EndMargin=1, Rows=2)
        self.checkCut(cuts, (32 + 30) * STRIP / 4)

    def testSketch(self):
        """A circle 0.8 across tiled: 1.6 long, 2 between, 1 at each end,
        eleven of them centred in the 38"""
        faces = self.far()
        tile = self.Body.newObject("Sketcher::SketchObject", "Tile")
        tile.MapMode = "Deactivated"
        tile.addGeometry(Part.Circle(V(7, 0.1, 0), V(0, 0, 1), 0.8))
        self.Doc.recompute()
        cuts = self.cuts(faces, Pattern="Sketch", Tile=tile, BridgeLength=2, EndMargin=1)
        self.checkCut(cuts, 11 * math.pi * 0.64)
        # the first from x 1.2, its middle 2 on; 0.1 past the strip's middle
        self.assertFalse(cuts.Shape.isInside(bendAt(math.pi / 4 + 0.1 / 1.5, 2), 1e-9, True))
        self.assertTrue(cuts.Shape.isInside(bendAt(math.pi / 4, 3.9), 1e-9, True))

    def testTwoBends(self):
        """Both bends of a U channel, each the same"""
        self.flange("Near", NEAR)
        self.flange("Far", FAR)
        self.volume = self.Tip.Shape.Volume
        self.area = self.flatArea(self.Tip)
        cuts = self.cuts(self.bendFaces(self.Tip, "Bend1", "Bend2"))
        self.checkCut(cuts, 60 * STRIP)
        self.assertFalse(cuts.Shape.isInside(bendAt(math.pi / 4, 20), 1e-9, True))
        near = V(20, 2 - 1.5 * math.sin(math.pi / 4), 2 - 1.5 * math.cos(math.pi / 4))
        self.assertFalse(cuts.Shape.isInside(near, 1e-9, True))

    def testRefused(self):
        """A wall's face; slots too long for the bend; a sketch pattern with
        no sketch"""
        faces = self.far()
        wall = [
            "Face%d" % (i + 1) for i in range(len(self.Tip.Shape.Faces))
            if self.Tip.sheetElementOf("Face%d" % (i + 1)) == "Wall1"
        ][:1]
        cuts = self.cuts(wall)
        self.assertIn("Invalid", cuts.State)
        self.assertIn("not a bend", cuts.getStatusString())
        cuts.Bends = (self.Tip, faces)
        cuts.SlotLength = 37
        self.Doc.recompute()
        self.assertIn("Invalid", cuts.State)
        self.assertIn("no whole slot", cuts.getStatusString())
        cuts.SlotLength = 10
        cuts.Pattern = "Sketch"
        self.Doc.recompute()
        self.assertIn("Invalid", cuts.State)
        self.assertIn("sketch", cuts.getStatusString())
        cuts.Pattern = "Slots"
        self.Doc.recompute()
        self.checkValid(cuts)
