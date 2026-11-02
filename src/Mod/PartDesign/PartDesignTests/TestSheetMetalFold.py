# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's fold, PartDesign::SheetMetalFold (docs/SheetMetal.md sec 5.1,
# ruled 2026-11-02): a wall bent along a sketched line, the side picked
# moving with what is on it. The strip the bend takes in the flat, its
# allowance wide, lies about the line (Middle), from it (Start, on the
# moving side) or up to it (End); the rest of the moving side is carried
# rigidly, so the flat is the wall as it was. On a plate 40 x 20, 1 thick,
# the rule's 1 mm radius and K 0.5: a 90 deg bend's allowance is
# pi / 2 * 1.5, and the part keeps its volume.

import math
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector
BA = math.pi / 2 * 1.5
HALF = BA / 2


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


class TestSheetMetalFold(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalFold")
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

    def line(self, x=30, z=1):
        """A sketch on the plate's top, a line across it at x"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "Line")
        sketch.MapMode = "Deactivated"
        sketch.Placement = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation())
        sketch.addGeometry(Part.LineSegment(V(x, -5, 0), V(x, 25, 0)))
        self.Doc.recompute()
        return sketch

    def fold(self, sketch=None, **props):
        fold = self.Body.newObject("PartDesign::SheetMetalFold", "Fold")
        fold.Profile = sketch if sketch is not None else self.line()
        for k, v in props.items():
            setattr(fold, k, v)
        self.Doc.recompute()
        return fold

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

    def testMiddle(self):
        """The line in the bend's middle; the smaller side, past x 30,
        moves, bent up toward the sketch: the plate to 30 - BA / 2, the
        leg standing on it, as long as what was past the strip"""
        fold = self.fold()
        self.checkValid(fold)
        self.assertEqual(fold.LinePosition, "Middle")
        self.assertAlmostEqual(fold.Shape.Volume, 800, places=6)
        self.checkBox(fold.Shape, XMin=0, XMax=30 - HALF + 2, ZMin=0, ZMax=2 + 10 - HALF)
        self.assertTrue(fold.Shape.isInside(V(30 - HALF + 1.5, 10, 6), 1e-9, True))
        self.assertFalse(fold.Shape.isInside(V(35, 10, 0.5), 1e-9, True))
        flat = self.flat(fold)
        self.assertEqual(flat.BendCount, 1)
        self.assertAlmostEqual(flat.FlatLength.Value, 40, places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, 20, places=6)
        self.assertAlmostEqual(flat.FlatArea.Value, 800, places=5)

    def testStart(self):
        """The bend starts at the line: the plate to x 30"""
        fold = self.fold(LinePosition="Start")
        self.checkValid(fold)
        self.checkBox(fold.Shape, XMax=32, ZMax=2 + 10 - BA)

    def testEnd(self):
        """The bend ends at the line: the leg all that was past it"""
        fold = self.fold(LinePosition="End")
        self.checkValid(fold)
        self.checkBox(fold.Shape, XMax=30 - BA + 2, ZMax=12)

    def testReversed(self):
        """Bent down, away from the sketch"""
        fold = self.fold(Reversed=True)
        self.checkValid(fold)
        self.assertAlmostEqual(fold.Shape.Volume, 800, places=6)
        self.checkBox(fold.Shape, XMax=30 - HALF + 2, ZMax=1, ZMin=-1 - (10 - HALF))

    def testMovingPicked(self):
        """The larger side picked to move: it stands up from the right"""
        end = [
            "Face%d" % (i + 1)
            for i, f in enumerate(self.Plate.Shape.Faces)
            if abs(f.BoundBox.XMin) < 1e-7 and abs(f.BoundBox.XMax) < 1e-7
        ]
        fold = self.Body.newObject("PartDesign::SheetMetalFold", "Fold")
        fold.Profile = self.line()
        fold.Moving = (self.Plate, end)
        self.Doc.recompute()
        self.checkValid(fold)
        self.checkBox(fold.Shape, XMin=30 + HALF - 2, XMax=40, ZMax=2 + 30 - HALF)

    def testAngle(self):
        """45 deg: the allowance a quarter turn's half"""
        fold = self.fold(Angle=45)
        self.checkValid(fold)
        self.assertAlmostEqual(fold.Shape.Volume, 800, places=6)
        flat = self.flat(fold)
        self.assertAlmostEqual(flat.FlatLength.Value, 40, places=6)
        self.assertAlmostEqual(flat.MaxBendAngle.Value, 45, places=6)

    def testCarriesHole(self):
        """A hole on the side that moves goes with it, onto the leg"""
        hole = self.Body.newObject("Sketcher::SketchObject", "Hole")
        hole.MapMode = "Deactivated"
        hole.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        hole.addGeometry(Part.Circle(V(35, 10, 0), V(0, 0, 1), 2))
        cutout = self.Body.newObject("PartDesign::SheetMetalCutout", "Cutout")
        cutout.Profile = hole
        self.Doc.recompute()
        self.assertNotIn("Invalid", cutout.State)
        fold = self.fold()
        self.checkValid(fold)
        self.assertAlmostEqual(fold.Shape.Volume, 800 - 4 * math.pi, places=6)
        # on the leg, 35 - (30 + BA / 2) up from where it starts
        z = 2 + 5 - HALF
        x = 30 - HALF + 1.5
        self.assertFalse(fold.Shape.isInside(V(x, 10, z), 1e-9, True))
        self.assertTrue(fold.Shape.isInside(V(x, 10, z + 2.5), 1e-9, True))
        flat = self.flat(fold)
        self.assertAlmostEqual(flat.FlatArea.Value, 800 - 4 * math.pi, places=5)

    def testHoleOnLine(self):
        """A hole on the line goes into the bend, along its radius: the
        volume a flat hole's, its area through the sheet at K 0.5"""
        hole = self.Body.newObject("Sketcher::SketchObject", "Hole")
        hole.MapMode = "Deactivated"
        hole.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        hole.addGeometry(Part.Circle(V(30, 10, 0), V(0, 0, 1), 0.8))
        cutout = self.Body.newObject("PartDesign::SheetMetalCutout", "Cutout")
        cutout.Profile = hole
        self.Doc.recompute()
        fold = self.fold()
        self.checkValid(fold)
        self.assertAlmostEqual(fold.Shape.Volume, 800 - 0.64 * math.pi, places=5)
        # halfway round the bend, in the sheet's middle
        a = math.pi / 4
        at = V(30 - HALF + 1.5 * math.sin(a), 10, 2 - 1.5 * math.cos(a))
        self.assertFalse(fold.Shape.isInside(at, 1e-9, True))
        self.assertTrue(fold.Shape.isInside(at + V(0, 1.5, 0), 1e-9, True))
        flat = self.flat(fold)
        self.assertAlmostEqual(flat.FlatArea.Value, 800 - 0.64 * math.pi, places=4)

    def testCarriesFlange(self):
        """A flange on the side that moves goes with it: the flat as the
        flange's"""
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Plate, [edgeOf(self.Plate, (40, 0, 1), (40, 20, 1))])
        self.Doc.recompute()
        volume = flange.Shape.Volume
        before = self.flat(flange)
        size = before.FlatLength.Value, before.FlatWidth.Value, before.FlatArea.Value
        fold = self.fold()
        self.checkValid(fold)
        self.assertAlmostEqual(fold.Shape.Volume, volume, places=6)
        flat = self.flat(fold)
        self.assertEqual(flat.BendCount, 2)
        self.assertAlmostEqual(flat.FlatLength.Value, size[0], places=6)
        self.assertAlmostEqual(flat.FlatWidth.Value, size[1], places=6)
        self.assertAlmostEqual(flat.FlatArea.Value, size[2], places=5)

    def testRefused(self):
        """A line off the sheet; a line across a flange's bend; a circle"""
        fold = self.fold(self.line(x=50))
        self.assertIn("Invalid", fold.State)
        circle = self.Body.newObject("Sketcher::SketchObject", "Circle")
        circle.MapMode = "Deactivated"
        circle.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        circle.addGeometry(Part.Circle(V(10, 10, 0), V(0, 0, 1), 2))
        fold.Profile = circle
        self.Doc.recompute()
        self.assertIn("Invalid", fold.State)

    def testRefusedAcrossFlange(self):
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Plate, [edgeOf(self.Plate, (0, 0, 1), (40, 0, 1))])
        self.Doc.recompute()
        fold = self.fold()
        self.assertIn("Invalid", fold.State)
