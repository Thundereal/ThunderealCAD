# SPDX-License-Identifier: LGPL-2.1-or-later

"""A reference kept in two properties moves once when a document written
with another element map version is recomputed.

Fixtures/TwinRefs_map-1-15-70200.FCStd is the first four features of the
model of realthunder/FreeCAD issue #672, written by 0.20 with element map
1.15.70200.4: an octagon padded, a sketch on a side face of the pad
(Support, and AttachmentSupport its mirror) and a second pad up to another
side face (UpToFace, and UpToShape its mirror). Opened now the map version
differs, so every reference is found again by the geometry of its old
element; and the first pad's faces come out numbered one on, its sketch's
wire starting an edge later than it did.

Each property of such a pair is pasted the other's value as soon as that
one is brought up to date. The second was then brought up to date itself,
its new number read as an old one: the sketch's face went from Face4 to
Face3, which is the same face, and then to Face2, which is not; the sketch
failed with malformed constraints and the recompute stopped there. Seven of
the fork's 27 issue models stopped so.
"""

import os
import unittest

import FreeCAD

FIXTURES = os.path.join(os.path.dirname(__file__), "Fixtures")


class TestTwinReferences(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.openDocument(os.path.join(FIXTURES, "TwinRefs_map-1-15-70200.FCStd"))

    def tearDown(self):
        FreeCAD.closeDocument(self.Doc.Name)

    def face(self, value):
        """The one face a link names, and where it is"""
        if isinstance(value, list):
            value = value[0]
        obj, subs = value
        self.assertEqual(len(subs), 1)
        return subs[0], obj.Shape.getElement(subs[0]).CenterOfMass

    def testReferencesMoveOnce(self):
        doc = self.Doc
        sketch, pad = doc.Sketch001, doc.Pad001
        _name, support = self.face(sketch.AttachmentSupport)
        _name, upTo = self.face(pad.UpToFace)
        volume = pad.Shape.Volume

        doc.recompute()

        for obj in doc.Objects:
            self.assertEqual(obj.State, ["Up-to-date"], obj.Name)
        # the same faces, whatever their numbers are now
        for prop in ("Support", "AttachmentSupport"):
            name, centre = self.face(getattr(sketch, prop))
            self.assertLess(centre.distanceToPoint(support), 1e-7, "%s is %s" % (prop, name))
        for prop in ("UpToFace", "UpToShape"):
            name, centre = self.face(getattr(pad, prop))
            self.assertLess(centre.distanceToPoint(upTo), 1e-7, "%s is %s" % (prop, name))
        self.assertAlmostEqual(pad.Shape.Volume, volume, 6)
