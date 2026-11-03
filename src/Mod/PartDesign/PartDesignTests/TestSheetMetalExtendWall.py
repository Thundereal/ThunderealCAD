# SPDX-License-Identifier: LGPL-2.1-or-later

# Sheet metal's extend wall, PartDesign::SheetMetalExtendWall
# (docs/SheetMetal.md sec 5.1): a wall's end face pushed out by a length, cut
# back by a negative one, or carried up to a face. The extension is the
# wall's own: the model keeps its elements and the flat grows with it.

import math
import os
import tempfile
import unittest

import FreeCAD
import Part

from .SheetMetalFlatView import Flat

V = FreeCAD.Vector


class TestSheetMetalExtendWall(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSheetMetalExtendWall")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def wall(self, points, closed=False, **props):
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        n = len(points) if closed else len(points) - 1
        for i in range(n):
            a, b = points[i], points[(i + 1) % len(points)]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        wall = self.Body.newObject("PartDesign::SheetMetalBaseWall", "BaseWall")
        wall.Profile = sketch
        for k, v in props.items():
            setattr(wall, k, v)
        self.Doc.recompute()
        return wall

    def faceWhere(self, feature, test):
        found = ["Face%d" % (i + 1) for i, f in enumerate(feature.Shape.Faces) if test(f.BoundBox)]
        self.assertEqual(len(found), 1, found)
        return found[0]

    def extend(self, base, faces, **props):
        ext = self.Body.newObject("PartDesign::SheetMetalExtendWall", "ExtendWall")
        ext.Faces = (base, faces)
        for k, v in props.items():
            setattr(ext, k, v)
        self.Doc.recompute()
        return ext

    def plate(self):
        return self.wall([(0, 0), (40, 0), (40, 20), (0, 20)], closed=True)

    def farEnd(self, feature, y=20):
        return self.faceWhere(feature, lambda b: abs(b.YMin - y) < 1e-7 and abs(b.YMax - y) < 1e-7)

    def testLength(self):
        """The far end pushed out by 5"""
        plate = self.plate()
        ext = self.extend(plate, [self.farEnd(plate)], Length=5)
        self.assertNotIn("Invalid", ext.State)
        self.assertTrue(ext.Shape.isValid())
        self.assertAlmostEqual(ext.Shape.Volume, 40 * 25)
        self.assertAlmostEqual(ext.Shape.BoundBox.YMax, 25)
        self.assertEqual(ext.Rule, plate.Rule)
        self.assertEqual(
            [e["Name"] for e in ext.SheetModel["Elements"]],
            [e["Name"] for e in plate.SheetModel["Elements"]],
        )
        owners = [ext.sheetElementOf("Face%d" % (i + 1)) for i in range(len(ext.Shape.Faces))]
        self.assertNotIn(None, owners)

    def testNoSeam(self):
        """The extension is one with its wall: no seam where the end was, on
        a plate or on an L's leg, and the L's caps still split at its bend"""
        plate = self.plate()
        ext = self.extend(plate, [self.farEnd(plate)], Length=5)
        self.assertEqual(len(ext.Shape.Faces), len(plate.Shape.Faces))
        self.assertEqual(len(ext.Shape.Edges), len(plate.Shape.Edges))
        # a sheet of its own, for its flat
        self.Body = self.Doc.addObject("PartDesign::Body", "Body001")
        wall = self.wall([(0, 0), (20, 0), (20, 10)], Length=15)
        end = self.faceWhere(wall, lambda b: abs(b.YMin - 10) < 1e-7 and abs(b.YMax - 10) < 1e-7)
        ext = self.extend(wall, [end], Length=4)
        self.assertNotIn("Invalid", ext.State)
        self.assertTrue(ext.Shape.isValid())
        self.assertEqual(len(ext.Shape.Faces), len(wall.Shape.Faces))
        owners = [ext.sheetElementOf("Face%d" % (i + 1)) for i in range(len(ext.Shape.Faces))]
        self.assertNotIn(None, owners)
        self.Doc.recompute()
        flat = Flat(ext)
        self.assertNotIn("Invalid", flat.State)

    def testCutBack(self):
        """A negative length cuts the wall back"""
        plate = self.plate()
        ext = self.extend(plate, [self.farEnd(plate)], Length=-5)
        self.assertNotIn("Invalid", ext.State)
        self.assertAlmostEqual(ext.Shape.Volume, 40 * 15)
        self.assertAlmostEqual(ext.Shape.BoundBox.YMax, 15)

    def testTwoFaces(self):
        plate = self.plate()
        near = self.farEnd(plate, y=0)
        ext = self.extend(plate, [self.farEnd(plate), near], Length=3)
        self.assertAlmostEqual(ext.Shape.Volume, 40 * 26)

    def testUpToFace(self):
        """Up to a face across the way: a datum plane at y = 32"""
        plate = self.plate()
        plane = self.Body.newObject("PartDesign::Plane", "Plane")
        plane.MapMode = "Deactivated"
        plane.Placement = FreeCAD.Placement(V(0, 32, 0), FreeCAD.Rotation(V(1, 0, 0), 90))
        self.Doc.recompute()
        ext = self.extend(plate, [self.farEnd(plate)], UpToFace=(plane, [""]))
        self.assertNotIn("Invalid", ext.State)
        self.assertAlmostEqual(ext.Shape.BoundBox.YMax, 32)
        self.assertAlmostEqual(ext.Shape.Volume, 40 * 32)

    def testProfileLeg(self):
        """The end of an L's leg, and the flat longer by as much"""
        wall = self.wall([(0, 0), (20, 0), (20, 10)], Length=15)
        end = self.faceWhere(wall, lambda b: abs(b.YMin - 10) < 1e-7 and abs(b.YMax - 10) < 1e-7)
        ext = self.extend(wall, [end], Length=4)
        self.assertNotIn("Invalid", ext.State)
        self.assertAlmostEqual(ext.Shape.Volume, wall.Shape.Volume + 4 * 1 * 15)
        self.Doc.recompute()
        before = Flat(wall)
        after = Flat(ext)
        self.assertAlmostEqual(after.FlatLength.Value, before.FlatLength.Value + 4, places=6)

    def testGaps(self):
        """Gap1 and Gap2 narrow the extension at its two ends; gaps that
        leave nothing are refused"""
        plate = self.plate()
        ext = self.extend(plate, [self.farEnd(plate)], Length=5, Gap1=3, Gap2=7)
        self.assertNotIn("Invalid", ext.State)
        self.assertTrue(ext.Shape.isValid())
        self.assertAlmostEqual(ext.Shape.Volume, 40 * 20 + 30 * 5, places=6)
        grown = [v.Point.x for v in ext.Shape.Vertexes if v.Point.y > 20 + 1e-7]
        self.assertAlmostEqual(max(grown) - min(grown), 30, places=6)
        self.assertIn(sorted([round(min(grown), 6), round(40 - max(grown), 6)]), [[3, 7]])
        ext.Gap2 = 37
        self.Doc.recompute()
        self.assertIn("Invalid", ext.State)

    def testClearance(self):
        """An extension that would run into another wall of the part stops
        the rule's gap short of it: a C's lip carried toward the other lip"""
        wall = self.wall([(10, 10), (0, 10), (0, 0), (30, 0), (30, 10), (20, 10)], Length=15)
        self.assertNotIn("Invalid", wall.State)
        ends = sorted(
            (f.BoundBox.XMin, "Face%d" % (i + 1))
            for i, f in enumerate(wall.Shape.Faces)
            if f.BoundBox.XLength < 1e-7 and abs(f.Area - 15) < 1e-6 and 5 < f.BoundBox.XMin < 25
        )
        self.assertEqual(len(ends), 2, ends)
        (a, first), (b, second) = ends
        ext = self.extend(wall, [first], Length=15)
        self.assertNotIn("Invalid", ext.State)
        self.assertTrue(ext.Shape.isValid())
        gap = wall.Rule.Gap.Value
        self.assertAlmostEqual(ext.Shape.Volume - wall.Shape.Volume, (b - gap - a) * 15, places=6)
        # up to a face beyond the other lip, the same
        ext.Length = 1
        right = wall.Shape.BoundBox.XMax
        beyond = self.faceWhere(
            wall, lambda bb: abs(bb.XMin - right) < 1e-7 and bb.XLength < 1e-7
        )
        ext.UpToFace = (wall, [beyond])
        self.Doc.recompute()
        self.assertNotIn("Invalid", ext.State)
        self.assertAlmostEqual(ext.Shape.Volume - wall.Shape.Volume, (b - gap - a) * 15, places=6)

    def testErrors(self):
        """A skin is no end; nothing left of a wall cut back past it"""
        plate = self.plate()
        top = self.faceWhere(plate, lambda b: abs(b.ZMin - 1) < 1e-7 and abs(b.ZMax - 1) < 1e-7)
        ext = self.extend(plate, [top], Length=5)
        self.assertIn("Invalid", ext.State)
        ext.Faces = (plate, [self.farEnd(plate)])
        ext.Length = -25
        self.Doc.recompute()
        self.assertIn("Invalid", ext.State)
        ext.Length = 2
        self.Doc.recompute()
        self.assertNotIn("Invalid", ext.State)

    def testSaveAndRestore(self):
        plate = self.plate()
        ext = self.extend(plate, [self.farEnd(plate)], Length=5)
        path = os.path.join(tempfile.mkdtemp(), "extend.FCStd")
        self.Doc.saveAs(path)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(path)
        ext = self.Doc.getObject("ExtendWall")
        ext.touch()
        self.Doc.recompute()
        self.assertNotIn("Invalid", ext.State)
        self.assertAlmostEqual(ext.Shape.Volume, 40 * 25)
