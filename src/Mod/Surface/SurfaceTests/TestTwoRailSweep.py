# SPDX-License-Identifier: LGPL-2.1-or-later

"""The two-rail sweep (docs/Surface.md sec 5.1, 9, U3): sections swept
between two rails that scale them; the rails are its sides; an input change
and a save and reopen; a section off the rails named."""

import os
import tempfile
import unittest

import FreeCAD as App
import Part
from FreeCAD import Vector as V


class TestTwoRailSweep(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceTwoRailSweep")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))
            self.assertFalse(o.Shape.isNull(), o.Name)
            self.assertTrue(o.Shape.isValid(), o.Name)

    def line(self, name, a, b):
        o = self.doc.addObject("Part::Line", name)
        o.X1, o.Y1, o.Z1 = a
        o.X2, o.Y2, o.Z2 = b
        return o

    def arch(self, name, a, b, height):
        mid = (V(*a) + V(*b)) * 0.5 + V(0, 0, height)
        o = self.doc.addObject("Part::Feature", name)
        o.Shape = Part.Arc(V(*a), mid, V(*b)).toShape()
        return o

    def sweep(self):
        r1 = self.line("Rail1", (0, 0, 0), (20, 0, 0))
        r2 = self.line("Rail2", (0, 10, 0), (20, 16, 0))
        sec = self.arch("Section", (0, 0, 0), (0, 10, 0), 5)
        s = self.doc.addObject("Surface::TwoRailSweep", "Sweep")
        s.Rail1 = (r1, "Edge1")
        s.Rail2 = (r2, "Edge1")
        s.Sections = [(sec, "Edge1")]
        return r1, r2, sec, s

    def testSweptAndScaled(self):
        r1, r2, sec, s = self.sweep()
        self.recompute(s)
        face = s.Shape.Faces[0]
        for o in (r1, r2, sec):
            self.assertLess(face.distToShape(o.Shape)[0], 2e-4, o.Name)
        # the far end's chord is 16 long: its half circle is 8 high
        self.assertLess(face.distToShape(Part.Vertex(V(20, 8, 8)))[0], 2e-3)
        # the rails are its sides
        self.assertEqual(len(s.Shape.Edges), 4)

    def testAnInputChange(self):
        r1, r2, sec, s = self.sweep()
        self.recompute(s)
        r2.X2, r2.Y2 = 20, 20
        self.recompute(s)
        face = s.Shape.Faces[0]
        self.assertLess(face.distToShape(Part.Vertex(V(20, 10, 10)))[0], 2e-3)
        self.assertLess(face.distToShape(r2.Shape)[0], 2e-4)

        path = os.path.join(tempfile.gettempdir(), "SurfaceTwoRailSweep.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        s = self.doc.Sweep
        s.touch()
        self.recompute(s)
        self.assertLess(s.Shape.distToShape(Part.Vertex(V(20, 10, 10)))[0], 2e-3)
        os.remove(path)

    def testASectionOffTheRails(self):
        r1, r2, sec, s = self.sweep()
        sec.Placement = App.Placement(V(0, 0, 1), App.Rotation())
        self.doc.recompute()
        self.assertFalse(s.isValid())
        self.assertIn("section 1", s.getStatusString())

    def testSectionsAtCurvedRailsEnds(self):
        """Sections on the ends of rails that leave them at an angle: a
        section's far end has no foot square on the first rail, and the
        sweep still finds which end is on it"""

        def spline(name, *pts):
            c = Part.BSplineCurve()
            c.interpolate([V(*p) for p in pts])
            o = self.doc.addObject("Part::Feature", name)
            o.Shape = c.toShape()
            return o

        r1 = spline("Rail1", (0, 0, 0), (20, -4, 1), (40, 0, 0))
        r2 = spline("Rail2", (0, 15, 0), (20, 22, 2), (40, 25, 0))
        s1 = self.arch("S1", (0, 0, 0), (0, 15, 0), 6)
        s2 = self.arch("S2", (40, 0, 0), (40, 25, 0), 3)
        s = self.doc.addObject("Surface::TwoRailSweep", "Sweep")
        s.Rail1 = (r1, "Edge1")
        s.Rail2 = (r2, "Edge1")
        s.Sections = [(s1, "Edge1"), (s2, "Edge1")]
        self.recompute(s)
        face = s.Shape.Faces[0]
        for o in (r1, r2, s1, s2):
            self.assertLess(face.distToShape(o.Shape)[0], 2e-4, o.Name)
        # either section given the other way round
        s1.Shape = Part.Arc(V(0, 15, 0), V(0, 7.5, 6), V(0, 0, 0)).toShape()
        self.recompute(s)
