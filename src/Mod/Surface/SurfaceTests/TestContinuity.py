# SPDX-License-Identifier: LGPL-2.1-or-later

"""Edge continuity (docs/Surface.md sec 7, U4): how two faces meet along an
edge, measured -- a box's crease, a fillet's tangent edge with its jump of
curvature, a blend surface's edges G2 -- from Surface.Geom.continuity."""

import math
import unittest

import FreeCAD as App
import Part
import Surface
from FreeCAD import Vector as V


def facesOf(shape, edge):
    """The faces of `shape` that `edge` is an edge of"""
    return [f for f in shape.Faces if any(e.isSame(edge) for e in f.Edges)]


class TestContinuity(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceContinuity")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def testABoxsCrease(self):
        box = Part.makeBox(10, 10, 10)
        e = box.Edges[0]
        a, b = facesOf(box, e)
        r = Surface.Geom.continuity(e, a, b)
        self.assertAlmostEqual(math.degrees(r["MaxAngle"]), 90, places=6)
        self.assertEqual(r["Order"], 0)

    def testAFilletIsTangentWithACurvatureJump(self):
        box = self.doc.addObject("Part::Box", "Box")
        self.doc.recompute()
        f = self.doc.addObject("Part::Fillet", "Fillet")
        f.Base = box
        f.Edges = [(1, 2.0, 2.0)]
        self.doc.recompute()
        shape = f.Shape
        rounds = [x for x in shape.Faces if isinstance(x.Surface, Part.Cylinder)]
        self.assertEqual(len(rounds), 1)
        edge = [e for e in rounds[0].Edges if isinstance(e.Curve, Part.Line)][0]
        flat = [x for x in facesOf(shape, edge) if not x.isSame(rounds[0])][0]
        r = Surface.Geom.continuity(edge, rounds[0], flat)
        self.assertLess(r["MaxAngle"], 1e-6)
        self.assertAlmostEqual(r["MaxCurvatureGap"], 0.5, places=5)
        self.assertEqual(r["Order"], 1)

    def testABlendIsWhatItSaid(self):
        low = self.doc.addObject("Part::Plane", "Low")
        low.Length = low.Width = 10
        high = self.doc.addObject("Part::Plane", "High")
        high.Length = high.Width = 10
        high.Placement = App.Placement(V(15, 0, 3), App.Rotation(V(0, 1, 0), -16.7))
        self.doc.recompute()
        e1 = [
            e
            for e in low.Shape.Edges
            if abs(e.BoundBox.XMin - 10) < 1e-6 and e.BoundBox.XLength < 1e-6
        ][0]
        e2 = [
            e
            for e in high.Shape.Edges
            if abs(e.BoundBox.XMin - 15) < 1e-6 and e.BoundBox.XLength < 1e-6
        ][0]
        b = self.doc.addObject("Surface::BlendSurface", "Blend")
        b.Edge1 = (low, self.nameOf(low.Shape, e1))
        b.Edge2 = (high, self.nameOf(high.Shape, e2))
        b.Side1Continuity = b.Side2Continuity = "G2"
        self.doc.recompute()
        self.assertTrue(b.isValid(), b.getStatusString())
        r = Surface.Geom.continuity(e1, b.Shape.Faces[0], low.Shape.Faces[0])
        self.assertLess(r["MaxGap"], 1e-6)
        self.assertLess(math.degrees(r["MaxAngle"]), 0.1)
        self.assertEqual(r["Order"], 2, r)

    def nameOf(self, shape, edge):
        for i, e in enumerate(shape.Edges):
            if e.isSame(edge):
                return "Edge%d" % (i + 1)
        return None

    def testWhatItTakes(self):
        box = Part.makeBox(1, 1, 1)
        with self.assertRaises(TypeError):
            Surface.Geom.continuity(box.Faces[0], box.Faces[0], box.Faces[1])
