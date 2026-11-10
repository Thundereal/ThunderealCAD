# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's tab, PartDesign::SheetMetalTab (docs/SheetMetal.md sec 5.1):
# a closed sketch on a wall's skin, or on a plane parallel to it, adds a
# plate of the sheet's thickness on that wall, its slab where the wall's
# is. The nearest such wall it meets takes it; it may not run past a bend
# of the wall. On a plate 40 x 20, 1 thick.

import math
import unittest

import FreeCAD
import Part

from . import SheetMetalNames
from .SheetMetalFlatView import Flat

V = FreeCAD.Vector


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


class TestSheetMetalTab(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalTab")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        self.rectangle(sketch, (0, 0), (40, 20))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    @staticmethod
    def rectangle(sketch, a, b):
        pts = [(a[0], a[1]), (b[0], a[1]), (b[0], b[1]), (a[0], b[1])]
        for i in range(4):
            p, q = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(p[0], p[1], 0), V(q[0], q[1], 0)))

    def sketch(self, placement=None, rect=None, circle=None, line=None):
        """A sketch at z 1 (the plate's top) unless placed, of a rectangle
        from a to b, a circle and a line as given"""
        s = self.Body.newObject("Sketcher::SketchObject", "TabSketch")
        s.MapMode = "Deactivated"
        s.Placement = placement or FreeCAD.Placement(V(0, 0, 1), FreeCAD.Rotation())
        if rect:
            self.rectangle(s, *rect)
        if circle:
            s.addGeometry(Part.Circle(V(circle[0], circle[1], 0), V(0, 0, 1), circle[2]))
        if line:
            s.addGeometry(Part.LineSegment(V(*line[0]), V(*line[1])))
        self.Doc.recompute()
        return s

    def tab(self, sketch):
        tab = self.Body.newObject("PartDesign::SheetMetalTab", "Tab")
        tab.Profile = sketch
        self.Doc.recompute()
        return tab

    def checkValid(self, feature):
        self.assertNotIn("Invalid", feature.State)
        self.assertTrue(feature.Shape.isValid())
        self.assertEqual(len(feature.Shape.Solids), 1)
        self.assertNotIn(
            None,
            [feature.sheetElementOf("Face%d" % (i + 1)) for i in range(len(feature.Shape.Faces))],
        )

    def flat(self, feature):
        self.Doc.recompute()
        flat = Flat(feature)
        self.assertNotIn("Invalid", flat.State)
        self.assertEqual(len(flat.Shape.Solids), 1)
        self.assertTrue(flat.Shape.Solids[0].isValid())
        return flat

    def checkBox(self, shape, **box):
        bb = shape.BoundBox
        for k, v in box.items():
            self.assertAlmostEqual(getattr(bb, k), v, places=6, msg=k)

    def testOnEdge(self):
        """A 10 x 10 tab on the plate's right edge: the plate's slab, the
        flat as long"""
        tab = self.tab(self.sketch(rect=((40, 5), (50, 15))))
        self.checkValid(tab)
        self.assertAlmostEqual(tab.Shape.Volume, 900, places=6)
        self.checkBox(tab.Shape, XMin=0, XMax=50, ZMin=0, ZMax=1)
        self.assertEqual(tab.sheetElementOf("Face1"), self.Plate.sheetElementOf("Face1"))
        flat = self.flat(tab)
        self.assertEqual(flat.BendCount, 0)
        self.assertAlmostEqual(flat.FlatArea.Value, 900, places=5)

    def testOverlap(self):
        """A tab drawn partly over the plate adds what lies off it"""
        tab = self.tab(self.sketch(rect=((35, 5), (50, 15))))
        self.checkValid(tab)
        self.assertAlmostEqual(tab.Shape.Volume, 900, places=6)
        self.checkBox(tab.Shape, XMax=50, ZMax=1)

    def testOtherSkin(self):
        """Drawn on the bottom skin, or on a plane above, the same tab"""
        for z in (0, 6):
            tab = self.tab(
                self.sketch(
                    placement=FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation()),
                    rect=((40, 5), (50, 15)),
                )
            )
            self.checkValid(tab)
            self.assertAlmostEqual(tab.Shape.Volume, 900, places=6, msg=z)
            self.checkBox(tab.Shape, ZMin=0, ZMax=1)
            self.Doc.removeObject(tab.Name)
            self.Doc.recompute()

    def testHole(self):
        """A circle inside the tab's outline is a hole in it"""
        tab = self.tab(self.sketch(rect=((40, 5), (50, 15)), circle=(45, 10, 2)))
        self.checkValid(tab)
        self.assertAlmostEqual(tab.Shape.Volume, 900 - 4 * math.pi, places=6)
        self.assertFalse(tab.Shape.isInside(V(45, 10, 0.5), 1e-9, True))
        flat = self.flat(tab)
        self.assertAlmostEqual(flat.FlatArea.Value, 900 - 4 * math.pi, places=5)

    def testOnFlange(self):
        """A tab on a flange's leg, past its end: the leg's slab, the flat
        larger by it"""
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Plate, [edgeOf(self.Plate, (40, 0, 1), (40, 20, 1))])
        self.Doc.recompute()
        before = self.flat(flange).FlatArea.Value
        bb = flange.Shape.BoundBox
        # in the leg's outer skin, the plane x = XMax, its sketch X along
        # the world's Y and its Y up
        rot = FreeCAD.Rotation(V(0, 1, 0), V(0, 0, 1), V(1, 0, 0), "ZXY")
        tab = self.tab(
            self.sketch(
                placement=FreeCAD.Placement(V(bb.XMax, 0, 0), rot),
                rect=((5, bb.ZMax - 1), (15, bb.ZMax + 5)),
            )
        )
        self.checkValid(tab)
        self.assertAlmostEqual(tab.Shape.Volume, flange.Shape.Volume + 50, places=6)
        self.checkBox(tab.Shape, ZMax=bb.ZMax + 5, XMax=bb.XMax)
        flat = self.flat(tab)
        self.assertEqual(flat.BendCount, 1)
        self.assertAlmostEqual(flat.FlatArea.Value, before + 50, places=5)

    @staticmethod
    def facesIn(shape, point, normal):
        """How many faces of a shape lie in the plane through point, normal"""
        p, n = V(*point), V(*normal)
        count = 0
        for f in shape.Faces:
            s = f.Surface
            if not isinstance(s, Part.Plane) or abs(abs(s.Axis.dot(n)) - 1) > 1e-9:
                continue
            if abs((s.Position - p).dot(n)) < 1e-7:
                count += 1
        return count

    def testOneSkin(self):
        """The wall a tab joins keeps a skin of one face each side, no seam
        where the tab meets it, on the part and in the flat"""
        tab = self.tab(self.sketch(rect=((35, 5), (50, 15))))
        self.checkValid(tab)
        for z in (0, 1):
            self.assertEqual(self.facesIn(tab.Shape, (0, 0, z), (0, 0, 1)), 1, msg=z)
        flat = self.flat(tab)
        bb = flat.Shape.BoundBox
        for z in (bb.ZMin, bb.ZMax):
            self.assertEqual(self.facesIn(flat.Shape, (0, 0, z), (0, 0, 1)), 1, msg=z)

    def testOneSkinOnFlange(self):
        """A tab on a flange's leg past its end: the leg's skins one face each"""
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Plate, [edgeOf(self.Plate, (40, 0, 1), (40, 20, 1))])
        self.Doc.recompute()
        bb = flange.Shape.BoundBox
        rot = FreeCAD.Rotation(V(0, 1, 0), V(0, 0, 1), V(1, 0, 0), "ZXY")
        tab = self.tab(
            self.sketch(
                placement=FreeCAD.Placement(V(bb.XMax, 0, 0), rot),
                rect=((5, bb.ZMax - 1), (15, bb.ZMax + 5)),
            )
        )
        self.checkValid(tab)
        for x in (bb.XMax - 1, bb.XMax):
            self.assertEqual(self.facesIn(tab.Shape, (x, 0, 0), (1, 0, 0)), 1, msg=x)

    def testRefused(self):
        """Off the plate; an open sketch; past a bend of its wall"""
        tab = self.tab(self.sketch(rect=((45, 5), (55, 15))))
        self.assertIn("Invalid", tab.State)
        tab.Profile = self.sketch(line=((40, 5, 0), (50, 5, 0)))
        self.Doc.recompute()
        self.assertIn("Invalid", tab.State)

    def testRefusedPastBend(self):
        flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        flange.Edges = (self.Plate, [edgeOf(self.Plate, (40, 0, 1), (40, 20, 1))])
        self.Doc.recompute()
        tab = self.tab(self.sketch(rect=((30, 5), (50, 15))))
        self.assertIn("Invalid", tab.State)
