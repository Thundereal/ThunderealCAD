# SPDX-License-Identifier: LGPL-2.1-or-later
"""A reference into a view follows an element that did not change, when the
view's centre moves.

A view's geometry is centred on the centre of its sources' bounding box, so
anything in the view that changes the box moves every projected element. The
saved geometry a reference is confirmed by (a hatch's and a centre line's
signature, a dimension's saved shape) was compared in the centred frame, so an
unchanged face failed its confirmation when another source changed: the
hatch's face, renumbered, was left behind "by its name alone", in error. Each
saved value now carries the centre it was taken under, and is compared in a
frame the centring does not move (DrawViewPart::geometrySignature(),
DrawViewDimension::SavedGeometryCentres). The user's rule (2026-11-26) stands:
a recompute repoints only what the geometry search confirms; a pure name match
is the user's to take.

Scene: two boxes seen from above, the small one (10 x 10) at the origin, the
big one (20 x 20) at x 40. Faces are numbered by area.
  A: the big box shrunk to 5 x 5 -- the small box's face, unchanged, is
     renumbered and the view's centre moves;
  B: the small box itself grown to 30 x 30 -- its face changed: found by its
     name alone, left for the user, in error;
  C: as A with a third box holding the view's extents: the centre does not
     move (as before the change).
A value written before the change (untagged) is compared as it always was,
and is given its centre once it matches.
"""

import os
import tempfile
import unittest

import FreeCAD


