# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's corner closure, PartDesign::SheetMetalCornerClosure
# (docs/SheetMetal.md sec 5.2, ruled 2026-11-02): two legs meeting at a
# corner, left open, carried on to close it. Butt carries both on to the
# plane halving the corner, the rule's gap between (the automatic miter's
# corner); Overlap carries the first leg's bend and leg on to the second
# leg's outer skin, the second stopping the gap short of the first's inner
# skin and its bend the gap short of where the first's bend starts;
# Underlap the other way round. On a plate 40 x 20, 1 thick, with the
# rule's 1 mm radius: a 90 deg bend is a quarter ring, 3 pi / 4 in section,
# a flange's leg 8 straight.

import math
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector
RING = 3 * math.pi / 4
FAR = ((0, 20, 1), (40, 20, 1))
RIGHT = ((40, 0, 1), (40, 20, 1))


def build(doc, far_gaps, right_gaps):
    """A plate in doc, a flange on its far edge, one on its right edge, the
    gaps given as the distance kept from each end: (at x 0, at x 40) and
    (at y 0, at y 20)"""
    body = doc.addObject("PartDesign::Body", "Body")
    sketch = body.newObject("Sketcher::SketchObject", "Sketch")
    pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
    for i in range(4):
        a, b = pts[i], pts[(i + 1) % 4]
        sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
    plate = body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
    plate.Profile = sketch
    doc.recompute()
    base = plate
    flanges = []
    for name, (a, b), gaps in (("Far", FAR, far_gaps), ("Right", RIGHT, right_gaps)):
        flange = body.newObject("PartDesign::SheetMetalFlange", name)
        sub = edgeOf(base, a, b)
        flange.Edges = (base, [sub])
        # Gap1 is at the edge's first vertex
        first = base.getSubObject(sub).Vertexes[0].Point
        flipped = not first.isEqual(V(*a), 1e-7)
        flange.Gap1, flange.Gap2 = (gaps[1], gaps[0]) if flipped else gaps
        doc.recompute()
        flanges.append(flange)
        base = flange
    return body, flanges


def edgeOf(feature, a, b):
    """The edge of a feature's shape on the line from a to b, the longest"""
    best, length = None, 0
    pa, pb = V(*a), V(*b)
    along = (pb - pa).normalize()
    for i, e in enumerate(feature.Shape.Edges):
        if len(e.Vertexes) != 2 or not hasattr(e.Curve, "Direction"):
            continue
        p, q = e.Vertexes[0].Point, e.Vertexes[1].Point
        on = all(
            ((v - pa) - along * (v - pa).dot(along)).Length < 1e-7 for v in (p, q)
        )
        if on and e.Length > length:
            best, length = "Edge%d" % (i + 1), e.Length
    if best is None:
        raise ValueError("no edge on %s %s" % (a, b))
    return best


