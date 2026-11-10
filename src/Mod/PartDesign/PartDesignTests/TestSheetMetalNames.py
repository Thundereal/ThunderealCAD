# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's element names across an edit before the feature
# (docs/SheetMetal.md sec 12, the audit of 2026-11-10). A feature names the
# faces it makes after its walls and bends, and the sheet numbers those on
# from the ones it has: Bend2 is Bend1 once the flange before it is gone. The
# faces' names are by the feature's own count, so they stay -- a reference to
# one is then found by its name, not by where it lies, and holds when the
# edit moves it too.
#
# Each test: a plate 40 x 20, 1 thick; a flange on its left edge; the feature
# on the result, clear of that flange. The faces the feature made keep their
# names with the flange suppressed, and with it back. (What each feature's
# highlight and names must be at all is SheetMetalNames.py, asked of every
# sheet metal test's document.)

import unittest

import FreeCAD
import Part

from . import SheetMetalNames
from .TestSheetMetalContourFlange import edgeOf

V = FreeCAD.Vector
RIGHT = ((40, 0, 1), (40, 20, 1))


class TestSheetMetalNames(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalNames")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()
        self.Early = self.Body.newObject("PartDesign::SheetMetalFlange", "Early")
        self.Early.Edges = (self.Plate, [edgeOf(self.Plate, (0, 0, 1), (0, 20, 1))])
        self.Doc.recompute()
        self.assertNotIn("Invalid", self.Early.State)

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    def line(self, x=30):
        """A sketch on the plate's top, a line across it at x"""
        sketch = self.Body.newObject("Sketcher::SketchObject", "Line")
        sketch.MapMode = "Deactivated"
        sketch.Placement = FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        sketch.addGeometry(Part.LineSegment(V(x, -5, 0), V(x, 25, 0)))
        self.Doc.recompute()
        return sketch

    def made(self, feature):
        """The names of the faces feature made, by where each lies"""
        shape = feature.Shape
        res = {}
        for name in feature.getGeneratedElements():
            if not name.startswith("Face"):
                continue
            face = shape.getElement(name)
            c = face.CenterOfMass
            key = (round(face.Area, 5), round(c.x, 5), round(c.y, 5), round(c.z, 5))
            res[key] = shape.getElementName(name, 1)
        return res

    def checkNamesHold(self, feature, least):
        """feature's faces, least of them at least, keep their names with
        the flange before it suppressed and with it back"""
        self.Doc.recompute()
        self.assertNotIn("Invalid", feature.State)
        before = self.made(feature)
        self.assertGreaterEqual(len(before), least)
        for suppress in (True, False):
            self.Early.Suppress = suppress
            self.Doc.recompute()
            self.assertNotIn("Invalid", feature.State)
            after = self.made(feature)
            # every face the feature made is where it was, the flange being
            # clear of it
            self.assertEqual(sorted(after), sorted(before), suppress)
            renamed = {
                key: (before[key], after[key]) for key in before if before[key] != after[key]
            }
            self.assertEqual(renamed, {}, suppress)

    def testFlange(self):
        """A flange's bend, leg and ends"""
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        self.checkNamesHold(flange, 8)

    def testFlangeOnFlange(self):
        """A flange on the leg of a flange after the first: its own names,
        and its reference to that leg's edge"""
        first = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        first.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        self.Doc.recompute()
        top = first.Shape.BoundBox.ZMax
        second = self.Body.newObject("PartDesign::SheetMetalFlange", "Second")
        second.Edges = (first, [edgeOf(first, (40, 0, top), (40, 20, top))])
        second.Angle = 45
        self.checkNamesHold(second, 8)

    def testHem(self):
        """A hem's bend and leg"""
        hem = self.Body.newObject("PartDesign::SheetMetalHem", "Hem")
        hem.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        hem.Length = 6
        self.checkNamesHold(hem, 6)

    def testContourFlange(self):
        """A contour flange's two bends and two walls"""
        s = self.Body.newObject("Sketcher::SketchObject", "ContourSketch")
        s.MapMode = "Deactivated"
        # x along the plate's x, y up, on the plane y 10
        s.Placement = FreeCAD.Placement(
            V(0, 10, 0), FreeCAD.Rotation(V(1, 0, 0), V(0, 0, 1), V(0, -1, 0), "ZXY")
        )
        pts = [(40, 1), (40, 11), (50, 11)]
        for p, q in zip(pts, pts[1:]):
            s.addGeometry(Part.LineSegment(V(p[0], p[1], 0), V(q[0], q[1], 0)))
        self.Doc.recompute()
        contour = self.Body.newObject("PartDesign::SheetMetalContourFlange", "ContourFlange")
        contour.Edges = (self.Early, [edgeOf(self.Early, *RIGHT)])
        contour.Profile = s
        self.checkNamesHold(contour, 12)

    def testFold(self):
        """A fold's bend"""
        fold = self.Body.newObject("PartDesign::SheetMetalFold", "Fold")
        fold.Profile = self.line()
        self.checkNamesHold(fold, 4)

    def testJog(self):
        """A jog's two bends and its run"""
        jog = self.Body.newObject("PartDesign::SheetMetalJog", "Jog")
        jog.Profile = self.line()
        jog.Offset = 5
        self.checkNamesHold(jog, 12)
