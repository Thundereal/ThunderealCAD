# SPDX-License-Identifier: LGPL-2.1-or-later

"""Trim, Split and Untrim (docs/Surface.md sec 5.2, 9, U2): a face cut by
curves laid onto it, or by surfaces; Trim keeps the side picked, stored as
a 3D point and found again after an input change; Split keeps every piece;
a mutual trim cuts both surfaces by each other; Untrim gives a face its
natural boundary back, or the one it had before a trim."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part
from FreeCAD import Vector as V


def wave(u, v):
    x, y = 20 * u, 12 * v
    return V(x, y, 2 * math.sin(x / 4) * math.cos(y / 5))


def waveFace():
    s = Part.BSplineSurface()
    s.interpolate([[wave(i / 8, j / 6) for j in range(7)] for i in range(9)])
    return s.toShape()


class TestTrim(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceTrim")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))
            self.assertFalse(o.Shape.isNull(), o.Name)
            self.assertTrue(o.Shape.isValid(), o.Name)

    def base(self):
        src = self.doc.addObject("Part::Feature", "Wave")
        src.Shape = waveFace()
        return src

    def line(self, x):
        ln = self.doc.addObject("Part::Line", "Cut")
        ln.X1, ln.Y1, ln.Z1 = x, -2, 6
        ln.X2, ln.Y2, ln.Z2 = x, 14, 6
        return ln

    def testTrimKeepsThePickedSide(self):
        src, ln = self.base(), self.line(10)
        t = self.doc.addObject("Surface::Trim", "Trim")
        t.Base = [(src, "Face1")]
        t.Tools = [(ln, "Edge1")]
        t.Direction = V(0, 0, -1)
        t.KeepPoints = [V(3, 6, 0)]
        self.recompute(t)
        self.assertEqual(len(t.Shape.Faces), 1)
        bb = t.Shape.optimalBoundingBox(False, False)
        self.assertAlmostEqual(bb.XMin, 0, places=5)
        self.assertAlmostEqual(bb.XMax, 10, places=4)
        self.assertEqual(t.Pieces, 2)

        # the line moved: the side the point is on is still the one kept
        ln.X1 = ln.X2 = 14
        self.recompute(t)
        self.assertAlmostEqual(t.Shape.optimalBoundingBox(False, False).XMax, 14, places=4)
        self.assertAlmostEqual(t.Shape.optimalBoundingBox(False, False).XMin, 0, places=5)
        # and past the point, the point's side is the other one
        ln.X1 = ln.X2 = 2
        self.recompute(t)
        self.assertAlmostEqual(t.Shape.optimalBoundingBox(False, False).XMin, 2, places=4)

        path = os.path.join(tempfile.gettempdir(), "SurfaceTrim.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        t = self.doc.Trim
        self.assertEqual(len(t.KeepPoints), 1)
        t.touch()
        self.recompute(t)
        self.assertAlmostEqual(t.Shape.optimalBoundingBox(False, False).XMin, 2, places=4)
        os.remove(path)

    def testTrimAlongTheNormal(self):
        src, ln = self.base(), self.line(10)
        t = self.doc.addObject("Surface::Trim", "Trim")
        t.Base = [(src, "Face1")]
        t.Tools = [(ln, "Edge1")]
        t.KeepPoints = [V(17, 6, 0)]
        self.recompute(t)
        self.assertEqual(len(t.Shape.Faces), 1)
        # along the normal the cut wanders off x = 10 where the wave tilts,
        # but the side kept is the one past it
        self.assertGreater(t.Shape.optimalBoundingBox(False, False).XMin, 8)
        self.assertAlmostEqual(t.Shape.optimalBoundingBox(False, False).XMax, 20, places=5)

    def testSplitKeepsEveryPiece(self):
        src, ln = self.base(), self.line(10)
        s = self.doc.addObject("Surface::Split", "Split")
        s.Base = [(src, "Face1")]
        s.Tools = [(ln, "Edge1")]
        s.Direction = V(0, 0, -1)
        self.recompute(s)
        self.assertEqual(len(s.Shape.Faces), 2)
        self.assertAlmostEqual(s.Shape.Area, src.Shape.Area, delta=1e-6 * src.Shape.Area)

    def testMutualTrim(self):
        h = self.doc.addObject("Part::Plane", "H")
        h.Length, h.Width = 20, 20
        v = self.doc.addObject("Part::Plane", "V")
        v.Length, v.Width = 10, 20
        v.Placement = App.Placement(V(10, 0, -5), App.Rotation(V(0, 1, 0), -90))
        self.doc.recompute()
        t = self.doc.addObject("Surface::Trim", "Trim")
        t.Base = [(h, "Face1")]
        t.Tools = [(v, "Face1")]
        t.Mutual = True
        t.KeepPoints = [V(5, 10, 0), V(10, 10, 3)]
        self.recompute(t)
        self.assertEqual(t.Pieces, 4)
        self.assertEqual(len(t.Shape.Faces), 2)
        areas = sorted(f.Area for f in t.Shape.Faces)
        self.assertAlmostEqual(areas[0], 100, places=5)
        self.assertAlmostEqual(areas[1], 200, places=5)
        bb = t.Shape.optimalBoundingBox(False, False)
        self.assertAlmostEqual(bb.XMax, 10, places=5)
        self.assertAlmostEqual(bb.ZMin, 0, places=5)
        self.assertAlmostEqual(bb.ZMax, 5, places=5)

    def testASketchCutsAHole(self):
        h = self.doc.addObject("Part::Plane", "H")
        h.Length, h.Width = 20, 20
        sk = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        sk.Placement = App.Placement(V(0, 0, 4), App.Rotation())
        sk.addGeometry(Part.Circle(V(10, 10, 0), V(0, 0, 1), 3))
        self.doc.recompute()
        t = self.doc.addObject("Surface::Trim", "Trim")
        t.Base = [(h, "Face1")]
        t.Tools = [(sk, "")]
        t.KeepPoints = [V(1, 1, 0)]
        self.recompute(t)
        self.assertEqual(len(t.Shape.Faces), 1)
        self.assertAlmostEqual(t.Shape.Area, 400 - math.pi * 9, places=4)

    def testATrimWithNothingKeptIsRefused(self):
        src, ln = self.base(), self.line(10)
        t = self.doc.addObject("Surface::Trim", "Trim")
        t.Base = [(src, "Face1")]
        t.Tools = [(ln, "Edge1")]
        self.doc.recompute()
        self.assertFalse(t.isValid())
        self.assertIn("keep", t.getStatusString())

    def testUntrimToTheNaturalBoundary(self):
        src, ln = self.base(), self.line(10)
        t = self.doc.addObject("Surface::Trim", "Trim")
        t.Base = [(src, "Face1")]
        t.Tools = [(ln, "Edge1")]
        t.Direction = V(0, 0, -1)
        t.KeepPoints = [V(3, 6, 0)]
        u = self.doc.addObject("Surface::Untrim", "Untrim")
        u.Face = (t, "Face1")
        self.recompute(t, u)
        self.assertAlmostEqual(u.Shape.Area, src.Shape.Area, delta=1e-6 * src.Shape.Area)

    def testUntrimToBeforeATrim(self):
        h = self.doc.addObject("Part::Plane", "H")
        h.Length, h.Width = 20, 20
        sk = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        sk.addGeometry(Part.Circle(V(10, 10, 0), V(0, 0, 1), 3))
        ln = self.doc.addObject("Part::Line", "Cut")
        ln.X1, ln.Y1, ln.Z1 = 15, -2, 0
        ln.X2, ln.Y2, ln.Z2 = 15, 22, 0
        self.doc.recompute()
        t1 = self.doc.addObject("Surface::Trim", "Hole")
        t1.Base = [(h, "Face1")]
        t1.Tools = [(sk, "")]
        t1.KeepPoints = [V(1, 1, 0)]
        t2 = self.doc.addObject("Surface::Trim", "Side")
        t2.Base = [(t1, "Face1")]
        t2.Tools = [(ln, "Edge1")]
        t2.KeepPoints = [V(1, 1, 0)]
        u = self.doc.addObject("Surface::Untrim", "Untrim")
        u.Face = (t2, "Face1")
        u.Before = t2
        self.recompute(t1, t2, u)
        # the plane is infinite: "natural" is its parameter box, and before
        # the second trim it had its hole
        self.assertAlmostEqual(u.Shape.Area, 400 - math.pi * 9, places=4)
        u.Before = t1
        self.recompute(u)
        self.assertAlmostEqual(u.Shape.Area, 400, places=4)
