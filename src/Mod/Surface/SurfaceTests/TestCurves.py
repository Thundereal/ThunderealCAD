# SPDX-License-Identifier: LGPL-2.1-or-later

"""The curve tools (docs/Surface.md sec 5.3, 9, U5): isocurves, curves laid
on a surface, intersection curves, joined curves and extended curves, each
as a feature; the curves on a surface lie on their face exactly, as curves
of its surface."""

import math
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


class TestCurves(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceCurves")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))
            self.assertFalse(o.Shape.isNull(), o.Name)

    def feature(self, name, shape):
        o = self.doc.addObject("Part::Feature", name)
        o.Shape = shape
        return o

    def assertOnTheFace(self, edges, face):
        """Each edge a curve of the face's surface, and on the face"""
        for e in edges:
            self.assertLess(face.distToShape(e)[0], 1e-7)
            mid = e.valueAt((e.FirstParameter + e.LastParameter) / 2)
            u, v = face.Surface.parameter(mid)
            self.assertLess((face.Surface.value(u, v) - mid).Length, 1e-6)

    def testIsocurves(self):
        outer = Part.makePolygon([V(0, 0, 0), V(10, 0, 0), V(10, 10, 0), V(0, 10, 0), V(0, 0, 0)])
        hole = Part.Wire(Part.makeCircle(2, V(5, 5, 0)))
        # Bullseye: Part.Face([outer, hole]) adds the hole rather than cut it
        src = self.feature("Holed", Part.makeFace([outer, hole], "Part::FaceMakerBullseye"))
        self.assertTrue(src.Shape.isValid())
        self.assertAlmostEqual(src.Shape.Area, 100 - 4 * math.pi, places=6)
        iso = self.doc.addObject("Surface::Isocurve", "Iso")
        iso.Face = (src, "Face1")
        iso.NumberU, iso.NumberV = 3, 0
        self.recompute(iso)
        self.assertEqual(len(iso.Shape.Edges), 4)
        self.assertAlmostEqual(iso.Shape.Length, 26, places=5)
        self.assertOnTheFace(iso.Shape.Edges, src.Shape.Faces[0])
        iso.NumberV = 1
        self.recompute(iso)
        self.assertEqual(len(iso.Shape.Edges), 6)

    def testACurveLaidOnASurface(self):
        face = self.feature("Wave", waveFace())
        line = self.doc.addObject("Part::Line", "Line")
        line.X1, line.Y1, line.Z1, line.X2, line.Y2, line.Z2 = 10, -2, 6, 10, 14, 6
        c = self.doc.addObject("Surface::CurveOnSurface", "OnFace")
        c.Curves = [(line, "Edge1")]
        c.Faces = [(face, "Face1")]
        c.Direction = V(0, 0, -1)
        self.recompute(c)
        self.assertGreaterEqual(len(c.Shape.Edges), 1)
        self.assertOnTheFace(c.Shape.Edges, face.Shape.Faces[0])
        # along -Z the curve is at x = 10, from one side of the face to the other
        bb = c.Shape.optimalBoundingBox(False, False)
        self.assertAlmostEqual(bb.XMin, 10, places=6)
        self.assertAlmostEqual(bb.YLength, 12, places=4)
        # a ribbon along it, tangent to the face there
        c.Ribbon = 2
        self.recompute(c)
        self.assertEqual(len(c.Shape.Faces), 1)
        r = c.Shape.Faces[0]
        p = c.Shape.Edges[0].valueAt(c.Shape.Edges[0].FirstParameter + 1)
        n1 = r.normalAt(*r.Surface.parameter(p))
        n2 = face.Shape.Faces[0].normalAt(*face.Shape.Faces[0].Surface.parameter(p))
        self.assertLess(math.degrees(math.acos(min(1, abs(n1.dot(n2))))), 0.5)

    def testAnIntersection(self):
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        cyl.Radius, cyl.Height = 5, 10
        plane = self.doc.addObject("Part::Plane", "Plane")
        plane.Length = plane.Width = 30
        # tilted 20 deg about X through z = 5 over the axis: it cuts the
        # cylinder's side all round, between its ends
        plane.Placement = App.Placement(
            V(-15, -15, 5 - 15 * math.tan(math.radians(20))), App.Rotation(V(1, 0, 0), 20)
        )
        self.doc.recompute()
        x = self.doc.addObject("Surface::IntersectionCurve", "Cut")
        x.Shape1 = [(cyl, "")]
        x.Shape2 = [(plane, "")]
        self.recompute(x)
        self.assertGreaterEqual(len(x.Shape.Edges), 1)
        for e in x.Shape.Edges:
            self.assertLess(cyl.Shape.distToShape(e)[0], 1e-6)
            self.assertLess(plane.Shape.distToShape(e)[0], 1e-6)
        # an ellipse round the cylinder
        self.assertGreater(x.Shape.Length, 2 * math.pi * 5)

    def testJoinedCurves(self):
        s = 5 / math.sqrt(2)
        a = self.feature("A", Part.Arc(V(5, 0, 0), V(s, s, 0), V(0, 5, 0)).toShape())
        b = self.feature("B", Part.Arc(V(-5, 0, 0), V(-s, s, 0), V(0, 5, 0)).toShape())
        j = self.doc.addObject("Surface::JoinCurves", "Join")
        j.Curves = [(b, "Edge1"), (a, "Edge1")]
        self.recompute(j)
        self.assertEqual(len(j.Shape.Edges), 1)
        self.assertAlmostEqual(j.Shape.Length, 5 * math.pi, places=3)
        # the arcs reach (-5, 0, 0) heading down -Y: this leaves at right angles
        corner = self.feature("Corner", Part.makeLine(V(-5, 0, 0), V(-11, 0, 0)))
        j.Curves = [(b, "Edge1"), (a, "Edge1"), (corner, "Edge1")]
        self.recompute(j)
        self.assertEqual(len(j.Shape.Edges), 2)

    def testExtendedCurves(self):
        line = self.doc.addObject("Part::Line", "Line")
        line.X1, line.Y1, line.Z1, line.X2, line.Y2, line.Z2 = 0, 0, 0, 10, 0, 0
        e = self.doc.addObject("Surface::ExtendCurve", "Longer")
        e.Curve = (line, "Edge1")
        e.Extension = "Linear"
        e.StartLength, e.EndLength = 2, 5
        self.recompute(e)
        self.assertAlmostEqual(e.Shape.Length, 17, places=6)
        bb = e.Shape.BoundBox
        self.assertAlmostEqual(bb.XMin, -2, places=6)
        self.assertAlmostEqual(bb.XMax, 15, places=6)
        # an arc carried on naturally is the same circle further round
        arc = self.feature("Arc", Part.Arc(V(5, 0, 0), V(0, 5, 0), V(-5, 0, 0)).toShape())
        n = self.doc.addObject("Surface::ExtendCurve", "Round")
        n.Curve = (arc, "Edge1")
        n.EndLength = 3
        self.recompute(n)
        edge = n.Shape.Edges[0]
        for k in range(11):
            p = edge.valueAt(
                edge.FirstParameter + (edge.LastParameter - edge.FirstParameter) * k / 10
            )
            self.assertAlmostEqual(p.Length, 5, places=6)
        end = edge.valueAt(edge.LastParameter)
        self.assertAlmostEqual((end - V(-5, 0, 0)).Length, 3, places=6)
