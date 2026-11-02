# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's cutout, PartDesign::SheetMetalCutout (docs/SheetMetal.md sec
# 5.2, ruled 2026-11-02): a closed sketch cut normal to the sheet on every
# wall and bend it crosses. What the sketch's prism covers on the skin
# facing it is laid flat and cut through the sheet from there, a wall's
# straight through, a bend's along its radius, so the hole is what a laser
# cuts in the blank. Through all by default, UpToFirst the first wall or
# bend met only. On a plate 40 x 20, 1 thick, with the rule's 1 mm radius
# and K 0.5: a flange on the far edge bends up about the axis y 18, z 2,
# its inner skin a quarter of radius 1, its outer of 2, 3 pi / 4 in section
# and pi / 2 * 1.5 long in the flat; its leg y 19 to 20, z 2 to 10.

import math
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector
RING = 3 * math.pi / 4
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


class TestSheetMetalCutout(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalCutout")
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

    def sketch(self, placement, rects=(), circles=()):
        """A sketch at placement, its rectangles (x0, y0, x1, y1) and circles
        (x, y, r) given in the document's frame on its plane"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "Profile")
        sketch.MapMode = "Deactivated"
        sketch.Placement = placement
        inv = placement.inverse()
        for rect in rects:
            pts = [inv.multVec(V(*p)) for p in rect]
            for i in range(len(pts)):
                sketch.addGeometry(Part.LineSegment(pts[i], pts[(i + 1) % len(pts)]))
        for c, r in circles:
            sketch.addGeometry(
                Part.Circle(inv.multVec(V(*c)), V(0, 0, 1), r)
            )
        self.Doc.recompute()
        return sketch

    def cut(self, sketch, **props):
        cutout = self.Body.newObject("PartDesign::SheetMetalCutout", "Cutout")
        cutout.Profile = sketch
        for k, v in props.items():
            setattr(cutout, k, v)
        self.Doc.recompute()
        return cutout

    def checkValid(self, feature):
        self.assertNotIn("Invalid", feature.State)
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
        return flat, flat.FlatArea.Value

    def outside(self, y):
        """Looking at the part from y, toward it: the sketch's normal away
        from it"""
        rot = FreeCAD.Rotation(V(1, 0, 0), -90 if y > 10 else 90)
        return FreeCAD.Placement(V(0, y, 0), rot)

    def testPlateHole(self):
        """A circle on the plate's top, through it: the same circle in the
        flat"""
        sketch = self.sketch(FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation()),
                             circles=[((10, 10, 1), 3)])
        _, before = self.flatArea(self.Plate)
        cutout = self.cut(sketch)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, 800 - 9 * math.pi, places=5)
        flat, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - 9 * math.pi, places=4)
        circles = [
            e for e in flat.Shape.Solids[0].Edges
            if isinstance(e.Curve, Part.Circle) and abs(e.Curve.Radius - 3) < 1e-7
        ]
        self.assertEqual(len(circles), 2)

    def testAcrossBendOuter(self):
        """From outside the far flange, across its leg and bend: the leg's
        outer skin and the bend's outer one face the sketch; the bend goes
        whole along its radius, the leg up to z 6"""
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        _, before = self.flatArea(self.Tip)
        sketch = self.sketch(self.outside(30),
                             rects=[[(10, 30, -1), (20, 30, -1), (20, 30, 6), (10, 30, 6)]])
        cutout = self.cut(sketch)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 10 * (RING + 4), places=5)
        shape = cutout.Shape
        self.assertFalse(shape.isInside(V(15, 19.5, 5), 1e-9, True))
        self.assertTrue(shape.isInside(V(15, 19.5, 7), 1e-9, True))
        self.assertTrue(shape.isInside(V(15, 17, 0.5), 1e-9, True))
        _, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - 10 * (STRIP + 4), places=4)

    def testAcrossBendPartly(self):
        """Up to z 1 only on the bend's outer skin: the bend cut along its
        radius from where it starts to the angle that reaches z 1 outside,
        60 deg, through the sheet; the flat's hole that much of the strip"""
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        _, before = self.flatArea(self.Tip)
        sketch = self.sketch(self.outside(30),
                             rects=[[(10, 30, -1), (20, 30, -1), (20, 30, 1), (10, 30, 1)]])
        cutout = self.cut(sketch)
        self.checkValid(cutout)
        sector = math.pi / 3 / 2 * (4 - 1)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 10 * sector, places=5)
        shape = cutout.Shape

        def at(angle):
            return V(15, 18 + 1.5 * math.sin(angle), 2 - 1.5 * math.cos(angle))

        self.assertFalse(shape.isInside(at(math.pi / 3 - 0.05), 1e-9, True))
        self.assertTrue(shape.isInside(at(math.pi / 3 + 0.05), 1e-9, True))
        _, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - 10 * math.pi / 3 * 1.5, places=4)

    def testAcrossBendInner(self):
        """From above the plate, across the far bend: the plate's top and
        the bend's inner skin face the sketch, the leg stands edge on; the
        plate goes to where the bend starts, the bend whole, the leg stays"""
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        _, before = self.flatArea(self.Tip)
        sketch = self.sketch(FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation()),
                             rects=[[(10, 15, 1), (20, 15, 1), (20, 25, 1), (10, 25, 1)]])
        cutout = self.cut(sketch)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 10 * (3 + RING), places=5)
        shape = cutout.Shape
        self.assertTrue(shape.isInside(V(15, 19.5, 5), 1e-9, True))
        self.assertFalse(shape.isInside(V(15, 16, 0.5), 1e-9, True))
        _, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - 10 * (3 + STRIP), places=4)

    def twoLegs(self):
        self.flange("Near", NEAR)
        self.flange("Far", FAR)
        return self.sketch(self.outside(-10),
                           rects=[[(10, -10, 4), (20, -10, 4), (20, -10, 8), (10, -10, 8)]])

    def testThroughAll(self):
        """Across a U channel, both legs"""
        sketch = self.twoLegs()
        volume = self.Tip.Shape.Volume
        cutout = self.cut(sketch)
        self.checkValid(cutout)
        self.assertFalse(cutout.UpToFirst)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 80, places=5)
        self.assertFalse(cutout.Shape.isInside(V(15, 0.5, 6), 1e-9, True))
        self.assertFalse(cutout.Shape.isInside(V(15, 19.5, 6), 1e-9, True))
        self.flatArea(cutout)

    def testUpToFirst(self):
        """Across a U channel, the first leg only"""
        sketch = self.twoLegs()
        volume = self.Tip.Shape.Volume
        cutout = self.cut(sketch, UpToFirst=True)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 40, places=5)
        self.assertFalse(cutout.Shape.isInside(V(15, 0.5, 6), 1e-9, True))
        self.assertTrue(cutout.Shape.isInside(V(15, 19.5, 6), 1e-9, True))

    def testOblique(self):
        """A sketch tilted 30 deg over the plate: taken onto the top along
        the cut's way, an ellipse 3 by 3 / cos 30 about y 12.309, then
        straight through the sheet, not on along the slant as a pocket"""
        rot = FreeCAD.Rotation(V(1, 0, 0), 30)
        placement = FreeCAD.Placement(V(20, 10, 5), rot)
        sketch = self.body_sketch_circle(placement, 3)
        cutout = self.cut(sketch)
        self.checkValid(cutout)
        b = 3 / math.cos(math.radians(30))
        self.assertAlmostEqual(cutout.Shape.Volume, 800 - math.pi * 3 * b, places=5)
        cy = 10 + 4 * math.tan(math.radians(30))
        shape = cutout.Shape
        for y in (cy + b - 0.3, cy - b + 0.3):
            for z in (0.1, 0.9):
                self.assertFalse(shape.isInside(V(20, y, z), 1e-9, True), (y, z))
        self.assertTrue(shape.isInside(V(20, cy - b - 0.1, 0.1), 1e-9, True))
        _, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, 800 - math.pi * 3 * b, places=4)

    def body_sketch_circle(self, placement, r):
        sketch = self.Body.newObject("Sketcher::SketchObject", "Profile")
        sketch.MapMode = "Deactivated"
        sketch.Placement = placement
        sketch.addGeometry(Part.Circle(V(0, 0, 0), V(0, 0, 1), r))
        self.Doc.recompute()
        return sketch

    def testRefused(self):
        """Turned away from the sheet, nothing to cut; an open sketch"""
        sketch = self.sketch(FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation()),
                             circles=[((10, 10, 1), 3)])
        cutout = self.cut(sketch, Reversed=True)
        self.assertIn("Invalid", cutout.State)
        line = self.Body.newObject("Sketcher::SketchObject", "Line")
        line.MapMode = "Deactivated"
        line.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        line.addGeometry(Part.LineSegment(V(5, 5, 0), V(15, 5, 0)))
        cutout.Profile = line
        cutout.Reversed = False
        self.Doc.recompute()
        self.assertIn("Invalid", cutout.State)
