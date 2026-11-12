# SPDX-License-Identifier: LGPL-2.1-or-later

"""A feature read from a file with a Placement keeps taking one.

Until 2022-09 a feature of this fork took the Placement of what it was built
on -- its base feature, or the plane of its sketch -- as upstream's still do.
Since then it is made where the body is and has none. Both are version 1,
so what tells an earlier feature is the Placement its file holds.

It matters to what reads that Placement. A sketch on a flat face gets only
its normal from the face and picks its X axis among the axes of the feature
the face belongs to: once such a feature was made again with no Placement,
the sketches on its faces could turn in their plane. In the model of
realthunder/FreeCAD issue #474 one turned a quarter, its constraints folded
it flat, and the pad over it was not made.
"""

import os
import tempfile
import unittest

import FreeCAD
import Part

V = FreeCAD.Vector


class TestEarlierPlacement(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestEarlierPlacement")
        self.Opened = None

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)
        if self.Opened:
            FreeCAD.closeDocument(self.Opened.Name)

    @staticmethod
    def rectangle(sketch, x, y):
        corners = [V(-x, -y, 0), V(x, -y, 0), V(x, y, 0), V(-x, y, 0)]
        sketch.addGeometry(
            [Part.LineSegment(corners[i], corners[(i + 1) % 4]) for i in range(4)]
        )

    def build(self):
        """A pad off the XZ plane, and a second pad over a sketch on the
        side face of the first that looks along X"""
        doc = self.Doc
        body = doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        sketch.Support = (doc.getObject("XZ_Plane"), [""])
        sketch.MapMode = "FlatFace"
        self.rectangle(sketch, 10, 3)
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 4
        pad.Midplane = True
        doc.recompute()
        side = [
            "Face%d" % (i + 1)
            for i, face in enumerate(pad.Shape.Faces)
            if face.normalAt(0, 0).isEqual(V(1, 0, 0), 1e-9)
        ]
        self.assertEqual(len(side), 1)
        sketch2 = body.newObject("Sketcher::SketchObject", "Sketch2")
        sketch2.Support = (pad, side)
        sketch2.MapMode = "FlatFace"
        # longer along the sketch's X than along its Y
        self.rectangle(sketch2, 1.5, 0.5)
        pad2 = body.newObject("PartDesign::Pad", "Pad2")
        pad2.Profile = sketch2
        pad2.Length = 2
        doc.recompute()
        self.assertEqual(pad2.State, ["Up-to-date"])

    def reopened(self):
        path = os.path.join(tempfile.gettempdir(), "PartDesignTestEarlierPlacement.FCStd")
        self.Doc.saveCopy(path)
        self.Opened = FreeCAD.openDocument(path)
        os.remove(path)
        return self.Opened

    @staticmethod
    def extent(shape):
        box = shape.BoundBox
        return (round(box.XLength, 6), round(box.YLength, 6), round(box.ZLength, 6))

    def testEarlierFeatureKeepsItsPlacement(self):
        self.build()
        doc = self.Doc
        # as a build of before 2022-09 made them: placed like upstream's,
        # which is version 0, and saved as version 1
        for name in ("Pad", "Pad2"):
            doc.getObject(name)._ProfileBasedVersion = 0
        doc.recompute()
        plane = doc.getObject("Sketch").Placement
        self.assertFalse(plane.isIdentity())
        self.assertTrue(doc.Pad.Placement.isSame(plane, 1e-9))
        self.assertTrue(doc.Pad2.Placement.isSame(plane, 1e-9))
        turn = doc.Sketch2.Placement
        extent = self.extent(doc.Sketch2.Shape)
        volume = doc.Pad2.Shape.Volume
        centre = doc.Pad2.Shape.CenterOfMass
        for name in ("Pad", "Pad2"):
            doc.getObject(name)._ProfileBasedVersion = 1

        doc = self.reopened()
        for obj in doc.Objects:
            obj.touch()
        doc.recompute()

        for name in ("Pad", "Pad2"):
            self.assertEqual(doc.getObject(name).State, ["Up-to-date"])
            self.assertEqual(doc.getObject(name)._ProfileBasedVersion, 1)
            self.assertTrue(doc.getObject(name).Placement.isSame(plane, 1e-9), name)
        self.assertTrue(doc.Sketch2.Placement.isSame(turn, 1e-9))
        self.assertEqual(self.extent(doc.Sketch2.Shape), extent)
        self.assertAlmostEqual(doc.Pad2.Shape.Volume, volume, 6)
        self.assertLess((doc.Pad2.Shape.CenterOfMass - centre).Length, 1e-7)

    def testTheSketchWouldTurn(self):
        """What the first test guards: the sketch on the side face lies
        another way under a placed pad than under an unplaced one"""
        self.build()
        doc = self.Doc
        unplaced = self.extent(doc.Sketch2.Shape)
        self.assertEqual(sorted(unplaced), [0, 1, 3])
        for name in ("Pad", "Pad2"):
            doc.getObject(name)._ProfileBasedVersion = 0
        doc.recompute()
        placed = self.extent(doc.Sketch2.Shape)
        self.assertEqual(sorted(placed), [0, 1, 3])
        self.assertNotEqual(placed, unplaced)

    def testFeatureOfNowHasNone(self):
        self.build()
        doc = self.reopened()
        for obj in doc.Objects:
            obj.touch()
        doc.recompute()
        for name in ("Pad", "Pad2"):
            self.assertEqual(doc.getObject(name).State, ["Up-to-date"])
            self.assertTrue(doc.getObject(name).Placement.isIdentity(), name)
        self.assertEqual(self.extent(doc.Sketch2.Shape), self.extent(self.Doc.Sketch2.Shape))
