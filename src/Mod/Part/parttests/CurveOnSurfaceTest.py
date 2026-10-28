# SPDX-License-Identifier: LGPL-2.1-or-later

"""Part.CurveOnSurface: a 2D curve in a surface's (u, v), on the surface exactly
(docs/Sketch3D.md, decision f). It is evaluated, projected, transformed, copied and
saved as any curve, and its edge carries the 2D curve on the surface."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part

V = App.Vector
V2 = App.Base.Vector2d


def helix(radius=10.0, height=20.0):
    """Half a turn of a helix on a cylinder about Z: the line (0, 0) -> (pi, height)."""
    cyl = Part.Cylinder()
    cyl.Radius = radius
    line = Part.Geom2d.Line2dSegment(V2(0, 0), V2(math.pi, height))
    return Part.CurveOnSurface(line, cyl)


class CurveOnSurfaceTest(unittest.TestCase):
    def assertVector(self, a, b, tol=1e-9):
        self.assertLess((a - b).Length, tol, f"{a} != {b}")

    def testOnTheSurfaceExactly(self):
        c = helix()
        f, l = c.FirstParameter, c.LastParameter
        for i in range(11):
            u = f + (l - f) * i / 10
            p = c.value(u)
            # the line's parameter runs along its length: angle and height in proportion
            s = (u - f) / (l - f)
            self.assertVector(p, V(10 * math.cos(math.pi * s), 10 * math.sin(math.pi * s), 20 * s))
            self.assertAlmostEqual(math.hypot(p.x, p.y), 10, 12)
        self.assertVector(c.StartPoint, V(10, 0, 0))
        self.assertVector(c.EndPoint, V(-10, 0, 20))

    def testDerivatives(self):
        # D1, D2, D3 by the chain rule against central differences, on a curved 2D curve
        # on a curved surface.
        sph = Part.Sphere()
        sph.Radius = 7
        arc = Part.Geom2d.BSplineCurve2d()
        arc.interpolate([V2(0.1, -0.5), V2(1.0, 0.2), V2(2.0, -0.1), V2(2.8, 0.6)])
        c = Part.CurveOnSurface(arc, sph)
        h = 1e-5
        u = 0.5 * (c.FirstParameter + c.LastParameter)
        # getDn(u) is (point, D1, ..., Dn)
        nth = [c.value, lambda t: c.getD1(t)[1], lambda t: c.getD2(t)[2], lambda t: c.getD3(t)[3]]
        for n in (1, 2, 3):
            numeric = nth[n - 1](u + h).sub(nth[n - 1](u - h)).multiply(1 / (2 * h))
            exact = nth[n](u)
            self.assertLess((exact - numeric).Length, 1e-4 * max(1, exact.Length), n)

    def testEdgeCarriesTheCurveOnTheSurface(self):
        c = helix()
        for e in (c.toShape(), Part.Edge(c)):
            self.assertTrue(e.isValid())
            pcurve = e.curveOnSurface(0)
            self.assertIsNotNone(pcurve)
            self.assertEqual(type(pcurve[1]).__name__, "Cylinder")
            # its 3D curve matches the exact one
            for i in range(9):
                u = c.FirstParameter + (c.LastParameter - c.FirstParameter) * i / 8
                self.assertLess(e.distToShape(Part.Vertex(c.value(u)))[0], 1e-6)
        part = c.toShape(c.FirstParameter, 0.5 * (c.FirstParameter + c.LastParameter))
        self.assertVector(
            part.Vertexes[-1].Point, c.value(0.5 * (c.FirstParameter + c.LastParameter)), 1e-7
        )

    def testTransformAndCopy(self):
        c = helix()
        u = 0.3 * c.LastParameter
        before = c.value(u)
        plm = App.Placement(V(5, -3, 2), App.Rotation(V(1, 1, 0), 35))
        moved = c.copy()
        moved.transform(plm.toMatrix())
        self.assertVector(moved.value(u), plm.multVec(before))
        self.assertVector(c.value(u), before)  # the copy is its own
        # a plane scaled: its (u, v) scale with it, and so does the 2D curve
        flat = Part.CurveOnSurface(Part.Geom2d.Line2dSegment(V2(1, 1), V2(4, 3)), Part.Plane())
        p = flat.value(flat.LastParameter)
        flat.scale(V(0, 0, 0), 2)
        self.assertVector(flat.EndPoint, p * 2)

    def testLikeOtherCurves(self):
        c = helix()
        # projection and parameter
        u = 0.4 * c.LastParameter
        self.assertAlmostEqual(c.parameter(c.value(u)), u, 6)
        # reversed: the ends swap
        r = c.copy()
        r.reverse()
        self.assertVector(r.StartPoint, c.EndPoint)
        # an approximation by a B-spline
        b = c.toBSpline()
        self.assertLess(b.toShape().distToShape(Part.Vertex(c.value(u)))[0], 1e-6)
        # its parts
        self.assertEqual(type(c.Curve2d).__name__, "Line2dSegment")
        self.assertEqual(type(c.Surface).__name__, "Cylinder")
        self.assertTrue(c.isSame(helix(), 1e-9, 1e-9))
        self.assertFalse(c.isSame(helix(height=21), 1e-9, 1e-9))

    def testSavedInADocument(self):
        doc = App.newDocument("CurveOnSurface")
        try:
            obj = doc.addObject("App::FeaturePython", "Holder")
            obj.addProperty("Part::PropertyGeometryList", "Geo")
            obj.Geo = [helix()]
            path = os.path.join(tempfile.mkdtemp(), "curveonsurface.FCStd")
            doc.saveAs(path)
            App.closeDocument(doc.Name)
            doc = App.openDocument(path)
            c = doc.getObject("Holder").Geo[0]
            self.assertEqual(type(c).__name__, "CurveOnSurface")
            self.assertTrue(c.isSame(helix(), 1e-12, 1e-12))
        finally:
            App.closeDocument(doc.Name)


if __name__ == "__main__":
    unittest.main()
