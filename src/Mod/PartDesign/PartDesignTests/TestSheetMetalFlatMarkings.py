# SPDX-License-Identifier: LGPL-2.1-or-later

# Flat-only markings (docs/SheetMetal.md sec 12, M4): the sheet's flat object
# takes objects as its Markings -- a sketch, a shape string -- whose edges it
# draws on the blank's top, read in the frame a sketch taken from the tip's
# flat lies in (the cutout's WrapFrame Flat), each lying parallel to XY. They
# are the flat object's only: the folded part and every feature's flat
# (feature.getSubObject("Flat"), which a sketch may take geometry from) know
# nothing of them. The DXF and SVG of the sheet write them on a layer of
# their own, MARKING.
#
# A plate 40 x 20, 1 thick, on XY (its first skin there, so the flat's frame
# is the body's), a flange 10 high on its far edge.

import os
import tempfile
import unittest

import FreeCAD
import Part
import PartDesign

from PartDesignTests.TestSheetMetalFlat import readDXF, readSVG

from . import SheetMetalNames

V = FreeCAD.Vector
FAR = ((0, 20, 1), (40, 20, 1))


class TestSheetMetalFlatMarkings(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalFlatMarkings")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (40, 0), (40, 20), (0, 20)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        self.Plate = self.Body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
        self.Plate.Profile = sketch
        self.Doc.recompute()
        self.Flange = self.Body.newObject("PartDesign::SheetMetalFlange", "Flange")
        self.Flange.Edges = (self.Plate, [self.edge(self.Plate, *FAR)])
        self.Doc.recompute()
        self.assertNotIn("Invalid", self.Flange.State)
        sheet = self.Body.getSolidBody(self.Flange)
        found = [
            o
            for o in self.Doc.Objects
            if o.isDerivedFrom("PartDesign::SheetMetalFlatPattern") and o.Sheet == sheet
        ]
        self.assertEqual(len(found), 1)
        self.Flat = found[0]
        self.Flat.Visibility = True
        self.Doc.recompute()

    def tearDown(self):
        SheetMetalNames.close(self, self.Doc)

    def edge(self, feature, a, b):
        for i, e in enumerate(feature.Shape.Edges):
            ends = [v.Point for v in e.Vertexes]
            if len(ends) == 2 and (
                (ends[0].isEqual(V(*a), 1e-7) and ends[1].isEqual(V(*b), 1e-7))
                or (ends[0].isEqual(V(*b), 1e-7) and ends[1].isEqual(V(*a), 1e-7))
            ):
                return "Edge%d" % (i + 1)
        self.fail("no edge from %s to %s" % (a, b))

    def mark(self, name="Mark", placement=None):
        """A sketch of a line (5, 5) to (15, 5) and a circle at (30, 10),
        radius 3, outside the body's solid chain"""
        sketch = self.Body.newObject("Sketcher::SketchObject", name)
        if placement:
            sketch.Placement = placement
        sketch.addGeometry(Part.LineSegment(V(5, 5, 0), V(15, 5, 0)))
        sketch.addGeometry(Part.Circle(V(30, 10, 0), V(0, 0, 1), 3))
        self.Doc.recompute()
        return sketch

    def markings(self, shape):
        """The flat's marking edges, by name"""
        res = {}
        for i in range(len(shape.Edges)):
            name = shape.getElementMappedName("Edge%d" % (i + 1))
            if name and name.startswith("Marking"):
                res[name.split(";")[0]] = shape.Edges[i]
        return res

    def testDrawn(self):
        """Each edge of a marking object on the blank's top where it lies
        over the flat, named Marking1, Marking2, ..."""
        volume = self.Flat.Shape.Solids[0].Volume
        sketch = self.mark()
        self.Flat.Markings = [sketch]
        self.Doc.recompute()
        self.assertEqual(self.Flat.Status, "")
        marks = self.markings(self.Flat.Shape)
        self.assertEqual(sorted(marks), ["Marking1", "Marking2"])
        top = self.Flat.Shape.Solids[0].BoundBox.ZMax
        for e in marks.values():
            self.assertAlmostEqual(e.BoundBox.ZMin, top, places=6)
            self.assertAlmostEqual(e.BoundBox.ZMax, top, places=6)
        line = [e for e in marks.values() if isinstance(e.Curve, Part.Line)][0]
        self.assertAlmostEqual(line.Length, 10, places=6)
        self.assertTrue(line.Vertexes[0].Point.isEqual(V(5, 5, top), 1e-6))
        circle = [e for e in marks.values() if isinstance(e.Curve, Part.Circle)][0]
        self.assertTrue(circle.Curve.Center.isEqual(V(30, 10, top), 1e-6))
        # the blank as it was
        self.assertEqual(len(self.Flat.Shape.Solids), 1)
        self.assertAlmostEqual(self.Flat.Shape.Solids[0].Volume, volume, places=6)

    def testFlatOnly(self):
        """The folded part and every feature's flat know nothing of them"""
        volume = self.Flange.Shape.Volume
        edges = len(self.Flange.getSubObject("Flat").Edges)
        self.Flat.Markings = [self.mark()]
        self.Doc.recompute()
        self.assertAlmostEqual(self.Flange.Shape.Volume, volume, places=9)
        flat = self.Flange.getSubObject("Flat")
        self.assertEqual(len(flat.Edges), edges)
        self.assertEqual(self.markings(flat), {})

    def testFollowsObject(self):
        sketch = self.mark()
        self.Flat.Markings = [sketch]
        self.Doc.recompute()
        sketch.movePoint(0, 2, V(25, 5, 0))
        self.Doc.recompute()
        line = [
            e for e in self.markings(self.Flat.Shape).values() if isinstance(e.Curve, Part.Line)
        ]
        self.assertAlmostEqual(line[0].Length, 20, places=6)

    def testTwoObjects(self):
        """Numbered on across the objects, in the order of the list"""
        first = self.mark("First")
        second = self.Body.newObject("Sketcher::SketchObject", "Second")
        second.addGeometry(Part.LineSegment(V(5, 15, 0), V(35, 15, 0)))
        self.Doc.recompute()
        self.Flat.Markings = [first, second]
        self.Doc.recompute()
        marks = self.markings(self.Flat.Shape)
        self.assertEqual(sorted(marks), ["Marking1", "Marking2", "Marking3"])
        self.assertAlmostEqual(marks["Marking3"].Length, 30, places=6)

    def testPlacedSketch(self):
        """A sketch on a plane parallel to XY, above the sheet, lies on the
        blank's top as it lies over the flat"""
        sketch = self.mark(placement=FreeCAD.Placement(V(0, 0, 7), FreeCAD.Rotation()))
        self.Flat.Markings = [sketch]
        self.Doc.recompute()
        self.assertEqual(self.Flat.Status, "")
        line = [
            e for e in self.markings(self.Flat.Shape).values() if isinstance(e.Curve, Part.Line)
        ]
        top = self.Flat.Shape.Solids[0].BoundBox.ZMax
        self.assertTrue(line[0].Vertexes[0].Point.isEqual(V(5, 5, top), 1e-6))

    def testMovesWithFlat(self):
        """The flat object's placement moves its markings with its blank"""
        self.Flat.Markings = [self.mark()]
        self.Flat.Placement = FreeCAD.Placement(V(100, 0, 0), FreeCAD.Rotation())
        self.Doc.recompute()
        circle = [
            e for e in self.markings(self.Flat.Shape).values() if isinstance(e.Curve, Part.Circle)
        ]
        self.assertAlmostEqual(circle[0].Curve.Center.x, 130, places=6)

    def testNotParallel(self):
        """A marking not parallel to XY is refused: the flat says why, as it
        does of a tip that does not lie flat"""
        sketch = self.mark(placement=FreeCAD.Placement(V(), FreeCAD.Rotation(V(1, 0, 0), 90)))
        self.Flat.Markings = [sketch]
        self.Doc.recompute()
        self.assertIn("parallel", self.Flat.Status)
        self.assertIn(sketch.Label, self.Flat.Status)

    def testText(self):
        """A part number as sketch text: each glyph's curves on the blank's
        top, and in the SVG"""
        font = os.path.join(
            FreeCAD.getResourceDir(), "Mod", "TechDraw", "Resources", "fonts", "osifont-lgpl3fe.ttf"
        )
        if not os.path.isfile(font):
            self.skipTest("no bundled font file to write text with")
        import Sketcher

        sketch = self.Body.newObject("Sketcher::SketchObject", "PartNumber")
        handle = sketch.addGeometry(Part.LineSegment(V(5, 5, 0), V(5, 10, 0)), True)
        index = sketch.addConstraint(Sketcher.Constraint("Text", [handle, 0], "P-101", font))
        sketch.setTextAndFont(index, "P-101", font)
        self.Doc.recompute()
        self.Flat.Markings = [sketch]
        self.Doc.recompute()
        self.assertEqual(self.Flat.Status, "")
        marks = self.markings(self.Flat.Shape)
        self.assertEqual(len(marks), len(sketch.Shape.Edges))
        top = self.Flat.Shape.Solids[0].BoundBox.ZMax
        box = FreeCAD.BoundBox()
        for e in marks.values():
            box.add(e.BoundBox)
        self.assertAlmostEqual(box.ZMin, top, places=6)
        self.assertAlmostEqual(box.ZMax, top, places=6)
        self.assertAlmostEqual(box.YMin, 5, places=4)
        self.assertAlmostEqual(box.YMax, 10, places=4)
        with tempfile.TemporaryDirectory() as tmp:
            svg = os.path.join(tmp, "flat.svg")
            PartDesign.exportFlat(self.Flat, svg)
            layers, _ = readSVG(svg)
            self.assertEqual(len(layers["MARKING"]), len(marks))

    def testEmptyObject(self):
        """An object with no edge marks nothing"""
        empty = self.Body.newObject("Sketcher::SketchObject", "Empty")
        self.Doc.recompute()
        self.Flat.Markings = [empty]
        self.Doc.recompute()
        self.assertEqual(self.Flat.Status, "")
        self.assertEqual(self.markings(self.Flat.Shape), {})

    def testExport(self):
        """The sheet's DXF and SVG have them on the layer MARKING; an
        earlier feature's flat has none"""
        self.Flat.Markings = [self.mark()]
        self.Doc.recompute()
        with tempfile.TemporaryDirectory() as tmp:
            dxf = os.path.join(tmp, "flat.dxf")
            PartDesign.exportFlat(self.Body.getSolidBody(self.Flange), dxf)
            entities = [e for e in readDXF(dxf) if e[1] == "MARKING"]
            self.assertEqual(sorted(e[0] for e in entities), ["CIRCLE", "LINE"])
            svg = os.path.join(tmp, "flat.svg")
            PartDesign.exportFlat(self.Flat, svg)
            layers, _ = readSVG(svg)
            self.assertEqual(sorted(t for t, _, _ in layers["MARKING"]), ["circle", "line"])
            plate = os.path.join(tmp, "plate.dxf")
            PartDesign.exportFlat(self.Plate, plate)
            self.assertEqual([e for e in readDXF(plate) if e[1] == "MARKING"], [])

    def testExportFlats(self):
        self.Flat.Markings = [self.mark()]
        self.Doc.recompute()
        with tempfile.TemporaryDirectory() as tmp:
            done = PartDesign.exportFlats(self.Doc, tmp, "dxf")
            self.assertEqual(len(done), 1)
            path = done[0]["File"]
            self.assertEqual(len([e for e in readDXF(path) if e[1] == "MARKING"]), 2)

    def testHiddenFlatExport(self):
        """Exported, the markings are read though the flat object is
        hidden and out of date"""
        sketch = self.mark()
        self.Flat.Markings = [sketch]
        self.Flat.Visibility = False
        self.Doc.recompute()
        with tempfile.TemporaryDirectory() as tmp:
            dxf = os.path.join(tmp, "flat.dxf")
            PartDesign.exportFlat(self.Flange, dxf)
            self.assertEqual(len([e for e in readDXF(dxf) if e[1] == "MARKING"]), 2)
