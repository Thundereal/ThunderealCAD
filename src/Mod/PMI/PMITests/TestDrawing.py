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
import Part

try:
    import TechDraw  # noqa: F401
    import TechDrawPMI
except ImportError:
    TechDrawPMI = None

from PMITests.TestModel import BOTTOM, HOLE, LEFT, RIGHT, TOP, face_at, needs_part_design

V = App.Vector


def wait_for(view, seconds=20.0):
    """The view's projection: done when it names its edges -- at once headless,
    where it runs in place; in the GUI once its thread's end is delivered"""
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
@needs_part_design
class TestImportModelAnnotations(unittest.TestCase):
    def setUp(self):
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
        add("Dimension", "HoleSize", Type="Diameter", Features=[feat(HOLE)], Upper=0.1,
            Lower=0.0, DecimalPlaces=1, Plane=App.Placement(V(26, 4, 10), up))
        sides = [face_at(solid.Shape, LEFT), face_at(solid.Shape, RIGHT)]
        add("Dimension", "Width", Type="Width", Upper=0.2, Lower=-0.2,
            Features=[add("TolerancedFeature", "Feature", References=[(solid, sides)])],
            Plane=App.Placement(V(20, 24, 10), up))
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
        view.ScaleType = "Custom"
        view.Scale = 2.0
        doc.recompute()
        self.assertTrue(wait_for(view), "the view never projected")

    def imported(self):
        return {o.Source.Name: o for o in self.doc.Objects
                if o.isDerivedFrom("TechDraw::DrawPMICallout") and o.Source}

    def dimensions(self):
        return {o.Source.Name: o for o in self.doc.Objects
                if o.isDerivedFrom("TechDraw::DrawViewDimension") and o.Source}

    def test_what_faces_the_view_and_shows(self):
        made, updated, removed = TechDrawPMI.importAnnotations(self.view)
        got = self.imported()
        # A on the bottom is hidden from the top; B on the left does not
        # face the view
        self.assertEqual(sorted(got), ["Flatness", "Note", "Position"])
        self.assertEqual((len(made), updated, removed), (5, [], []))
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
        edges = [self.view.getEdgeByIndex(i)
                 for i, (name, _source, _index) in enumerate(self.view.getEdgeNames())
                 if ";HLR:V" in name]
        self.assertTrue(edges, "no visible edge named as the view names them")
        got = self.imported()
        for name, c in got.items():
            # The leader ends on a visible edge of the view: in its middle; on
            # a circle, pointed at its centre from the frame's side
            tip = V(c.OriginX.Value, c.OriginY.Value, 0)
            e = min(edges, key=lambda e: e.distToShape(Part.Vertex(tip))[0])
            self.assertLess(e.distToShape(Part.Vertex(tip))[0], 1e-6, name)
            if isinstance(e.Curve, Part.Circle):
                out = tip - e.Curve.Center
                frame = V(c.X.Value, c.Y.Value, 0) - e.Curve.Center
                self.assertLess(out.cross(frame).Length, 1e-6 * frame.Length, name)
                self.assertGreater(out.dot(frame), 0, name)
            else:
                m = e.valueAt((e.FirstParameter + e.LastParameter) / 2)
                self.assertLess((m - tip).Length, 1e-6, name)
            self.assertEqual(list(c.ImportOffset),
                             [c.X.Value - c.OriginX.Value, c.Y.Value - c.OriginY.Value])
        # Seen from the top, x right and y up, in the model's lengths: the
        # flatness, clear of all on paper, stands against its leader's end as
        # its 3D frame against the middle of the long top edge it ends on, at
        # (20, 0) or (20, 20) in the Body
        f = got["Flatness"]
        at = f.Source.Plane.multVec(TechDrawPMI._centre3d(f.Source))
        stands = (f.X.Value - f.OriginX.Value, f.Y.Value - f.OriginY.Value)
        self.assertTrue(any(abs(stands[0] - (at.x - 20)) < 1e-6
                            and abs(stands[1] - (at.y - y)) < 1e-6 for y in (0, 20)),
                        (stands, at))
        # The leaders' ends as far apart as in the model: the flatness's in
        # the middle of a long top edge (x 20, y 0 or 20), the position's on
        # the hole's rim, 3 from its axis at (20, 10)
        p, f = got["Position"], got["Flatness"]
        dx = p.OriginX.Value - f.OriginX.Value
        dy = p.OriginY.Value - f.OriginY.Value
        self.assertLess(abs(dx), 3.0 + 1e-6)
        self.assertAlmostEqual(math.hypot(dx, abs(dy) - 10.0), 3.0, places=6)

    def test_leaders_and_dimensions_clear(self):
        """No leader crosses a line of the view -- a hole's from the near side,
        pointed at its centre; each from the middle of its frame's side nearer
        its end, as TechDraw draws it -- nor a frame; no dimension's lines, from what
        it measures out to its text, cross a frame"""
        TechDrawPMI.importAnnotations(self.view)
        scale = self.view.getScale()
        lines = TechDrawPMI._lines(self.view, self.view.getEdgeNames(), scale)
        got = self.imported()
        boxes = {n: TechDrawPMI._box(c, scale) for n, c in got.items()}
        for name, c in got.items():
            # From the middle of the frame's side nearer its end
            b = boxes[name]
            tip = (c.OriginX.Value * scale, c.OriginY.Value * scale)
            seg = TechDrawPMI._leader(((b[0] + b[2]) / 2, (b[1] + b[3]) / 2),
                                      (b[2] - b[0], b[3] - b[1]), tip)
            self.assertIsNotNone(seg, name)
            self.assertFalse([s for s in lines if TechDrawPMI._crosses(*seg, *s)], name)
            self.assertFalse([o for o, ob in boxes.items()
                              if o != name and TechDrawPMI._clip(ob, *seg)], name)
        dims = [d for d in self.doc.Objects
                if d.isDerivedFrom("TechDraw::DrawViewDimension") and d.Source]
        self.assertEqual(sorted(d.Source.Name for d in dims), ["HoleSize", "Width"])
        for d in dims:
            size = TechDrawPMI._text_size(d.Source, d.Type)
            segs = TechDrawPMI._dimension_lines(self.view, list(d.References2D[0][1]), d.Type,
                                                (d.X.Value, d.Y.Value), size, scale)
            self.assertTrue(segs, d.Source.Name)
            self.assertFalse([o for o, b in boxes.items()
                              if any(TechDrawPMI._clip(b, p, q) for p, q in segs)],
                             d.Source.Name)

    def test_clear_of_each_other_and_of_the_lines(self):
        """On paper the frames keep the 3D view's places where those are clear;
        where one would stand on another or on the view's lines it goes to the
        nearest place that is not"""
        import PMI

        TechDrawPMI.importAnnotations(self.view)
        scale = self.view.getScale()
        height = TechDrawPMI._height()
        lines = []
        for i, (name, _source, _index) in enumerate(self.view.getEdgeNames()):
            if ";HLR:V" in name:
                pts = [(p.x * scale, -p.y * scale)
                       for p in self.view.getEdgeByIndex(i).discretize(Deflection=0.05)]
                lines += list(zip(pts, pts[1:]))
        self.assertTrue(lines, "no visible edge named as the view names them")
        boxes = {}
        for name, c in self.imported().items():
            x0, y0, x1, y1 = PMI.extent(c.Source, height)
            x, y = c.X.Value * scale, c.Y.Value * scale
            boxes[name] = (x - (x1 - x0) / 2, y - (y1 - y0) / 2,
                           x + (x1 - x0) / 2, y + (y1 - y0) / 2)
        self.assertEqual(len(boxes), 3)
        names = sorted(boxes)
        for i, a in enumerate(names):
            for b in names[i + 1:]:
                self.assertFalse(TechDrawPMI._overlap(boxes[a], boxes[b]), (a, b))
            for seg in lines:
                self.assertFalse(TechDrawPMI._clip(boxes[a], *seg), (a, seg))

    def test_again_it_follows_the_model_and_keeps_moves(self):
        TechDrawPMI.importAnnotations(self.view)
        got = self.imported()
        moved = got["Flatness"]
        moved.X = moved.X.Value + 7
        offset = (moved.X.Value - moved.OriginX.Value, moved.Y.Value - moved.OriginY.Value)
        before = {n: (c.X.Value, c.Y.Value) for n, c in got.items()}
        made, updated, removed = TechDrawPMI.importAnnotations(self.view)
        self.assertEqual((made, len(updated), removed), ([], 5, []))
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

    def test_dimensions_linked_one_value(self):
        """The hole's diameter seen end-on and the width between the left and
        right faces seen edge-on come in as TechDraw dimensions on those
        edges, measuring them, their tolerance the 3D one's either way"""
        TechDrawPMI.importAnnotations(self.view)
        got = self.dimensions()
        self.assertEqual(sorted(got), ["HoleSize", "Width"])
        hole, width = got["HoleSize"], got["Width"]
        self.assertEqual(hole.Type, "Diameter")
        self.assertEqual(width.Type, "DistanceX")
        self.assertEqual((len(TechDrawPMI._subs(hole)), len(TechDrawPMI._subs(width))), (1, 2))
        self.doc.recompute()
        self.assertAlmostEqual(hole.getRawValue(), 6.0, places=6)
        self.assertAlmostEqual(width.getRawValue(), 40.0, places=6)
        # Its tolerance and decimals the 3D one's
        self.assertAlmostEqual(hole.OverTolerance.Value, 0.1, places=9)
        self.assertAlmostEqual(hole.UnderTolerance.Value, 0.0, places=9)
        self.assertFalse(hole.EqualTolerance)
        self.assertRegex(hole.FormatSpec, r"%\.1[a-zA-Z]")
        # +0.1 under the diameter's value, not a second diameter sign
        self.assertTrue(hole.FormatSpecOverTolerance.startswith("%+.1"),
                        hole.FormatSpecOverTolerance)
        self.assertTrue(width.EqualTolerance)
        self.assertAlmostEqual(width.OverTolerance.Value, 0.2, places=9)
        # Changed in 3D, the drawing follows
        src = hole.Source
        src.Upper = 0.05
        self.doc.recompute()
        self.assertAlmostEqual(hole.OverTolerance.Value, 0.05, places=9)
        src.Fit = "H7"
        self.doc.recompute()
        self.assertTrue(hole.FormatSpec.endswith(" H7"), hole.FormatSpec)
        self.assertEqual(hole.OverTolerance.Value, 0.0)
        # Edited in the drawing, the 3D one changes: one value; a tolerance
        # typed in replaces the fit
        hole.OverTolerance = 0.08
        self.assertAlmostEqual(src.Upper.Value, 0.08, places=9)
        self.assertEqual(src.Fit, "")
        hole.TheoreticalExact = True
        self.assertTrue(src.Basic)
        self.doc.recompute()
        self.assertEqual(hole.OverTolerance.Value, 0.0)
        src.Basic = False
        self.doc.recompute()
        self.assertFalse(hole.TheoreticalExact)
        self.assertAlmostEqual(hole.OverTolerance.Value, 0.08, places=9)
        # Again: none made twice, the user's place kept
        width.X = width.X.Value + 5
        x = width.X.Value
        made, updated, removed = TechDrawPMI.importAnnotations(self.view)
        self.assertEqual(sorted(self.dimensions()), ["HoleSize", "Width"])
        self.assertEqual(made, [])
        self.assertEqual(self.dimensions()["Width"].X.Value, x)
        # Its 3D one gone: it stays, a plain dimension
        self.doc.removeObject("Width")
        self.doc.recompute()
        TechDrawPMI.importAnnotations(self.view)
        self.assertEqual(sorted(self.dimensions()), ["HoleSize"])
        self.assertEqual(len([o for o in self.doc.Objects
                              if o.isDerivedFrom("TechDraw::DrawViewDimension")]), 2)

    def test_a_page_from_pmi_views(self):
        """Two PMI views, the top and the front, each showing its own: a page
        with a part view for each, looking as its camera does, named after
        it, at a standard scale on the sheet, with what it shows imported --
        whatever its plane -- and nothing it does not"""
        pset = self.doc.getObject("PMI")

        def pmi_view(name, standard):
            v = self.doc.addObject("PMI::View", name)
            pset.addObject(v)
            v.Standard = standard
            return v

        top, front = pmi_view("TopView", "Top"), pmi_view("FrontView", "Front")
        for name in ("Flatness", "Position", "HoleSize"):
            self.doc.getObject(name).Views = [top]
        # The datum on the bottom, seen edge-on from the front
        self.doc.getObject("DatumA").Views = [front]
        self.doc.recompute()
        page, views = TechDrawPMI.pageFromViews([top, front])
        self.assertIsNot(page, self.page)
        self.assertEqual([v.Label for v in views], ["TopView view", "FrontView view"])
        self.assertEqual([v.Caption for v in views], ["TopView", "FrontView"])
        self.assertEqual([v.PMIView for v in views], [top, front])
        dt, df = views
        self.assertTrue(dt.Direction.isEqual(V(0, 0, 1), 1e-9), dt.Direction)
        self.assertTrue(df.Direction.isEqual(V(0, -1, 0), 1e-9), df.Direction)
        self.assertTrue(df.XDirection.isEqual(V(1, 0, 0), 1e-9), df.XDirection)
        # One standard scale, the views apart and on the sheet
        scale = page.Scale
        self.assertTrue(any(abs(scale - k * 10.0**e) < 1e-12
                            for e in range(-4, 4) for k in (1, 2, 5)), scale)
        self.assertEqual({v.getScale() for v in views}, {scale})
        width, height = page.Template.Width.Value, page.Template.Height.Value
        boxes = []
        for v in views:
            w, h = 40 * scale, 20 * scale  # the box from either side, or less
            box = (v.X.Value - w / 2, v.Y.Value - h / 2, v.X.Value + w / 2, v.Y.Value + h / 2)
            self.assertTrue(0 <= box[0] and box[2] <= width and 0 <= box[1]
                            and box[3] <= height, box)
            boxes.append(box)
        self.assertFalse(TechDrawPMI._overlap(boxes[0], boxes[1]), (boxes, scale, width, height))
        # Each with what it shows, the datum though its plane is not the view's
        on = {v.Name: sorted(c.Source.Name for c in self.doc.Objects
                             if c.isDerivedFrom("TechDraw::DrawPMICallout")
                             and c.SourceView == v)
              for v in views}
        self.assertEqual(on, {dt.Name: ["Flatness", "Position"], df.Name: ["DatumA"]})
        # The datum on the bottom, its leader to the bottom line: y up below
        # the middle (the top view, the part symmetric in it, cannot tell)
        datum = [c for c in self.doc.Objects if c.isDerivedFrom("TechDraw::DrawPMICallout")
                 and c.SourceView == df][0]
        self.assertAlmostEqual(datum.OriginY.Value, -5.0, places=6)
        # Square to it, its triangle flat on the line: the leader straight
        # down from the middle of the frame's side nearer it
        b = TechDrawPMI._box(datum, scale)
        tip = (datum.OriginX.Value * scale, datum.OriginY.Value * scale)
        seg = TechDrawPMI._leader(((b[0] + b[2]) / 2, (b[1] + b[3]) / 2),
                                  (b[2] - b[0], b[3] - b[1]), tip)
        self.assertIsNotNone(seg)
        self.assertAlmostEqual(seg[0][0], seg[1][0], places=6)
        # Moved, it stays, and its leader square to the line from there
        datum.X = datum.X.Value + 3.0
        self.doc.recompute()
        TechDrawPMI.importAnnotations(df)
        b = TechDrawPMI._box(datum, scale)
        tip = (datum.OriginX.Value * scale, datum.OriginY.Value * scale)
        seg = TechDrawPMI._leader(((b[0] + b[2]) / 2, (b[1] + b[3]) / 2),
                                  (b[2] - b[0], b[3] - b[1]), tip)
        self.assertAlmostEqual(seg[0][0], seg[1][0], places=6)
        at = (datum.X.Value, datum.OriginX.Value)
        TechDrawPMI.importAnnotations(df)
        self.assertEqual((datum.X.Value, datum.OriginX.Value), at)
        # Every frame on the sheet, clear of the title block and of the
        # other view
        for v in views:
            bounds, others = TechDrawPMI._sheet(v)
            for c in self.doc.Objects:
                if not (c.isDerivedFrom("TechDraw::DrawPMICallout") and c.SourceView == v):
                    continue
                b = TechDrawPMI._box(c, scale)
                self.assertTrue(bounds[0] <= b[0] and b[2] <= bounds[2] and bounds[1] <= b[1]
                                and b[3] <= bounds[3], (c.Label, b, bounds))
                self.assertEqual([o for o in others if TechDrawPMI._overlap(b, o)], [],
                                 c.Label)
        dims = [d for d in self.doc.Objects if d.isDerivedFrom("TechDraw::DrawViewDimension")]
        self.assertEqual([(d.Source.Name, d.References2D[0][0]) for d in dims],
                         [("HoleSize", dt)])
        # Again: the same, nothing more -- the note is not the view's
        made, _updated, _removed = TechDrawPMI.importAnnotations(dt)
        self.assertEqual(made, [])

    def test_a_clipped_view_is_a_section(self):
        """A PMI view from the top clipped at half height: a section view cut
        there, the top taken away, on a base view from the front seeing the
        cut edge-on; on it what the view shows of the hole, not the flatness
        of the top it no longer has"""
        pset = self.doc.getObject("PMI")
        cut = self.doc.addObject("PMI::View", "CutView")
        pset.addObject(cut)
        cut.Standard = "Top"
        cut.Clip = True
        cut.ClipPlane = App.Placement(V(20, 10, 5), App.Rotation())  # the set's frame
        for name in ("Flatness", "Position", "HoleSize"):
            self.doc.getObject(name).Views = [cut]
        self.doc.recompute()
        page, views = TechDrawPMI.pageFromViews([cut])
        self.assertEqual(len(views), 1)
        sec = views[0]
        self.assertTrue(sec.isDerivedFrom("TechDraw::DrawViewSection"), sec.TypeId)
        base = sec.BaseView
        self.assertEqual(base.Label, "CutView base")
        self.assertTrue(base.Direction.isEqual(V(0, -1, 0), 1e-9), base.Direction)
        self.assertTrue(sec.SectionNormal.isEqual(V(0, 0, 1), 1e-9), sec.SectionNormal)
        # The Body is at z 5: the cut at 10, the part below it kept
        self.assertAlmostEqual(sec.SectionOrigin.z, 10.0, places=9)
        box = sec.getCutPieces().BoundBox
        self.assertAlmostEqual(box.ZMin, 5.0, places=6)
        self.assertAlmostEqual(box.ZMax, 10.0, places=6)
        self.assertEqual(sorted(o.Label for o in page.Views
                                if o.isDerivedFrom("TechDraw::DrawViewPart")),
                         ["CutView base", "CutView view"])
        on = sorted(c.Source.Name for c in self.doc.Objects
                    if c.isDerivedFrom("TechDraw::DrawPMICallout") and c.SourceView == sec)
        self.assertEqual(on, ["Position"])
        dims = [d for d in self.doc.Objects if d.isDerivedFrom("TechDraw::DrawViewDimension")]
        self.assertEqual([(d.Source.Name, d.References2D[0][0]) for d in dims],
                         [("HoleSize", sec)])
        self.doc.recompute()
        self.assertAlmostEqual(dims[0].getRawValue(), 6.0, places=6)

    def test_a_page_follows_its_pmi_views_cameras(self):
        """A page from the top and the front PMI views, their cameras changed
        after: the top turned a quarter about itself, the front made the
        right. Each drawing view is out of sync, says what would change, and
        synced looks as its camera does, with what it shows imported again
        -- the dimension placed anew, the datum on the line it sits on now.
        The page's scale stays; synced again, nothing changes"""
        pset = self.doc.getObject("PMI")

        def pmi_view(name, standard):
            v = self.doc.addObject("PMI::View", name)
            pset.addObject(v)
            v.Standard = standard
            return v

        top, front = pmi_view("TopView", "Top"), pmi_view("FrontView", "Front")
        for name in ("Flatness", "Position", "HoleSize"):
            self.doc.getObject(name).Views = [top]
        self.doc.getObject("DatumA").Views = [front]
        self.doc.recompute()
        page, views = TechDrawPMI.pageFromViews([top, front])
        dt, df = views
        self.assertEqual(TechDrawPMI.pmiViews([page]), views)
        self.assertEqual([TechDrawPMI.outOfSync(v) for v in views], [[], []])
        self.assertEqual(TechDrawPMI.syncViews([page]), ([], []))

        def on():
            return {v.Name: sorted(c.Source.Name for c in self.doc.Objects
                                   if c.isDerivedFrom("TechDraw::DrawPMICallout")
                                   and c.SourceView == v)
                    for v in views}

        shown = on()
        scale = page.Scale
        top.Camera = App.Placement(top.Camera.Base,
                                   top.Camera.Rotation.multiply(App.Rotation(V(0, 0, 1), 90)))
        front.Standard = "Right"
        self.doc.recompute()
        self.assertEqual(sorted(p for _o, p, _v in TechDrawPMI.outOfSync(dt)), ["XDirection"])
        self.assertEqual(sorted(p for _o, p, _v in TechDrawPMI.outOfSync(df)),
                         ["Direction", "XDirection"])
        synced, left = TechDrawPMI.syncViews([page])
        self.assertEqual((synced, left), ([dt, df], []))
        self.assertTrue(dt.Direction.isEqual(V(0, 0, 1), 1e-9), dt.Direction)
        self.assertTrue(dt.XDirection.isEqual(V(0, 1, 0), 1e-9), dt.XDirection)
        self.assertTrue(df.Direction.isEqual(V(1, 0, 0), 1e-9), df.Direction)
        self.assertEqual(page.Scale, scale)
        self.assertEqual(on(), shown)
        # The datum on the bottom line, seen from the right as from the front
        datum = [c for c in self.doc.Objects if c.isDerivedFrom("TechDraw::DrawPMICallout")
                 and c.SourceView == df][0]
        self.assertAlmostEqual(datum.OriginY.Value, -5.0, places=6)
        # The hole's diameter, measured on the turned view, placed on the sheet
        dims = [d for d in self.doc.Objects if d.isDerivedFrom("TechDraw::DrawViewDimension")]
        self.assertEqual([(d.Source.Name, d.References2D[0][0]) for d in dims],
                         [("HoleSize", dt)])
        self.doc.recompute()
        self.assertAlmostEqual(dims[0].getRawValue(), 6.0, places=6)
        bounds, _others = TechDrawPMI._sheet(dt)
        # On paper from the view's middle, as the bounds are
        x, y = dims[0].X.Value, dims[0].Y.Value
        self.assertTrue(bounds[0] <= x <= bounds[2] and bounds[1] <= y <= bounds[3],
                        (x, y, bounds))
        self.assertEqual([TechDrawPMI.outOfSync(v) for v in views], [[], []])
        self.assertEqual(TechDrawPMI.syncViews([page]), ([], []))

    def test_a_section_follows_its_cut(self):
        """A section from a clipped PMI view follows its clipping plane moved;
        a view whose kind its PMI view no longer is -- a plain one clipped
        now, a section unclipped -- cannot follow, and says why"""
        pset = self.doc.getObject("PMI")
        cut = self.doc.addObject("PMI::View", "CutView")
        pset.addObject(cut)
        cut.Standard = "Top"
        cut.Clip = True
        cut.ClipPlane = App.Placement(V(20, 10, 5), App.Rotation())
        plain = self.doc.addObject("PMI::View", "PlainView")
        pset.addObject(plain)
        plain.Standard = "Front"
        self.doc.recompute()
        page, views = TechDrawPMI.pageFromViews([cut, plain])
        sec, dp = views
        cut.ClipPlane = App.Placement(V(20, 10, 3), App.Rotation())
        self.doc.recompute()
        self.assertEqual([p for _o, p, _v in TechDrawPMI.outOfSync(sec)], ["SectionOrigin"])
        self.assertEqual(TechDrawPMI.syncViews([sec]), ([sec], []))
        self.assertAlmostEqual(sec.SectionOrigin.z, 8.0, places=9)
        box = sec.getCutPieces().BoundBox
        self.assertAlmostEqual(box.ZMax, 8.0, places=6)
        cut.Clip = False
        plain.Clip = True
        plain.ClipPlane = App.Placement(V(20, 10, 5), App.Rotation(V(1, 0, 0), -90))
        self.doc.recompute()
        synced, left = TechDrawPMI.syncViews([page])
        self.assertEqual(synced, [])
        self.assertEqual({v.Name for v, _why in left}, {sec.Name, dp.Name})
        self.assertTrue(all(isinstance(why, str) and why for _v, why in left), left)

    def test_a_view_not_projected_imports_nothing(self):
        view = self.doc.addObject("TechDraw::DrawViewPart", "Unseen")
        self.page.addView(view)
        view.Source = [self.body]
        self.assertIsNone(TechDrawPMI.importAnnotations(view))
