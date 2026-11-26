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
import shutil
import tempfile
import unittest

import FreeCAD as App
import Part  # noqa: F401 -- the shapes' types
import PMI

try:
    import PartDesign  # noqa: F401 -- the parts the tests are of are Bodies
except ImportError:
    PartDesign = None

# The PMI module needs Part and Import only; a build without PartDesign runs
# the tests that need no Body
needs_part_design = unittest.skipIf(PartDesign is None, "PartDesign is not built")

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


@needs_part_design
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

    def test_a_pattern_of_holes_is_of_size(self):
        # Two more holes, one of the same size: "2X" is a pattern of features
        # of size, a hole of another size with them is not
        for name, x, r in (("Hole2", 8, 3), ("Small", 32, 2)):
            h = self.doc.addObject("PartDesign::SubtractiveCylinder", name)
            self.body.addObject(h)
            h.Radius, h.Height = r, 10
            h.Placement = App.Placement(V(x, 10, 0), App.Rotation())
        self.doc.recompute()
        two = self.feature(HOLE, V(11, 10, 5))
        mixed = self.feature(HOLE, V(34, 10, 5))
        self.recompute()
        self.assertTrue(two.IsSize)
        self.assertFalse(mixed.IsSize)

    def cut(self, kind, name, at, **dims):
        obj = self.doc.addObject("PartDesign::Subtractive" + kind, name)
        self.body.addObject(obj)
        for key, value in dims.items():
            setattr(obj, key, value)
        obj.Placement = App.Placement(at, App.Rotation())
        return obj

    def faces_where(self, test):
        shape = self.solid.Shape
        return ["Face%d" % (i + 1) for i, f in enumerate(shape.Faces) if test(f)]

    def slot(self, name, x0, x1):
        """A slot through, 4 wide across y at y = 10, its ends round"""
        self.cut("Box", name, V(x0, 8, 0), Length=x1 - x0, Width=4, Height=10)
        for i, x in enumerate((x0, x1)):
            self.cut("Cylinder", "%sEnd%d" % (name, i), V(x, 10, 0), Radius=2, Height=10)

    def test_slots_and_widths_in_a_pattern_are_of_size(self):
        self.slot("Slot", 4, 12)
        self.slot("Other", 28, 36)
        self.doc.recompute()

        def within(x0, x1):
            def test(f):
                b = f.BoundBox
                return (b.XMin > x0 - 1e-6 and b.XMax < x1 + 1e-6 and b.YMin > 8 - 1e-6
                        and b.YMax < 12 + 1e-6 and b.ZLength > 1)
            return test

        def planar(test):
            return lambda f: test(f) and f.Surface.TypeId == "Part::GeomPlane"

        one = self.faces_where(within(2, 14))
        walls = self.faces_where(planar(within(2, 14))) + self.faces_where(planar(within(26, 38)))
        # Two walls and the round ends (one may be split at its seam)
        self.assertEqual(len(walls), 4)
        self.assertGreaterEqual(len(one), 4)
        back = face_at(self.solid.Shape, BACK)
        mixed = self.faces_where(planar(within(2, 14)))[:1] + [back]

        feats = {}
        for name, subs in (("Slot", one), ("Walls", walls), ("Facing", mixed)):
            feats[name] = self.add("TolerancedFeature", name, References=[(self.solid, subs)])
        dims = {}
        for name in ("Slot", "Walls"):
            dims[name] = self.add("Dimension", name + "Width", Type="Width",
                                  Features=[feats[name]])
        self.recompute()
        # A slot, two slots' walls ("2X"): each a width; two faces facing
        # the same way are a depth, no size
        self.assertTrue(feats["Slot"].IsSize)
        self.assertTrue(feats["Walls"].IsSize)
        self.assertFalse(feats["Facing"].IsSize)
        for d in dims.values():
            self.assertAlmostEqual(d.Nominal, 4, 9)
        # Its frame is read looking down the slot, its leader to the top of
        # a wall
        gt = self.add("GeometricTolerance", "SlotPosition", Characteristic="Position",
                      Feature=feats["Slot"], Value=0.3)
        self.recompute()
        self.assertTrue(gt.Plane.Rotation.multVec(V(0, 0, 1)).isEqual(V(0, 0, 1), 1e-9),
                        gt.Plane)
        self.assertAlmostEqual(gt.Plane.Base.z, 10, 0)

    def test_a_step_is_no_width(self):
        """Two parallel planes facing the same way (a slot's wall and the
        back it faces like) are a step: the height between them is a
        distance, not a width"""
        self.slot("Slot", 4, 12)
        self.doc.recompute()
        shape = self.solid.Shape

        def wall(f):
            b = f.BoundBox
            return (f.Surface.TypeId == "Part::GeomPlane" and b.XMin > 2 and b.XMax < 14
                    and b.YLength < 1e-6 and f.normalAt(0, 0).y > 0.5)

        walls = self.faces_where(wall)
        self.assertEqual(len(walls), 1)
        back = face_at(shape, BACK)
        self.assertGreater(shape.getElement(back).normalAt(0, 0).y, 0.5)
        step = self.add("TolerancedFeature", "Step", References=[(self.solid, walls + [back])])
        width = self.add("Dimension", "StepWidth", Type="Width", Features=[step])
        self.doc.recompute()
        self.assertEqual(width.Condition, "Invalid")
        self.assertIn("step", width.ConditionMessage)

    def test_counterbored_holes_are_of_size(self):
        # The hole of the base (radius 3 at (20, 10)) bored to radius 5, 3
        # deep, and another the same at (8, 10); a third, plain
        for name, x in (("Bore", 20), ("Hole2", 8), ("Bore2", 8), ("Plain", 32)):
            if name.startswith("Bore"):
                self.cut("Cylinder", name, V(x, 10, 7), Radius=5, Height=3)
            else:
                self.cut("Cylinder", name, V(x, 10, 0), Radius=3, Height=10)
        self.doc.recompute()

        def around(x, r=5.5):
            def test(f):
                c = f.BoundBox.Center
                return (f.BoundBox.XLength < 2 * r and abs(c.x - x) < 1e-6
                        and abs(c.y - 10) < 1e-6)
            return test

        one = self.faces_where(around(20))
        both = one + self.faces_where(around(8))
        mixed = one + self.faces_where(around(32))
        self.assertEqual((len(one), len(both), len(mixed)), (3, 6, 4))
        feats = {}
        for name, subs in (("One", one), ("Both", both), ("Mixed", mixed)):
            feats[name] = self.add("TolerancedFeature", name, References=[(self.solid, subs)])
        axis = self.add("TolerancedFeature", "BoreAxis", References=[(self.solid, one)],
                        Kind="Axis")
        self.recompute()
        self.assertTrue(feats["One"].IsSize)
        self.assertTrue(feats["Both"].IsSize)
        self.assertFalse(feats["Mixed"].IsSize)
        # Its axis the hole's, whichever face comes first
        a, b = axis.Shape.Vertexes[0].Point, axis.Shape.Vertexes[1].Point
        self.assertAlmostEqual(a.x, 20, 6)
        self.assertAlmostEqual(b.y, 10, 6)

    def test_a_holes_depth(self):
        """A Depth: a counterbore's, its bore down to its floor (3), of one
        hole or a pattern of them, of its axis too; a plain hole's its
        length (10). Holes of two depths, or a face that is no hole, have
        none. Shown with the depth sign (STC-06, STC-10, docs sec 10.12)"""
        for name, x in (("Bore", 20), ("Hole2", 8), ("Bore2", 8), ("Plain", 32)):
            if name.startswith("Bore"):
                self.cut("Cylinder", name, V(x, 10, 7), Radius=5, Height=3)
            else:
                self.cut("Cylinder", name, V(x, 10, 0), Radius=3, Height=10)
        self.doc.recompute()

        def around(x, r=5.5):
            def test(f):
                c = f.BoundBox.Center
                return (f.BoundBox.XLength < 2 * r and abs(c.x - x) < 1e-6
                        and abs(c.y - 10) < 1e-6)
            return test

        one = self.faces_where(around(20))
        two = one + self.faces_where(around(8))
        plain = self.faces_where(around(32))

        def depth(name, subs, kind="Integral"):
            f = self.add("TolerancedFeature", name + "F", References=[(self.solid, subs)],
                         Kind=kind)
            return self.add("Dimension", name, Type="Depth", Features=[f], Upper=0.1,
                            Lower=-0.1)

        dims = {"One": depth("One", one), "Two": depth("Two", two),
                "Axis": depth("Axis", one, "Axis"), "Plain": depth("Plain", plain)}
        self.recompute()
        for name, value in (("One", 3), ("Two", 3), ("Axis", 3), ("Plain", 10)):
            dim = dims[name]
            self.assertEqual(dim.Condition, "Ok", (name, dim.ConditionMessage))
            self.assertAlmostEqual(dim.Nominal.Value, value, 6, name)
        self.assertEqual(dims["One"].EquivalentText, "\u21a73.00 \u00b10.1")
        mixed = depth("Mixed", one + plain)
        flat = depth("Flat", [face_at(self.solid.Shape, BOTTOM)])
        self.doc.recompute()
        self.assertEqual(mixed.Condition, "Invalid")
        self.assertIn("one depth", mixed.ConditionMessage)
        self.assertEqual(flat.Condition, "Invalid")
        self.assertIn("a hole", flat.ConditionMessage)

    def test_blind_holes_from_either_side_are_two(self):
        """Two blind holes of one size on one axis, from the top and from
        the bottom ("2X", material between): each its own depth (3), not
        the two and the gap between as one (10)"""
        self.cut("Cylinder", "Down", V(32, 10, 7), Radius=2, Height=3)
        self.cut("Cylinder", "Up", V(32, 10, 0), Radius=2, Height=3)
        self.doc.recompute()

        def around(f):
            b = f.BoundBox
            return b.XLength < 4.5 and abs(b.Center.x - 32) < 1e-6 and abs(b.Center.y - 10) < 1e-6

        subs = self.faces_where(around)
        # Two walls and two floors (a wall may be split at its seam)
        self.assertGreaterEqual(len(subs), 4)
        f = self.add("TolerancedFeature", "BothF", References=[(self.solid, subs)])
        dim = self.add("Dimension", "Both", Type="Depth", Features=[f], Upper=0.1, Lower=-0.1)
        self.recompute()
        self.assertEqual(dim.Condition, "Ok", dim.ConditionMessage)
        self.assertAlmostEqual(dim.Nominal.Value, 3, 6)

    def test_a_depth_on_its_floor(self):
        """A depth given on a counterbore's floor alone (STC-06) is of the
        bore the floor closes: the wall that runs out of it, not the hole
        below it. Saved, reopened and edited, it still is: the bores are
        found from the part, not kept from the last recompute"""
        self.cut("Cylinder", "Bore", V(8, 10, 7), Radius=5, Height=3)
        self.cut("Cylinder", "Through", V(8, 10, 0), Radius=2, Height=10)
        self.doc.recompute()
        floor = [n for n in ("Face%d" % (i + 1) for i in range(len(self.solid.Shape.Faces)))
                 if self.solid.Shape.getElement(n).Surface.TypeId == "Part::GeomPlane"
                 and abs(self.solid.Shape.getElement(n).BoundBox.ZMin - 7) < 1e-6
                 and abs(self.solid.Shape.getElement(n).BoundBox.ZMax - 7) < 1e-6]
        self.assertTrue(floor)
        f = self.add("TolerancedFeature", "FloorF", References=[(self.solid, floor)])
        dim = self.add("Dimension", "Floor", Type="Depth", Features=[f], Upper=0.1, Lower=-0.1)
        self.recompute()
        self.assertEqual(dim.Condition, "Ok", dim.ConditionMessage)
        self.assertAlmostEqual(dim.Nominal.Value, 3, 6)
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "floor.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        dim = self.doc.getObject("Floor")
        dim.Upper = 0.2
        self.doc.recompute()
        self.assertEqual(dim.Condition, "Ok", dim.ConditionMessage)
        self.assertAlmostEqual(dim.Nominal.Value, 3, 6)

    def test_limits_stay_when_the_nominal_is_measured(self):
        """A dimension given as its limits keeps them: its deviations are
        about its nominal, which a file gives as the middle of the limits and
        the model measures elsewhere (10.00-10.05 on a model at 10)"""
        hole = self.feature(HOLE, kind="Axis")
        dim = self.add("Dimension", "Lim", Type="Diameter", Features=[hole], Limits=True)
        self.recompute()
        self.assertAlmostEqual(dim.Nominal.Value, 6, 6)
        # 6.00-6.05, and the hole made 6.02
        dim.Upper, dim.Lower = 0.05, 0.0
        self.hole.Radius = 3.01
        self.doc.recompute()
        self.assertAlmostEqual(dim.Nominal.Value, 6.02, 6)
        self.assertAlmostEqual(dim.Nominal.Value + dim.Upper.Value, 6.05, 6)
        self.assertAlmostEqual(dim.Nominal.Value + dim.Lower.Value, 6.0, 6)

    def test_datum_letters_past_z_and_a_feature_of_no_kind(self):
        """Past Z the letters go AA, AB, ... (Y14.5 7.11), without I, O, Q
        under ASME; a feature of a kind that is none, or on a face the solid
        has not, is refused, not made"""
        top = self.feature(TOP)
        asme = self.set.Standard.startswith("ASME")
        for i in range(23 if asme else 26):
            letter = PMI.nextDatumLetter(self.set)
            self.assertEqual(len(letter), 1)
            if asme:
                self.assertNotIn(letter, "IOQ")
            self.add("Datum", "D%d" % i, Letter=letter, Feature=top)
        self.assertEqual(PMI.nextDatumLetter(self.set), "AA")
        self.add("Datum", "DAA", Letter="AA", Feature=top)
        self.assertEqual(PMI.nextDatumLetter(self.set), "AB")
        before = len(self.doc.Objects)
        with self.assertRaises(Exception):
            PMI.feature(self.set, self.solid, ["Face1"], "NoSuchKind")
        with self.assertRaises(Exception):
            PMI.feature(self.set, self.solid, ["Face999"])
        self.assertEqual(len(self.doc.Objects), before)

    def test_a_fillet_radius(self):
        cyl = Part.makeCylinder(5, 10)
        top = [e for e in cyl.Edges if e.Curve.TypeId == "Part::GeomCircle"
               and abs(e.Vertexes[0].Point.z - 10) < 1e-9]
        part = self.doc.addObject("Part::Feature", "Filleted")
        part.Shape = cyl.makeFillet(1, top)
        faces = part.Shape.Faces
        torus = [i for i, f in enumerate(faces) if f.Surface.TypeId == "Part::GeomToroid"]
        self.assertEqual(len(torus), 1)
        pmi = self.doc.addObject("PMI::AnnotationSet", "FilletPMI")
        pmi.Part = part
        feat = self.doc.addObject("PMI::TolerancedFeature", "Fillet")
        pmi.addObject(feat)
        feat.References = [(part, ["Face%d" % (torus[0] + 1)])]
        dim = self.doc.addObject("PMI::Dimension", "R")
        pmi.addObject(dim)
        dim.Type, dim.Features = "Radius", [feat]
        self.recompute()
        self.assertAlmostEqual(dim.Nominal, 1, 9)

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
        self.add("Datum", "DatumA", Letter="A", Feature=self.feature(BOTTOM))
        self.add("Datum", "DatumB", Letter="B", Feature=hole)
        targets = [self.add("DatumTarget", "T", TargetType="Point", Number=i + 1,
                            Target=App.Placement(p, App.Rotation(V(1, 0, 0), 180)),
                            Feature=self.feature(p))
                   for i, p in enumerate((V(5, 5, 10), V(35, 5, 10), V(20, 15, 10)))]
        self.add("Datum", "DatumC", Letter="C", Targets=targets)
        self.set.Standard = "ASME Y14.5-2018"
        self.recompute()
        shapes = {n: self.doc.getObject(n).Shape.copy() for n in ("DatumA", "DatumB", "DatumC")}
        shapes.update({t.Name: t.Shape.copy() for t in targets})
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "pmi.FCStd")
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
        # The derived geometry is back before any recompute; the simulated
        # datums too, and the targets (transient, made again)
        self.assertFalse(hole.Shape.isNull())
        for n, shape in shapes.items():
            got = self.doc.getObject(n).Shape
            self.assertFalse(got.isNull(), n)
            self.assertEqual(got.ShapeType, shape.ShapeType, n)
            self.assertTrue(got.BoundBox.isInside(shape.BoundBox.Center), n)
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

    def test_zero_at_a_material_condition(self):
        drf = self.frame()
        at_mmc = self.tolerance("Position", self.feature(HOLE, kind="Axis"), ZoneShape="Diameter",
                                MaterialCondition="Maximum", DatumSystem=drf)
        at_mmc.Value = 0
        self.assertClean(at_mmc)
        plain = self.tolerance("Position", self.feature(HOLE, kind="Axis"), ZoneShape="Diameter",
                               DatumSystem=drf)
        plain.Value = 0
        self.assertRule(plain, "greater than zero")

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

    def test_a_composite_frame_is_drawn_as_one(self):
        """A composite frame's lower segments (ASME Y14.5 sec 10.5.1) are rows
        under its upper, flush, left edges in line, in its plane, with no
        symbol and no leader of their own: the upper's symbol cell goes on down
        them, closed under the last"""
        drf = self.frame()
        hole = self.feature(HOLE, kind="Axis")
        upper = self.tolerance("Position", hole, ZoneShape="Diameter", DatumSystem=drf,
                               ShowLeader=False)
        lowers = []
        for value in (0.02, 0.01):
            lower = self.tolerance("Position", hole, ZoneShape="Diameter", DatumSystem=drf,
                                   Composite=upper)
            lower.Value = value
            lowers.append(lower)
        for lower in lowers:
            self.assertClean(lower)
        self.assertClean(upper)
        inv = upper.Plane.inverse().toMatrix()

        def box(shape):
            s = shape.copy()
            s.transformShape(inv)
            return s.BoundBox

        h = upper.TextHeight.Value or 3.5
        rows = [box(upper.Presentation)] + [box(lower.Presentation) for lower in lowers]
        for above, below in zip(rows, rows[1:]):
            self.assertAlmostEqual(below.XMin, above.XMin, 6)
            self.assertAlmostEqual(below.YMax, above.YMin, 6)
            self.assertAlmostEqual(below.YMax - below.YMin, 2 * h, 6)
            self.assertAlmostEqual(below.ZLength, 0, 6)
        for lower in lowers:
            self.assertTrue(lower.Plane.Rotation.isSame(upper.Plane.Rotation, 1e-9))
        # Seen from below they turn as one, about the middle of all the rows:
        # each about its own, the upper read under its lowers
        mid = (rows[0].YMax + rows[-1].YMin) / 2
        for c in [upper] + lowers:
            self.assertEqual(len(c.UprightAt), 1, c.Name)
            pivot = inv.multVec(c.Plane.multVec(c.UprightAt[0]))
            self.assertAlmostEqual(pivot.y, mid, 6, c.Name)
        # No symbol below: the lower rows' strokes are lines alone
        for lower in lowers:
            kinds = {e.Curve.__class__.__name__ for e in lower.Presentation.Edges
                     if box(e).XMax <= rows[0].XMin + 2 * h + 1e-6}
            self.assertEqual(kinds, {"Line"})


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

    def test_what_each_reference_was_is_kept_by_its_index(self):
        """A reference resolved to nothing keeps its place in SavedGeometry:
        the shapes alone in a compound moved each one after it onto the
        reference before it"""
        solid = self.solid
        feat = self.add("TolerancedFeature", "Feature", Kind="Integral",
                        References=[(solid, ["Face999", face_at(solid.Shape, LEFT),
                                             face_at(solid.Shape, RIGHT)])])
        self.doc.recompute()
        saved = feat.SavedGeometry.childShapes()
        self.assertEqual(len(saved), 3)
        self.assertEqual(saved[0].ShapeType, "Compound")
        self.assertEqual(len(saved[0].childShapes()), 0)
        self.assertAlmostEqual(saved[1].CenterOfMass.x, 0, 6)
        self.assertAlmostEqual(saved[2].CenterOfMass.x, 40, 6)
        # Read again by the next recompute, each its own
        self.body.touch()
        self.doc.recompute()
        saved = feat.SavedGeometry.childShapes()
        self.assertEqual(len(saved), 3)
        self.assertAlmostEqual(saved[1].CenterOfMass.x, 0, 6)
        self.assertAlmostEqual(saved[2].CenterOfMass.x, 40, 6)

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

    def test_a_files_graphic_until_edited(self):
        # An imported callout is drawn as the file drew it (sec 5.1 step 3);
        # edited, it is drawn from what it says
        gt = self.add("GeometricTolerance", "GT", Characteristic="Flatness",
                      Feature=self.feature(TOP), Value=0.05)
        self.recompute()
        ours = gt.Presentation.copy()
        theirs = Part.makeCircle(7, V(0, 0, 30))
        gt.ImportedGraphic = theirs
        gt.UseImported = True
        self.recompute()
        self.assertTrue(gt.UseImported)
        box = gt.Presentation.BoundBox
        self.assertAlmostEqual(box.XLength, 14, 6)
        self.assertAlmostEqual(box.ZMin, 30, 6)
        # Moving it into a view, or a recompute, is no edit
        view = self.add("View", "Top", Standard="Top", AnnotationPlane=False)
        gt.Views = [view]
        self.recompute()
        self.assertTrue(gt.UseImported)
        # An edit is: drawn from what it says again, as before the import
        gt.Value = 0.1
        self.recompute()
        self.assertFalse(gt.UseImported)
        gt.Value = 0.05
        self.recompute()
        self.assertAlmostEqual(gt.Presentation.BoundBox.XLength, ours.BoundBox.XLength, 6)

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

    def test_what_turns_to_read_upright(self):
        # Presentation is what stays -- a leader, extension lines -- then
        # each part the view turns to read upright, about UprightAt in the
        # plane (sec 6.5): a frame whole, a dimension its value alone
        flat = self.add("GeometricTolerance", "Flat", Characteristic="Flatness",
                        Feature=self.feature(TOP), Value=0.05)
        datum = self.add("Datum", "DatumA", Letter="A", Feature=self.feature(BOTTOM))
        width = self.add("Dimension", "Width", Type="Width",
                         Features=[self.feature(LEFT, RIGHT)])
        self.recompute()

        def local(c, shape):
            inv = c.Plane.inverse()
            pts = [inv.multVec(v.Point) for v in shape.Vertexes]
            return (min(p.x for p in pts), max(p.x for p in pts),
                    min(p.y for p in pts), max(p.y for p in pts))

        for c in (flat, datum, width):
            parts = c.Presentation.childShapes()
            self.assertEqual(len(c.UprightAt), 1, c.Name)
            self.assertEqual(len(parts), 2, c.Name)
            # The leader, or the lines, stay
            self.assertTrue(parts[0].Edges, c.Name)
            x0, x1, y0, y1 = local(c, parts[1])
            at = c.UprightAt[0]
            self.assertAlmostEqual(at.z, 0, 9)
            self.assertTrue(x0 < at.x < x1 and y0 < at.y < y1, (c.Name, at, x0, x1, y0, y1))
        # The width's value only: narrower than the 40 between its lines
        x0, x1, _, _ = local(width, width.Presentation.childShapes()[1])
        self.assertLess(x1 - x0, 20)
        # A file's graphic does not turn
        flat.ImportedGraphic = Part.makeLine(V(0, 0, 20), V(10, 0, 20))
        flat.UseImported = True
        self.recompute()
        self.assertEqual(len(flat.UprightAt), 0)

    def test_a_linear_dimension_has_extension_and_dimension_lines(self):
        width = self.add("Dimension", "Width", Type="Width",
                         Features=[self.feature(LEFT, RIGHT)])
        dist = self.add("Dimension", "Dist", Type="Distance",
                        Features=[self.feature(LEFT), self.feature(HOLE)])
        self.recompute()

        def segments(dim):
            return [(e.Vertexes[0].Point, e.Vertexes[-1].Point)
                    for e in dim.Presentation.Edges if len(e.Vertexes) == 2]

        def across(dim, x):
            # An extension line: along y at x
            return any(abs(a.x - x) < 1e-6 and abs(b.x - x) < 1e-6 and abs(a.y - b.y) > 1
                       for a, b in segments(dim))

        def along(dim, x0, x1):
            # The dimension line: along x from x0 to x1
            return any(abs(a.y - b.y) < 1e-6 and abs(min(a.x, b.x) - x0) < 1e-6
                       and abs(max(a.x, b.x) - x1) < 1e-6 for a, b in segments(dim))

        # Read along x in a top view, beside the part, nothing over it
        rot = width.Plane.Rotation
        self.assertAlmostEqual(rot.multVec(V(1, 0, 0)).x, 1, 9)
        self.assertAlmostEqual(rot.multVec(V(0, 0, 1)).z, 1, 9)
        self.assertTrue(across(width, 0) and across(width, 40), segments(width))
        self.assertTrue(along(width, 0, 40), segments(width))
        self.assertGreater(min(v.Point.y for v in width.Presentation.Vertexes), 20)
        self.assertEqual(width.EquivalentText, "40.00")
        # To a hole: from its axis
        self.assertTrue(across(dist, 0) and across(dist, 20), segments(dist))
        self.assertTrue(along(dist, 0, 20), segments(dist))
        # Stacked: the second is further out than the first
        self.assertGreater(dist.Plane.Base.y, width.Plane.Base.y)

    def test_a_frame_stacked_under_a_size(self):
        # A hole's diameter and its position: one stack on one leader
        hole = self.feature(HOLE)
        dia = self.add("Dimension", "Dia", Type="Diameter", Features=[hole])
        pos = self.add("GeometricTolerance", "Pos", Characteristic="Position", Feature=hole,
                       ZoneShape="Diameter", Value=0.1)
        self.recompute()
        h = dia.TextHeight.Value

        def reach(c):
            return min(hole.Shape.distToShape(Part.Vertex(v.Point))[0]
                       for v in c.Presentation.Vertexes)

        self.assertLess(reach(pos), 0.1 * h + 1e-6)
        pos.AttachedTo = dia
        self.recompute()
        self.assertTrue(pos.Plane.Rotation.isSame(dia.Plane.Rotation, 1e-9))
        # Below the diameter's text, its left edge in line with it
        inv = dia.Plane.inverse()
        local = [inv.multVec(v.Point) for v in pos.Presentation.Vertexes]
        self.assertLess(max(p.y for p in local), 0)
        self.assertAlmostEqual(min(p.x for p in local), 0, delta=0.5 * dia.TextHeight.Value)
        # No leader of its own
        self.assertGreater(reach(pos), h)
        # Moved with it
        dia.Plane = dia.Plane.multiply(App.Placement(V(10, 5, 0), App.Rotation()))
        self.recompute()
        moved = [dia.Plane.inverse().multVec(v.Point) for v in pos.Presentation.Vertexes]
        for a, b in zip(local, moved):
            self.assertTrue(a.isEqual(b, 1e-6), (a, b))
        # A loop is no stack, nor is anything but a callout
        dia.AttachedTo = pos
        self.assertIsNone(dia.AttachedTo)
        dia.AttachedTo = hole
        self.assertIsNone(dia.AttachedTo)
        self.recompute()
        self.assertEqual(dia.Condition, "Ok")

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
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "pmi-look.FCStd")
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
        alone = App.Vector(a.Plane.Base)
        # The first moved elsewhere (where each lands is kept through a
        # recompute only, and while its own and its feature's stay as they
        # were): the second is alone, down its slot
        a.Feature = self.feature(LEFT)
        self.recompute()
        self.assertGreater((b.Plane.Base - a.Plane.Base).Length, 12 * h)
        self.assertAlmostEqual((b.Plane.Base - alone).Length, 0, 6)
        # Back, and turned off: stacked again, then alone again
        a.Feature = top
        self.recompute()
        self.assertAlmostEqual((b.Plane.Base - a.Plane.Base).Length, 3 * h, 6)
        a.AutoPlace = False
        self.recompute()
        self.assertAlmostEqual((b.Plane.Base - alone).Length, 0, 6)

    def test_an_axis_leader_ends_where_the_axis_leaves_the_part(self):
        gt = self.add("GeometricTolerance", "GT", Characteristic="Position",
                      Feature=self.feature(HOLE, kind="Axis"), ZoneShape="Diameter", Value=0.1)
        self.recompute()
        h = gt.TextHeight.Value
        # Looked at along the axis, from above; the leader's tip at the top end
        self.assertAlmostEqual(gt.Plane.Rotation.multVec(V(0, 0, 1)).z, 1, 9)
        tip = V(20, 10, 10 + 0.05 * h)
        self.assertLess(min((v.Point - tip).Length for v in gt.Presentation.Vertexes), 1e-6)

    def test_a_holes_size_is_read_where_it_is_a_circle(self):
        # Looking down the hole, its leader to the rim -- not edge on from
        # the top, as a plane facing out of the cylinder would be
        dia = self.add("Dimension", "Dia", Type="Diameter", Features=[self.feature(HOLE)])
        self.recompute()
        self.assertTrue(dia.Plane.Rotation.multVec(V(0, 0, 1)).isEqual(V(0, 0, 1), 1e-9),
                        dia.Plane)
        self.assertAlmostEqual(dia.Plane.Base.z, 10, 0)


