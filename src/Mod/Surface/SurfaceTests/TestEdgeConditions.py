# SPDX-License-Identifier: LGPL-2.1-or-later

"""Edge conditions on the existing features (docs/Surface.md sec 5.1, 9, U1):
Sections at its first and last section, GeomFillSurface on each boundary --
through the network's match boundary. A loft's sides and the sides of a
fill of two curves have no curve of the user's, so the corners are met."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part
from FreeCAD import Vector as V


def spline(doc, name, *pts):
    c = Part.BSplineCurve()
    c.interpolate([V(*p) for p in pts])
    o = doc.addObject("Part::Feature", name)
    o.Shape = c.toShape()
    return o


def neighbour(doc, curve, direction, name="Neighbour"):
    """A face the curve is an edge of: the curve swept along `direction`"""
    ext = doc.addObject("Part::Extrusion", name)
    ext.Base = curve
    ext.DirMode = "Custom"
    ext.Dir = direction
    ext.LengthFwd = 5
    return ext


def edgeOn(shape, edge, tol=1e-6):
    for i, e in enumerate(shape.Edges):
        mid = Part.Vertex(e.valueAt((e.FirstParameter + e.LastParameter) / 2))
        if edge.distToShape(mid)[0] < tol and edge.distToShape(e.Vertexes[0])[0] < tol:
            return "Edge%d" % (i + 1)
    return None


def angleAlong(test, shape, other, edge):
    """The worst angle between the face of `shape` and the face `other`
    along `edge`, which both have"""
    face = shape.Faces[0]
    name = edgeOn(shape, edge, 1e-4)
    test.assertIsNotNone(name, "no edge of the surface on the shared curve")
    e = shape.getElement(name)
    # Where the edge is on the face by the edge's own curve on it, not by
    # projecting the point: a fill of two curves that meet has two of its
    # corners at that one point, both exact answers to the projection, and
    # which of them came back turned on the last bit of the arithmetic.
    onFace = face.curveOnSurface(e)
    worst = 0
    for k in range(41):
        t = e.FirstParameter + (e.LastParameter - e.FirstParameter) * k / 40
        pt = e.valueAt(t)
        if onFace:
            uv = onFace[0].value(t)
            n1 = face.normalAt(uv.x, uv.y)
        else:
            n1 = face.normalAt(*face.Surface.parameter(pt))
        n2 = other.normalAt(*other.Surface.parameter(pt))
        worst = max(worst, math.degrees(math.acos(min(1, abs(n1.dot(n2))))))
    return worst


class TestEdgeConditions(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceEdgeConditions")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))

    # -- Sections -------------------------------------------------------------

    def sections(self):
        s1 = spline(self.doc, "s1", (0, 0, 0), (0, 5, 1), (0, 10, 0))
        s2 = spline(self.doc, "s2", (5, 0, 2), (5, 5, 3), (5, 10, 2))
        s3 = spline(self.doc, "s3", (10, 0, 0), (10, 5, 1), (10, 10, 0))
        ext = neighbour(self.doc, s1, V(-1, 0, 1))
        self.doc.recompute()
        s = self.doc.addObject("Surface::Sections", "Sec")
        s.NSections = [(ext, edgeOn(ext.Shape, s1.Shape.Edges[0]))] + [
            (o, "Edge1") for o in (s2, s3)
        ]
        return s, ext, (s1, s2, s3)

    def testSectionsFirstG1ToItsOwnFace(self):
        s, ext, (s1, s2, s3) = self.sections()
        self.recompute(s)
        self.assertGreater(angleAlong(self, s.Shape, ext.Shape.Faces[0], s1.Shape.Edges[0]), 5)
        s.FirstSectionContinuity = "G1"
        self.recompute(s)
        self.assertLess(angleAlong(self, s.Shape, ext.Shape.Faces[0], s1.Shape.Edges[0]), 0.1)
        self.assertTrue(any("first section" in x and "not met" not in x for x in s.Achieved))
        # the sections still lie on it
        for c in (s1, s2, s3):
            self.assertLess(s.Shape.distToShape(c.Shape)[0], 1e-4, c.Name)

        path = os.path.join(tempfile.gettempdir(), "SurfaceEdgeConditions.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        s = self.doc.Sec
        self.assertEqual(s.FirstSectionContinuity, "G1")
        s.touch()
        self.recompute(s)
        self.assertLess(
            angleAlong(
                self, s.Shape, self.doc.Neighbour.Shape.Faces[0], self.doc.s1.Shape.Edges[0]
            ),
            0.1,
        )
        os.remove(path)

    def testSectionsLastG2Explicit(self):
        s, ext, (s1, s2, s3) = self.sections()
        other = neighbour(self.doc, s3, V(1, 0, 0.5), "Other")
        s.LastSectionContinuity = "G2"
        s.LastSectionSupport = (other, "Face1")
        self.recompute(other, s)
        self.assertLess(angleAlong(self, s.Shape, other.Shape.Faces[0], s3.Shape.Edges[0]), 0.1)
        line = [x for x in s.Achieved if "last section" in x]
        self.assertTrue(line and "G2" in line[0] and "not met" not in line[0], s.Achieved)

    def testSectionsOldFileUnchanged(self):
        # no condition: the same surface as before the conditions existed
        s, ext, _ = self.sections()
        self.recompute(s)
        self.assertEqual(s.FirstSectionContinuity, "G0")
        self.assertEqual(list(s.Achieved), [])

    # -- GeomFillSurface ------------------------------------------------------

    def testFillOfTwoCurvesG1(self):
        # two curves meeting at (10, 0, 0); the second leaves it along the
        # neighbour, (0, 1, -1), so that corner can be met, and the corner at
        # (0, 0, 0) is on a side the fill makes, free
        c1 = spline(self.doc, "c1", (0, 0, 0), (5, 0, 2), (10, 0, 0))
        c = Part.BSplineCurve()
        c.interpolate(
            [V(10, 0, 0), V(10, 5, -2), V(10, 10, 0)],
            InitialTangent=V(0, 1, -1),
            FinalTangent=V(0, 1, 1),
        )
        c2 = self.doc.addObject("Part::Feature", "c2")
        c2.Shape = c.toShape()
        ext = neighbour(self.doc, c1, V(0, -1, 1))
        self.doc.recompute()
        g = self.doc.addObject("Surface::GeomFillSurface", "GFS")
        g.BoundaryList = [(ext, edgeOn(ext.Shape, c1.Shape.Edges[0])), (c2, "Edge1")]
        self.recompute(g)
        self.assertGreater(angleAlong(self, g.Shape, ext.Shape.Faces[0], c1.Shape.Edges[0]), 5)
        g.Boundary1Continuity = "G1"
        self.recompute(g)
        self.assertLess(angleAlong(self, g.Shape, ext.Shape.Faces[0], c1.Shape.Edges[0]), 0.1)
        self.assertLess(g.Shape.distToShape(c2.Shape)[0], 1e-6, "the other curve stays")

    def testFillOfFourCurvesReportsItsCorners(self):
        # four curves fix the corners: a neighbour they do not agree with is
        # met between them and reported at them
        pts = [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0)]
        mids = [(5, 0, 2), (10, 5, -2), (5, 10, 2), (0, 5, -2)]
        loop = [spline(self.doc, "c%d" % i, pts[i], mids[i], pts[(i + 1) % 4]) for i in range(4)]
        ext = neighbour(self.doc, loop[0], V(0, -1, 1))
        self.doc.recompute()
        g = self.doc.addObject("Surface::GeomFillSurface", "GFS")
        g.BoundaryList = [(ext, edgeOn(ext.Shape, loop[0].Shape.Edges[0]))] + [
            (c, "Edge1") for c in loop[1:]
        ]
        g.FillType = "Curved"
        g.Boundary1Continuity = "G1"
        self.recompute(g)
        self.assertTrue(any("corner" in x for x in g.Achieved), g.Achieved)
