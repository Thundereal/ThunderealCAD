# SPDX-License-Identifier: LGPL-2.1-or-later

"""Pipeshell (docs/Surface.md sec 5.1, 9, U3): profiles swept along a spine
with every mode of OCCT's pipe shell, each measured where it differs from
the others: the Frenet frame on a helix, a fixed binormal keeping a profile
upright, an auxiliary spine turning it, a support face, sharp-turn
transitions, a solid."""

import math
import unittest

import FreeCAD as App
import Part
from FreeCAD import Vector as V


class TestPipeShell(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfacePipeShell")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))
            self.assertFalse(o.Shape.isNull(), o.Name)
            self.assertTrue(o.Shape.isValid(), o.Name)

    def feature(self, name, shape):
        o = self.doc.addObject("Part::Feature", name)
        o.Shape = shape
        return o

    def pipe(self, spine, profile, mode):
        p = self.doc.addObject("Surface::PipeShell", "Pipe")
        p.Spine = [(spine, "")]
        p.Profiles = [(profile, "")]
        p.Mode = mode
        return p

    def testFrenetOnAHelixMakesASolid(self):
        helix = self.feature("Helix", Part.makeHelix(4, 12, 5))
        start = helix.Shape.Edges[0].valueAt(helix.Shape.Edges[0].FirstParameter)
        tangent = helix.Shape.Edges[0].tangentAt(helix.Shape.Edges[0].FirstParameter)
        circle = self.feature("Circle", Part.Wire(Part.makeCircle(0.5, start, tangent)))
        p = self.pipe(helix, circle, "Frenet")
        p.Solid = True
        self.recompute(p)
        self.assertEqual(len(p.Shape.Solids), 1)
        length = helix.Shape.Length
        self.assertAlmostEqual(p.Shape.Volume, math.pi * 0.25 * length, delta=0.01 * p.Shape.Volume)

    def testABinormalKeepsTheProfileUpright(self):
        arc = self.feature("Arc", Part.Arc(V(0, 0, 0), V(10, 5, 0), V(20, 0, 0)).toShape())
        # a 2 x 1 rectangle standing on the spine's start, square to it
        t = arc.Shape.Edges[0].tangentAt(arc.Shape.Edges[0].FirstParameter)
        side = V(-t.y, t.x, 0)
        corners = [V(0, 0, 0) + side * -1, V(0, 0, 0) + side * 1]
        rect = Part.makePolygon(
            [corners[0], corners[1], corners[1] + V(0, 0, 1), corners[0] + V(0, 0, 1), corners[0]]
        )
        prof = self.feature("Rect", rect)
        p = self.pipe(arc, prof, "Binormal")
        p.Direction = V(0, 0, 1)
        self.recompute(p)
        bb = p.Shape.BoundBox
        self.assertAlmostEqual(bb.ZMin, 0, places=4)
        self.assertAlmostEqual(bb.ZMax, 1, places=4)

    def testAnAuxiliarySpineTurnsTheProfile(self):
        line = self.feature("Line", Part.makeLine(V(0, 0, 0), V(20, 0, 0)))
        # a helix about the X axis, one turn over the length, starting at +Y
        pts = [
            V(20 * k / 64, 5 * math.cos(2 * math.pi * k / 64), 5 * math.sin(2 * math.pi * k / 64))
            for k in range(65)
        ]
        c = Part.BSplineCurve()
        c.interpolate(pts)
        aux = self.feature("Aux", c.toShape())
        prof = self.feature("Bar", Part.makeLine(V(0, 0, 0), V(0, 1, 0)))
        p = self.pipe(line, prof, "Auxiliary")
        p.AuxiliarySpine = [(aux, "")]
        self.recompute(p)
        # half way the bar points the other way, to -Y
        self.assertLess(p.Shape.distToShape(Part.Vertex(V(10, -1, 0)))[0], 1e-2)
        corrected = self.pipe(line, prof, "CorrectedFrenet")
        self.recompute(corrected)
        self.assertGreater(corrected.Shape.distToShape(Part.Vertex(V(10, -1, 0)))[0], 0.5)

    def testOnASupportFace(self):
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        cyl.Radius, cyl.Height = 5, 10
        self.doc.recompute()
        side = [
            "Face%d" % (i + 1)
            for i, f in enumerate(cyl.Shape.Faces)
            if isinstance(f.Surface, Part.Cylinder)
        ][0]
        # a spine on the cylinder: a curve of its face, inside its parameters
        face = cyl.Shape.getElement(side)
        u1, u2, v1, v2 = face.ParameterRange
        a = App.Base.Vector2d(u1, v1 + 0.2 * (v2 - v1))
        b = App.Base.Vector2d(u1 + 1.5, v1 + 0.8 * (v2 - v1))
        edge = Part.Geom2d.Line2dSegment(a, b).toShape(face)
        self.assertLess(face.distToShape(edge)[0], 1e-6)
        sp = self.feature("Spine", edge)
        start = edge.valueAt(edge.FirstParameter)
        prof = self.feature("Bar", Part.makeLine(start, start + face.normalAt(a.x, a.y) * 2))
        p = self.pipe(sp, prof, "Support")
        p.Support = [(cyl, side)]
        self.recompute(p)
        self.assertGreater(p.Shape.Area, 1)

    def testASpineFromElsewhereOnTheSupport(self):
        # a helix on the cylinder, made on its own: no curve of it on the face
        cyl = self.doc.addObject("Part::Cylinder", "Cyl")
        cyl.Radius, cyl.Height = 5, 10
        helix = self.doc.addObject("Part::Helix", "Helix")
        helix.Pitch, helix.Height, helix.Radius = 20, 8, 5
        self.doc.recompute()
        side = [
            "Face%d" % (i + 1)
            for i, f in enumerate(cyl.Shape.Faces)
            if isinstance(f.Surface, Part.Cylinder)
        ][0]
        e = helix.Shape.Edges[0]
        start = e.valueAt(e.FirstParameter)
        prof = self.feature("Bar", Part.makeLine(start, start + V(start.x, start.y, 0) * 0.2))
        p = self.pipe(helix, prof, "Support")
        p.Support = [(cyl, side)]
        self.recompute(p)
        self.assertGreater(p.Shape.Area, 1)

    def testASharpTurnRounded(self):
        poly = self.feature("Poly", Part.makePolygon([V(0, 0, 0), V(10, 0, 0), V(10, 10, 0)]))
        circle = self.feature("Circle", Part.Wire(Part.makeCircle(1, V(0, 0, 0), V(1, 0, 0))))
        p = self.pipe(poly, circle, "CorrectedFrenet")
        p.Transition = "RoundCorner"
        p.Solid = True
        self.recompute(p)
        self.assertEqual(len(p.Shape.Solids), 1)
        self.assertGreater(p.Shape.Volume, math.pi * 18)

    def testNoSpineIsSaid(self):
        circle = self.feature("Circle", Part.Wire(Part.makeCircle(1)))
        p = self.doc.addObject("Surface::PipeShell", "Pipe")
        p.Profiles = [(circle, "")]
        self.doc.recompute()
        self.assertFalse(p.isValid())
        self.assertIn("spine", p.getStatusString())
