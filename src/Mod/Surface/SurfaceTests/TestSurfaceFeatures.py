# SPDX-License-Identifier: LGPL-2.1-or-later

"""Each Surface feature builds on OCCT 8.0.1, names its output, and keeps a
downstream reference on the same piece of geometry across an input change and
a save and reopen. docs/Surface.md sec 9, U0."""

import os
import tempfile
import unittest

import FreeCAD as App
import Part
from FreeCAD import Vector as V


def spline(doc, name, *pts):
    c = Part.BSplineCurve()
    c.interpolate([V(*p) for p in pts])
    o = doc.addObject("Part::Feature", name)
    o.Shape = c.toShape()
    return o


def saddleLoop(doc):
    """Four degree-2 curves meeting at the corners of a 10 x 10 saddle"""
    return [
        spline(doc, "c1", (0, 0, 0), (5, 0, 2), (10, 0, 0)),
        spline(doc, "c2", (10, 0, 0), (10, 5, -2), (10, 10, 0)),
        spline(doc, "c3", (10, 10, 0), (5, 10, 2), (0, 10, 0)),
        spline(doc, "c4", (0, 10, 0), (0, 5, -2), (0, 0, 0)),
    ]


def holder(doc, target, sub):
    """A downstream object holding one element of `target` by name"""
    h = doc.addObject("App::FeaturePython", "Ref")
    h.addProperty("App::PropertyLinkSub", "Ref")
    h.Ref = (target, [sub])
    return h


def held(h):
    return h.Ref[0].Shape.getElement(h.Ref[1][0])


def edgeOn(shape, curveObj):
    """The name of the edge of `shape` that lies on `curveObj`'s edge"""
    def mid(e):
        return Part.Vertex(e.valueAt((e.FirstParameter + e.LastParameter) / 2))

    # midpoints both ways: the whole curve, not one shared end
    c = curveObj.Shape.Edges[0]
    for i, e in enumerate(shape.Edges):
        if c.distToShape(mid(e))[0] < 1e-6 and e.distToShape(mid(c))[0] < 1e-6:
            return "Edge%d" % (i + 1)
    return None


def faceNamed(test, obj):
    shape = obj.Shape
    test.assertGreater(shape.ElementMapSize, 0, "%s has no element map" % obj.Name)
    for i in range(len(shape.Faces)):
        test.assertTrue(shape.getElementMappedName("Face%d" % (i + 1)), obj.Name)
    for i in range(len(shape.Edges)):
        test.assertTrue(shape.getElementMappedName("Edge%d" % (i + 1)), obj.Name)


