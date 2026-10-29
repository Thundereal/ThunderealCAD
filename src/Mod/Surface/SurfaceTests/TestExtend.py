# SPDX-License-Identifier: LGPL-2.1-or-later

"""Extend per edge (docs/Surface.md sec 5.2, 9, U2): an edge of a face
extended by a length, naturally -- the same surface carried on, G2 with
itself, measured -- or linearly, straight and tangent; the edge followed
through an input change; an edge inside the face refused; the whole-face
mode as it was."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part
from FreeCAD import Vector as V


def wave(u, v):
    return V(20 * u, 12 * v, 2 * math.sin(3 * u) * math.cos(2 * v) + u * v)


def waveFace():
    s = Part.BSplineSurface()
    s.interpolate([[wave(i / 6, j / 5) for j in range(6)] for i in range(7)])
    return s.toShape()


def edgeAt(shape, point, tol=1e-6):
    """The name of the edge of `shape` through `point`'s nearest point on it"""
    best, name = None, None
    for i, e in enumerate(shape.Edges):
        mid = e.valueAt((e.FirstParameter + e.LastParameter) / 2)
        d = (mid - point).Length
        if best is None or d < best:
            best, name = d, "Edge%d" % (i + 1)
    return name


def meanCurvature(surface, u, v):
    return surface.curvature(u, v, "Mean")


