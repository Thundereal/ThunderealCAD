# SPDX-License-Identifier: LGPL-2.1-or-later

"""The blend surface (docs/Surface.md sec 5.1, 9, U3): a bridge between two
edges on two faces, G0 to G2 and a weight on each side, what each side
achieved read out, through an input change and a save and reopen."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part
from FreeCAD import Vector as V


def edgeAtX(shape, x):
    for i, e in enumerate(shape.Edges):
        bb = e.BoundBox
        if abs(bb.XMin - x) < 1e-6 and abs(bb.XMax - x) < 1e-6:
            return "Edge%d" % (i + 1)
    return None


def worstAngle(blend, face, edge):
    """The worst angle between the blend and `face` along `edge`, in degrees"""
    worst = 0
    for k in range(11):
        p = edge.valueAt(edge.FirstParameter + (edge.LastParameter - edge.FirstParameter) * k / 10)
        n1 = blend.normalAt(*blend.Surface.parameter(p))
        n2 = face.normalAt(*face.Surface.parameter(p))
        worst = max(worst, math.degrees(math.acos(min(1, abs(n1.dot(n2))))))
    return worst


class TestBlendSurface(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceBlendSurface")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))
            self.assertFalse(o.Shape.isNull(), o.Name)
            self.assertTrue(o.Shape.isValid(), o.Name)

    def gap(self):
        low = self.doc.addObject("Part::Plane", "Low")
        low.Length = low.Width = 10
        high = self.doc.addObject("Part::Plane", "High")
        high.Length = high.Width = 10
        high.Placement = App.Placement(V(15, 0, 3), App.Rotation(V(0, 1, 0), -16.7))
        self.doc.recompute()
        b = self.doc.addObject("Surface::BlendSurface", "Blend")
        b.Edge1 = (low, edgeAtX(low.Shape, 10))
        b.Edge2 = (high, edgeAtX(high.Shape, 15))
        return low, high, b

    def check(self, low, high, b, tol=0.1):
        face = b.Shape.Faces[0]
        e1 = low.Shape.getElement(b.Edge1[1][0])
        e2 = high.Shape.getElement(b.Edge2[1][0])
        self.assertLess(worstAngle(face, low.Shape.Faces[0], e1), tol)
        self.assertLess(worstAngle(face, high.Shape.Faces[0], e2), tol)
        self.assertLess(face.distToShape(e1)[0], 1e-6)
        self.assertLess(face.distToShape(e2)[0], 1e-6)

    def testG1ByDefault(self):
        low, high, b = self.gap()
        self.recompute(b)
        self.assertEqual((b.Side1Continuity, b.Side2Continuity), ("G1", "G1"))
        self.check(low, high, b)
        self.assertEqual(len(b.Achieved), 2)
        for line in b.Achieved:
            self.assertIn("G1", line)
            self.assertNotIn("not met", line)

    def testG2AndAWeight(self):
        low, high, b = self.gap()
        b.Side1Continuity = b.Side2Continuity = "G2"
        self.recompute(b)
        self.check(low, high, b)
        area = b.Shape.Area
        b.Side1Weight = 2.5
        self.recompute(b)
        self.check(low, high, b)
        self.assertGreater(abs(b.Shape.Area - area), 1e-3)

    def testAnInputChange(self):
        low, high, b = self.gap()
        self.recompute(b)
        high.Placement = App.Placement(V(16, 0, 5), App.Rotation(V(0, 1, 0), 10))
        self.recompute(b)
        self.check(low, high, b)

        path = os.path.join(tempfile.gettempdir(), "SurfaceBlendSurface.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        b = self.doc.Blend
        b.touch()
        self.recompute(b)
        self.check(self.doc.Low, self.doc.High, b)
        os.remove(path)

    def testPositionAlone(self):
        low, high, b = self.gap()
        b.Side1Continuity = b.Side2Continuity = "G0"
        self.recompute(b)
        self.assertEqual(list(b.Achieved), [])
        face = b.Shape.Faces[0]
        for obj, link in ((low, b.Edge1), (high, b.Edge2)):
            self.assertLess(face.distToShape(obj.Shape.getElement(link[1][0]))[0], 1e-6)
