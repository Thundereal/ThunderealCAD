# SPDX-License-Identifier: LGPL-2.1-or-later

"""The network surface (docs/Surface.md sec 5.1 and 9, U1): profiles and
guides from any edges -- a 3D sketch's among them -- spanned at degree 3,
in any order and either way round; its face and edges named, a reference
that survives an input change and a save and reopen; a gap read out; and
Surface.Geom.network for scripts."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part
from FreeCAD import Vector as V


def wave(u, v):
    x, y = 20 * u, 12 * v
    return V(x, y + 1.5 * math.sin(u * 3), 2 * math.sin(x / 4) * math.cos(y / 5))


VS = [0.0, 0.3, 0.55, 0.8, 1.0]
US = [0.0, 0.35, 0.7, 1.0]


def through(alongU, fixed, cross, extra, f=wave):
    """A curve through f at every crossing and `extra` points between"""
    pts = []
    for i, t in enumerate(cross):
        pts.append(f(t, fixed) if alongU else f(fixed, t))
        if i + 1 < len(cross):
            for k in range(1, extra + 1):
                s = t + (cross[i + 1] - t) * k / (extra + 1)
                pts.append(f(s, fixed) if alongU else f(fixed, s))
    c = Part.BSplineCurve()
    c.interpolate(pts)
    return c


def waveCurves(f=wave):
    profiles = [through(True, v, US, 1 + k % 3, f) for k, v in enumerate(VS)]
    guides = [through(False, u, VS, 1 + k % 2, f) for k, u in enumerate(US)]
    return profiles, guides


def edgeOn(shape, edge, tol=1e-3):
    """The name of the edge of `shape` lying on `edge` (the network's edges
    lie on its outer curves within its fit tolerance)"""
    for i, e in enumerate(shape.Edges):
        mid = Part.Vertex(e.valueAt((e.FirstParameter + e.LastParameter) / 2))
        if edge.distToShape(mid)[0] < tol and edge.distToShape(e.Vertexes[0])[0] < tol:
            return "Edge%d" % (i + 1)
    return None


class TestNetwork(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceNetwork")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def features(self, curves, prefix):
        objs = []
        for i, c in enumerate(curves):
            o = self.doc.addObject("Part::Feature", "%s%d" % (prefix, i))
            o.Shape = c.toShape()
            objs.append(o)
        return objs

    def network(self, profiles, guides):
        n = self.doc.addObject("Surface::Network", "Network")
        n.Profiles = [(o, "Edge1") for o in profiles]
        n.Guides = [(o, "Edge1") for o in guides]
        return n

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))
            self.assertFalse(o.Shape.isNull(), o.Name)
            self.assertTrue(o.Shape.isValid(), o.Name)

    def assertOnFace(self, n, objs):
        face = n.Shape.Faces[0]
        for o in objs:
            self.assertLess(face.distToShape(o.Shape)[0], 2 * n.FitTolerance, o.Name)
            mid = o.Shape.Edges[0].valueAt(
                (o.Shape.Edges[0].FirstParameter + o.Shape.Edges[0].LastParameter) / 2
            )
            self.assertLess(face.distToShape(Part.Vertex(mid))[0], 2 * n.FitTolerance, o.Name)

    def testBuilds(self):
        p, g = waveCurves()
        profiles, guides = self.features(p, "p"), self.features(g, "g")
        n = self.network(profiles, guides)
        self.recompute(n)
        self.assertEqual(len(n.Shape.Faces), 1)
        s = n.Shape.Faces[0].Surface
        self.assertIsInstance(s, Part.BSplineSurface)
        self.assertEqual((s.UDegree, s.VDegree), (3, 3))
        self.assertFalse(s.isURational() or s.isVRational())
        self.assertLessEqual(max(s.NbUPoles, s.NbVPoles), 60)
        self.assertOnFace(n, profiles + guides)
        self.assertLess(n.MaxGap.Value, 1e-6)
        self.assertLessEqual(n.MaxFitDeviation.Value, n.FitTolerance)
        # the outer curves are its boundary
        self.assertEqual(len(n.Shape.Edges), 4)
        for o in (profiles[0], profiles[-1], guides[0], guides[-1]):
            self.assertIsNotNone(edgeOn(n.Shape, o.Shape), o.Name)

    def testAnyOrderEitherWay(self):
        p, g = waveCurves()
        n = self.network(self.features(p, "p"), self.features(g, "g"))
        self.recompute(n)
        p2 = [p[2], p[0].copy(), p[4], p[1].copy(), p[3]]
        p2[1].reverse()
        p2[3].reverse()
        g2 = [g[3], g[1], g[0], g[2]]
        m = self.network(self.features(p2, "q"), self.features(g2, "h"))
        self.recompute(m)
        self.assertAlmostEqual(m.Shape.Area, n.Shape.Area, places=4)
        for u, v in ((0.2, 0.3), (0.5, 0.5), (0.9, 0.1)):
            pt = wave(u, v)
            self.assertLess(m.Shape.distToShape(Part.Vertex(pt))[0], 0.05)
            self.assertAlmostEqual(
                m.Shape.distToShape(Part.Vertex(pt))[0],
                n.Shape.distToShape(Part.Vertex(pt))[0],
                places=4,
            )

    def testNamesSurviveAnInputChange(self):
        p, g = waveCurves()
        profiles, guides = self.features(p, "p"), self.features(g, "g")
        n = self.network(profiles, guides)
        self.recompute(n)
        shape = n.Shape
        self.assertGreater(shape.ElementMapSize, 0)
        for i in range(len(shape.Edges)):
            self.assertTrue(shape.getElementMappedName("Edge%d" % (i + 1)))
        self.assertTrue(shape.getElementMappedName("Face1"))
        first = edgeOn(shape, profiles[0].Shape)
        h = self.doc.addObject("App::FeaturePython", "Ref")
        h.addProperty("App::PropertyLinkSub", "Ref")
        h.Ref = (n, [first])

        # the curves lifted, the first profile with them: the reference follows it
        def lifted(u, v):
            q = wave(u, v)
            return V(q.x, q.y, q.z + 1 + u)

        p2, g2 = waveCurves(lifted)
        for o, c in zip(profiles + guides, p2 + g2):
            o.Shape = c.toShape()
        self.recompute(n)
        self.assertEqual(h.Ref[1][0], edgeOn(n.Shape, profiles[0].Shape))

        # and through a save and reopen
        path = os.path.join(tempfile.gettempdir(), "SurfaceNetwork.FCStd")
        self.doc.saveAs(path)
        area = n.Shape.Area
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        n, h = self.doc.Network, self.doc.Ref
        self.assertAlmostEqual(n.Shape.Area, area, places=6)
        n.touch()
        self.recompute(n)
        self.assertEqual(h.Ref[1][0], edgeOn(n.Shape, self.doc.p0.Shape))
        os.remove(path)

    def testGap(self):
        p, g = waveCurves()
        p[2].translate(V(0, 0, 0.01))
        n = self.network(self.features(p, "p"), self.features(g, "g"))
        self.doc.recompute()
        self.assertFalse(n.isValid())
        self.assertIn("do not meet", n.getStatusString())
        n.Tolerance = 0.02
        self.recompute(n)
        self.assertAlmostEqual(n.MaxGap.Value, 0.01, delta=1e-3)

    def testFrom3DSketch(self):
        sk = self.doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        p, g = waveCurves()
        for c in p + g:
            sk.addGeometry(c)
        self.doc.recompute()

        # the sketch names its edges in the order of the wires it builds, not
        # of its geometry: each curve's edge is found as a user picks it, by
        # where it is
        def named(c):
            ends = (c.StartPoint, c.EndPoint)
            for i, e in enumerate(sk.Shape.Edges):
                pts = (e.Vertexes[0].Point, e.Vertexes[-1].Point)
                if any(all(a.isEqual(b, 1e-7) for a, b in zip(ends, o)) for o in (pts, pts[::-1])):
                    return "Edge%d" % (i + 1)
            self.fail("no edge of the sketch on %s" % c)

        n = self.doc.addObject("Surface::Network", "Network")
        names = [named(c) for c in p + g]
        self.assertEqual(len(set(names)), len(names))
        n.Profiles = [(sk, x) for x in names[: len(p)]]
        n.Guides = [(sk, x) for x in names[len(p) :]]
        self.recompute(n)
        face = n.Shape.Faces[0]
        for e in sk.Shape.Edges:
            self.assertLess(face.distToShape(e)[0], 2 * n.FitTolerance)

    def testGeomFunction(self):
        import Surface

        p, g = waveCurves()
        s, info = Surface.Geom.network(p, g)
        self.assertIsInstance(s, Part.BSplineSurface)
        self.assertEqual((s.UDegree, s.VDegree), (3, 3))
        self.assertEqual(info["ProfileOrder"], [0, 1, 2, 3, 4])
        self.assertEqual(info["GuideOrder"], [0, 1, 2, 3])
        self.assertLess(info["MaxGap"], 1e-6)
        self.assertLessEqual(info["MaxFitDeviation"], 1e-4)
        # edges as well as curves
        s2, _ = Surface.Geom.network([c.toShape() for c in p], [c.toShape() for c in g])
        self.assertEqual((s2.NbUPoles, s2.NbVPoles), (s.NbUPoles, s.NbVPoles))
        with self.assertRaises(ValueError):
            Surface.Geom.network(p[:1], g)