class TestExtend(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceExtend")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))
            self.assertFalse(o.Shape.isNull(), o.Name)
            self.assertTrue(o.Shape.isValid(), o.Name)

    def extend(self, source, edges, length, mode="Natural"):
        e = self.doc.addObject("Surface::Extend", "Ext")
        e.Face = (source, "Face1")
        e.Mode = "Edges"
        e.Edges = [(source, x) for x in edges]
        e.Length = length
        e.Extension = mode
        return e

    def testNaturalIsG2WithItself(self):
        src = self.doc.addObject("Part::Feature", "Src")
        src.Shape = waveFace()
        self.doc.recompute()
        old = src.Shape.Faces[0].Surface
        u1, u2, v1, v2 = old.bounds()
        name = edgeAt(src.Shape, old.value(u2, (v1 + v2) / 2))
        e = self.extend(src, [name], 5)
        self.recompute(e)
        face = e.Shape.Faces[0]
        s = face.Surface
        self.assertEqual((s.UDegree, s.VDegree), (old.UDegree, old.VDegree))
        self.assertGreater(face.Area, src.Shape.Area + 4 * 12)
        # what was there stays
        for u, v in ((0.1, 0.2), (0.5, 0.5), (0.95, 0.9)):
            p = old.value(u1 + (u2 - u1) * u, v1 + (v2 - v1) * v)
            self.assertLess(face.distToShape(Part.Vertex(p))[0], 1e-7)
        # the new edge is the length from the old one, on average
        oldEdge = src.Shape.getElement(name)
        # the new edge: not the old one, nor the opposite one 20 away
        far = [x for x in face.Edges if 1 < x.distToShape(oldEdge)[0] < 10]
        self.assertEqual(len(far), 1)
        d = [
            far[0].distToShape(Part.Vertex(oldEdge.valueAt(t)))[0]
            for t in (
                oldEdge.FirstParameter,
                (oldEdge.FirstParameter + oldEdge.LastParameter) / 2,
                oldEdge.LastParameter,
            )
        ]
        self.assertAlmostEqual(sum(d) / 3, 5, delta=0.25)
        # G2 with itself across the old edge, measured
        for v in (0.2, 0.5, 0.8):
            vv = v1 + (v2 - v1) * v
            a = meanCurvature(s, u2 - 1e-5, vv)
            b = meanCurvature(s, u2 + 1e-5, vv)
            self.assertAlmostEqual(a, b, delta=1e-4)

    def testLinearIsStraight(self):
        src = self.doc.addObject("Part::Feature", "Src")
        src.Shape = waveFace()
        self.doc.recompute()
        old = src.Shape.Faces[0].Surface
        u1, u2, v1, v2 = old.bounds()
        name = edgeAt(src.Shape, old.value((u1 + u2) / 2, v1))
        e = self.extend(src, [name], 3, "Linear")
        self.recompute(e)
        s = e.Shape.Faces[0].Surface
        nu1, nu2, nv1, nv2 = s.bounds()
        self.assertLess(nv1, v1)
        for u in (0.2, 0.6):
            uu = u1 + (u2 - u1) * u
            base = old.value(uu, v1)
            pts = [s.value(uu, nv1 + (v1 - nv1) * k / 4) for k in range(5)]
            line = Part.LineSegment(base, pts[0])
            for p in pts[1:4]:
                self.assertLess(line.toShape().distToShape(Part.Vertex(p))[0], 1e-7)
            # tangent to the surface there
            t = old.tangent(uu, v1)[1]
            self.assertLess(abs((pts[0] - base).normalize().cross(t).Length), 1e-6)

    def testAnInputChangeKeepsTheEdge(self):
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        cyl.Radius, cyl.Height = 5, 10
        self.doc.recompute()
        # the cylinder's side, and its top edge
        top = [
            "Edge%d" % (i + 1)
            for i, x in enumerate(cyl.Shape.Edges)
            if isinstance(x.Curve, Part.Circle) and abs(x.Curve.Center.z - 10) < 1e-9
        ]
        side = [
            "Face%d" % (i + 1)
            for i, f in enumerate(cyl.Shape.Faces)
            if isinstance(f.Surface, Part.Cylinder)
        ]
        e = self.doc.addObject("Surface::Extend", "Ext")
        e.Face = (cyl, side[0])
        e.Mode = "Edges"
        e.Edges = [(cyl, top[0])]
        e.Length = 3
        e.Extension = "Linear"
        self.recompute(e)
        bb = e.Shape.optimalBoundingBox(False, False)
        self.assertAlmostEqual(bb.ZMax, 13, places=5)
        self.assertAlmostEqual(bb.ZMin, 0, places=5)
        cyl.Height = 20
        self.recompute(e)
        bb = e.Shape.optimalBoundingBox(False, False)
        self.assertAlmostEqual(bb.ZMax, 23, places=5)
        self.assertAlmostEqual(bb.ZMin, 0, places=5)

        path = os.path.join(tempfile.gettempdir(), "SurfaceExtend.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        e = self.doc.Ext
        self.assertEqual((e.Mode, e.Extension), ("Edges", "Linear"))
        e.touch()
        self.recompute(e)
        self.assertAlmostEqual(e.Shape.optimalBoundingBox(False, False).ZMax, 23, places=5)
        os.remove(path)

    def testAnEdgeInsideTheFaceIsRefused(self):
        outer = Part.makePolygon([V(0, 0, 0), V(10, 0, 0), V(10, 10, 0), V(0, 10, 0), V(0, 0, 0)])
        hole = Part.Wire(Part.makeCircle(2, V(5, 5, 0)))
        src = self.doc.addObject("Part::Feature", "Src")
        src.Shape = Part.Face([outer, hole])
        self.doc.recompute()
        circle = [
            "Edge%d" % (i + 1)
            for i, x in enumerate(src.Shape.Edges)
            if isinstance(x.Curve, Part.Circle)
        ]
        e = self.extend(src, circle, 2)
        self.doc.recompute()
        self.assertFalse(e.isValid())
        self.assertIn("untrim", e.getStatusString())

    def testTheWholeFaceAsItWas(self):
        src = self.doc.addObject("Part::Feature", "Src")
        src.Shape = waveFace()
        e = self.doc.addObject("Surface::Extend", "Ext")
        e.Face = (src, "Face1")
        self.recompute(e)
        self.assertEqual(e.Mode, "Face")
        self.assertGreater(e.Shape.Area, src.Shape.Area * 1.15)
