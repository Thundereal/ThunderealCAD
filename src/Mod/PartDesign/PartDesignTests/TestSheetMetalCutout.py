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
#
# Wrap (ruled 2026-11-03): the sketch as if drawn on the flat. The wall
# parallel to the sketch and nearest it stays where it is, the sketch is
# taken onto it straight, and what lies on the flat goes back through every
# bend at its developed length; what lies off the flat cuts nothing. With
# the plate fixed, the far flange lies flat on from y 18: its bend to
# 18 + STRIP, its leg 8 on from there; the near one the same toward -y
# from y 2.

import math
import os
import tempfile
import unittest

import FreeCAD
import Part

from .SheetMetalFlatView import Flat, flatObject

V = FreeCAD.Vector
RING = 3 * math.pi / 4
STRIP = math.pi / 2 * 1.5
FAR = ((0, 20, 1), (40, 20, 1))
RIGHT = ((40, 0, 1), (40, 20, 1))
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
        self.Doc.recompute()
        flat = Flat(feature)
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
        """From above the part, across the far bend: the plate's top and
        the bend's inner skin face the sketch, the leg stands edge on; the
        plate goes to where the bend starts, the bend whole, the leg stays"""
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        _, before = self.flatArea(self.Tip)
        sketch = self.sketch(FreeCAD.Placement(V(0, 0, 11), FreeCAD.Rotation()),
                             rects=[[(10, 15, 11), (20, 15, 11), (20, 25, 11), (10, 25, 11)]])
        cutout = self.cut(sketch)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 10 * (3 + RING), places=5)
        shape = cutout.Shape
        self.assertTrue(shape.isInside(V(15, 19.5, 5), 1e-9, True))
        self.assertFalse(shape.isInside(V(15, 16, 0.5), 1e-9, True))
        _, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - 10 * (3 + STRIP), places=4)

    def testBehindSketch(self):
        """The same drawn on the plate's top: the bend rises behind the
        sketch, out of its way, and stays whole"""
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        sketch = self.sketch(FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation()),
                             rects=[[(10, 15, 1), (20, 15, 1), (20, 25, 1), (10, 25, 1)]])
        cutout = self.cut(sketch)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 30, places=5)
        self.assertTrue(cutout.Shape.isInside(V(15, 19.06, 0.94), 1e-9, True))
        self.flatArea(cutout)

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

    def top(self, z=1, flipped=False):
        """A sketch plane parallel to the plate at z, its normal up or down"""
        rot = FreeCAD.Rotation(V(1, 0, 0), 180) if flipped else FreeCAD.Rotation()
        return FreeCAD.Placement(V(0, 0, z), rot)

    def rect(self, x0, y0, x1, y1, z=1):
        return [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)]

    def testWrapAcrossBend(self):
        """Drawn on the flat across the far bend: 3 of the plate, the whole
        strip, the leg up to 25 - 18 - STRIP on from where it starts, 10
        wide; the flat's hole that rectangle"""
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        _, before = self.flatArea(self.Tip)
        sketch = self.sketch(self.top(), rects=[self.rect(10, 15, 20, 25)])
        cutout = self.cut(sketch, Wrap=True)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 100, places=5)
        shape = cutout.Shape
        leg = 2 + 25 - 18 - STRIP
        self.assertFalse(shape.isInside(V(15, 19.5, leg - 0.1), 1e-9, True))
        self.assertTrue(shape.isInside(V(15, 19.5, leg + 0.1), 1e-9, True))
        self.assertFalse(shape.isInside(V(15, 16, 0.5), 1e-9, True))
        self.assertTrue(shape.isInside(V(15, 14, 0.5), 1e-9, True))
        mid = math.pi / 4
        self.assertFalse(
            shape.isInside(V(15, 18 + 1.5 * math.sin(mid), 2 - 1.5 * math.cos(mid)), 1e-9, True))
        self.assertTrue(shape.isInside(V(21, 19.5, leg - 0.1), 1e-9, True))
        _, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - 100, places=4)

    def testWrapTwoBends(self):
        """Across a U channel's two bends from leg to leg, 30 long: the same
        hole in the flat as cut there directly, 10 by 30"""
        self.flange("Near", NEAR)
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        _, before = self.flatArea(self.Tip)
        sketch = self.sketch(self.top(), rects=[self.rect(10, -5, 20, 25)])
        cutout = self.cut(sketch, Wrap=True)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 300, places=5)
        shape = cutout.Shape
        # y -5 is 5 - 2 + 2 - STRIP on along the near leg, as 25 on the far
        leg = 2 + 25 - 18 - STRIP
        for y in (0.5, 19.5):
            self.assertFalse(shape.isInside(V(15, y, leg - 0.1), 1e-9, True))
            self.assertTrue(shape.isInside(V(15, y, leg + 0.1), 1e-9, True))
        flat, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - 300, places=4)
        # its skin on XY, the faces of its elements merged: one hole
        solid = flat.Shape.Solids[0]
        z = solid.BoundBox.ZMin
        faces = [
            f for f in solid.Faces
            if f.BoundBox.ZLength < 1e-7 and abs(f.BoundBox.ZMin - z) < 1e-7
        ]
        skin = faces[0].fuse(faces[1:]).removeSplitter()
        self.assertEqual(len(skin.Faces), 1)
        holes = [w.BoundBox for w in skin.Faces[0].Wires if not w.isSame(skin.Faces[0].OuterWire)]
        self.assertEqual(len(holes), 1)
        self.assertAlmostEqual(min(holes[0].XLength, holes[0].YLength), 10, places=5)
        self.assertAlmostEqual(max(holes[0].XLength, holes[0].YLength), 30, places=5)

    def testWrapCircleOnBend(self):
        """A circle 1 in radius drawn on the bend's middle line: wholly on
        the bend, it lies flat as that circle, pi of the blank, the hole's
        wall square to the flat all round, not twisted between its two skins"""
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        _, before = self.flatArea(self.Tip)
        sketch = self.sketch(self.top(), circles=[((20, 18 + STRIP / 2, 1), 1)])
        cutout = self.cut(sketch, Wrap=True)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - math.pi, places=6)
        flat, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - math.pi, places=6)
        walls = [f for f in flat.Shape.Solids[0].Faces if not isinstance(f.Surface, Part.Plane)]
        self.assertTrue(walls)
        for f in walls:
            u0, u1, v0, v1 = f.ParameterRange
            for i in range(41):
                u = u0 + (u1 - u0) * i / 40
                p, q = f.valueAt(u, v0), f.valueAt(u, v1)
                self.assertLess(math.hypot(p.x - q.x, p.y - q.y), 1e-6)

    def testWrapPlanes(self):
        """The same cut from the plate's top, its bottom, planes above and
        below it, and the top with the sketch turned over: taken onto the
        plate straight, the side it faces no matter"""
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        cutout = None
        for z, flipped in ((1, False), (0, False), (5, False), (-3, False), (1, True)):
            sketch = self.sketch(self.top(z, flipped), rects=[self.rect(10, 15, 20, 25, z)])
            if cutout is None:
                cutout = self.cut(sketch, Wrap=True)
            else:
                cutout.Profile = sketch
                self.Doc.recompute()
            self.checkValid(cutout)
            self.assertAlmostEqual(cutout.Shape.Volume, volume - 100, places=5, msg=(z, flipped))

    def testWrapOffOutline(self):
        """Over the plate's left end, the far bend and the leg: what lies
        off the flat, x below 0, cuts nothing; a notch 5 wide, 16 long"""
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        _, before = self.flatArea(self.Tip)
        sketch = self.sketch(self.top(), rects=[self.rect(-5, 10, 5, 26)])
        cutout = self.cut(sketch, Wrap=True)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - 80, places=5)
        shape = cutout.Shape
        leg = 2 + 26 - 18 - STRIP
        self.assertFalse(shape.isInside(V(2, 19.5, leg - 0.1), 1e-9, True))
        self.assertTrue(shape.isInside(V(2, 19.5, leg + 0.1), 1e-9, True))
        self.assertTrue(shape.isInside(V(6, 19.5, 5), 1e-9, True))
        _, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - 80, places=4)

    def testWrapNearestWall(self):
        """Parallel to both legs of a U channel, 5 beyond the far one: the
        far leg is the wall that stays, the circle a hole in it there"""
        self.flange("Near", NEAR)
        self.flange("Far", FAR)
        volume = self.Tip.Shape.Volume
        sketch = self.sketch(self.outside(25), circles=[((15, 25, 6), 1)])
        cutout = self.cut(sketch, Wrap=True)
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - math.pi, places=5)
        self.assertFalse(cutout.Shape.isInside(V(15, 19.5, 6), 1e-9, True))
        self.assertTrue(cutout.Shape.isInside(V(15, 0.5, 6), 1e-9, True))
        self.assertTrue(cutout.Shape.isInside(V(15, 19.5, 7.2), 1e-9, True))

    def testWrapRefused(self):
        """A sketch parallel to no wall; one wholly off the flat, past the
        far end, and one in the gap at the corner of two flanges, which
        their walls run on into folded"""
        self.flange("Far", FAR)
        rot = FreeCAD.Rotation(V(1, 0, 0), 30)
        cutout = self.cut(self.body_sketch_circle(FreeCAD.Placement(V(20, 10, 5), rot), 3),
                          Wrap=True)
        self.assertIn("Invalid", cutout.State)
        self.assertIn("parallel to no wall", cutout.getStatusString())
        cutout.Profile = self.sketch(self.top(), rects=[self.rect(10, 30, 20, 35)])
        self.Doc.recompute()
        self.assertIn("Invalid", cutout.State)
        self.assertIn("none of the flat", cutout.getStatusString())
        cutout.Wrap = False
        cutout.Profile = self.sketch(self.top(), circles=[((10, 10, 1), 3)])
        self.Doc.recompute()
        self.checkValid(cutout)

    # The sketch in the flat's own frame (WrapFrame Flat): laid as the
    # sheet lays it, its root, second skin and align edge, so geometry taken
    # from a feature's flat is where Wrap reads it

    def testWrapFlatFrame(self):
        """The flat laid by its leg (Root Wall2), its frame not the plate's
        where it lies folded: a circle 1 across drawn at the middle of the
        plate as it lies in the flat is the hole in the cutout's flat there,
        and pi less material folded"""
        self.flange("Far", FAR)
        flatObject(self.Tip).Root = "Wall2"
        self.Doc.recompute()
        volume = self.Tip.Shape.Volume
        flat, before = self.flatArea(self.Tip)
        solid = flat.Shape.Solids[0]
        top = solid.BoundBox.ZMax
        plate = max(
            (
                f
                for f in solid.Faces
                if isinstance(f.Surface, Part.Plane)
                and f.BoundBox.ZLength < 1e-7
                and abs(f.BoundBox.ZMin - top) < 1e-7
            ),
            key=lambda f: f.Area,
        )
        c = plate.CenterOfMass
        sketch = self.sketch(
            FreeCAD.Placement(V(0, 0, top), FreeCAD.Rotation()), circles=[((c.x, c.y, top), 1)]
        )
        cutout = self.cut(sketch, Wrap=True, WrapFrame="Flat")
        self.checkValid(cutout)
        self.assertAlmostEqual(cutout.Shape.Volume, volume - math.pi, places=6)
        flat, area = self.flatArea(cutout)
        self.assertAlmostEqual(area, before - math.pi, places=6)
        holes = [
            e
            for e in flat.Shape.Solids[0].Edges
            if isinstance(e.Curve, Part.Circle) and abs(e.Curve.Radius - 1) < 1e-6
        ]
        self.assertTrue(
            any(math.hypot(e.Curve.Center.x - c.x, e.Curve.Center.y - c.y) < 1e-6 for e in holes),
            [e.Curve.Center for e in holes],
        )

    def testWrapFlatFrameFollowsSettings(self):
        """Read in the flat's frame, the cutout follows the flat's settings:
        its Root changed, the cutout is cut again on the flat laid the new
        way, as cutting it again by hand does; a load or an undo, which sets
        the settings and the cut back together, does not touch it"""

        def hole(feature):
            # where the circle 1 across is cut, its edges' middle
            edges = [
                e
                for e in feature.Shape.Edges
                if isinstance(e.Curve, Part.Circle) and abs(e.Curve.Radius - 1) < 1e-6
            ]
            self.assertTrue(edges)
            return sum((e.Curve.Center for e in edges), V()) * (1.0 / len(edges))

        self.flange("Far", FAR)
        # on the plate in the flat laid by it, on the leg in one laid by that
        sketch = self.sketch(
            FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation()), circles=[((4, -10, 1), 1)]
        )
        cutout = self.cut(sketch, Wrap=True, WrapFrame="Flat")
        self.checkValid(cutout)
        before = hole(cutout)
        flatObject(cutout).Root = "Wall2"
        self.Doc.recompute()
        self.checkValid(cutout)
        after = hole(cutout)
        self.assertGreater((after - before).Length, 1)
        cutout.touch()
        self.Doc.recompute()
        self.assertLess((hole(cutout) - after).Length, 1e-9)
        name = cutout.Name
        path = os.path.join(tempfile.mkdtemp(), "wrap.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        cutout = self.Doc.getObject(name)
        self.assertEqual(flatObject(cutout).Root, "Wall2")
        self.assertNotIn("Touched", cutout.State)
        # read once: a shape not read since the open comes back null from an
        # undo of a recompute that changed it, any feature's (a fault of the
        # undo's, not the cutout's; found here 2026-11-05)
        self.assertLess((hole(cutout) - after).Length, 1e-9)
        self.Doc.UndoMode = 1
        self.Doc.openTransaction("Root")
        flatObject(cutout).Root = ""
        self.Doc.commitTransaction()
        self.Doc.recompute()
        self.assertLess((hole(cutout) - before).Length, 1e-9)
        self.Doc.undo()
        self.assertEqual(flatObject(cutout).Root, "Wall2")
        self.assertNotIn("Touched", cutout.State)
        self.assertLess((hole(cutout) - after).Length, 1e-9)

    def testWrapFlatFrameRefused(self):
        """In the flat's frame a sketch must lie parallel to its XY"""
        self.flange("Far", FAR)
        sketch = self.sketch(self.outside(30), circles=[((20, 30, 5), 1)])
        cutout = self.cut(sketch, Wrap=True, WrapFrame="Flat")
        self.assertIn("Invalid", cutout.State)
        self.assertIn("XY", cutout.getStatusString())

    def testWrapFrameReadOnly(self):
        """WrapFrame is for a wrap: read only while Wrap is off"""
        sketch = self.sketch(self.top(), circles=[((20, 10, 1), 1)])
        cutout = self.cut(sketch)
        self.assertIn("ReadOnly", cutout.getPropertyStatus("WrapFrame"))
        cutout.Wrap = True
        self.assertNotIn("ReadOnly", cutout.getPropertyStatus("WrapFrame"))

    def testWrapCornerGap(self):
        """Two flanges meeting at a corner: their legs lie flat beside the
        gap between them, and a sketch there cuts nothing, though either
        leg's wall runs on through it folded"""
        self.flange("Far", FAR)
        self.flange("Right", RIGHT)
        cutout = self.cut(self.sketch(self.top(), rects=[self.rect(42, 22, 47, 26)]), Wrap=True)
        self.assertIn("Invalid", cutout.State)
        self.assertIn("none of the flat", cutout.getStatusString())