def edge_at(shape, *points):
    """The edge through all the points"""
    for i, edge in enumerate(shape.Edges):
        if all(edge.distToShape(Part.Vertex(p))[0] < 1e-6 for p in points):
            return "Edge%d" % (i + 1)
    raise AssertionError("no edge through %s" % (points,))


def vertex_at(shape, point):
    for i, v in enumerate(shape.Vertexes):
        if v.Point.distanceToPoint(point) < 1e-6:
            return "Vertex%d" % (i + 1)
    raise AssertionError("no vertex at %s" % point)


# The top's front and back edges, the hole's top rim, a corner
TOP_FRONT = (V(5, 0, 10), V(35, 0, 10))
TOP_BACK = (V(5, 20, 10), V(35, 20, 10))
RIM = (V(23, 10, 10), V(17, 10, 10))
CORNER = V(0, 0, 0)


class TestEdges(Base):
    """Edges and points referred to (docs/GDandT.md sec 10.14): a file's
    dimension between two edges, a line profile, a hole's rim"""

    def element(self, sub, kind="Integral", solid=None):
        solid = solid or self.solid
        return self.add("TolerancedFeature", "Feature", References=[(solid, [sub])], Kind=kind)

    def edge(self, points, **kw):
        return self.element(edge_at(self.solid.Shape, *points), **kw)

    def test_an_edge_and_a_point_are_features(self):
        edge = self.edge(TOP_FRONT)
        rim = self.edge(RIM)
        corner = self.element(vertex_at(self.solid.Shape, CORNER))
        self.recompute()
        for f in (edge, rim, corner):
            self.assertEqual(f.Condition, "Ok", f.ConditionMessage)
        self.assertEqual(edge.Shape.ShapeType, "Edge")
        self.assertAlmostEqual(edge.Shape.Length, 40, 6)
        self.assertEqual(corner.Shape.ShapeType, "Vertex")
        self.assertFalse(rim.IsSize)
        # A rim's axis and centre
        axis = self.edge(RIM, kind="Axis")
        centre = self.edge(RIM, kind="CentrePoint")
        self.recompute()
        self.assertAlmostEqual(axis.Shape.Edges[0].Curve.Direction.cross(V(0, 0, 1)).Length, 0, 9)
        self.assertTrue(centre.Shape.Vertexes[0].Point.isEqual(V(20, 10, 10), 1e-6))

    def test_dimensions_on_edges_and_points(self):
        across = self.add("Dimension", "Across", Type="Distance",
                          Features=[self.edge(TOP_FRONT), self.edge(TOP_BACK)])
        corner = self.element(vertex_at(self.solid.Shape, CORNER))
        to_end = self.add("Dimension", "ToEnd", Type="Distance",
                          Features=[corner, self.feature(RIGHT)])
        down = self.add("Dimension", "Down", Type="Distance",
                        Features=[self.edge(TOP_FRONT), self.feature(BOTTOM)])
        rim = self.add("Dimension", "Rim", Type="Diameter", Features=[self.edge(RIM)])
        located = self.add("Dimension", "Located", Type="Distance",
                           Features=[self.edge(RIM), self.feature(LEFT)])
        self.recompute()
        for dim, value in ((across, 20), (to_end, 40), (down, 10), (rim, 6), (located, 20)):
            self.assertEqual(dim.Condition, "Ok", (dim.Name, dim.ConditionMessage))
            self.assertAlmostEqual(dim.Nominal.Value, value, 6, dim.Name)
        # An edge has no diameter
        bad = self.add("Dimension", "Bad", Type="Diameter", Features=[self.edge(TOP_FRONT)])
        self.doc.recompute()
        self.assertEqual(bad.Condition, "Invalid")
        self.assertIn("circular edge", bad.ConditionMessage)

    def test_a_rim_and_an_edge_written_as_splines(self):
        """A circle and a line a file wrote as B-splines (FTC-10's hole rims,
        cubic, 1.7e-4 off the circle, the edge's tolerance saying so): what
        they are, within it -- a distance to them measured, a rim's diameter"""
        box = Part.makeBox(40, 20, 10)

        def spline(points):
            curve = Part.BSplineCurve()
            curve.approximate(Points=points, DegMax=3, Tolerance=1e-4)
            edge = curve.toShape()
            edge.fixTolerance(2e-4)
            return edge

        rim = spline([V(20 + 3 * math.cos(a), 10 + 3 * math.sin(a), 30)
                      for a in (math.pi * k / 20 for k in range(21))])
        line = spline([V(4 * k, 0, 25) for k in range(11)])
        self.assertEqual(rim.Curve.TypeId, "Part::GeomBSplineCurve")
        part = self.doc.addObject("Part::Feature", "Splined")
        part.Shape = Part.Compound([box, rim, line])
        self.doc.recompute()
        shape = part.Shape
        rim_at = edge_at(shape, V(23, 10, 30))
        line_at = edge_at(shape, V(20, 0, 25))
        bottom = self.add("TolerancedFeature", "Bottom",
                          References=[(part, [face_at(shape, BOTTOM)])])
        to_rim = self.add("Dimension", "ToRim", Type="Distance",
                          Features=[self.element(rim_at, solid=part), bottom])
        to_line = self.add("Dimension", "ToLine", Type="Distance",
                           Features=[self.element(line_at, solid=part), bottom])
        across = self.add("Dimension", "Across", Type="Diameter",
                          Features=[self.element(rim_at, solid=part)])
        self.recompute()
        for dim, value in ((to_rim, 30), (to_line, 25), (across, 6)):
            self.assertEqual(dim.Condition, "Ok", (dim.Name, dim.ConditionMessage))
            self.assertAlmostEqual(dim.Nominal.Value, value, 3, dim.Name)

    def test_tolerances_on_edges(self):
        edge = self.edge(TOP_FRONT)
        rim = self.edge(RIM)
        self.recompute()
        self.assertEqual(PMI.advise(edge)["characteristics"][0], "Straightness")
        self.assertIn("Circularity", PMI.advise(rim)["characteristics"])
        self.assertIn("Diameter", PMI.advise(rim, "Position")["zoneShapes"])
        straight = self.add("GeometricTolerance", "Straight", Characteristic="Straightness",
                            Feature=edge, Value=0.02)
        for letter, point in (("A", BOTTOM), ("B", LEFT), ("C", FRONT)):
            self.add("Datum", "Datum" + letter, Letter=letter, Feature=self.feature(point))
        drf = self.add("DatumReferenceFrame", "DRF", Compartments=["A", "B", "C"])
        pos = self.add("GeometricTolerance", "Pos", Characteristic="Position", Feature=rim,
                       Value=0.1, ZoneShape="Diameter", DatumSystem=drf)
        self.recompute()
        for t in (straight, pos):
            self.assertEqual(self.issues(t, "Error"), [], t.Name)
            self.assertEqual(t.Condition, "Ok", t.ConditionMessage)
            self.assertFalse(t.Presentation.isNull(), t.Name)
        # A line element read side on, from the top, its leader to its
        # middle -- not along itself, to its end, as an axis is. The plane
        # sits a twentieth of the text height off the part
        normal = straight.Plane.Rotation.multVec(V(0, 0, 1))
        self.assertAlmostEqual(abs(normal.z), 1, 6)
        lift = 0.05 * straight.TextHeight + 1e-6
        middle = Part.Vertex(V(20, 0, 10))
        self.assertLess(straight.Presentation.distToShape(middle)[0], lift)

    def test_a_pick_on_an_edge_or_a_vertex(self):
        tip = self.body.Tip
        sub = "%s.%s" % (tip.Name, edge_at(tip.Shape, *TOP_FRONT))
        solid, name = PMI.solidReference(self.body, sub)
        self.assertEqual(solid, self.solid)
        self.assertTrue(name.startswith("Edge"), name)
        self.assertEqual(name, edge_at(self.solid.Shape, *TOP_FRONT))
        sub = "%s.%s" % (tip.Name, vertex_at(tip.Shape, CORNER))
        solid, name = PMI.solidReference(self.body, sub)
        self.assertEqual(name, vertex_at(self.solid.Shape, CORNER))

    def test_an_edge_elsewhere_left_alone_and_found_again(self):
        edge = self.edge(TOP_FRONT)
        self.recompute()
        second = self.doc.addObject("PartDesign::SubtractiveCylinder", "Second")
        self.body.addObject(second)
        second.Radius, second.Height = 2, 10
        second.Placement = App.Placement(V(32, 10, 0), App.Rotation())
        self.recompute()
        self.assertIn(edge.Condition, ("Ok", "Remapped"), edge.ConditionMessage)
        self.assertNotIn("geometry", edge.ConditionMessage)
        self.assertEqual(edge.Shape.ShapeType, "Edge")
        self.assertAlmostEqual(edge.Shape.Length, 40, 6)

    def test_an_edge_of_a_rebuilt_feature_remaps_by_geometry(self):
        lid = self.doc.addObject("PartDesign::AdditiveBox", "Lid")
        self.body.addObject(lid)
        lid.Placement = App.Placement(V(0, 0, 10), App.Rotation())
        self.recompute()
        edge = self.edge((V(2, 0, 20), V(8, 0, 20)))
        self.recompute()
        self.body.removeObject(lid)
        self.doc.removeObject(lid.Name)
        again = self.doc.addObject("PartDesign::AdditiveBox", "Lid2")
        self.body.addObject(again)
        again.Placement = App.Placement(V(0, 0, 10), App.Rotation())
        self.doc.recompute()
        self.assertEqual(edge.Condition, "Remapped", edge.ConditionMessage)
        self.assertIn("geometry", edge.ConditionMessage)
        self.assertEqual(edge.Shape.ShapeType, "Edge")
        self.assertAlmostEqual(edge.Shape.CenterOfMass.z, 20, 6)
        self.assertAlmostEqual(edge.Shape.CenterOfMass.y, 0, 6)

    def test_saved_and_reopened(self):
        rim = self.edge(RIM)
        dim = self.add("Dimension", "Dia", Type="Diameter", Features=[rim])
        corner = self.element(vertex_at(self.solid.Shape, CORNER))
        self.recompute()
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "pmi-edges.FCStd")
        names = rim.Name, corner.Name
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.set = self.doc.getObject("PMI")
        rim, corner = (self.doc.getObject(n) for n in names)
        # Back before a recompute, from what was saved
        self.assertEqual(rim.Shape.ShapeType, "Edge")
        self.assertEqual(corner.Shape.ShapeType, "Vertex")
        self.assertAlmostEqual(self.doc.getObject("Dia").Nominal.Value, 6, 6)
        self.recompute()
        self.assertEqual(rim.Condition, "Ok", rim.ConditionMessage)