class TestSurfaceFeatures(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SurfaceFeatures")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def recompute(self, *objs):
        self.doc.recompute()
        for o in objs:
            self.assertTrue(o.isValid(), "%s: %s" % (o.Name, o.getStatusString()))
            self.assertFalse(o.Shape.isNull(), o.Name)
            self.assertTrue(o.Shape.isValid(), o.Name)

    # -- GeomFillSurface ------------------------------------------------------

    def testGeomFillSurfaceStyles(self):
        # Coons asks OCCT for 4 poles a side; the feature raises the curves itself
        loop = saddleLoop(self.doc)
        g = self.doc.addObject("Surface::GeomFillSurface", "GFS")
        g.BoundaryList = [(c, "Edge1") for c in loop]
        for style in ("Stretched", "Coons", "Curved"):
            g.FillType = style
            g.touch()
            self.recompute(g)
            self.assertEqual(len(g.Shape.Faces), 1, style)
            for c in loop:
                self.assertIsNotNone(edgeOn(g.Shape, c), "%s: %s" % (style, c.Name))

    def testGeomFillSurfaceCoonsOnLines(self):
        pts = [V(0, 0, 0), V(10, 0, 1), V(10, 10, 0), V(0, 10, 1)]
        objs = []
        for i in range(4):
            o = self.doc.addObject("Part::Feature", "l%d" % i)
            o.Shape = Part.LineSegment(pts[i], pts[(i + 1) % 4]).toShape()
            objs.append(o)
        g = self.doc.addObject("Surface::GeomFillSurface", "GFS")
        g.BoundaryList = [(o, "Edge1") for o in objs]
        g.FillType = "Coons"
        self.recompute(g)
        self.assertEqual(len(g.Shape.Faces), 1)

    def testGeomFillSurfaceNames(self):
        loop = saddleLoop(self.doc)
        g = self.doc.addObject("Surface::GeomFillSurface", "GFS")
        g.BoundaryList = [(c, "Edge1") for c in loop]
        self.recompute(g)
        faceNamed(self, g)
        h = holder(self.doc, g, edgeOn(g.Shape, loop[0]))
        # the same curves in another order
        g.BoundaryList = [(c, "Edge1") for c in loop[2:] + loop[:2]]
        self.recompute(g)
        self.assertEqual(edgeOn(g.Shape, loop[0]), h.Ref[1][0])

    # -- Filling --------------------------------------------------------------

    def testFillingNames(self):
        loop = saddleLoop(self.doc)
        f = self.doc.addObject("Surface::Filling", "Fill")
        f.BoundaryEdges = [(c, "Edge1") for c in loop]
        self.recompute(f)
        faceNamed(self, f)
        h = holder(self.doc, f, edgeOn(f.Shape, loop[1]))
        f.BoundaryEdges = [(c, "Edge1") for c in reversed(loop)]
        self.recompute(f)
        self.assertEqual(edgeOn(f.Shape, loop[1]), h.Ref[1][0])

    # -- Sections -------------------------------------------------------------

    def sections(self):
        s1 = spline(self.doc, "s1", (0, 0, 0), (0, 5, 1), (0, 10, 0))
        s2 = spline(self.doc, "s2", (5, 0, 2), (5, 5, 3), (5, 10, 2))
        s3 = spline(self.doc, "s3", (10, 0, 0), (10, 5, 1), (10, 10, 0))
        s = self.doc.addObject("Surface::Sections", "Sec")
        s.NSections = [(o, "Edge1") for o in (s1, s2, s3)]
        return s, (s1, s2, s3)

    def testSectionsBuild(self):
        s, (s1, s2, s3) = self.sections()
        self.recompute(s)
        face = s.Shape.Faces[0]
        for c in (s1, s2, s3):
            self.assertLess(face.distToShape(c.Shape)[0], 1e-6, c.Name)
        self.assertIsNotNone(edgeOn(s.Shape, s1))
        self.assertIsNotNone(edgeOn(s.Shape, s3))

    def testSectionsNames(self):
        s, (s1, s2, s3) = self.sections()
        self.recompute(s)
        faceNamed(self, s)
        h = holder(self.doc, s, edgeOn(s.Shape, s3))
        # the last section moves; the reference follows it
        c = Part.BSplineCurve()
        c.interpolate([V(12, 0, 0), V(12, 5, 2), V(12, 10, 0)])
        s3.Shape = c.toShape()
        self.recompute(s)
        self.assertEqual(edgeOn(s.Shape, s3), h.Ref[1][0])

    # -- Extend ---------------------------------------------------------------

    def testExtendNames(self):
        loop = saddleLoop(self.doc)
        g = self.doc.addObject("Surface::GeomFillSurface", "GFS")
        g.BoundaryList = [(c, "Edge1") for c in loop]
        e = self.doc.addObject("Surface::Extend", "Ext")
        e.Face = (g, "Face1")
        self.recompute(g, e)
        self.assertGreater(e.Shape.Area, g.Shape.Area)
        for v in g.Shape.Vertexes:
            self.assertLess(e.Shape.distToShape(v)[0], e.Tolerance, v.Point)
        faceNamed(self, e)
        # the face traces back to the face it extends
        hist = e.getElementHistory(e.Shape.getElementMappedName("Face1"), recursive=False)
        self.assertEqual(hist[1][0].Name, g.Name)
        self.assertEqual(hist[1][1], g.Shape.getElementMappedName("Face1"))

    # -- Sewing ---------------------------------------------------------------

    def testSewingNames(self):
        a = self.doc.addObject("Part::Plane", "A")
        b = self.doc.addObject("Part::Plane", "B")
        b.Placement.Base = V(10, 0, 0)
        a.Length = b.Length = a.Width = b.Width = 10
        sew = self.doc.addObject("Surface::Sewing", "Sew")
        sew.ShapeList = [(a, "Face1"), (b, "Face1")]
        self.recompute(sew)
        self.assertEqual(len(sew.Shape.Shells), 1)
        self.assertEqual(len(sew.Shape.Faces), 2)
        self.assertEqual(len(sew.Shape.Edges), 7)
        faceNamed(self, sew)
        h = holder(self.doc, sew, "Face1")
        centre = held(h).CenterOfMass
        sew.ShapeList = [(b, "Face1"), (a, "Face1")]
        self.recompute(sew)
        self.assertLess(held(h).CenterOfMass.distanceToPoint(centre), 1e-9)

    # -- Cut ------------------------------------------------------------------

    def testCutNames(self):
        # no command (docs/Surface.md sec 12 h), but old files keep loading
        loop = saddleLoop(self.doc)
        g = self.doc.addObject("Surface::GeomFillSurface", "GFS")
        g.BoundaryList = [(c, "Edge1") for c in loop]
        box = self.doc.addObject("Part::Box", "Box")
        box.Placement.Base = V(3, 3, -2)
        cut = self.doc.addObject("Surface::Cut", "Cut")
        cut.ShapeList = [(g, "Face1"), (box, "")]
        self.recompute(g, cut)
        self.assertLess(cut.Shape.Area, g.Shape.Area)
        faceNamed(self, cut)

    # -- BlendCurve -----------------------------------------------------------

    def testBlendCurveNames(self):
        loop = saddleLoop(self.doc)
        bc = self.doc.addObject("Surface::FeatureBlendCurve", "BC")
        bc.StartEdge = (loop[0], "Edge1")
        bc.EndEdge = (loop[2], "Edge1")
        self.recompute(bc)
        self.assertGreater(bc.Shape.ElementMapSize, 0)
        self.assertTrue(bc.Shape.getElementMappedName("Edge1"))

    # -- all of them, saved and reopened --------------------------------------

    def testSaveReopen(self):
        loop = saddleLoop(self.doc)
        g = self.doc.addObject("Surface::GeomFillSurface", "GFS")
        g.BoundaryList = [(c, "Edge1") for c in loop]
        f = self.doc.addObject("Surface::Filling", "Fill")
        f.BoundaryEdges = [(c, "Edge1") for c in loop]
        s, _ = self.sections()
        e = self.doc.addObject("Surface::Extend", "Ext")
        e.Face = (g, "Face1")
        feats = (g, f, s, e)
        self.recompute(*feats)
        before = {o.Name: (o.Shape.Area, o.Shape.ElementMapSize) for o in feats}
        path = os.path.join(tempfile.mkdtemp(), "surface.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        for name, (area, size) in before.items():
            o = self.doc.getObject(name)
            self.assertAlmostEqual(o.Shape.Area, area, 6, name)
            self.assertEqual(o.Shape.ElementMapSize, size, name)
        for o in self.doc.Objects:
            o.touch()
        self.recompute(*[self.doc.getObject(n) for n in before])
