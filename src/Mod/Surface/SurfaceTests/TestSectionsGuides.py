# SPDX-License-Identifier: LGPL-2.1-or-later

"""Sections with guides (docs/Surface.md sec 5.1, 9, U3): a loft whose
sides follow curves of the user's. Sections plus guides is a network, and
is built as one; the end conditions still apply; a loft without guides is
what it was."""

import math
import unittest

import FreeCAD as App
import Part
from FreeCAD import Vector as V


def wave(u, v):
    x, y = 20 * u, 12 * v
    return V(x, y + 1.5 * math.sin(u * 3), 2 * math.sin(x / 4) * math.cos(y / 5))


US = [0.0, 0.5, 1.0]
VS = [0.0, 0.4, 1.0]


def through(alongU, fixed, cross, f=wave):
    pts = []
    for i, t in enumerate(cross):
        pts.append(f(t, fixed) if alongU else f(fixed, t))
        if i + 1 < len(cross):
            s = (t + cross[i + 1]) / 2
            pts.append(f(s, fixed) if alongU else f(fixed, s))
    c = Part.BSplineCurve()
    c.interpolate(pts)
    return c


class TestSectionsGuides(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceSectionsGuides")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))
            self.assertFalse(o.Shape.isNull(), o.Name)

    def features(self, curves, prefix):
        objs = []
        for i, c in enumerate(curves):
            o = self.doc.addObject("Part::Feature", "%s%d" % (prefix, i))
            o.Shape = c.toShape()
            objs.append(o)
        return objs

    def curves(self):
        sections = self.features([through(True, v, US) for v in VS], "s")
        guides = self.features([through(False, u, VS) for u in US], "g")
        return sections, guides

    def testWithGuidesItIsTheNetwork(self):
        sections, guides = self.curves()
        s = self.doc.addObject("Surface::Sections", "Loft")
        s.NSections = [(o, "Edge1") for o in sections]
        s.Guides = [(o, "Edge1") for o in guides]
        n = self.doc.addObject("Surface::Network", "Net")
        n.Profiles = [(o, "Edge1") for o in sections]
        n.Guides = [(o, "Edge1") for o in guides]
        self.recompute(s, n)
        self.assertAlmostEqual(s.Shape.Area, n.Shape.Area, places=6)
        # the middle guide is on it, which a loft of the sections alone is not
        mid = guides[1].Shape
        self.assertLess(s.Shape.distToShape(mid)[0], 2e-4)
        loft = self.doc.addObject("Surface::Sections", "Plain")
        loft.NSections = [(o, "Edge1") for o in sections]
        self.recompute(loft)
        e = mid.Edges[0]
        far = max(
            loft.Shape.distToShape(Part.Vertex(e.valueAt(t)))[0]
            for t in (e.FirstParameter + (e.LastParameter - e.FirstParameter) * k / 8 for k in range(9))
        )
        self.assertGreater(far, 1e-2)

    def testAGuideMissingASectionIsNamed(self):
        sections, guides = self.curves()
        short = self.doc.addObject("Part::Feature", "short")
        short.Shape = through(False, 0.5, [0.0, 0.4]).toShape()
        s = self.doc.addObject("Surface::Sections", "Loft")
        s.NSections = [(o, "Edge1") for o in sections]
        s.Guides = [(guides[0], "Edge1"), (short, "Edge1"), (guides[2], "Edge1")]
        self.doc.recompute()
        self.assertFalse(s.isValid())
        self.assertIn("do not meet", s.getStatusString())

    def testAnEndConditionStillApplies(self):
        sections, guides = self.curves()
        ext = self.doc.addObject("Part::Extrusion", "Neighbour")
        ext.Base = sections[0]
        ext.DirMode = "Custom"
        ext.Dir = V(0, -1, 0.5)
        ext.LengthFwd = 5
        self.doc.recompute()
        s = self.doc.addObject("Surface::Sections", "Loft")
        s.NSections = [(o, "Edge1") for o in sections]
        s.Guides = [(o, "Edge1") for o in guides]
        s.FirstSectionContinuity = "G1"
        s.FirstSectionSupport = (ext, "Face1")
        self.recompute(s)
        line = [x for x in s.Achieved if "first section" in x]
        self.assertTrue(line and "G1" in line[0], s.Achieved)
        # the outer guides are its sides, which the condition leaves alone
        for g in (guides[0], guides[-1]):
            self.assertLess(s.Shape.distToShape(g.Shape)[0], 2e-4, g.Name)

    def testWithoutGuidesAsItWas(self):
        sections, _ = self.curves()
        s = self.doc.addObject("Surface::Sections", "Loft")
        s.NSections = [(o, "Edge1") for o in sections]
        self.recompute(s)
        self.assertEqual(len(s.Guides), 0)
        self.assertEqual(len(s.Shape.Faces), 1)