class TestViewObjects(Base):
    """A view's Objects (docs/GDandT.md sec 6.3): what shows while it is
    active"""

    def test_a_view_lists_the_body_it_is_in(self):
        # In the set inside the Body it names, with no loop
        view = self.add("View", "Bodies", Standard="Isometric")
        view.Objects = [self.body]
        self.recompute()
        self.assertEqual(view.Condition, "Ok")
        self.assertFalse("Touched" in self.body.State or "Touched" in view.State)
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "pmi-view-objects.FCStd")
        body = self.body.Name
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.assertEqual([o.Name for o in self.doc.getObject("Bodies").Objects], [body])


class TestDatumTarget(Base):
    """Datum targets (docs/GDandT.md sec 4.3): a datum established from
    points, lines and areas of the part rather than from a whole face"""

    def target(self, kind, at, number, **props):
        return self.add("DatumTarget", "T", TargetType=kind, Number=number,
                        Target=App.Placement(at, App.Rotation()), **props)

    def test_a_datum_from_three_points(self):
        # Three points on the bottom face: datum A is theirs, no feature
        points = [V(5, 5, 0), V(35, 5, 0), V(20, 15, 0)]
        targets = [self.target("Point", p, i + 1, Feature=self.feature(p))
                   for i, p in enumerate(points)]
        for t in targets:
            t.Target = App.Placement(t.Target.Base, App.Rotation(V(1, 0, 0), 180))
        datum = self.add("Datum", "A", Letter="A", Targets=targets)
        self.recompute()
        self.assertEqual(datum.Condition, "Ok", datum.ConditionMessage)
        self.assertEqual(len(datum.Shape.Vertexes), 3)
        self.assertTrue(datum.Presentation.isNull())
        for t in targets:
            self.assertEqual(t.Condition, "Ok", t.ConditionMessage)
            self.assertEqual(t.EquivalentText, "A%d" % t.Number)
            # An X on the part at the point, and the symbol with its leader
            near = min(e.distToShape(Part.Vertex(t.Target.Base))[0]
                       for e in t.Presentation.Edges)
            self.assertLess(near, 1e-6)
        # A frame citing A finds it on the solid
        drf = self.add("DatumReferenceFrame", "DRF", Compartments=["A"])
        gt = self.add("GeometricTolerance", "GT", Characteristic="Parallelism",
                      Feature=self.feature(TOP), Value=0.1, DatumSystem=drf)
        self.recompute()
        self.assertEqual(self.issues(gt), [])

    def test_the_letter_is_the_datums(self):
        t = self.target("Point", V(5, 5, 0), 2)
        datum = self.add("Datum", "B", Letter="B", Targets=[t])
        self.recompute()
        self.assertEqual(t.EquivalentText, "B2")
        datum.Letter = "C"
        self.recompute()
        self.assertEqual(t.EquivalentText, "C2")

    def test_a_circular_curve_is_a_circle_with_no_area(self):
        """AP242's 'circular curve' target (NIST FTC-06): a circle of its
        diameter that is a curve, no face; drawn as the circle alone, no
        hatching within"""
        curve = self.target("CircularCurve", V(10, 10, 10), 1, Length=6)
        circ = self.target("Circle", V(30, 10, 10), 2, Length=6)
        self.add("Datum", "A", Letter="A", Targets=[curve, circ])
        self.recompute()
        self.assertEqual(curve.Condition, "Ok", curve.ConditionMessage)
        self.assertEqual(curve.EquivalentText, "\u2300" + "6 A1")
        self.assertEqual(curve.Shape.ShapeType, "Edge")
        self.assertAlmostEqual(curve.Shape.Length, math.pi * 6, 6)
        self.assertEqual(len(curve.Shape.Faces), 0)

        def on_part(t):
            return [e for e in t.Presentation.Edges
                    if abs(e.BoundBox.ZMin - 10) < 1e-6 and abs(e.BoundBox.ZMax - 10) < 1e-6]

        # The circle's dashes only: a circle's area has its hatching besides
        self.assertTrue(on_part(curve))
        self.assertLess(len(on_part(curve)), len(on_part(circ)))
        for e in on_part(curve):
            for v in e.Vertexes:
                self.assertAlmostEqual((v.Point - V(10, 10, 10)).Length, 3, 6)
        # No diameter, no target: as a circle's
        curve.Length = 0
        self.doc.recompute()
        self.assertEqual(curve.Condition, "Invalid")
        self.assertIn("a circle target needs its diameter", curve.ConditionMessage)

    def test_areas_carry_their_size(self):
        rect = self.target("Rectangle", V(20, 10, 10), 1, Length=8, Width=4)
        circ = self.target("Circle", V(10, 10, 10), 2, Length=6)
        line = self.target("Line", V(30, 10, 10), 3, Length=5)
        self.add("Datum", "A", Letter="A", Targets=[rect, circ, line])
        self.recompute()
        self.assertEqual(rect.EquivalentText, "8X4 A1")
        self.assertEqual(circ.EquivalentText, "\u2300" + "6 A2")
        self.assertEqual(line.EquivalentText, "A3")
        self.assertEqual(rect.Shape.ShapeType, "Face")
        self.assertAlmostEqual(rect.Shape.Area, 32, 6)
        self.assertAlmostEqual(circ.Shape.Area, math.pi * 9, 6)
        self.assertAlmostEqual(line.Shape.Length, 5, 6)
        # Drawn on the part, in the face: the outline and the hatching within
        # the area, the phantom line along the line
        inside = {
            rect.Name: lambda p: abs(p.x - 20) <= 4 + 1e-6 and abs(p.y - 10) <= 2 + 1e-6,
            circ.Name: lambda p: (p - V(10, 10, 10)).Length <= 3 + 1e-6,
            line.Name: lambda p: abs(p.x - 30) <= 2.5 + 1e-6 and abs(p.y - 10) < 1e-6,
        }
        for t in (rect, circ, line):
            marks = [e for e in t.Presentation.Edges
                     if abs(e.BoundBox.ZMin - 10) < 1e-6 and abs(e.BoundBox.ZMax - 10) < 1e-6]
            self.assertGreaterEqual(len(marks), 2, t.Name)
            for e in marks:
                for v in e.Vertexes:
                    self.assertTrue(inside[t.Name](v.Point), (t.Name, v.Point))

    def test_rules(self):
        lone = self.target("Point", V(5, 5, 0), 1)
        line = self.target("Line", V(5, 5, 0), 2)
        rect = self.target("Rectangle", V(5, 5, 0), 3, Length=4)
        area = self.target("Area", V(5, 5, 0), 4)
        self.add("Datum", "A", Letter="A", Targets=[line, rect, area])
        self.doc.recompute()
        self.assertTrue([m for m in self.issues(lone) if "Targets" in m])
        self.assertTrue([m for m in self.issues(line) if "length" in m])
        self.assertTrue([m for m in self.issues(rect) if "width" in m])
        self.assertTrue([m for m in self.issues(area) if "its faces" in m])
        for t in (lone, line, rect, area):
            self.assertEqual(t.Condition, "Invalid", t.Name)
        empty = self.add("Datum", "B", Letter="B")
        self.doc.recompute()
        self.assertTrue([m for m in self.issues(empty) if "feature or datum targets" in m])

    def test_saved_and_restored(self):
        t = self.target("Circle", V(10, 10, 10), 1, Length=6, Feature=self.feature(TOP))
        self.add("Datum", "A", Letter="A", Targets=[t])
        self.recompute()
        name = t.Name
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "targets.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.doc.recompute()
        t = self.doc.getObject(name)
        datum = [o for o in self.doc.Objects if o.TypeId == "PMI::Datum"][0]
        self.assertEqual(datum.Targets, [t])
        self.assertEqual(t.TargetType, "Circle")
        self.assertEqual(t.EquivalentText, "\u2300" + "6 A1")
        self.assertEqual(t.Condition, "Ok", t.ConditionMessage)