class TestSheetMetalCornerClosure(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalCornerClosure")
        # the corner at (40, 20) left open: each flange 5 short of it, and of
        # its other end
        self.Body, (self.Far, self.Right) = build(self.Doc, (5, 5), (5, 5))

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def legEnd(self, feature, axis, at):
        """The face of a leg's end, across the leg at that coordinate"""
        found = []
        for i, f in enumerate(feature.Shape.Faces):
            box = f.BoundBox
            lo, hi = (box.XMin, box.XMax) if axis == "x" else (box.YMin, box.YMax)
            if abs(lo - at) < 1e-7 and abs(hi - at) < 1e-7 and box.ZMax > 9:
                found.append("Face%d" % (i + 1))
        self.assertEqual(len(found), 1, found)
        return found[0]

    def close(self, **props):
        base = self.Right
        closure = self.Body.newObject("PartDesign::SheetMetalCornerClosure", "Closure")
        closure.Faces = (base, [self.legEnd(base, "x", 35), self.legEnd(base, "y", 15)])
        for k, v in props.items():
            setattr(closure, k, v)
        self.Doc.recompute()
        return closure

    def checkValid(self, feature):
        self.assertNotIn("Invalid", feature.State)
        self.assertTrue(feature.Shape.isValid())
        self.assertEqual(len(feature.Shape.Solids), 1)
        self.assertNotIn(
            None,
            [feature.sheetElementOf("Face%d" % (i + 1)) for i in range(len(feature.Shape.Faces))],
        )

    def flat(self, feature):
        flat = self.Body.newObject("PartDesign::SheetMetalFlatPattern", "Flat")
        flat.Source = feature
        self.Doc.recompute()
        self.assertNotIn("Invalid", flat.State)
        self.assertEqual(len(flat.Shape.Solids), 1)
        self.assertTrue(flat.Shape.Solids[0].isValid())
        self.assertEqual(flat.BendCount, 2)
        return flat

    def testOpen(self):
        """The corner as the gaps leave it: open, neither leg near it"""
        shape = self.Right.Shape
        self.assertFalse(shape.isInside(V(37, 19.5, 5), 1e-9, True))
        self.assertFalse(shape.isInside(V(39.5, 17, 5), 1e-9, True))

    def testButt(self):
        """Butt, the default: the corner the two flanges make when the
        later one is mitered against the earlier (gaps at their far ends
        only), but for the reliefs the gaps cut, which stay below where the
        bends start, 1 x 1 each; the same flat"""
        closure = self.close()
        self.assertEqual(closure.CornerType, "Butt")
        self.checkValid(closure)
        doc = FreeCAD.newDocument("PartDesignTestSheetMetalCornerClosureReference")
        try:
            body, (far, right) = build(doc, (5, 0), (5, 0))
            volume = right.Shape.Volume
            flat = body.newObject("PartDesign::SheetMetalFlatPattern", "Flat")
            flat.Source = right
            doc.recompute()
            size = flat.FlatLength.Value, flat.FlatWidth.Value
        finally:
            FreeCAD.closeDocument(doc.Name)
        self.assertAlmostEqual(closure.Shape.Volume, volume - 2, places=6)
        shape = closure.Shape
        for x in (38.6, 39.0, 39.5, 39.96):
            self.assertFalse(shape.isInside(V(x, x - 20, 5), 1e-9, True), x)
        self.assertTrue(shape.isInside(V(38.5, 19.5, 5), 1e-9, True))
        self.assertTrue(shape.isInside(V(39.5, 18.5, 5), 1e-9, True))
        flat = self.flat(closure)
        self.assertAlmostEqual(flat.FlatLength.Value, size[0], places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, size[1], places=6)

    # the plate left: 40 x 20, less y 18..20 from x 5 (the far flange's cut
    # and the corner's), x 38..40 from y 5 to 18 (the right one's), the two
    # reliefs at the far ends (3 each) and what the corner's leave below
    # where the bends start (1 each)
    PLATE = 800 - 35 * 2 - 13 * 2 - 2 * 3 - 2 * 1

    def testOverlap(self):
        """The far leg over the right one's end: the far bend and leg on to
        x 40; the right leg to y 18.9, its bend to y 17.9"""
        closure = self.close(CornerType="Overlap")
        self.checkValid(closure)
        shape = closure.Shape
        self.assertAlmostEqual(
            shape.Volume,
            self.PLATE + 35 * (RING + 8) + 12.9 * RING + 13.9 * 8,
            places=6,
        )
        self.assertTrue(shape.isInside(V(39.95, 19.5, 5), 1e-9, True))
        self.assertTrue(shape.isInside(V(39.5, 18.8, 5), 1e-9, True))
        self.assertFalse(shape.isInside(V(39.5, 18.95, 5), 1e-9, True))
        self.assertFalse(shape.isInside(V(39.5, 17.95, 1.5), 1e-9, True))
        flat = self.flat(closure)
        self.assertAlmostEqual(flat.Shape.Volume, shape.Volume, places=4)

    def testUnderlap(self):
        """The right leg over the far one's end: the right bend and leg on to
        y 20; the far leg to x 38.9, its bend to x 37.9"""
        closure = self.close(CornerType="Underlap")
        self.checkValid(closure)
        shape = closure.Shape
        self.assertAlmostEqual(
            shape.Volume,
            self.PLATE + 15 * (RING + 8) + 32.9 * RING + 33.9 * 8,
            places=6,
        )
        self.assertTrue(shape.isInside(V(39.5, 19.95, 5), 1e-9, True))
        self.assertTrue(shape.isInside(V(38.8, 19.5, 5), 1e-9, True))
        self.assertFalse(shape.isInside(V(38.95, 19.5, 5), 1e-9, True))
        flat = self.flat(closure)
        self.assertAlmostEqual(flat.Shape.Volume, shape.Volume, places=4)

    def testOverlapOnMiter(self):
        """A corner the automatic miter made (gaps at the far ends only),
        its legs' slanted ends picked: Overlap as on the open corner, with
        no reliefs at the corner to leave"""
        doc = FreeCAD.newDocument("PartDesignTestSheetMetalCornerClosureMiter")
        try:
            body, (far, right) = build(doc, (5, 0), (5, 0))
            ends = [
                "Face%d" % (i + 1)
                for i, f in enumerate(right.Shape.Faces)
                if f.BoundBox.ZMax > 9
                and f.BoundBox.XMin > 38.9
                and f.BoundBox.YMin > 18.9
                and f.BoundBox.XLength > 0.5
                and f.BoundBox.YLength > 0.5
            ]
            self.assertEqual(len(ends), 2, ends)
            # the far leg's first
            ends.sort(key=lambda n: -right.Shape.getElement(n).BoundBox.Center.y)
            closure = body.newObject("PartDesign::SheetMetalCornerClosure", "Closure")
            closure.Faces = (right, ends)
            closure.CornerType = "Overlap"
            doc.recompute()
            self.checkValid(closure)
            self.assertAlmostEqual(
                closure.Shape.Volume,
                self.PLATE + 2 + 35 * (RING + 8) + 12.9 * RING + 13.9 * 8,
                places=6,
            )
            flat = body.newObject("PartDesign::SheetMetalFlatPattern", "Flat")
            flat.Source = closure
            doc.recompute()
            self.assertNotIn("Invalid", flat.State)
            self.assertAlmostEqual(flat.Shape.Volume, closure.Shape.Volume, places=4)
        finally:
            FreeCAD.closeDocument(doc.Name)

    def testRefused(self):
        """Two faces, each a leg's end; the plate's skin is not one"""
        closure = self.Body.newObject("PartDesign::SheetMetalCornerClosure", "Closure")
        top = [
            "Face%d" % (i + 1)
            for i, f in enumerate(self.Right.Shape.Faces)
            if abs(f.BoundBox.ZMin - 1) < 1e-7 and abs(f.BoundBox.ZMax - 1) < 1e-7
        ][0]
        closure.Faces = (self.Right, [self.legEnd(self.Right, "x", 35), top])
        self.Doc.recompute()
        self.assertIn("Invalid", closure.State)
        closure.Faces = (self.Right, [self.legEnd(self.Right, "x", 35)])
        self.Doc.recompute()
        self.assertIn("Invalid", closure.State)
