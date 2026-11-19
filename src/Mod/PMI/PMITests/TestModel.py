# SPDX-License-Identifier: LGPL-2.1-or-later
"""The PMI model, headless (docs/GDandT.md sec 10.1).

A Body -- a 40 x 20 x 10 box with a through hole of radius 3 at (20, 10) --
holding an annotation set, whose annotations refer to the Body's solid
through its solid body: (Solid, "Face6"), never (Body, "Face6"), which
follows the Body's tip to whichever solid it is in (docs/GDandT.md sec 3). What is checked: the objects and their
geometry (TestModel), the rules of sec 8 both ways (TestCheck), and the
references through edits of the Body under them (TestRepair, sec 7).
"""

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
