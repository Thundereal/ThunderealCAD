# SPDX-License-Identifier: LGPL-2.1-or-later
"""The 3D model's PMI in a drawing (docs/GDandT.md sec 9, P4): Import Model
Annotations, headless (TechDrawPMI).

The Body of TestModel -- a 40 x 20 x 10 box with a through hole of radius 3
at (20, 10) -- placed off the origin, with a flatness on its top, a position
on its hole and a note, all in planes facing up; datum A on its bottom and B
on its left. A page with the Body seen from the top: what faces the view and
whose feature it shows comes in, linked, its leader on the feature, standing
where the 3D view shows it; again, it follows the model and keeps the user's
moves.
"""

import math
import time
import unittest

import FreeCAD as App

try:
    import TechDraw  # noqa: F401
    import TechDrawPMI
except ImportError:
    TechDrawPMI = None

from PMITests.TestModel import BOTTOM, HOLE, LEFT, TOP, face_at

V = App.Vector
_app = None


def qt_events():
    """A view's projection runs in a thread whose end Qt's event loop
    delivers; FreeCADCmd has no application for one: made before the view is
    computed, for the tests' time only -- left behind, a later test would take
    it for a GUI and draw with fonts it has not got"""
    import sys

    from PySide import QtCore

    global _app
    if QtCore.QCoreApplication.instance() is None:
        _app = QtCore.QCoreApplication(sys.argv[:1])


def qt_events_done():
    global _app
    if _app is not None:
        _app.shutdown()
        _app = None


def wait_for(view, seconds=20.0):
    """The view's projection: done when it names its edges"""
    from PySide import QtCore

    end = time.time() + seconds
    while time.time() < end:
        if view.getEdgeNames():
            return True
        loop = QtCore.QEventLoop()
        QtCore.QTimer.singleShot(50, loop.quit)
        loop.exec_()
    return False


