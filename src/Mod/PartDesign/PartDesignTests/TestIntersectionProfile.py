# SPDX-License-Identifier: LGPL-2.1-or-later

"""A pad on a sketch's cut of the body it stands on.

A sketch that refers to a face by its intersection with the sketch plane
gets the cut as a B-spline, and simplifies it (ExternalBSplineMaxDegree,
ExternalBSplineTolerance: degree 5, within 1e-4). The simpler curve is some
1e-5 off the face it was cut from, and crosses it dozens of times. An edge
made of it at the default tolerance of 1e-7 claims to be on neither side,
and a pad built on it -- its wall standing on the face along that edge -- was
fused into the body wrongly without a word: in the model of
realthunder/FreeCAD issue #617 the pad added nothing at all.

The curve now carries how far it may be from the one it stands for (a
Part.GeometryDoubleExtension named SimplifiedWithin), the sketch's edge
takes that as its tolerance, and the pieces a face maker cuts the edge
into keep it.

The volumes wanted are those of the body fused with the pad's solid moved
0.01 along the pad either way, halved.
"""

import unittest

import FreeCAD
import Part

V = FreeCAD.Vector


class TestIntersectionProfile(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestIntersectionProfile")
        doc = self.Doc
        self.Body = doc.addObject("PartDesign::Body", "Body")
        # a torus about a tilted axis
        sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        sketch.AttachmentSupport = (doc.getObject("XY_Plane"), [""])
        sketch.MapMode = "FlatFace"
        sketch.addGeometry(Part.LineSegment(V(15, 40, 0), V(9, 0, 0)))
        sketch.addGeometry(Part.Circle(V(39, 12, 0), V(0, 0, 1), 12))
        doc.recompute()
        self.Torus = self.Body.newObject("PartDesign::Revolution", "Revolution")
        self.Torus.Profile = (sketch, ["InternalFace1"])
        self.Torus.ReferenceAxis = (sketch, ["Edge1"])
        self.Torus.Angle = 360
        doc.recompute()
        # its cut by the YZ plane: two closed curves
        self.Cut = self.Body.newObject("Sketcher::SketchObject", "Sketch001")
        self.Cut.AttachmentSupport = (doc.getObject("YZ_Plane"), [""])
        self.Cut.MapMode = "FlatFace"
        doc.recompute()
        self.Cut.addExternal(self.Torus.Name, "Face1", True, True)
        doc.recompute()

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def pad(self, profile, reversed_, want):
        pad = self.Body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = profile
        pad.Length = 60
        pad.Reversed = reversed_
        self.Doc.recompute()
        self.assertTrue(pad.isValid(), pad.getStatusString())
        shape = pad.Shape
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.Volume / want, 1.0, delta=1e-4)

    def testTheCutSaysHowFarItIs(self):
        self.assertAlmostEqual(self.Torus.Shape.Volume, 79270.1506, delta=0.01)
        curves = self.Cut.ExternalGeo[2:]
        self.assertEqual(len(curves), 2)
        within = []
        for curve in curves:
            self.assertEqual(curve.Degree, 5)
            self.assertTrue(curve.hasExtensionOfName("SimplifiedWithin"))
            within.append(curve.getExtensionOfName("SimplifiedWithin").Value)
        for value in within:
            self.assertGreater(value, 1e-6)
            self.assertLess(value, 1.1e-4)
        edges = self.Cut.Shape.Edges
        self.assertEqual(len(edges), 2)
        for edge in edges:
            self.assertAlmostEqual(edge.Tolerance, max(within), delta=1e-9)

    def testPadOnOneFace(self):
        self.pad((self.Cut, ["InternalFace1"]), False, 93743.25)

    def testPadOnOneFaceReversed(self):
        self.pad((self.Cut, ["InternalFace1"]), True, 102168.09)

    def testPadOnTheSketchReversed(self):
        # both closed curves, each one edge
        self.pad(self.Cut, True, 125066.03)
