# SPDX-License-Identifier: LGPL-2.1-or-later

"""Surface features in a PartDesign Body, as a sketch is: in the body, not
solid, never its Tip, in the body's frame, and used by its features.

The curves come into the body through a SubShapeBinder, as a sketch's
outside references would."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part

import TestSketcherApp

V = App.Vector

# Every feature of the workbench
TYPES = [
    "Surface::Filling",
    "Surface::Sewing",
    "Surface::Cut",
    "Surface::GeomFillSurface",
    "Surface::Extend",
    "Surface::FeatureBlendCurve",
    "Surface::Sections",
    "Surface::Network",
    "Surface::BlendSurface",
    "Surface::Split",
    "Surface::Trim",
    "Surface::Untrim",
    "Surface::TwoRailSweep",
    "Surface::PipeShell",
    "Surface::Isocurve",
    "Surface::CurveOnSurface",
    "Surface::IntersectionCurve",
    "Surface::JoinCurves",
    "Surface::ExtendCurve",
]


def f(u, v):
    return V(10 * u, 10 * v, 2 * math.sin(math.pi * u) * math.sin(math.pi * v))


def curve(alongU, fixed):
    c = Part.BSplineCurve()
    c.interpolate([f(t / 4, fixed) if alongU else f(fixed, t / 4) for t in range(5)])
    return c


class TestBody(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceBody")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def network(self):
        """A body with a pad, and a network in it on curves bound in"""
        body = self.doc.addObject("PartDesign::Body", "Body")
        src = self.doc.addObject("Part::Feature", "Curves")
        src.Shape = Part.Compound(
            [curve(a, x).toShape() for a in (True, False) for x in (0, 0.5, 1)]
        )
        binder = body.newObject("PartDesign::SubShapeBinder", "Binder")
        binder.Support = [(src, ["Edge%d" % i for i in range(1, 7)])]
        # the curves, not a face of the loop round them
        binder.MakeFace = False
        sketch = body.newObject("Sketcher::SketchObject", "Square")
        sketch.Support = (
            [o for o in body.Origin.OriginFeatures if o.Role == "XY_Plane"][0],
            [""],
        )
        sketch.MapMode = "FlatFace"
        TestSketcherApp.CreateRectangleSketch(sketch, (2, 2), (6, 6))
        self.doc.recompute()
        self.assertEqual(len(binder.Shape.Edges), 6)
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 1
        self.doc.recompute()
        net = self.doc.addObject("Surface::Network", "Net")
        body.addObject(net)
        net.Profiles = [(binder, "")]
        self.doc.recompute()
        return body, net, pad, sketch

    def testEveryFeatureIsAllowed(self):
        body = self.doc.addObject("PartDesign::Body", "Body")
        for t in TYPES:
            obj = self.doc.addObject(t, t.split("::")[1])
            body.addObject(obj)
            self.assertIn(obj, body.Group, t)

    def testANetworkInABody(self):
        body, net, pad, _ = self.network()
        self.assertTrue(net.isValid(), net.getStatusString())
        self.assertIn(net, body.Group)
        # not solid: the pad stays the Tip, and the body's shape is the pad's
        self.assertEqual(body.Tip, pad)
        self.assertAlmostEqual(body.Shape.Volume, pad.Shape.Volume, places=6)
        # the body moved: on curves bound in from outside, the network stays
        # on them, where they are in the world (the binder keeps them there)
        box = net.Shape.BoundBox
        iso = self.doc.addObject("Surface::Isocurve", "Iso")
        body.addObject(iso)
        top = [i for i, fc in enumerate(pad.Shape.Faces) if abs(fc.BoundBox.ZMin - 1) < 1e-9]
        iso.Face = (pad, "Face%d" % (top[0] + 1))
        self.doc.recompute()
        self.assertTrue(iso.isValid(), iso.getStatusString())
        isoBox = iso.Shape.BoundBox
        body.Placement = App.Placement(V(0, 0, 5), App.Rotation())
        self.doc.recompute()
        self.assertTrue(net.isValid(), net.getStatusString())
        self.assertAlmostEqual(net.getGlobalPlacement().Base.z, 5, places=9)
        self.assertLess(net.Shape.distToShape(self.doc.getObject("Binder").Shape)[0], 1e-6)
        world = net.Shape.transformed(net.getGlobalPlacement().toMatrix())
        self.assertAlmostEqual(world.BoundBox.ZMax, box.ZMax, places=6)
        # on the body's own geometry, a feature goes with the body: its shape
        # in the body's frame is as it was
        self.assertTrue(iso.isValid(), iso.getStatusString())
        self.assertAlmostEqual(iso.Shape.BoundBox.ZMax, isoBox.ZMax, places=9)
        self.assertAlmostEqual(iso.getGlobalPlacement().Base.z, 5, places=9)
        self.assertEqual(body.Tip, pad)

    def testAPadUpToTheSurface(self):
        body, net, pad, sketch = self.network()
        pad.Type = "UpToFace"
        pad.UpToFace = (net, ["Face1"])
        self.doc.recompute()
        self.assertTrue(pad.isValid(), pad.getStatusString())
        # the square under the bump: the pad rises to it, its top corners on
        # the surface and nothing of it above
        corners = [v for v in pad.Shape.Vertexes if v.Point.z > 0.5]
        self.assertEqual(len(corners), 4)
        for v in corners:
            self.assertLess(net.Shape.distToShape(v)[0], 1e-4, v.Point)
        self.assertLessEqual(pad.Shape.BoundBox.ZMax, net.Shape.BoundBox.ZMax + 1e-6)
        self.assertEqual(body.Tip, pad)

    def testSavedAndReopened(self):
        body, net, pad, _ = self.network()
        path = os.path.join(tempfile.gettempdir(), "SurfaceBody.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        body = self.doc.getObject("Body")
        net = self.doc.getObject("Net")
        self.doc.recompute()
        self.assertIn(net, body.Group)
        self.assertTrue(net.isValid(), net.getStatusString())
        self.assertEqual(body.Tip, self.doc.getObject("Pad"))
