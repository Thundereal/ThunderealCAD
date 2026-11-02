# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's jog, PartDesign::SheetMetalJog (docs/SheetMetal.md sec 5.1,
# ruled 2026-11-02): a wall stepped along a sketched line by two opposite
# bends and a straight run between them. On a plate 40 x 20, 1 thick, the
# rule's 1 mm radius and K 0.5, a line at x 25, a step of 5 at 90 deg: the
# run is 5 - (2 R + T) = 2 long, the jog 2 BA + 2 in the flat and 2 R + T = 3
# across. By default the far end stays where it was, the flat longer by
# what the jog adds; else the jog takes it from the wall, as a fold does.

import math
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector
BA = math.pi / 2 * 1.5
HALF = BA / 2
# what a step of 5 at 90 deg adds: its developed length less its width
ADDED = 2 * BA + 2 - 3


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


class TestSheetMetalJog(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalJog")
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

    def line(self, x=25, z=1):
        """A sketch on the plate's top, a line across it at x"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "Line")
        sketch.MapMode = "Deactivated"
        sketch.Placement = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation())
        sketch.addGeometry(Part.LineSegment(V(x, -5, 0), V(x, 25, 0)))
        self.Doc.recompute()
        return sketch

    def hole(self, x, y, r):
        sketch = self.Body.newObject("Sketcher::SketchObject", "Hole")
        sketch.MapMode = "Deactivated"
        sketch.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        sketch.addGeometry(Part.Circle(V(x, y, 0), V(0, 0, 1), r))
        cutout = self.Body.newObject("PartDesign::SheetMetalCutout", "Cutout")
        cutout.Profile = sketch
        self.Doc.recompute()
        self.assertNotIn("Invalid", cutout.State)
        return cutout

    def jog(self, sketch=None, **props):
        jog = self.Body.newObject("PartDesign::SheetMetalJog", "Jog")
        jog.Profile = sketch if sketch is not None else self.line()
        props.setdefault("Offset", 5)
        for k, v in props.items():
            setattr(jog, k, v)
        self.Doc.recompute()
        return jog

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
        return flat

    def checkBox(self, shape, **box):
        bb = shape.BoundBox
        for k, v in box.items():
            self.assertAlmostEqual(getattr(bb, k), v, places=6, msg=k)

    def checkInside(self, shape, inside=(), outside=()):
        for p in inside:
            self.assertTrue(shape.isInside(V(*p), 1e-9, True), msg="inside %s" % (p,))
        for p in outside:
            self.assertFalse(shape.isInside(V(*p), 1e-9, True), msg="outside %s" % (p,))

    def testStep(self):
        """A step of 5: the smaller side, past x 25, raised by 5 and its end
        where it was; the run standing between the bends, the first about
        the line; the flat longer by what the jog adds"""
        jog = self.jog()
        self.checkValid(jog)
        self.assertTrue(jog.FixProjectedLength)
        self.assertEqual(jog.OffsetMeasure, "Step")
        self.assertEqual(jog.LinePosition, "Middle")
        self.assertAlmostEqual(jog.Shape.Volume, (40 + ADDED) * 20, places=6)
        self.checkBox(jog.Shape, XMin=0, XMax=40, ZMin=0, ZMax=6)
        x1 = 25 - HALF
        self.checkInside(
            jog.Shape,
            inside=[(10, 10, 0.5), (x1 + 1.5, 10, 3), (35, 10, 5.5)],
            outside=[(35, 10, 0.5), (x1 + 1.5, 10, 0.5)],
        )
        flat = self.flat(jog)
        self.assertEqual(flat.BendCount, 2)
        self.assertAlmostEqual(flat.FlatLength.Value, 40 + ADDED, places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, 20, places=6)
        self.assertAlmostEqual(flat.FlatArea.Value, (40 + ADDED) * 20, places=5)

    def testTaken(self):
        """The jog taken from the wall: the far end comes in, the flat the
        plate as it was"""
        jog = self.jog(FixProjectedLength=False)
        self.checkValid(jog)
        self.assertAlmostEqual(jog.Shape.Volume, 800, places=6)
        self.checkBox(jog.Shape, XMax=40 - ADDED, ZMax=6)
        flat = self.flat(jog)
        self.assertEqual(flat.BendCount, 2)
        self.assertAlmostEqual(flat.FlatLength.Value, 40, places=6)
        self.assertAlmostEqual(flat.FlatArea.Value, 800, places=5)

    def testMeasures(self):
        """The same step measured inside, 4, and overall, 6"""
        jog = self.jog()
        for measure, offset in (("Inside", 4), ("Overall", 6)):
            jog.OffsetMeasure = measure
            jog.Offset = offset
            self.Doc.recompute()
            self.checkValid(jog)
            self.assertAlmostEqual(jog.Shape.Volume, (40 + ADDED) * 20, places=6, msg=measure)
            self.checkBox(jog.Shape, XMax=40, ZMax=6)
            self.checkInside(jog.Shape, inside=[(35, 10, 5.5)])

    def testLinePosition(self):
        """The first bend starting at the line, or ending there"""
        jog = self.jog()
        for position, x1 in (("Start", 25), ("End", 25 - BA)):
            jog.LinePosition = position
            self.Doc.recompute()
            self.checkValid(jog)
            self.checkBox(jog.Shape, XMax=40, ZMax=6)
            self.checkInside(
                jog.Shape,
                inside=[(x1 + 1.5, 10, 3)],
                outside=[(x1 + 0.5, 10, 3), (x1 + 2.5, 10, 3)],
            )

    def testReversed(self):
        """Stepped down, away from the sketch"""
        jog = self.jog(Reversed=True)
        self.checkValid(jog)
        self.assertAlmostEqual(jog.Shape.Volume, (40 + ADDED) * 20, places=6)
        self.checkBox(jog.Shape, XMax=40, ZMin=-5, ZMax=1)
        self.checkInside(jog.Shape, inside=[(35, 10, -4.5)], outside=[(35, 10, 0.5)])

    def testLeast(self):
        """The least step at 90 deg, 2 R + T: the bends with no run between"""
        jog = self.jog(Offset=3)
        self.checkValid(jog)
        added = 2 * BA - 3
        self.assertAlmostEqual(jog.Shape.Volume, (40 + added) * 20, places=6)
        self.checkBox(jog.Shape, XMax=40, ZMax=4)
        flat = self.flat(jog)
        self.assertAlmostEqual(flat.FlatLength.Value, 40 + added, places=6)

    def testAngle(self):
        """45 deg: the run along the slant, (5 - 3 (1 - cos)) / sin long"""
        jog = self.jog(Angle=45)
        self.checkValid(jog)
        c = s = math.sqrt(0.5)
        run = (5 - 3 * (1 - c)) / s
        added = 2 * (math.pi / 4 * 1.5) + run - (3 * s + run * c)
        self.assertAlmostEqual(jog.Shape.Volume, (40 + added) * 20, places=6)
        self.checkBox(jog.Shape, XMax=40, ZMax=6)
        flat = self.flat(jog)
        self.assertAlmostEqual(flat.FlatLength.Value, 40 + added, places=6)
        self.assertAlmostEqual(flat.MaxBendAngle.Value, 45, places=6)

    def testMovingPicked(self):
        """The larger side picked to move: it is raised, its end at x 0"""
        end = [
            "Face%d" % (i + 1)
            for i, f in enumerate(self.Plate.Shape.Faces)
            if abs(f.BoundBox.XMin) < 1e-7 and abs(f.BoundBox.XMax) < 1e-7
        ]
        jog = self.jog(Moving=(self.Plate, end))
        self.checkValid(jog)
        self.checkBox(jog.Shape, XMin=0, XMax=40, ZMax=6)
        self.checkInside(jog.Shape, inside=[(5, 10, 5.5), (35, 10, 0.5)], outside=[(5, 10, 0.5)])

    def testCarriesHole(self):
        """A hole on the side that moves goes with it, where it was"""
        self.hole(35, 10, 2)
        jog = self.jog()
        self.checkValid(jog)
        self.assertAlmostEqual(jog.Shape.Volume, (40 + ADDED) * 20 - 4 * math.pi, places=6)
        self.checkInside(jog.Shape, inside=[(35, 12.5, 5.5)], outside=[(35, 10, 5.5)])
        flat = self.flat(jog)
        self.assertAlmostEqual(flat.FlatArea.Value, (40 + ADDED) * 20 - 4 * math.pi, places=5)

    def testHoleInBend(self):
        """A hole on the line goes into the first bend, along its radius"""
        self.hole(25, 10, 0.8)
        jog = self.jog()
        self.checkValid(jog)
        area = (40 + ADDED) * 20 - 0.64 * math.pi
        self.assertAlmostEqual(jog.Shape.Volume, area, places=5)
        # halfway round the first bend, in the sheet's middle
        a = math.pi / 4
        at = V(25 - HALF + 1.5 * math.sin(a), 10, 2 - 1.5 * math.cos(a))
        self.checkInside(jog.Shape, inside=[at + V(0, 1.5, 0)], outside=[at])
        flat = self.flat(jog)
        self.assertAlmostEqual(flat.FlatArea.Value, area, places=4)

    def testHoleAcrossEnd(self):
        """A hole across where the jog's width ends, its end kept: the jog
        adds the wall's section there, so the hole is stretched into a slot
        by what the jog adds, one piece in the flat"""
        end = 25 - HALF + 3
        self.hole(end, 10, 1)
        jog = self.jog()
        self.checkValid(jog)
        area = (40 + ADDED) * 20 - (math.pi + 2 * ADDED)
        self.assertAlmostEqual(jog.Shape.Volume, area, places=4)
        flat = self.flat(jog)
        self.assertAlmostEqual(flat.FlatArea.Value, area, places=4)
        # the slot in the flat along its middle, the plate's end at x 0:
        # open from end - 1 to end + ADDED + 1, closed past either end
        shape = flat.Shape
        y = (shape.BoundBox.YMin + shape.BoundBox.YMax) / 2
        z = (shape.BoundBox.ZMin + shape.BoundBox.ZMax) / 2
        for t in (0, 0.25, 0.5, 0.75, 1):
            at = V(end - 0.9 + t * (ADDED + 1.8), y, z)
            self.assertFalse(shape.isInside(at, 1e-9, True), msg="slot %s" % at)
        for x in (end - 1.1, end + ADDED + 1.1):
            self.assertTrue(shape.isInside(V(x, y, z), 1e-9, True), msg="past %s" % x)

    def testCarriesFlange(self):
        """A flange on the side that moves goes with it"""
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Plate, [edgeOf(self.Plate, (40, 0, 1), (40, 20, 1))])
        self.Doc.recompute()
        volume = flange.Shape.Volume
        before = self.flat(flange)
        size = before.FlatLength.Value, before.FlatWidth.Value, before.FlatArea.Value
        jog = self.jog()
        self.checkValid(jog)
        self.assertAlmostEqual(jog.Shape.Volume, volume + ADDED * 20, places=6)
        flat = self.flat(jog)
        self.assertEqual(flat.BendCount, 3)
        self.assertAlmostEqual(flat.FlatLength.Value, size[0] + ADDED, places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, size[1], places=6)
        self.assertAlmostEqual(flat.FlatArea.Value, size[2] + ADDED * 20, places=5)

    def testFlangeBehind(self):
        """A flange on the plate's far end, its leg as high as the stepped side:
        the stepped side's slab goes on behind the jog's bends, but its wall
        starts where the bend ends; the leg's faces are the flange's own"""
        jog = self.jog()
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (jog, [edgeOf(jog, (0, 0, 1), (0, 20, 1))])
        # the leg's straight part from z 2 to 9, its middle in the stepped
        # side's slab, z 5 to 6
        flange.BendPosition = "MaterialInside"
        flange.LengthType = "Leg"
        flange.Length = 7
        self.Doc.recompute()
        self.checkValid(flange)
        legs = {
            flange.sheetElementOf("Face%d" % (i + 1))
            for i, f in enumerate(flange.Shape.Faces)
            if f.BoundBox.XMax < 1 + 1e-7 and f.BoundBox.ZMin > 2 - 1e-7
        }
        self.assertEqual(legs, {"Wall4"})
        flat = self.flat(flange)
        self.assertEqual(flat.BendCount, 3)

    def testRefused(self):
        """A step too small for a run between the bends, saying the least;
        a line off the sheet; a circle; a line across a flange's bend"""
        jog = self.jog(Offset=2.5)
        self.assertIn("Invalid", jog.State)
        self.assertIn("3 mm", jog.getStatusString())
        jog.Offset = 5
        jog.Profile = self.line(x=50)
        self.Doc.recompute()
        self.assertIn("Invalid", jog.State)
        circle = self.Body.newObject("Sketcher::SketchObject", "Circle")
        circle.MapMode = "Deactivated"
        circle.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        circle.addGeometry(Part.Circle(V(10, 10, 0), V(0, 0, 1), 2))
        jog.Profile = circle
        self.Doc.recompute()
        self.assertIn("Invalid", jog.State)

    def testRefusedAcrossFlange(self):
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Plate, [edgeOf(self.Plate, (0, 0, 1), (40, 0, 1))])
        self.Doc.recompute()
        jog = self.jog()
        self.assertIn("Invalid", jog.State)
