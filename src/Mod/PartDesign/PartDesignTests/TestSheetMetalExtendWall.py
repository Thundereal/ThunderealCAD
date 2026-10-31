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
        before = self.Body.newObject("PartDesign::SheetMetalFlatPattern", "Flat")
        before.Source = wall
        after = self.Body.newObject("PartDesign::SheetMetalFlatPattern", "Flat001")
        after.Source = ext
        self.Doc.recompute()
        self.assertAlmostEqual(after.FlatLength.Value, before.FlatLength.Value + 4, places=6)

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