@unittest.skipIf(TechDrawPMI is None, "TechDraw is not built")
class TestImportModelAnnotations(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        qt_events()

    @classmethod
    def tearDownClass(cls):
        qt_events_done()

    def setUp(self):
        qt_events()
        doc = self.doc = App.newDocument("PMIDrawing")
        self.addCleanup(App.closeDocument, doc.Name)
        body = self.body = doc.addObject("PartDesign::Body", "Body")
        box = doc.addObject("PartDesign::AdditiveBox", "Box")
        body.addObject(box)
        box.Length, box.Width, box.Height = 40, 20, 10
        hole = doc.addObject("PartDesign::SubtractiveCylinder", "Hole")
        body.addObject(hole)
        hole.Radius, hole.Height = 3, 10
        hole.Placement = App.Placement(V(20, 10, 0), App.Rotation())
        # Off the origin: the callouts are the Body's, the view the document's
        body.Placement = App.Placement(V(100, 50, 5), App.Rotation())
        pset = doc.addObject("PMI::AnnotationSet", "PMI")
        body.addObject(pset)
        doc.recompute()
        solid = body.SolidBodies[0]

        def add(kind, name, **props):
            o = doc.addObject("PMI::" + kind, name)
            pset.addObject(o)
            for k, v in props.items():
                setattr(o, k, v)
            return o

        def feat(p):
            return add("TolerancedFeature", "Feature",
                       References=[(solid, [face_at(solid.Shape, p)])])

        up = App.Rotation()
        add("Datum", "DatumA", Letter="A", Feature=feat(BOTTOM))
        add("Datum", "DatumB", Letter="B", Feature=feat(LEFT))
        add("GeometricTolerance", "Flatness", Characteristic="Flatness", Value=0.05,
            Feature=feat(TOP), Plane=App.Placement(V(5, 25, 10), up))
        drf = add("DatumReferenceFrame", "DRF", Compartments=["A", "B"])
        add("GeometricTolerance", "Position", Characteristic="Position", Value=0.1,
            ZoneShape="Diameter", Feature=feat(HOLE), DatumSystem=drf,
            Plane=App.Placement(V(28, 14, 10), up))
        add("Note", "Note", Text=["BREAK EDGES"], Feature=feat(V(35, 15, 10)),
            Plane=App.Placement(V(30, -8, 10), up))
        doc.recompute()
        self.page = doc.addObject("TechDraw::DrawPage", "Page")
        template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        # A sheet with a size: without one every view is "larger than page"
        template.Template = App.getResourceDir() + "Mod/TechDraw/Templates/A4_Landscape_blank.svg"
        self.page.Template = template
        view = self.view = doc.addObject("TechDraw::DrawViewPart", "Top")
        self.page.addView(view)
        view.Source = [body]
        view.Direction = V(0, 0, 1)
        view.XDirection = V(1, 0, 0)
        view.Scale = 2.0
        doc.recompute()
        self.assertTrue(wait_for(view), "the view never projected")

    def imported(self):
        return {o.Source.Name: o for o in self.doc.Objects
                if o.isDerivedFrom("TechDraw::DrawPMICallout") and o.Source}

    def test_what_faces_the_view_and_shows(self):
        made, updated, removed = TechDrawPMI.importAnnotations(self.view)
        got = self.imported()
        # A on the bottom is hidden from the top; B on the left does not
        # face the view
        self.assertEqual(sorted(got), ["Flatness", "Note", "Position"])
        self.assertEqual((len(made), updated, removed), (3, [], []))
        for c in got.values():
            self.assertEqual(c.SourceView, self.view)
            self.assertIn(c, self.page.Views)
        # Everything asked for: B too, its feature's top edge seen; A still not
        TechDrawPMI.importAnnotations(self.view, every=True)
        got = self.imported()
        self.assertEqual(sorted(got), ["DatumB", "Flatness", "Note", "Position"])
        self.assertEqual(got["DatumB"].EndType, "Filled Triangle")
        self.assertEqual(got["Flatness"].EndType, "Filled Arrow")

    def test_on_the_feature_where_the_3d_view_shows_it(self):
        TechDrawPMI.importAnnotations(self.view)
        mids = []
        for i, (name, _source, _index) in enumerate(self.view.getEdgeNames()):
            if ";HLR:V" in name:
                e = self.view.getEdgeByIndex(i)
                m = e.valueAt((e.FirstParameter + e.LastParameter) / 2)
                mids.append((m.x, -m.y))
        to_doc = self.body.getGlobalPlacement()
        got = self.imported()
        frames = {}
        for name, c in got.items():
            # The leader ends in the middle of a visible edge of the view
            tip = (c.OriginX.Value, c.OriginY.Value)
            self.assertLess(min(abs(tip[0] - x) + abs(tip[1] - y) for x, y in mids), 1e-6,
                            name)
            self.assertEqual(list(c.ImportOffset),
                             [c.X.Value - c.OriginX.Value, c.Y.Value - c.OriginY.Value])
            src = c.Source
            frames[name] = to_doc.multVec(src.Plane.multVec(src.UprightAt[0]))
        # Seen from the top, x right and y up, in the model's lengths: the
        # frames as far apart on the page as in the model
        a, b = got["Flatness"], got["Note"]
        d = frames["Flatness"] - frames["Note"]
        self.assertAlmostEqual(a.X.Value - b.X.Value, d.x, places=6)
        self.assertAlmostEqual(a.Y.Value - b.Y.Value, d.y, places=6)
        # The leaders' ends as far apart as in the model: the flatness's in
        # the middle of a long top edge (x 20, y 0 or 20), the position's on
        # the hole's rim, 3 from its axis at (20, 10)
        p, f = got["Position"], got["Flatness"]
        dx = p.OriginX.Value - f.OriginX.Value
        dy = p.OriginY.Value - f.OriginY.Value
        self.assertLess(abs(dx), 3.0 + 1e-6)
        self.assertAlmostEqual(math.hypot(dx, abs(dy) - 10.0), 3.0, places=6)

    def test_again_it_follows_the_model_and_keeps_moves(self):
        TechDrawPMI.importAnnotations(self.view)
        got = self.imported()
        moved = got["Flatness"]
        moved.X = moved.X.Value + 7
        offset = (moved.X.Value - moved.OriginX.Value, moved.Y.Value - moved.OriginY.Value)
        before = {n: (c.X.Value, c.Y.Value) for n, c in got.items()}
        made, updated, removed = TechDrawPMI.importAnnotations(self.view)
        self.assertEqual((made, len(updated), removed), ([], 3, []))
        got = self.imported()
        self.assertEqual(sorted(got), ["Flatness", "Note", "Position"])
        # The moved one where the user put it; the others where they were
        self.assertEqual((got["Flatness"].X.Value - got["Flatness"].OriginX.Value,
                          got["Flatness"].Y.Value - got["Flatness"].OriginY.Value), offset)
        for n in ("Note", "Position"):
            self.assertAlmostEqual(got[n].X.Value, before[n][0], places=9)
            self.assertAlmostEqual(got[n].Y.Value, before[n][1], places=9)
        # The note's 3D object gone: its callout goes with it
        self.doc.removeObject("Note")
        self.doc.recompute()
        made, updated, removed = TechDrawPMI.importAnnotations(self.view)
        self.assertEqual(len(removed), 1)
        self.assertEqual(sorted(self.imported()), ["Flatness", "Position"])
        self.assertEqual([o for o in self.doc.Objects
                          if o.isDerivedFrom("TechDraw::DrawPMICallout") and not o.Source], [])

    def test_a_view_not_projected_imports_nothing(self):
        view = self.doc.addObject("TechDraw::DrawViewPart", "Unseen")
        self.page.addView(view)
        view.Source = [self.body]
        self.assertIsNone(TechDrawPMI.importAnnotations(view))