class ReferenceCentreTest(unittest.TestCase):
    def setUp(self):
        import TechDraw  # noqa: F401

        self.doc = FreeCAD.newDocument("ReferenceCentre")
        self.small = self.box("Small", 10, FreeCAD.Vector(0, 0, 0))
        self.big = self.box("Big", 20, FreeCAD.Vector(40, 0, 0))
        page = self.doc.addObject("TechDraw::DrawPage", "Page")
        template = self.doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        template.Template = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "TestTemplate.svg")
        page.Template = template
        self.page = page
        self.view = self.doc.addObject("TechDraw::DrawViewPart", "View")
        page.addView(self.view)
        self.view.Source = [self.small, self.big]
        self.view.Direction = FreeCAD.Vector(0, 0, 1)
        self.doc.recompute()

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def box(self, name, side, at):
        b = self.doc.addObject("Part::Box", name)
        b.Length = b.Width = side
        b.Height = 5
        b.Placement.Base = at
        return b

    def faceOf(self, view, name):
        """The number the face carrying `name` has now"""
        for i, (n, _) in enumerate(view.getFaceNames()):
            if n == name:
                return "Face%d" % i
        return None

    def hatch(self, kind, face):
        h = self.doc.addObject(kind, kind.split("::")[1])
        h.Source = (self.view, [face])
        self.doc.recompute()
        return h

    def smallFace(self):
        """The small box's face: the smaller of the two"""
        return "Face1"

    # -- hatches

    def checkHatchFollowsUnchangedFace(self, kind):
        h = self.hatch(kind, self.smallFace())
        name = h.SavedNames[0]
        self.assertIn(" @ ", h.SavedGeometry[0], "a signature carries its centre")
        self.big.Length = self.big.Width = 5  # case A
        self.doc.recompute()
        self.assertEqual(self.faceOf(self.view, name), "Face0", "the face was renumbered")
        self.assertEqual(list(h.Source[1]), ["Face0"], "followed, by its geometry")
        self.assertEqual(h.findByName(), [], "nothing left for the user")
        self.assertNotIn("Touched", h.State)

    def testAHatchFollowsAnUnchangedFace(self):
        self.checkHatchFollowsUnchangedFace("TechDraw::DrawHatch")

    def testAGeomHatchFollowsAnUnchangedFace(self):
        self.checkHatchFollowsUnchangedFace("TechDraw::DrawGeomHatch")

    def testAHatchOnAChangedFaceIsLeftForTheUser(self):
        h = self.hatch("TechDraw::DrawHatch", self.smallFace())
        self.small.Length = self.small.Width = 30  # case B
        self.doc.recompute()
        self.assertEqual(list(h.Source[1]), ["Face1"], "not moved")
        left = h.findByName()
        self.assertEqual(len(left), 1, left)

    def testAHatchFollowsWhenTheCentreHolds(self):
        pin = self.box("Pin", 1, FreeCAD.Vector(70, 0, 0))
        pin.Width = 20
        self.view.Source = [self.small, self.big, pin]
        self.doc.recompute()
        # by area: Big Face0, Small Face1, Pin Face2
        h = self.hatch("TechDraw::DrawHatch", "Face1")
        self.big.Length = self.big.Width = 5  # case C
        self.doc.recompute()
        self.assertEqual(list(h.Source[1]), ["Face0"])
        self.assertEqual(h.findByName(), [])

    # -- a dimension and a centre line: the sources' order swapped as well as
    # the big box shrunk, so that the edges and faces are renumbered while the
    # centre moves

    def renumberAndMoveCentre(self):
        self.big.Length = self.big.Width = 5
        self.view.Source = [self.big, self.small]
        self.doc.recompute()

    def testADimensionFollowsAnUnchangedEdge(self):
        names = self.view.getEdgeNames()
        # the small box's left edge: from (0, 0) to (0, 10), the first edge
        edge = "Edge0"
        name = names[0][0]
        dim = self.doc.addObject("TechDraw::DrawViewDimension", "Dimension")
        dim.Type = "Distance"
        dim.References2D = [(self.view, edge)]
        self.page.addView(dim)
        self.doc.recompute()
        self.assertEqual(len(dim.SavedGeometryCentres), 1, "a saved shape carries its centre")
        self.assertAlmostEqual(dim.getRawValue(), 10.0, 6)
        self.renumberAndMoveCentre()
        now = self.view.getGeometryReference(name)
        self.assertNotEqual(now, edge, "the edge was renumbered")
        self.assertEqual(dim.References2D[0][1][0], now, "followed")
        self.assertAlmostEqual(dim.getRawValue(), 10.0, 6)
        self.assertEqual(dim.findByName(), [], "nothing left for the user")

    def testACentreLineFollowsAnUnchangedFace(self):
        tag = self.view.makeCenterLine([self.smallFace()], 0)
        self.doc.recompute()
        cl = self.view.getCenterLine(tag)
        name = cl.FaceNames[0]
        self.assertIn(" @ ", cl.FaceGeometry[0])
        self.renumberAndMoveCentre()
        cl = self.view.getCenterLine(tag)
        self.assertEqual(list(cl.Faces), [self.faceOf(self.view, name)], "followed")

    # -- values written before the change

    def untag(self, h, dim=None):
        h.SavedGeometry = [g.split(" @ ")[0] for g in h.SavedGeometry]
        if dim is not None:
            dim.SavedGeometryCentres = []

    def testAnUntaggedValueIsComparedAsBefore(self):
        h = self.hatch("TechDraw::DrawHatch", self.smallFace())
        self.untag(h)
        self.big.Length = self.big.Width = 5  # case A, the value untagged
        self.doc.recompute()
        self.assertEqual(list(h.Source[1]), ["Face1"], "not moved, as before")
        self.assertEqual(len(h.findByName()), 1)
        self.assertNotIn(" @ ", h.SavedGeometry[0], "not tagged: it did not match")

    def testAnUntaggedValueIsTaggedOnceItMatchesAndSaved(self):
        h = self.hatch("TechDraw::DrawHatch", self.smallFace())
        dim = self.doc.addObject("TechDraw::DrawViewDimension", "Dimension")
        dim.Type = "Distance"
        dim.References2D = [(self.view, "Edge0")]
        self.page.addView(dim)
        self.doc.recompute()
        self.untag(h, dim)
        self.doc.recompute()
        self.assertNotIn(" @ ", h.SavedGeometry[0])
        # a recompute of the view, nothing changed: what is saved matches
        self.view.touch()
        self.doc.recompute()
        self.assertIn(" @ ", h.SavedGeometry[0], "tagged once it matched")
        self.assertEqual(len(dim.SavedGeometryCentres), 1)
        self.assertNotIn("Touched", h.State, "the tag touches nothing")
        self.assertNotIn("Touched", dim.State)
        self.assertEqual(self.doc.recompute(), 0, "nothing more to recompute")
        path = os.path.join(tempfile.mkdtemp(), "ReferenceCentre.FCStd")
        self.doc.saveAs(path)
        FreeCAD.closeDocument(self.doc.Name)
        self.doc = FreeCAD.openDocument(path)
        h = self.doc.getObject("DrawHatch")
        dim = self.doc.getObject("Dimension")
        self.assertIn(" @ ", h.SavedGeometry[0], "saved with its tag")
        self.assertEqual(len(dim.SavedGeometryCentres), 1)


if __name__ == "__main__":
    unittest.main()
