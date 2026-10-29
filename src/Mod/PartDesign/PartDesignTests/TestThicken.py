# SPDX-License-Identifier: LGPL-2.1-or-later

"""PartDesign Thicken: faces -- a surface's, or a solid's -- made into a solid
of a thickness and added to the body, or taken from it (docs/Surface.md)."""

import math
import os
import tempfile
import unittest

import FreeCAD
import Part

import TestSketcherApp

V = FreeCAD.Vector


def bump(u, v):
    return V(40 * u, 30 * v, 20 + 6 * math.sin(math.pi * u) * math.sin(math.pi * v))


def bumpFace():
    """A B-spline face 40 x 30, rising by up to 6 in its middle, its normal up"""
    pts = [[bump(i / 6, j / 6) for j in range(7)] for i in range(7)]
    s = Part.BSplineSurface()
    s.interpolate(pts)
    f = Part.Face(s)
    if f.normalAt(0.5, 0.5).z < 0:
        f.reverse()
    return f


class TestThicken(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument("PartDesignTestThicken")
        self.body = self.doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def surface(self):
        """The bump bound into the body, as a surface would be"""
        src = self.doc.addObject("Part::Feature", "Bump")
        src.Shape = bumpFace()
        binder = self.body.newObject("PartDesign::SubShapeBinder", "Binder")
        binder.Support = [(src, "Face1")]
        self.doc.recompute()
        return binder

    def pad(self, height=10):
        sk = self.body.newObject("Sketcher::SketchObject", "Square")
        sk.Support = (
            [o for o in self.body.Origin.OriginFeatures if o.Role == "XY_Plane"][0],
            [""],
        )
        sk.MapMode = "FlatFace"
        TestSketcherApp.CreateRectangleSketch(sk, (0, 0), (10, 10))
        self.doc.recompute()
        pad = self.body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sk
        pad.Length = height
        self.doc.recompute()
        return pad

    def faceAt(self, obj, test):
        for i, f in enumerate(obj.Shape.Faces):
            if test(f):
                return "Face%d" % (i + 1)
        return None

    def thicken(self, links, value, **props):
        th = self.body.newObject("PartDesign::Thicken", "Thicken")
        th.Faces = links
        th.Value = value
        for k, v in props.items():
            setattr(th, k, v)
        self.doc.recompute()
        self.assertTrue(th.isValid(), th.getStatusString())
        return th

    def filled(self, face, offset, start=0.0):
        """The solid between a face and its offset, built the other way: Part's
        offset with fill, a loft of the two boundaries"""
        if start:
            face = face.makeOffsetShape(start, 1e-7)
        return face.makeOffsetShape(offset, 1e-7, fill=True).Volume

    def testThickenASurface(self):
        binder = self.surface()
        face = binder.Shape.Faces[0]
        th = self.thicken([(binder, "Face1")], 2)
        self.assertTrue(th.Shape.isValid())
        self.assertEqual(len(th.Shape.Solids), 1)
        # the bump is convex up: more than the area times the thickness, as
        # much as the solid between the face and its offset made otherwise
        self.assertGreater(th.Shape.Volume, face.Area * 2)
        self.assertAlmostEqual(th.Shape.Volume / self.filled(face, 2), 1, delta=1e-4)
        self.assertEqual(self.body.Tip, th)
        # along the face's normal, up, from the face
        self.assertAlmostEqual(th.Shape.BoundBox.ZMin, 20, delta=1e-6)
        self.assertGreater(th.Shape.ElementMapSize, 0)

    def testReversedAndMidplane(self):
        binder = self.surface()
        face = binder.Shape.Faces[0]
        zs = []
        expect = (self.filled(face, 2), self.filled(face, -2), self.filled(face, 2, -1))
        for props, volume in zip(({}, {"Reversed": True}, {"Midplane": True}), expect):
            th = self.thicken([(binder, "Face1")], 2, **props)
            self.assertAlmostEqual(th.Shape.Volume / volume, 1, delta=1e-4, msg=str(props))
            zs.append(th.Shape.BoundBox.ZMin)
            self.body.removeObject(th)
            self.doc.removeObject(th.Name)
            self.doc.recompute()
        up, down, mid = zs
        self.assertAlmostEqual(up, 20, delta=1e-6)
        self.assertAlmostEqual(down, 18, delta=0.05)
        self.assertAlmostEqual(mid, 19, delta=0.05)

    def testAPadsFace(self):
        pad = self.pad()
        top = self.faceAt(pad, lambda f: abs(f.BoundBox.ZMin - 10) < 1e-9)
        th = self.thicken([(pad, top)], 5)
        self.assertAlmostEqual(th.Shape.Volume, 1500, places=6)
        self.assertAlmostEqual(th.Shape.BoundBox.ZMax, 15, places=9)
        # into the pad, taken from it: a pocket 5 deep
        th.Reversed = True
        th.AddSubType = "Subtractive"
        self.doc.recompute()
        self.assertTrue(th.isValid(), th.getStatusString())
        self.assertAlmostEqual(th.Shape.Volume, 500, places=6)

    def testTwoFacesAtACrease(self):
        pad = self.pad()
        top = self.faceAt(pad, lambda f: abs(f.BoundBox.ZMin - 10) < 1e-9)
        side = self.faceAt(pad, lambda f: abs(f.BoundBox.XMax - 10) < 1e-9 and abs(f.BoundBox.XMin - 10) < 1e-9)
        th = self.thicken([(pad, top), (pad, side)], 1)
        self.assertEqual(len(th.Shape.Solids), 1)
        bb = th.Shape.BoundBox
        self.assertAlmostEqual(bb.ZMax, 11, places=6)
        self.assertAlmostEqual(bb.XMax, 11, places=6)
        # the pad, a slab on its top and one on its side, round the edge
        # between them: more than the two slabs, less than their boxes
        self.assertGreater(th.Shape.Volume, 1000 + 100 + 100)
        self.assertLess(th.Shape.Volume, 1000 + 110 + 110)

    def testSavedAndReopened(self):
        binder = self.surface()
        th = self.thicken([(binder, "Face1")], 2)
        volume = th.Shape.Volume
        path = os.path.join(tempfile.gettempdir(), "PartDesignTestThicken.FCStd")
        self.doc.saveAs(path)
        FreeCAD.closeDocument(self.doc.Name)
        self.doc = FreeCAD.openDocument(path)
        th = self.doc.getObject("Thicken")
        th.touch()
        self.doc.recompute()
        self.assertTrue(th.isValid(), th.getStatusString())
        self.assertAlmostEqual(th.Shape.Volume, volume, places=6)
