# SPDX-License-Identifier: LGPL-2.1-or-later
"""The PMI model, headless (docs/GDandT.md sec 10.1).

A Body -- a 40 x 20 x 10 box with a through hole of radius 3 at (20, 10) --
holding an annotation set, whose annotations refer to the Body's solid
through its solid body: (Solid, "Face6"), never (Body, "Face6"), which
follows the Body's tip to whichever solid it is in (docs/GDandT.md sec 3). What is checked: the objects and their
geometry (TestModel), the rules of sec 8 both ways (TestCheck), and the
references through edits of the Body under them (TestRepair, sec 7).
"""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part  # noqa: F401 -- the shapes' types
import PMI

V = App.Vector


def face_at(shape, point):
    for i, face in enumerate(shape.Faces):
        if face.isInside(point, 1e-6, True):
            return "Face%d" % (i + 1)
    raise AssertionError("no face at %s" % point)


TOP, BOTTOM = V(5, 5, 10), V(5, 5, 0)
LEFT, RIGHT = V(0, 5, 5), V(40, 5, 5)
FRONT, BACK = V(5, 0, 5), V(5, 20, 5)
HOLE = V(23, 10, 5)


class Base(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("PMITest")
        self.body = self.doc.addObject("PartDesign::Body", "Body")
        self.box = self.doc.addObject("PartDesign::AdditiveBox", "Box")
        self.body.addObject(self.box)
        self.box.Length, self.box.Width, self.box.Height = 40, 20, 10
        self.hole = self.doc.addObject("PartDesign::SubtractiveCylinder", "Hole")
        self.body.addObject(self.hole)
        self.hole.Radius, self.hole.Height = 3, 10
        self.hole.Placement = App.Placement(V(20, 10, 0), App.Rotation())
        self.set = self.doc.addObject("PMI::AnnotationSet", "PMI")
        self.body.addObject(self.set)
        self.doc.recompute()
        self.solid = self.body.SolidBodies[0]

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def add(self, kind, name, **props):
        obj = self.doc.addObject("PMI::" + kind, name)
        self.set.addObject(obj)
        for key, value in props.items():
            setattr(obj, key, value)
        return obj

    def feature(self, *points, kind="Integral", solid=None):
        solid = solid or self.solid
        subs = [face_at(solid.Shape, p) for p in points]
        return self.add("TolerancedFeature", "Feature", References=[(solid, subs)], Kind=kind)

    def recompute(self):
        self.doc.recompute()
        bad = [o.Name for o in self.doc.Objects if "Invalid" in o.State or "Error" in o.State]
        bad += [o.Name for o in self.doc.Objects if getattr(o, "Condition", "Ok") == "Invalid"]
        self.assertEqual(bad, [], "objects failed to recompute")

    def issues(self, obj=None, severity=None):
        out = []
        for o, sev, msg in PMI.check(self.set):
            if (obj is None or o == obj) and (severity is None or sev == severity):
                out.append(msg)
        return out


class TestModel(Base):
    def test_set_lives_in_the_body(self):
        # No cycle: the set names the Body by a hidden link, and its
        # annotations depend on the solid body only
        self.assertIn(self.set, self.body.Group)
        self.assertEqual(self.set.Part, self.body)
        self.assertNotIn(self.body, self.set.OutListRecursive)
        # A lone solid has a solid body once the body holds a set
        self.assertEqual(len(self.body.SolidBodies), 1)
        self.feature(TOP)
        self.recompute()

    def test_set_beside_a_shape_that_is_no_body(self):
        box = self.doc.addObject("Part::Box", "Plain")
        self.doc.recompute()
        other = self.doc.addObject("PMI::AnnotationSet", "PlainPMI")
        other.Part = box
        feat = self.doc.addObject("PMI::TolerancedFeature", "PlainTop")
        other.addObject(feat)
        feat.References = [(box, [face_at(box.Shape, V(5, 5, 10))])]
        self.recompute()
        self.assertEqual(feat.Condition, "Ok", feat.ConditionMessage)
        self.assertEqual(PMI.findSet(box), other)

    def test_set_takes_pmi_objects_only(self):
        other = self.doc.addObject("Part::Box", "Other")
        try:
            self.set.addObject(other)
        except Exception:
            pass
        self.assertNotIn(other, self.set.Group)

    def test_features_resolve(self):
        top = self.feature(TOP)
        hole = self.feature(HOLE)
        width = self.feature(LEFT, RIGHT)
        self.recompute()
        for f in (top, hole, width):
            self.assertEqual(f.Condition, "Ok", f.ConditionMessage)
        self.assertEqual(top.Shape.ShapeType, "Face")
        self.assertFalse(top.IsSize)
        self.assertTrue(hole.IsSize)
        self.assertTrue(width.IsSize)

    def test_derived_axis_and_centre_plane(self):
        axis = self.feature(HOLE, kind="Axis")
        mid = self.feature(LEFT, RIGHT, kind="CentrePlane")
        self.recompute()
        self.assertEqual(axis.Shape.ShapeType, "Edge")
        a, b = axis.Shape.Vertexes[0].Point, axis.Shape.Vertexes[1].Point
        self.assertAlmostEqual(abs(a.z - b.z), 10, 6)
        self.assertAlmostEqual(a.x, 20, 6)
        self.assertAlmostEqual(a.y, 10, 6)
        self.assertEqual(mid.Shape.ShapeType, "Face")
        self.assertAlmostEqual(mid.Shape.CenterOfMass.x, 20, 6)
        bad = self.feature(TOP, kind="Axis")
        self.doc.recompute()
        self.assertEqual(bad.Condition, "Invalid")

    def test_dimensions_measure_the_model(self):
        hole, top, bottom = self.feature(HOLE), self.feature(TOP), self.feature(BOTTOM)
        left, width = self.feature(LEFT), self.feature(LEFT, RIGHT)
        cases = [
            ("Diameter", [hole], 6),
            ("Radius", [hole], 3),
            ("Width", [width], 40),
            ("Distance", [left, hole], 20),
            ("Distance", [top, bottom], 10),
            ("Angle", [top, left], 90),
        ]
        dims = [self.add("Dimension", "D", Type=t, Features=f) for t, f, _ in cases]
        self.recompute()
        for dim, (t, _, want) in zip(dims, cases):
            self.assertEqual(dim.Condition, "Ok", "%s: %s" % (t, dim.ConditionMessage))
            self.assertAlmostEqual(dim.Nominal.Value, want, 6, t)

    def test_a_size_is_measured_on_the_faces_of_an_axis(self):
        # The hole's feature is its axis, shared with a position tolerance;
        # its diameter is still the cylinder's
        axis = self.feature(HOLE, kind="Axis")
        dim = self.add("Dimension", "Dia", Type="Diameter", Features=[axis])
        self.recompute()
        self.assertAlmostEqual(dim.Nominal.Value, 6, 6)

    def test_find_feature_shares(self):
        top = self.feature(TOP)
        self.recompute()
        name = face_at(self.solid.Shape, TOP)
        self.assertEqual(PMI.findFeature(self.set, self.solid, [name]), top)
        self.assertIsNone(PMI.findFeature(self.set, self.solid, [face_at(self.solid.Shape, LEFT)]))
        self.assertEqual(PMI.findSet(self.body), self.set)
        self.assertEqual(PMI.findSet(self.solid), self.set)

    def test_datum_reference_frame(self):
        for letter, point in (("A", BOTTOM), ("B", LEFT), ("C", FRONT)):
            self.add("Datum", "Datum" + letter, Letter=letter, Feature=self.feature(point))
        good = self.add("DatumReferenceFrame", "DRF", Compartments=["A", "B", "C(M)"])
        common = self.add("DatumReferenceFrame", "DRF", Compartments=["A-B"])
        self.recompute()
        self.assertEqual(good.Condition, "Ok", good.ConditionMessage)
        self.assertEqual(common.Condition, "Ok", common.ConditionMessage)
        datum = self.doc.getObject("DatumA")
        self.assertEqual(datum.Shape.ShapeType, "Face")
        for bad in (["A", "A"], ["A(Q)"], ["A", "B", "C", "A"], [], ["a"]):
            drf = self.add("DatumReferenceFrame", "Bad", Compartments=bad)
            self.doc.recompute()
            self.assertEqual(drf.Condition, "Invalid", bad)

    def test_save_and_reopen(self):
        hole = self.feature(HOLE)
        dim = self.add("Dimension", "Dia", Type="Diameter", Features=[hole], Upper=0.1)
        self.set.Standard = "ASME Y14.5-2018"
        self.recompute()
        path = os.path.join(tempfile.mkdtemp(), "pmi.FCStd")
        name = hole.Name
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.set = self.doc.getObject("PMI")
        hole = self.doc.getObject(name)
        dim = self.doc.getObject("Dia")
        self.assertEqual(self.set.Standard, "ASME Y14.5-2018")
        self.assertEqual(self.set.Part, self.doc.getObject("Body"))
        self.assertIn(hole, self.set.Group)
        # The derived geometry is back before any recompute
        self.assertFalse(hole.Shape.isNull())
        self.assertAlmostEqual(dim.Nominal.Value, 6, 6)
        self.assertAlmostEqual(dim.Upper.Value, 0.1, 9)
        self.assertEqual(self.issues(dim, "Error"), [])
        self.recompute()
        self.assertEqual(hole.Condition, "Ok")


class TestCheck(Base):
    def frame(self):
        for letter, point in (("A", BOTTOM), ("B", LEFT), ("C", FRONT)):
            self.add("Datum", "Datum" + letter, Letter=letter, Feature=self.feature(point))
        return self.add("DatumReferenceFrame", "DRF", Compartments=["A", "B", "C"])

    def tolerance(self, characteristic, feature, **props):
        return self.add(
            "GeometricTolerance",
            "GT",
            Characteristic=characteristic,
            Feature=feature,
            Value=0.05,
            **props
        )

    def assertRule(self, obj, text):
        self.doc.recompute()
        found = [m for m in self.issues(obj, "Error") if text in m]
        self.assertTrue(found, "no error with '%s': %s" % (text, self.issues(obj)))
        self.assertEqual(obj.Condition, "Invalid")

    def assertClean(self, obj):
        self.doc.recompute()
        self.assertEqual(self.issues(obj, "Error"), [])
        self.assertEqual(obj.Condition, "Ok", obj.ConditionMessage)

    def test_form_takes_no_datums(self):
        drf = self.frame()
        top = self.feature(TOP)
        self.assertClean(self.tolerance("Flatness", top))
        self.assertRule(self.tolerance("Flatness", top, DatumSystem=drf), "takes no datums")

    def test_orientation_needs_datums(self):
        drf = self.frame()
        top = self.feature(TOP)
        self.assertRule(self.tolerance("Parallelism", top), "needs a datum reference frame")
        self.assertClean(self.tolerance("Parallelism", top, DatumSystem=drf))

    def test_position_without_datums_warns(self):
        gt = self.tolerance("Position", self.feature(HOLE, kind="Axis"), ZoneShape="Diameter")
        self.doc.recompute()
        self.assertEqual(self.issues(gt, "Error"), [])
        self.assertTrue(self.issues(gt, "Warning"))

    def test_zone_shape(self):
        drf = self.frame()
        top, axis = self.feature(TOP), self.feature(HOLE, kind="Axis")
        self.assertRule(self.tolerance("Flatness", top, ZoneShape="Diameter"), "diameter zone")
        self.assertClean(
            self.tolerance("Perpendicularity", axis, ZoneShape="Diameter", DatumSystem=drf)
        )
        self.assertRule(
            self.tolerance("Perpendicularity", top, ZoneShape="Diameter", DatumSystem=drf),
            "needs an axis",
        )

    def test_material_condition_needs_a_feature_of_size(self):
        drf = self.frame()
        self.assertClean(
            self.tolerance(
                "Position",
                self.feature(HOLE),
                ZoneShape="Diameter",
                MaterialCondition="Maximum",
                DatumSystem=drf,
            )
        )
        self.assertRule(
            self.tolerance("Parallelism", self.feature(TOP), MaterialCondition="Maximum",
                           DatumSystem=drf),
            "feature of size",
        )

    def test_asme_2018_has_no_concentricity(self):
        drf = self.frame()
        hole = self.feature(HOLE, kind="Axis")
        gt = self.tolerance("Concentricity", hole, DatumSystem=drf)
        self.set.Standard = "ASME Y14.5-2018"
        self.assertRule(gt, "not in ASME Y14.5-2018")
        self.set.Standard = "ASME Y14.5-2009"
        gt.touch()
        self.assertClean(gt)

    def test_datum_letters(self):
        drf = self.add("DatumReferenceFrame", "DRF", Compartments=["A", "D"])
        self.add("Datum", "A1", Letter="A", Feature=self.feature(BOTTOM))
        self.add("Datum", "A2", Letter="A", Feature=self.feature(TOP))
        self.doc.recompute()
        self.assertTrue([m for m in self.issues(drf) if "datum D is not defined" in m])
        self.assertTrue([m for m in self.issues() if "defined twice" in m])
        self.set.Standard = "ASME Y14.5-2018"
        letter_i = self.add("Datum", "I", Letter="I", Feature=self.feature(LEFT))
        self.doc.recompute()
        self.assertTrue([m for m in self.issues(letter_i) if "letter I" in m])
        self.assertEqual(letter_i.Condition, "Invalid")

    def test_dimension_rules(self):
        hole = self.feature(HOLE)
        basic = self.add("Dimension", "Basic", Type="Diameter", Features=[hole], Basic=True)
        self.assertClean(basic)
        basic.Upper = 0.1
        self.doc.recompute()
        self.assertTrue([m for m in self.issues(basic) if "basic dimension" in m])
        fit = self.add("Dimension", "Fit", Type="Diameter", Features=[hole], Fit="H7")
        self.assertClean(fit)
        fit.Fit = "H77"
        self.doc.recompute()
        self.assertTrue([m for m in self.issues(fit) if "ISO 286" in m])
        bad = self.add("Dimension", "Bad", Type="Diameter", Features=[self.feature(TOP)])
        self.doc.recompute()
        self.assertEqual(bad.Condition, "Invalid")

    def test_projected_zone_and_composite(self):
        drf = self.frame()
        hole = self.feature(HOLE, kind="Axis")
        p = self.tolerance("Position", hole, ZoneShape="Diameter", DatumSystem=drf, Modifiers=["P"])
        self.assertRule(p, "needs its length")
        p.ProjectedLength = 12
        self.assertClean(p)
        lower = self.tolerance("Perpendicularity", hole, ZoneShape="Diameter", DatumSystem=drf,
                               Composite=p)
        self.assertRule(lower, "composite")


class TestRepair(Base):
    def test_a_length_change_keeps_everything(self):
        width = self.feature(LEFT, RIGHT)
        dim = self.add("Dimension", "W", Type="Width", Features=[width], Upper=0.2, Lower=-0.2)
        self.recompute()
        self.box.Length = 50
        self.recompute()
        self.assertEqual(width.Condition, "Ok", width.ConditionMessage)
        self.assertAlmostEqual(dim.Nominal.Value, 50, 6)
        self.assertAlmostEqual(dim.Upper.Value, 0.2, 9)

    def test_a_feature_elsewhere_leaves_a_face_alone(self):
        left, top = self.feature(LEFT), self.feature(TOP)
        self.recompute()
        second = self.doc.addObject("PartDesign::SubtractiveCylinder", "Second")
        self.body.addObject(second)
        second.Radius, second.Height = 2, 10
        second.Placement = App.Placement(V(32, 10, 0), App.Rotation())
        self.recompute()
        # The left face is untouched; the top face is cut, by history
        self.assertEqual(left.Condition, "Ok", left.ConditionMessage)
        self.assertIn(top.Condition, ("Ok", "Remapped"), top.ConditionMessage)
        self.assertNotIn("geometry", top.ConditionMessage)
        self.assertAlmostEqual(top.Shape.CenterOfMass.z, 10, 6)

    def test_a_fillet_remaps_by_history(self):
        top = self.feature(TOP)
        self.recompute()
        area = top.Shape.Area
        fillet = self.doc.addObject("PartDesign::Fillet", "Fillet")
        self.body.addObject(fillet)
        fillet.Base = (self.hole, [face_at(self.hole.Shape, TOP)])
        fillet.Radius = 1
        self.recompute()
        self.assertEqual(top.Condition, "Remapped", top.ConditionMessage)
        self.assertIn("history", top.ConditionMessage)
        self.assertEqual(top.Shape.ShapeType, "Face")
        self.assertLess(top.Shape.Area, area)
        self.assertAlmostEqual(top.Shape.CenterOfMass.z, 10, 6)
        # It stays remapped until the user says it is right
        self.body.touch()
        self.recompute()
        self.assertEqual(top.Condition, "Remapped")
        top.Condition = "Ok"
        self.body.touch()
        self.recompute()
        self.assertEqual(top.Condition, "Ok", top.ConditionMessage)

    def test_a_face_gone_is_broken_and_kept(self):
        right = self.feature(RIGHT)
        dim = self.add("Dimension", "D", Type="Distance", Features=[self.feature(LEFT), right])
        self.recompute()
        cover = self.doc.addObject("PartDesign::AdditiveBox", "Cover")
        self.body.addObject(cover)
        cover.Length, cover.Width, cover.Height = 42, 22, 12
        cover.Placement = App.Placement(V(-1, -1, -1), App.Rotation())
        self.doc.recompute()
        self.assertEqual(right.Condition, "Broken")
        # Drawn where it was
        self.assertFalse(right.Shape.isNull())
        self.assertAlmostEqual(right.Shape.CenterOfMass.x, 40, 6)
        self.assertEqual(dim.Condition, "Broken")
        self.assertTrue([m for m in self.issues(right, "Error") if "broken" in m])

    def test_a_rebuilt_feature_remaps_by_geometry(self):
        lid = self.doc.addObject("PartDesign::AdditiveBox", "Lid")
        self.body.addObject(lid)
        lid.Placement = App.Placement(V(0, 0, 10), App.Rotation())
        self.recompute()
        top = self.feature(V(5, 5, 20))
        self.recompute()
        # The same lid made again, by a new feature: new names, no history
        self.body.removeObject(lid)
        self.doc.removeObject(lid.Name)
        again = self.doc.addObject("PartDesign::AdditiveBox", "Lid2")
        self.body.addObject(again)
        again.Placement = App.Placement(V(0, 0, 10), App.Rotation())
        self.doc.recompute()
        self.assertEqual(top.Condition, "Remapped", top.ConditionMessage)
        self.assertIn("geometry", top.ConditionMessage)
        self.assertAlmostEqual(top.Shape.CenterOfMass.z, 20, 6)

    def test_a_second_solid_does_not_move_a_reference(self):
        # (Body, "Face6") would now be the new solid's face (the Body's shape
        # is its tip's); (Solid, "Face6") stays on this one
        top = self.feature(TOP)
        self.recompute()
        other = self.doc.addObject("PartDesign::AdditiveBox", "Other")
        self.body.addObject(other)
        other.Placement = App.Placement(V(100, 0, 0), App.Rotation())
        other.NewSolid = True
        self.recompute()
        self.assertEqual(len(self.body.SolidBodies), 2)
        self.assertIn(self.solid, self.body.SolidBodies)
        self.assertEqual(top.Condition, "Ok", top.ConditionMessage)
        self.assertAlmostEqual(top.Shape.CenterOfMass.x, 20, 6)
        self.assertAlmostEqual(top.Shape.CenterOfMass.z, 10, 6)

    def test_a_rolled_back_solid_leaves_its_annotations_alone(self):
        hole = self.feature(HOLE)
        dim = self.add("Dimension", "Dia", Type="Diameter", Features=[hole])
        self.recompute()
        refs, com = hole.References, hole.Shape.CenterOfMass
        self.body.Tip = self.box
        self.doc.recompute()
        self.assertEqual(self.solid.Tip, self.box)
        # The hole is not there at this step: nothing resolved against it,
        # nothing broken, nothing written back
        self.assertEqual(hole.Condition, "Ok", hole.ConditionMessage)
        self.assertTrue(hole.Shape.CenterOfMass.isEqual(com, 1e-9))
        self.assertAlmostEqual(dim.Nominal.Value, 6, 6)
        self.body.Tip = self.hole
        self.recompute()
        self.assertEqual(hole.Condition, "Ok", hole.ConditionMessage)
        self.assertEqual(hole.References, refs)

    def test_editing_a_tolerance_leaves_the_body_alone(self):
        # A drawing or an assembly depends on the Body; editing a tolerance
        # must not rebuild either (docs/GDandT.md sec 3). A refine of the
        # Body stands for them: it makes a new shape whenever it executes.
        downstream = self.doc.addObject("Part::Refine", "Downstream")
        downstream.Source = self.body
        gt = self.add("GeometricTolerance", "GT", Characteristic="Flatness",
                      Feature=self.feature(TOP), Value=0.05)
        self.recompute()
        body, after = self.body.Shape.hashCode(), downstream.Shape.hashCode()
        gt.Value = 0.02
        self.recompute()
        self.assertEqual(self.body.Shape.hashCode(), body)
        self.assertEqual(downstream.Shape.hashCode(), after)
        # A change of the solid still goes through
        self.box.Length = 45
        self.recompute()
        self.assertNotEqual(downstream.Shape.hashCode(), after)

    def test_each_solid_has_its_own_datums(self):
        other = self.doc.addObject("PartDesign::AdditiveBox", "Other")
        self.body.addObject(other)
        other.Placement = App.Placement(V(100, 0, 0), App.Rotation())
        other.NewSolid = True
        self.recompute()
        second = [s for s in self.body.SolidBodies if s != self.solid][0]
        self.add("Datum", "A1", Letter="A", Feature=self.feature(BOTTOM))
        self.add("Datum", "A2", Letter="A", Feature=self.feature(V(105, 5, 0), solid=second))
        self.add("Datum", "B1", Letter="B", Feature=self.feature(LEFT))
        drf = self.add("DatumReferenceFrame", "DRF", Compartments=["A", "B"])
        gt = self.add("GeometricTolerance", "GT", Characteristic="Parallelism",
                      Feature=self.feature(V(105, 5, 10), solid=second), Value=0.05,
                      DatumSystem=drf)
        self.recompute()
        msgs = [m for _, _, m in PMI.check(self.set)]
        self.assertFalse([m for m in msgs if "defined twice" in m], msgs)
        self.assertTrue([m for m in self.issues(gt, "Warning") if "datum B is on another solid" in m])


class TestPresentation(Base):
    """What an annotation looks like, drawn from its semantics (sec 6)"""

    def frame(self):
        for letter, point in (("A", BOTTOM), ("B", LEFT), ("C", FRONT)):
            self.add("Datum", "Datum" + letter, Letter=letter, Feature=self.feature(point))
        return self.add("DatumReferenceFrame", "DRF", Compartments=["A", "B", "C(M)"])

    def test_a_frame_is_drawn_on_its_face(self):
        top = self.feature(TOP)
        gt = self.add("GeometricTolerance", "GT", Characteristic="Flatness", Feature=top,
                      Value=0.05)
        self.recompute()
        shape = gt.Presentation
        self.assertFalse(shape.isNull())
        self.assertTrue(shape.Edges, "no strokes")
        self.assertTrue(shape.Faces, "no text")
        # On the top face's own plane, a little above it, read from above
        h = gt.TextHeight.Value
        self.assertAlmostEqual(gt.Plane.Rotation.multVec(V(0, 0, 1)).z, 1, 9)
        for v in shape.Vertexes:
            self.assertAlmostEqual(v.Point.z, 10 + 0.05 * h, 6)
        self.assertEqual(gt.EquivalentText, "\u23e5|0.05")

    def test_the_leader_reaches_the_feature(self):
        top = self.feature(TOP)
        gt = self.add("GeometricTolerance", "GT", Characteristic="Flatness", Feature=top,
                      Value=0.05)
        self.recompute()
        face = top.Shape
        near = min(face.distToShape(Part.Vertex(v.Point))[0] for v in gt.Presentation.Vertexes)
        self.assertLess(near, 0.1 * gt.TextHeight.Value + 1e-6)

    def test_the_leader_ends_in_the_material(self):
        # The top face has the hole in its middle: the leader goes into the
        # face, not to the hole's rim
        top = self.feature(TOP)
        gt = self.add("GeometricTolerance", "GT", Characteristic="Flatness", Feature=top,
                      Value=0.05)
        self.recompute()
        face = top.Shape
        end = min(gt.Presentation.Vertexes, key=lambda v: face.distToShape(v)[0]).Point
        self.assertGreater(math.hypot(end.x - 20, end.y - 10), 6, end)

    def test_a_frame_with_datums(self):
        drf = self.frame()
        gt = self.add("GeometricTolerance", "GT", Characteristic="Position",
                      Feature=self.feature(HOLE, kind="Axis"), ZoneShape="Diameter",
                      Value=0.1, MaterialCondition="Maximum", DatumSystem=drf)
        self.recompute()
        self.assertEqual(gt.EquivalentText, "\u2316|\u23000.1\u24c2|A|B|C\u24c2")
        # Four cells and more strokes than a bare frame
        self.assertGreater(len(gt.Presentation.Edges), 20)

    def test_dimension_texts(self):
        hole = self.feature(HOLE)
        dim = self.add("Dimension", "Dia", Type="Diameter", Features=[hole], Upper=0.1, Lower=-0.1)
        fit = self.add("Dimension", "Fit", Type="Diameter", Features=[hole], Fit="H7")
        basic = self.add("Dimension", "Basic", Type="Distance",
                         Features=[self.feature(LEFT), hole], Basic=True)
        width = self.add("Dimension", "Width", Type="Width",
                         Features=[self.feature(LEFT, RIGHT)], Upper=0.2, Lower=-0.1,
                         DecimalPlaces=1)
        self.recompute()
        self.assertEqual(dim.EquivalentText, "\u23006.00 \u00b10.1")
        self.assertEqual(fit.EquivalentText, "\u23006.00 H7")
        self.assertEqual(basic.EquivalentText, "[20.00]")
        self.assertEqual(width.EquivalentText, "40.0 +0.2/-0.1")
        for d in (dim, fit, basic, width):
            self.assertTrue(d.Presentation.Faces, d.Name)

    def test_a_datum_ends_in_a_triangle_on_its_face(self):
        bottom = self.feature(BOTTOM)
        datum = self.add("Datum", "A", Letter="A", Feature=bottom)
        self.recompute()
        self.assertEqual(datum.EquivalentText, "A")
        # The triangle: a three-sided face touching the bottom face's plane
        tri = [f for f in datum.Presentation.Faces if len(f.Edges) == 3]
        self.assertTrue(tri)
        self.assertLess(min(abs(v.Point.z) for f in tri for v in f.Vertexes), 0.1 * 3.5 + 1e-6)

    def test_a_moved_callout_stays_where_it_was_put(self):
        gt = self.add("GeometricTolerance", "GT", Characteristic="Flatness",
                      Feature=self.feature(TOP), Value=0.05)
        self.recompute()
        self.assertTrue(gt.AutoPlace)
        put = App.Placement(V(0, -30, 5), App.Rotation(V(1, 0, 0), 90))
        gt.Plane = put
        self.assertFalse(gt.AutoPlace)
        self.box.Height = 14
        self.recompute()
        self.assertTrue(gt.Plane.isSame(put, 1e-9))
        # Its leader follows the face, which moved up: a target off the plane
        # is drawn where it projects onto it (CAx-IF sec 8.3) -- here the
        # plane y = -30, and the face's height, 14, above the frame's top
        tips = [v.Point for v in gt.Presentation.Vertexes if abs(v.Point.z - 14) < 1e-6]
        self.assertTrue(tips)
        self.assertTrue(all(abs(p.y + 30) < 1e-6 for p in tips))

    def test_a_note(self):
        note = self.add("Note", "Note", Text=["BREAK SHARP EDGES", "DEBURR"])
        self.recompute()
        self.assertEqual(note.EquivalentText, "BREAK SHARP EDGES\nDEBURR")
        self.assertTrue(note.Presentation.Faces)

    def test_reopened_it_is_drawn_before_a_recompute(self):
        gt = self.add("GeometricTolerance", "GT", Characteristic="Flatness",
                      Feature=self.feature(TOP), Value=0.05)
        self.recompute()
        edges = len(gt.Presentation.Edges)
        path = os.path.join(tempfile.mkdtemp(), "pmi-look.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        gt = self.doc.getObject("GT")
        self.assertEqual(len(gt.Presentation.Edges), edges)

    def test_callouts_together_are_stacked(self):
        top = self.feature(TOP)
        a = self.add("GeometricTolerance", "GT1", Characteristic="Flatness", Feature=top,
                     Value=0.05)
        b = self.add("GeometricTolerance", "GT2", Characteristic="Straightness", Feature=top,
                     Value=0.02)
        self.recompute()
        h = a.TextHeight.Value
        # Each a slot of three text heights up, in the set's order
        self.assertAlmostEqual((b.Plane.Base - a.Plane.Base).Length, 3 * h, 6)
        ba, bb = a.Presentation.BoundBox, b.Presentation.BoundBox
        self.assertGreater(bb.YMax, ba.YMax)

    def test_an_axis_leader_ends_where_the_axis_leaves_the_part(self):
        gt = self.add("GeometricTolerance", "GT", Characteristic="Position",
                      Feature=self.feature(HOLE, kind="Axis"), ZoneShape="Diameter", Value=0.1)
        self.recompute()
        h = gt.TextHeight.Value
        # Looked at along the axis, from above; the leader's tip at the top end
        self.assertAlmostEqual(gt.Plane.Rotation.multVec(V(0, 0, 1)).z, 1, 9)
        tip = V(20, 10, 10 + 0.05 * h)
        self.assertLess(min((v.Point - tip).Length for v in gt.Presentation.Vertexes), 1e-6)


class TestApi(Base):
    """What the commands call (docs/GDandT.md sec 3, 4)"""

    def test_a_pick_on_the_tip_becomes_a_solid_reference(self):
        # A pick lands on the Body's tip feature, through the Body
        name = face_at(self.hole.Shape, TOP)
        solid, face = PMI.solidReference(self.body, "%s.%s" % (self.hole.Name, name))
        self.assertEqual(solid, self.solid)
        self.assertEqual(face, face_at(self.solid.Shape, TOP))
        # Picked on the solid body itself
        solid, face = PMI.solidReference(self.solid, face_at(self.solid.Shape, LEFT))
        self.assertEqual(face, face_at(self.solid.Shape, LEFT))
        with self.assertRaises(ValueError):
            PMI.solidReference(self.body, "%s.Edge1" % self.hole.Name)

    def test_a_pick_on_a_moved_body(self):
        self.body.Placement = App.Placement(V(100, 0, 0), App.Rotation(V(0, 0, 1), 30))
        self.recompute()
        name = face_at(self.hole.Shape, TOP)
        solid, face = PMI.solidReference(self.body, "%s.%s" % (self.hole.Name, name))
        self.assertEqual((solid, face), (self.solid, face_at(self.solid.Shape, TOP)))

    def test_the_set_is_found_or_made(self):
        self.assertEqual(PMI.getSet(self.body), self.set)
        self.assertEqual(PMI.getSet(self.solid), self.set)
        body = self.doc.addObject("PartDesign::Body", "Body2")
        body.newObjectAt("PartDesign::AdditiveBox", "Box2")
        self.doc.recompute()
        self.assertIsNone(PMI.getSet(body, False))
        made = PMI.getSet(body)
        self.assertIn(made, body.Group)
        self.assertEqual(made.Part, body)
        self.doc.recompute()
        self.assertEqual(len(body.SolidBodies), 1)
        plain = self.doc.addObject("Part::Box", "Plain")
        beside = PMI.getSet(plain)
        self.assertEqual(beside.Part, plain)

    def test_features_are_shared(self):
        name = face_at(self.solid.Shape, TOP)
        a = PMI.feature(self.set, self.solid, [name])
        b = PMI.feature(self.set, self.solid, [name])
        self.assertEqual(a, b)
        axis = PMI.feature(self.set, self.solid, [face_at(self.solid.Shape, HOLE)], "Axis")
        self.assertEqual(axis.Kind, "Axis")

    def test_datum_letters_go_on(self):
        self.assertEqual(PMI.nextDatumLetter(self.set, self.solid), "A")
        for letter in "ABCDEFGH":
            self.add("Datum", "D" + letter, Letter=letter, Feature=self.feature(BOTTOM))
        self.set.Standard = "ASME Y14.5-2018"
        self.assertEqual(PMI.nextDatumLetter(self.set, self.solid), "J")
        self.set.Standard = "ISO GPS"
        self.assertEqual(PMI.nextDatumLetter(self.set, self.solid), "I")


class TestAdvisor(Base):
    """What the panel offers for a feature (the tolerancing advisor)"""

    def test_a_plane(self):
        top = self.feature(TOP)
        self.recompute()
        advice = PMI.advise(top)
        self.assertEqual(advice["characteristics"][0], "Flatness")
        self.assertNotIn("Cylindricity", advice["characteristics"])
        flat = PMI.advise(top, "Flatness")
        self.assertEqual(flat["zoneShapes"], ["None"])
        self.assertFalse(flat["materialCondition"])
        self.assertEqual(flat["datums"], -1)

    def test_a_hole(self):
        hole = self.feature(HOLE, kind="Axis")
        self.recompute()
        advice = PMI.advise(hole, "Position")
        self.assertEqual(advice["characteristics"][0], "Position")
        self.assertIn("Cylindricity", advice["characteristics"])
        self.assertEqual(advice["zoneShapes"][0], "Diameter")
        self.assertTrue(advice["materialCondition"])
        self.assertEqual(advice["datums"], 0)
        self.assertEqual(PMI.advise(hole, "Perpendicularity")["datums"], 1)

    def test_a_width(self):
        width = self.feature(LEFT, RIGHT)
        self.recompute()
        advice = PMI.advise(width)
        self.assertIn("Symmetry", advice["characteristics"])
        self.assertTrue(PMI.advise(width, "Position")["materialCondition"])

    def test_every_advised_choice_passes_the_check(self):
        # What the advisor offers, the check takes -- with the frame it asks for
        for letter, point in (("A", BOTTOM), ("B", LEFT), ("C", FRONT)):
            self.add("Datum", "D" + letter, Letter=letter, Feature=self.feature(point))
        drf = PMI.frame(self.set, ["A", "B", "C"])
        self.assertEqual(PMI.frame(self.set, ["A", "B", "C"]), drf)
        for feat in (self.feature(TOP), self.feature(HOLE, kind="Axis"), self.feature(LEFT, RIGHT)):
            self.recompute()
            for c in PMI.advise(feat)["characteristics"]:
                advice = PMI.advise(feat, c)
                gt = self.add("GeometricTolerance", "GT", Characteristic=c, Feature=feat,
                              Value=0.1, ZoneShape=advice["zoneShapes"][0])
                if advice["datums"] >= 0:
                    gt.DatumSystem = drf
                self.doc.recompute()
                errors = self.issues(gt, "Error")
                self.assertEqual(errors, [], "%s on %s: %s" % (c, feat.References, errors))
                self.doc.removeObject(gt.Name)