class TestView(Base):
    """PMI views: a camera, its annotations, their plane (docs/GDandT.md sec 6.3)"""

    def z(self, placement):
        return placement.Rotation.multVec(V(0, 0, 1))

    def test_standard_views(self):
        for name, z, up in (("Top", V(0, 0, 1), V(0, 1, 0)),
                            ("Front", V(0, -1, 0), V(0, 0, 1)),
                            ("Right", V(1, 0, 0), V(0, 0, 1))):
            view = self.add("View", name, Standard=name)
            self.assertTrue(self.z(view.Camera).isEqual(z, 1e-9), (name, self.z(view.Camera)))
            self.assertTrue(view.Camera.Rotation.multVec(V(0, 1, 0)).isEqual(up, 1e-9), name)
        iso = self.add("View", "Iso", Standard="Isometric")
        d = 1 / math.sqrt(3)
        self.assertTrue(self.z(iso.Camera).isEqual(V(d, -d, d), 1e-9), self.z(iso.Camera))
        # Turned by hand, it is no longer the standard one
        iso.Camera = App.Placement(V(), App.Rotation(V(0, 0, 1), 10))
        self.assertEqual(iso.Standard, "Custom")
        self.recompute()
        self.assertEqual(iso.Condition, "Ok")

    def test_a_callout_in_a_view_takes_its_plane(self):
        top = self.add("View", "Top", Standard="Top")
        front = self.feature(FRONT)
        gt = self.add("GeometricTolerance", "GT", Characteristic="Flatness", Feature=front,
                      Value=0.05)
        self.recompute()
        # Alone: the face's own plane
        self.assertTrue(self.z(gt.Plane).isEqual(V(0, -1, 0), 1e-9), self.z(gt.Plane))
        gt.Views = [top]
        self.recompute()
        self.assertTrue(self.z(gt.Plane).isEqual(V(0, 0, 1), 1e-9), self.z(gt.Plane))
        self.assertIn(gt, top.InList)
        # The view turned: laid out again
        top.Standard = "Right"
        self.recompute()
        self.assertTrue(self.z(gt.Plane).isEqual(V(1, 0, 0), 1e-9), self.z(gt.Plane))
        # A view without its plane only shows it
        top.AnnotationPlane = False
        self.recompute()
        self.assertTrue(self.z(gt.Plane).isEqual(V(0, -1, 0), 1e-9), self.z(gt.Plane))

    def test_a_width_in_a_view(self):
        front = self.add("View", "Front", Standard="Front")
        right = self.add("View", "Right", Standard="Right")
        width = self.add("Dimension", "Width", Type="Width",
                         Features=[self.feature(LEFT, RIGHT)], Views=[front])
        self.recompute()
        # Measured along x, which the front view holds
        self.assertTrue(self.z(width.Plane).isEqual(V(0, -1, 0), 1e-9), self.z(width.Plane))
        # The right view looks along it: the width keeps its own choice
        width.Views = [right]
        self.recompute()
        self.assertTrue(self.z(width.Plane).isEqual(V(0, 0, 1), 1e-9), self.z(width.Plane))


