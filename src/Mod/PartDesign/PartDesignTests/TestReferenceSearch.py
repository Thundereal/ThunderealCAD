# SPDX-License-Identifier: LGPL-2.1-or-later

"""A reference whose element has lost its name is looked for by geometry,
and once more a hundred times looser where it is not there to 1e-7.

A feature made again by another build may put the same element a little
off: in the model of realthunder/FreeCAD issue #474 the spline edges of a
swept helix come back 4e-7 from where the file has them, written with other
poles, and one where a pad meets the helix reversed. Three datum lines and
a fillet lost their edges to it.

What the looser search finds is taken only where it is the one element
there, and what is further off than 1e-5 stays missing.
"""

import math
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector


class TestReferenceSearch(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestReferenceSearch")

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    @staticmethod
    def spline(swell):
        """A spline between two fixed ends, swollen by  swell  midway"""
        count = 9
        points = []
        for i in range(count):
            s = i / (count - 1)
            points.append(V(10 * s, 3 + math.sin(3 * s) + swell * math.sin(math.pi * s), 0))
        curve = Part.BSplineCurve()
        curve.interpolate(points)
        return curve

    def redrawn(self, swell):
        """The reference to the top edge over a pad's spline, after the
        spline is drawn again that much off: (before, after)"""
        doc = self.Doc
        body = doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        curve = self.spline(0)
        start, end = curve.StartPoint, curve.EndPoint
        sketch.addGeometry(
            [
                Part.LineSegment(V(0, 0, 0), V(10, 0, 0)),
                Part.LineSegment(V(10, 0, 0), end),
                Part.LineSegment(start, V(0, 0, 0)),
            ]
        )
        drawn = sketch.addGeometry(curve)
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 5
        doc.recompute()

        def top():
            return [
                "Edge%d" % (i + 1)
                for i, edge in enumerate(pad.Shape.Edges)
                if edge.Curve.TypeId == "Part::GeomBSplineCurve"
                and abs(edge.Vertexes[0].Point.z - 5) < 1e-9
            ]

        before = top()
        self.assertEqual(len(before), 1)
        ref = doc.addObject("App::FeaturePython", "Ref")
        ref.addProperty("App::PropertyLinkSub", "S")
        ref.S = (pad, before)
        doc.recompute()

        # Another geometry of the sketch, so the pad's edge over it has
        # another name and the reference is looked for by where it was
        sketch.addGeometry(self.spline(swell))
        sketch.delGeometry(drawn)
        doc.recompute()
        self.assertEqual(pad.State, ["Up-to-date"])
        self.assertEqual(top(), before)
        return before[0], ref.S[1][0]

    def testFoundWhereItWas(self):
        before, after = self.redrawn(0)
        self.assertEqual(after, before)

    def testFoundALittleOff(self):
        """4e-7 off midway: not there to 1e-7, and the one edge there to 1e-5"""
        before, after = self.redrawn(4e-7)
        self.assertEqual(after, before)

    def testMissingFurtherOff(self):
        before, after = self.redrawn(2e-5)
        self.assertEqual(after, "?" + before)
