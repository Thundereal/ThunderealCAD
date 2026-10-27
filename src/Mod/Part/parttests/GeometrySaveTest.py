# SPDX-License-Identifier: LGPL-2.1-or-later

"""Every curve and surface geometry keeps in a document. The surfaces, and the trimmed
and offset curves, had no saved form ("Not yet implemented"), so a geometry list
holding one -- a 3D sketch's reference to a curved face -- could not be saved."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part

V = App.Vector


def geometries():
    bs = Part.BSplineSurface()
    bs.interpolate([[V(x, y, math.sin(x + y)) for y in range(4)] for x in range(4)])
    bez = Part.BezierSurface()
    cyl = Part.Cylinder()
    cyl.Radius = 3
    cone = Part.Cone()
    tor = Part.Toroid()
    sph = Part.Sphere()
    sph.Radius = 4
    rev = Part.SurfaceOfRevolution(Part.LineSegment(V(2, 0, 0), V(3, 0, 5)), V(), V(0, 0, 1))
    ext = Part.SurfaceOfExtrusion(Part.Circle(V(), V(0, 0, 1), 2), V(0, 1, 1))
    off = Part.OffsetSurface(Part.Sphere(), 0.5)
    trim = Part.RectangularTrimmedSurface(Part.Cylinder(), 0.0, 1.0, 0.0, 2.0)
    oc = Part.OffsetCurve(Part.Circle(V(), V(0, 0, 1), 5), 1.0, V(0, 0, 1))
    # (a plain trimmed curve has no Python constructor: trim() makes arcs and the like)
    return [bs, bez, cyl, cone, tor, sph, rev, ext, off, trim, oc]


def samples(g):
    if hasattr(g, "bounds") and not hasattr(g, "FirstParameter"):
        u0, u1, v0, v1 = g.bounds()
        u0, u1 = max(u0, -2), min(u1, 2)
        v0, v1 = max(v0, -2), min(v1, 2)
        return [
            g.value(u0 + (u1 - u0) * i / 3, v0 + (v1 - v0) * j / 3)
            for i in range(4)
            for j in range(4)
        ]
    return [
        g.value(g.FirstParameter + (g.LastParameter - g.FirstParameter) * i / 5) for i in range(6)
    ]


class GeometrySaveTest(unittest.TestCase):
    def testEveryKindKeeps(self):
        doc = App.newDocument("GeometrySave")
        try:
            obj = doc.addObject("App::FeaturePython", "Holder")
            obj.addProperty("Part::PropertyGeometryList", "Geo")
            before = geometries()
            obj.Geo = before
            path = os.path.join(tempfile.mkdtemp(), "geometrysave.FCStd")
            doc.saveAs(path)
            App.closeDocument(doc.Name)
            doc = App.openDocument(path)
            after = doc.getObject("Holder").Geo
            self.assertEqual([type(g).__name__ for g in after], [type(g).__name__ for g in before])
            for a, b in zip(before, after):
                for p, q in zip(samples(a), samples(b)):
                    self.assertLess((p - q).Length, 1e-9, type(a).__name__)
        finally:
            App.closeDocument(doc.Name)


if __name__ == "__main__":
    unittest.main()