class TestQuery(Base):
    """What applies to a face (docs/GDandT.md sec 6.5; upstream's query)"""

    def setUp(self):
        super().setUp()
        self.a = self.add("Datum", "DatumA", Letter="A", Feature=self.feature(BOTTOM))
        self.b = self.add("Datum", "DatumB", Letter="B", Feature=self.feature(LEFT))
        self.flat = self.add("GeometricTolerance", "Flatness", Characteristic="Flatness",
                             Feature=self.feature(TOP), Value=0.05)
        drf = self.add("DatumReferenceFrame", "DRF", Compartments=["A", "B"])
        self.pos = self.add("GeometricTolerance", "Position", Characteristic="Position",
                            Feature=self.feature(HOLE, kind="Axis"), Value=0.1,
                            ZoneShape="Diameter", DatumSystem=drf)
        self.width = self.add("Dimension", "Width", Type="Width",
                              Features=[self.feature(LEFT, RIGHT)])
        self.recompute()

    def query(self, point):
        return [(c.Name, how) for c, how in PMI.query(self.solid, face_at(self.solid.Shape, point))]

    def test_what_is_on_a_face(self):
        self.assertEqual(self.query(TOP), [("Flatness", "on")])
        self.assertEqual(self.query(HOLE), [("Position", "on")])
        self.assertEqual(self.query(BACK), [])

    def test_a_datum_face_lists_what_cites_it(self):
        self.assertEqual(self.query(BOTTOM), [("DatumA", "on"), ("Position", "datum A")])
        self.assertEqual(self.query(LEFT),
                         [("DatumB", "on"), ("Width", "on"), ("Position", "datum B")])
        # Each once: a dimension on two faces, asked of the other
        self.assertEqual(self.query(RIGHT), [("Width", "on")])

    def test_a_pick_on_the_body(self):
        # As a click gives it: the Body's tip feature and its face
        sub = "%s.%s" % (self.hole.Name, face_at(self.hole.Shape, TOP))
        self.assertEqual([(c.Name, how) for c, how in PMI.query(self.body, sub)],
                         [("Flatness", "on")])
        self.assertEqual(PMI.query(self.body, "%s.Edge1" % self.hole.Name), [])
        with self.assertRaises(ValueError):
            PMI.query(self.body, "%s.Edge9999" % self.hole.Name)


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
        # An edge or a vertex too (sec 10.14); not what is not there
        for kind in ("Edge", "Vertex"):
            solid, sub = PMI.solidReference(self.body, "%s.%s1" % (self.hole.Name, kind))
            self.assertEqual(solid, self.solid)
            self.assertTrue(sub.startswith(kind), sub)
        with self.assertRaises(ValueError):
            PMI.solidReference(self.body, "%s.Edge9999" % self.hole.Name)

    def test_a_pick_on_a_moved_body(self):
        self.body.Placement = App.Placement(V(100, 0, 0), App.Rotation(V(0, 0, 1), 30))
        self.recompute()
        name = face_at(self.hole.Shape, TOP)
        solid, face = PMI.solidReference(self.body, "%s.%s" % (self.hole.Name, name))
        self.assertEqual((solid, face), (self.solid, face_at(self.solid.Shape, TOP)))

    def test_a_callout_is_stacked_under_the_last_on_its_feature(self):
        hole = self.feature(HOLE)
        pos = self.add("GeometricTolerance", "Pos", Characteristic="Position", Feature=hole,
                       ZoneShape="Diameter", Value=0.1)
        self.assertIsNone(PMI.stackUnder(pos))
        dia = self.add("Dimension", "Dia", Type="Diameter", Features=[hole])
        self.assertEqual(PMI.stackUnder(dia), pos)
        dia.AttachedTo = pos
        note = self.add("Note", "Note", Text=["THRU"], Feature=hole)
        self.assertEqual(PMI.stackUnder(note), dia)
        # Not a width or a distance: they have their own lines
        left = self.feature(LEFT)
        flat = self.add("GeometricTolerance", "Flat", Characteristic="Flatness", Feature=left,
                        Value=0.05)
        dist = self.add("Dimension", "Dist", Type="Distance", Features=[left, hole])
        self.assertIsNone(PMI.stackUnder(dist))
        self.assertIsNone(PMI.stackUnder(flat))
        with self.assertRaises(TypeError):
            PMI.stackUnder(hole)

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
