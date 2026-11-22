# SPDX-License-Identifier: LGPL-2.1-or-later
"""The GD&T of a STEP file, read (docs/GDandT.md sec 5.1, P2) and written
(sec 5.2, P3).

NIST's CTC-01 (AP242 edition 1, from the NIST MBE PMI Validation and
Conformance Testing project, used without restriction) imported: its
datums, tolerances and dimensions become annotations beside the part, on
its faces, placed where the file places them.

The rest of the NIST set is read too when FC_PMI_NIST names its directory
(the 'NIST-PMI-STEP-Files' of nist.gov/document/nist-pmi-step-files): each
file imports, and nothing it makes is invalid.
"""

import collections
import glob
import os
import re
import tempfile
import unittest

import FreeCAD as App
import Import
import Part
import PMI  # noqa: F401 -- registers the pass

V = App.Vector
HERE = os.path.dirname(os.path.abspath(__file__))
CTC01 = os.path.join(HERE, "data", "nist_ctc_01_asme1_ap242-e1.stp")


def objects(doc, kind):
    return [o for o in doc.Objects if o.TypeId == "PMI::" + kind]


class Base(unittest.TestCase):
    def read(self, path):
        doc = App.newDocument("PMIStep")
        self.addCleanup(App.closeDocument, doc.Name)
        Import.insert(path, doc.Name)
        doc.recompute()
        return doc

    def assertSameGdt(self, a, b):
        """What two documents read of GD&T is the same: the datums, the
        tolerances with their frames, the dimensions (a nominal measured on
        the model again, to 1e-3), the datum targets and the notes"""

        def gdt(d):
            letters = sorted(x.Letter for x in objects(d, "Datum"))
            tols = sorted((gt.Characteristic, round(gt.Value, 6),
                           list(gt.DatumSystem.Compartments) if gt.DatumSystem else [],
                           round(gt.ProjectedLength.Value, 6))
                          for gt in objects(d, "GeometricTolerance"))
            dims = sorted((x.Type, round(x.Upper, 6), round(x.Lower, 6), x.Limits, x.Reference,
                           x.Nominal.Value) for x in objects(d, "Dimension"))
            targets = sorted((t.TargetType, t.Number) for t in objects(d, "DatumTarget"))
            return letters, tols, dims, targets, len(objects(d, "Note"))

        ga, gb = gdt(a), gdt(b)
        self.assertEqual(ga[0], gb[0])
        self.assertEqual(ga[1], gb[1])
        self.assertEqual([x[:5] for x in ga[2]], [x[:5] for x in gb[2]])
        for x, y in zip(ga[2], gb[2]):
            self.assertAlmostEqual(x[5], y[5], delta=1e-3)
        self.assertEqual(ga[3], gb[3])
        self.assertEqual(ga[4], gb[4])

    def assertSameViews(self, a, b):
        """Two documents' saved views are the same: named the same, the
        camera turned the same and looking along the same line, the same
        height, and showing the same kinds of callout"""

        def views(d):
            # A view lists presentations: a callout drawing nothing (an
            # invalid one) is in none in the file
            out = []
            for v in objects(d, "View"):
                shown = sorted(o.TypeId for o in d.Objects
                               if hasattr(o, "Views") and v in o.Views
                               and not o.Presentation.isNull())
                out.append((v.Label, v, shown))
            return sorted(out, key=lambda x: x[0])

        va, vb = views(a), views(b)
        self.assertEqual([x[0] for x in va], [x[0] for x in vb])
        for (name, x, sx), (_, y, sy) in zip(va, vb):
            for axis in (V(1, 0, 0), V(0, 1, 0), V(0, 0, 1)):
                self.assertLess((x.Camera.Rotation.multVec(axis)
                                 - y.Camera.Rotation.multVec(axis)).Length, 1e-9, name)
            z = x.Camera.Rotation.multVec(V(0, 0, 1))
            off = y.Camera.Base - x.Camera.Base
            self.assertLess((off - z * off.dot(z)).Length, 1e-6, name)
            self.assertAlmostEqual(x.Height.Value, y.Height.Value, delta=1e-6, msg=name)
            self.assertEqual(x.Perspective, y.Perspective, name)
            self.assertEqual(sx, sy, name)
            self.assertEqual(x.Clip, y.Clip, name)
            if x.Clip:
                n = x.ClipPlane.Rotation.multVec(V(0, 0, 1))
                self.assertLess((y.ClipPlane.Rotation.multVec(V(0, 0, 1)) - n).Length, 1e-9,
                                name)
                self.assertLess(abs((y.ClipPlane.Base - x.ClipPlane.Base).dot(n)), 1e-6, name)

    def write_and_read(self, doc):
        parts = []
        for s in objects(doc, "AnnotationSet"):
            if s.Part and s.Part not in parts:
                parts.append(s.Part)
        # And what their features are on beside them: supplemental geometry
        for f in objects(doc, "TolerancedFeature"):
            for o, _ in f.References:
                if o not in parts:
                    parts.append(o)
        path = self.written = os.path.join(tempfile.mkdtemp(prefix="pmi-roundtrip-"),
                                           "again.step")
        Import.export(parts, path)
        return self.read(path)

    def sound(self, doc):
        """What is wrong with what the import made: invalid or broken"""
        bad = []
        for o in doc.Objects:
            if getattr(o, "Condition", None) in ("Invalid", "Broken"):
                bad.append("%s %s: %s" % (o.Name, o.Condition, o.ConditionMessage))
        return bad


class TestStepImport(Base):
    def setUp(self):
        self.doc = self.read(CTC01)

    def test_it_reads_the_gdt(self):
        letters = sorted(d.Letter for d in objects(self.doc, "Datum"))
        self.assertEqual(letters, EXPECTED["datums"])
        tols = []
        for gt in objects(self.doc, "GeometricTolerance"):
            frame = list(gt.DatumSystem.Compartments) if gt.DatumSystem else []
            tols.append((gt.Characteristic, round(gt.Value, 6), frame))
        self.assertEqual(sorted(tols), sorted(EXPECTED["tolerances"]))
        dims = []
        for d in objects(self.doc, "Dimension"):
            dims.append((d.Type, round(d.Nominal, 4), round(d.Upper, 6), round(d.Lower, 6)))
        self.assertEqual(sorted(dims), sorted(EXPECTED["dimensions"]))
        self.assertEqual(self.sound(self.doc), [])

    def test_a_datum_feature_on_two_faces(self):
        # B and C are each a hole split in two half cylinders, given with a
        # SET_REPRESENTATION_ITEM: read by the OCCT fork (sec 5.3, item 6)
        faces = {}
        for d in objects(self.doc, "Datum"):
            faces[d.Letter] = sum(len(subs) for _, subs in d.Feature.References)
        self.assertEqual(faces, {"A": 1, "B": 2, "C": 2})

    def test_beside_the_part_on_its_faces(self):
        sets = objects(self.doc, "AnnotationSet")
        self.assertEqual(len(sets), 1)
        part = sets[0].Part
        self.assertTrue(part is not None and part.isDerivedFrom("Part::Feature"))
        counts = {k: len(getattr(part.Shape, k + "es" if k == "Vertex" else k + "s"))
                  for k in ("Face", "Edge", "Vertex")}
        edges = []
        for feat in objects(self.doc, "TolerancedFeature"):
            for obj, subs in feat.References:
                self.assertEqual(obj, part)
                for s in subs:
                    kind = s.rstrip("0123456789")
                    self.assertTrue(kind in counts and 0 < int(s[len(kind):]) <= counts[kind], s)
                    if kind != "Face":
                        edges.append(s)
        # Two of its graphic-only notes are on edges (docs/GDandT.md sec 10.14)
        self.assertEqual(len(edges), 2)
        self.assertTrue(all(e.startswith("Edge") for e in edges), edges)

    def test_frames_cite_their_datums(self):
        letters = set(d.Letter for d in objects(self.doc, "Datum"))
        cited = set()
        for gt in objects(self.doc, "GeometricTolerance"):
            if gt.DatumSystem:
                for c in gt.DatumSystem.Compartments:
                    cited.update(c.split("(")[0].split("-"))
        self.assertTrue(cited and cited <= letters, (cited, letters))

    def test_placed_where_the_file_places_them(self):
        callouts = [o for o in self.doc.Objects if hasattr(o, "AutoPlace")]
        self.assertTrue(callouts)
        placed = [o for o in callouts if not o.AutoPlace]
        self.assertEqual(len(placed), len(callouts))
        for o in callouts:
            self.assertFalse(o.Presentation.isNull(), o.Name)

    def test_drawn_as_the_file_draws(self):
        callouts = [o for o in self.doc.Objects if hasattr(o, "UseImported")]
        drawn = [o for o in callouts if o.UseImported]
        self.assertEqual(len(drawn), len(callouts))
        for o in drawn:
            self.assertFalse(o.ImportedGraphic.isNull(), o.Name)
            a, b = o.Presentation.BoundBox, o.ImportedGraphic.BoundBox
            self.assertAlmostEqual(a.DiagonalLength, b.DiagonalLength, 6)
        # Edited, drawn from what it says
        gt = objects(self.doc, "GeometricTolerance")[0]
        before = gt.Presentation.BoundBox.DiagonalLength
        gt.Value = gt.Value * 2
        self.doc.recompute()
        self.assertFalse(gt.UseImported)
        self.assertNotAlmostEqual(gt.Presentation.BoundBox.DiagonalLength, before, 3)

    def test_graphic_only_as_notes(self):
        notes = objects(self.doc, "Note")
        self.assertEqual(len(notes), EXPECTED["notes"])
        for n in notes:
            self.assertEqual(list(n.Text), [])
            self.assertFalse(n.ImportedGraphic.isNull(), n.Name)
        # The graphic of what is read, given again on its own, is no note:
        # CTC-01 gives its three datums' so
        read = [o for o in self.doc.Objects if hasattr(o, "ImportedGraphic") and o not in notes]
        for n in notes:
            b = n.ImportedGraphic.BoundBox
            for o in read:
                c = o.ImportedGraphic.BoundBox
                twin = (b.Center - c.Center).Length < 1e-6 and \
                    abs(b.DiagonalLength - c.DiagonalLength) < 1e-6
                self.assertFalse(twin, (n.Label, o.Name))

    def test_saved_views(self):
        views = objects(self.doc, "View")
        self.assertEqual(sorted(v.Label for v in views), EXPECTED["views"])
        shown = set()
        for v in views:
            self.assertFalse(v.AnnotationPlane)
            shown.update(o.Name for o in v.InList if hasattr(o, "Views") and v in o.Views)
        self.assertTrue(shown)

    def test_lettered_as_the_file_letters(self):
        # CTC-01's one-row frames are 24.5 high: twice the text, ASME's way
        heights = set(round(o.TextHeight.Value, 6) for o in self.doc.Objects
                      if hasattr(o, "AutoPlace"))
        self.assertEqual(heights, {12.25})


# CTC-01 as its drawing defines it (NIST nist_ctc_01_asme1_rd.pdf): every
# semantic tolerance and datum; of its twelve dimensions, the eight the file
# gives a value -- the other four, and the graphic-only callouts, are only
# a presentation (deviations as signed limits, the nominal the model's)
EXPECTED = {
    # Two texts and two sizes the file gives only as graphics
    "notes": 4,
    "views": ["MBD_0"],
    "datums": ["A", "B", "C"],
    "tolerances": [
        ("Flatness", 0.2, []),
        ("Perpendicularity", 1.5, ["A"]),
        ("ProfileOfSurface", 0.5, ["A"]),
        ("Position", 0.75, ["A", "B", "C"]),
        ("Position", 0.75, ["A", "B", "C"]),
        ("ProfileOfSurface", 1.25, ["A", "B", "C"]),
    ],
    "dimensions": [
        ("Angle", 60.0, 0.5, -0.5),
        ("Diameter", 35.0, 0.0, -0.2),
        ("Diameter", 35.0, 0.2, 0.0),
        ("Diameter", 20.0, 0.05, -0.1),
        ("Diameter", 20.0, 0.1, -0.05),
        ("Diameter", 35.0, 0.2, -0.2),
        ("Diameter", 35.0, 0.2, -0.2),
        ("Diameter", 25.0, 0.15, -0.15),
    ],
}


# The datum targets of the NIST set, by file and letter: (type, number)
TARGETS = {
    "nist_ctc_02_asme1_ap242-e2.stp": {
        "A": [("Point", 1), ("Point", 2), ("Point", 3)],
        "B": [("Point", 1), ("Point", 2), ("Point", 3), ("Point", 4)],
        "C": [("Point", 1)],
        "K": [("Circle", 1)],
    },
    "nist_ctc_05_asme1_ap242-e1.stp": {"C": [("Rectangle", 1)], "D": [("Rectangle", 1)]},
    "nist_ftc_06_asme1_ap242-e2.stp": {
        "J": [("Line", 1), ("Line", 2)],
        "K": [("Line", 1), ("Line", 2)],
    },
}


@unittest.skipUnless(os.environ.get("FC_PMI_NIST"), "FC_PMI_NIST names no NIST set")
class TestNistSet(Base):
    """Every AP242 file: it reads GD&T, every reference resolves. What the
    check finds invalid -- features the model has no kind for yet: slots,
    counterbores, depths -- is printed, the ledger of docs/GDandT.md sec 10.9"""

    def test_every_ap242_file(self):
        files = sorted(glob.glob(os.path.join(os.environ["FC_PMI_NIST"], "*ap242*.stp")))
        self.assertTrue(files)
        for path in files:
            with self.subTest(os.path.basename(path)):
                doc = self.read(path)
                # The -tg files carry their PMI only as tessellated graphics:
                # nothing semantic to read
                if not path.endswith("-tg.stp"):
                    self.assertTrue(objects(doc, "GeometricTolerance") or objects(doc, "Dimension"))
                bad = self.sound(doc)
                self.assertEqual([b for b in bad if " Broken: " in b], [])
                for b in bad:
                    print("NIST %s: %s" % (os.path.basename(path), b))
                # Every camera a view: a draughting model that is also a
                # tessellated shape representation (FTC-07, -10, -11) too
                with open(path, errors="replace") as f:
                    cameras = len(re.findall(r"=\s*CAMERA_MODEL_D3(_MULTI_CLIPPING)?\(", f.read()))
                self.assertEqual(len(objects(doc, "View")), cameras)
                # A saved view's objects, when the file names them, include
                # the part's solid: never construction curves alone
                for v in objects(doc, "View"):
                    if v.Objects:
                        self.assertTrue(any(o.Shape.Solids for o in v.Objects), v.Label)

    def test_every_ap242_file_round_trips(self):
        """Read, written and read again, each file reads the same: graphics
        alone on a tessellated part (-tg) too"""
        files = sorted(glob.glob(os.path.join(os.environ["FC_PMI_NIST"], "*ap242*.stp")))
        for path in files:
            with self.subTest(os.path.basename(path)):
                doc = self.read(path)
                again = self.write_and_read(doc)
                self.assertSameGdt(doc, again)
                self.assertSameViews(doc, again)
                self.assertEqual(len([b for b in self.sound(again) if " Broken: " in b]), 0)

    def test_a_target_on_its_geometry(self):
        """Written, a datum target's graphic is associated with the faces it
        is on -- its feature's shape aspects, which have geometry, not the
        target, which has none -- and the target described as one, as NIST's
        files have it: what SFA asks for its Associated Geometry"""
        for name in TARGETS:
            with self.subTest(name):
                doc = self.read(os.path.join(os.environ["FC_PMI_NIST"], name))
                self.write_and_read(doc)
                with open(self.written, errors="replace") as f:
                    ents = entities(f.read())
                # The faces' shape aspects, and what has them: a composite
                # its components, a target its feature
                used, parts = set(), collections.defaultdict(list)
                for v in ents.values():
                    m = re.match(r"GEOMETRIC_ITEM_SPECIFIC_USAGE\('[^']*',[^,]*,#(\d+),", v)
                    if m:
                        used.add(m.group(1))
                    m = re.search(r"SHAPE_ASPECT_RELATIONSHIP\('[^']*',('[^']*'|\$),"
                                  r"#(\d+),#(\d+)\)", v)
                    if m:
                        parts[m.group(2)].append(m.group(3))

                def has_geometry(n, seen=()):
                    return n in used or any(has_geometry(c, seen + (n,)) for c in parts[n]
                                            if c not in seen)

                links = []
                for v in ents.values():
                    m = re.match(r"DRAUGHTING_MODEL_ITEM_ASSOCIATION\('([^']*)',[^,]*,#(\d+),#\d+,"
                                 r"#(\d+)\)", v)
                    if m:
                        links.append(m.groups())
                link = "PMI representation to presentation link"
                semantic = {c for n, _, c in links if n == link}
                # Of what has semantics: a graphic alone (a note on nothing) is
                # associated with nothing
                lost = [ents[d].split("(")[0] for n, d, c in links
                        if c in semantic and n != link and not has_geometry(d)]
                self.assertEqual(lost, [])
                described = [v for v in ents.values()
                             if v.startswith("SHAPE_ASPECT_RELATIONSHIP('','datum target',")]
                self.assertEqual(len(described), len(objects(doc, "DatumTarget")))

    def test_graphics_typed_as_the_file_types_them(self):
        """Written again, a file's graphic is the kind the file named it --
        a 'label', a 'linear dimension' for a curve's length (STC-09) --
        and a file of graphics alone (FTC-08-e1-tg) is not all notes"""

        def kinds(path):
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", " ", f.read())
            return collections.Counter(
                k.lower().replace(" ", "")
                for k in re.findall(r"TESSELLATED_GEOMETRIC_SET\s*\(\s*'([^']*)'", text))

        nist = os.environ["FC_PMI_NIST"]
        for name in ("nist_ftc_08_asme1_ap242-e1-tg.stp", "nist_ctc_04_asme1_ap242-e1.stp",
                     "nist_stc_09_asme1_ap242-e3.stp"):
            with self.subTest(name):
                path = os.path.join(nist, name)
                again = self.write_and_read(self.read(path))
                theirs, ours = kinds(path), kinds(self.written)
                if name.endswith("-tg.stp"):
                    self.assertEqual(ours, theirs)
                else:
                    self.assertEqual(set(ours) - set(theirs), set())
                    self.assertIn("label", ours)
                kept = [c.ImportedGraphicType for c in again.Objects
                        if hasattr(c, "ImportedGraphicType")]
                self.assertTrue(kept and all(kept), kept)

    def test_notes_say_what_the_file_says(self):
        """A note's or label's words, the file's semantic text: CTC-04's
        labels of its datums and a counterbore, FTC-07's general note a line
        each -- and written again as one, read back the same"""
        nist = os.environ["FC_PMI_NIST"]

        def texts(doc):
            return sorted(tuple(n.Text) for n in objects(doc, "Note") if n.Text)

        doc = self.read(os.path.join(nist, "nist_ctc_04_asme1_ap242-e1.stp"))
        self.assertEqual(texts(doc), [("A",), ("B",), ("C",), ("\u2334",)])
        # On what the file says they are of: a datum's faces
        self.assertTrue(all(n.Feature for n in objects(doc, "Note") if n.Text))
        again = self.write_and_read(doc)
        self.assertEqual(texts(again), texts(doc))
        with open(self.written, errors="replace") as f:
            written = f.read()
        self.assertEqual(written.count("PROPERTY_DEFINITION('semantic text'"), 4)
        doc = self.read(os.path.join(nist, "nist_ftc_07_asme1_ap242-e2.stp"))
        general = [n.Text for n in objects(doc, "Note") if len(n.Text) > 30]
        self.assertEqual(len(general), 1)
        self.assertEqual(general[0][0], "NOTES (UNLESS OTHERWISE SPECIFIED):")
        self.assertEqual(general[0][-1], "6. UNITS: INCHES")
        self.assertEqual(texts(self.write_and_read(doc)), texts(doc))

    def test_on_supplemental_geometry(self):
        """GD&T on supplemental geometry -- a thread's cylinder drawn as its
        circles, its upper plane as its outline, curves of a constructive
        geometry representation: CTC-04's thread, 12 across and 25 from a
        face, with its position; FTC-10's three threads -- read, in the
        solid's set, and measured"""
        nist = os.environ["FC_PMI_NIST"]

        def supplemental(doc):
            """What is on curves, not the solid: (kind, value), each Ok and in
            the solid's set"""
            got = []
            for o in objects(doc, "Dimension") + objects(doc, "GeometricTolerance"):
                feats = o.Features if hasattr(o, "Features") else [o.Feature]
                objs = [r[0] for f in feats if f for r in f.References]
                if all(x.Shape.Solids for x in objs):
                    continue
                self.assertEqual(o.Condition, "Ok", (o.Label, o.ConditionMessage))
                sets = [s for s in o.InList if s.TypeId == "PMI::AnnotationSet"]
                self.assertTrue(sets and sets[0].Part.Shape.Solids, o.Label)
                value = o.Nominal if hasattr(o, "Nominal") else o.Value
                got.append((getattr(o, "Type", None) or o.Characteristic,
                            round(value.Value, 6)))
            return sorted(got)

        doc = self.read(os.path.join(nist, "nist_ctc_04_asme1_ap242-e1.stp"))
        self.assertEqual(supplemental(doc),
                         [("Diameter", 12.0), ("Distance", 25.0), ("Position", 0.75),
                          ("ProfileOfSurface", 0.5)])
        self.assertEqual(supplemental(self.write_and_read(doc)), supplemental(doc))
        doc = self.read(os.path.join(nist, "nist_ftc_10_asme1_ap242-e2.stp"))
        self.assertEqual(supplemental(doc), [("Diameter", 3.0)] * 3 + [("Diameter", 5.5)] * 2
                         + [("Position", 0.8)] * 3)
        self.assertEqual(supplemental(self.write_and_read(doc)), supplemental(doc))

    def test_what_the_drawing_says(self):
        """What SFA, against NIST's drawings, found lost: CTC-01's diameters
        given as their limits (34.8-35.2), CTC-03's thickness the file's
        though the model cannot measure it, FTC-11's reference dimension of a
        curve's length, (197.9); CTC-04's angle the file's 90, exactly"""
        nist = os.environ["FC_PMI_NIST"]

        def dims(name):
            return objects(self.read(os.path.join(nist, name)), "Dimension")

        limits = [d for d in dims("nist_ctc_01_asme1_ap242-e1.stp") if d.Limits]
        self.assertEqual(sorted((round(d.Nominal.Value + d.Lower.Value, 6),
                                 round(d.Nominal.Value + d.Upper.Value, 6)) for d in limits),
                         [(34.8, 35.2), (34.8, 35.2)])
        # Drawn so, to the set's decimal places
        self.assertEqual([d.Label for d in limits
                          if not re.search(r"34\.80?-35\.20?$", d.EquivalentText)], [])
        thick = [d for d in dims("nist_ctc_03_asme1_ap242-e2.stp") if d.Type == "Width"]
        self.assertEqual([round(d.Nominal.Value, 4) for d in thick], [20.828])
        ref = [d for d in dims("nist_ftc_11_asme1_ap242-e2.stp") if d.Reference]
        self.assertEqual([(d.Type, round(d.Nominal.Value, 1)) for d in ref],
                         [("CurveLength", 197.9)])
        self.assertRegex(ref[0].EquivalentText, r"^\(197\.92?\)$")
        # The file's 90, not the model's 90.000000000292: measured again, a
        # value that agrees is left as the file gives it
        angles = [d for d in dims("nist_ctc_04_asme1_ap242-e1.stp")
                  if d.Type in ("Angle", "AngularSize")]
        self.assertEqual([d.Nominal.Value for d in angles], [90.0])

    def test_a_clipped_view(self):
        # FTC-11's MBD_B is a section: cut by the plane x = 0, its normal
        # toward the eye, which looks along -x from the +x side
        doc = self.read(os.path.join(os.environ["FC_PMI_NIST"],
                                     "nist_ftc_11_asme1_ap242-e2.stp"))
        views = {v.Label: v for v in objects(doc, "View")}
        clipped = sorted(k for k, v in views.items() if v.Clip)
        self.assertEqual(clipped, ["MBD_B"])
        v = views["MBD_B"]
        n = v.ClipPlane.Rotation.multVec(V(0, 0, 1))
        self.assertLess((n - v.Camera.Rotation.multVec(V(0, 0, 1))).Length, 1e-9)
        sets = objects(doc, "AnnotationSet")
        to_part = sets[0].Part.Placement.inverse()
        self.assertLess((to_part.Rotation.multVec(n) - V(1, 0, 0)).Length, 1e-9)
        self.assertLess(abs(to_part.multVec(v.ClipPlane.Base).x), 1e-6)

    def test_datum_targets(self):
        # The datums only targets establish: points (CTC-02's A, B, C), a
        # circle (its K), rectangles (CTC-05's C, D), lines (FTC-06's J, K)
        for name, want in TARGETS.items():
            path = os.path.join(os.environ["FC_PMI_NIST"], name)
            with self.subTest(name):
                doc = self.read(path)
                got = {}
                for datum in objects(doc, "Datum"):
                    if datum.Targets:
                        got[datum.Letter] = [(t.TargetType, t.Number) for t in datum.Targets]
                        self.assertEqual(datum.Condition, "Ok", datum.ConditionMessage)
                self.assertEqual(got, want)
                for t in objects(doc, "DatumTarget"):
                    self.assertEqual(t.Condition, "Ok", (t.Label, t.ConditionMessage))
                    # On a face of the part, its centre on it
                    self.assertTrue(t.Feature, t.Label)
                    face = t.Feature.Shape
                    d = face.distToShape(Part.Vertex(t.Target.Base))[0]
                    self.assertLess(d, 1e-3, t.Label)


def entities(text):
    """A STEP file's entities by number, each on one line, its blanks after
    an opening bracket gone"""
    out = {}
    text = text.replace("\r", "")
    for m in re.finditer(r"#(\d+)\s*=\s*(.*?);\s*(?=#\d+\s*=|ENDSEC)", text, re.S):
        out[m.group(1)] = re.sub(r"\(\s+", "(", re.sub(r"\s+", " ", m.group(2)))
    return out


def schema_of(path):
    """The file's FILE_SCHEMA statement, over however many lines it takes"""
    with open(path, errors="replace") as f:
        text = f.read(20000)
    start = text.find("FILE_SCHEMA")
    return text[start:text.find(";", start)] if start >= 0 else ""


class TestStepExport(Base):
    """The GD&T of a part, written (docs/GDandT.md sec 5.2, P3): AP242, its
    semantics on the faces exported, each with its plane and its graphic,
    and read back as it was -- with the part's placement kept in the file
    and without"""

    def setUp(self):
        from PMITests.TestModel import BOTTOM, FRONT, HOLE, LEFT, RIGHT, TOP, face_at

        self.dir = tempfile.mkdtemp(prefix="pmi-export-")
        doc = self.doc = App.newDocument("PMIExport")
        self.addCleanup(App.closeDocument, doc.Name)
        body = self.body = doc.addObject("PartDesign::Body", "Body")
        box = doc.addObject("PartDesign::AdditiveBox", "Box")
        body.addObject(box)
        box.Length, box.Width, box.Height = 40, 20, 10
        hole = doc.addObject("PartDesign::SubtractiveCylinder", "Hole")
        body.addObject(hole)
        hole.Radius, hole.Height = 3, 10
        hole.Placement = App.Placement(V(20, 10, 0), App.Rotation())
        # Placed off the origin and turned: the annotations are in the
        # Body's coordinates, the file's part in its own or the document's
        body.Placement = App.Placement(V(100, 50, 5), App.Rotation(V(0, 0, 1), 30))
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

        def feat(*points):
            return add("TolerancedFeature", "Feature",
                       References=[(solid, [face_at(solid.Shape, p) for p in points])])

        for letter, p in (("A", BOTTOM), ("B", LEFT), ("C", FRONT)):
            add("Datum", "Datum" + letter, Letter=letter, Feature=feat(p))
        add("GeometricTolerance", "Flatness", Characteristic="Flatness", Value=0.05,
            Feature=feat(TOP))
        drf = add("DatumReferenceFrame", "DRF", Compartments=["A", "B", "C(M)"])
        hole_feature = feat(HOLE)
        add("GeometricTolerance", "Position", Characteristic="Position", Value=0.1,
            ZoneShape="Diameter", MaterialCondition="Maximum", Feature=hole_feature,
            DatumSystem=drf)
        add("Dimension", "Dia", Type="Diameter", Features=[hole_feature], Upper=0.05,
            Lower=-0.02)
        add("Dimension", "Length", Type="Distance", Features=[feat(LEFT), feat(RIGHT)])
        add("Note", "Note", Text=["BREAK EDGES"], Feature=feat(V(35, 15, 10)))
        # Saved views (P5): their cameras are the document's. One from the
        # top, its height given; one turned, in perspective, fitting all
        top = add("View", "Top", Standard="Top", Height=60)
        top.Camera = App.Placement(V(110, 60, 15), top.Camera.Rotation)
        turned = add("View", "Turned", Perspective=True,
                     Camera=App.Placement(V(105, 52, 8), App.Rotation(20, 35, 50)))
        # The turned one cut by a plane, tilted, through the hole
        turned.Clip = True
        turned.ClipPlane = App.Placement(V(110, 58, 7), App.Rotation(V(1, 1, 0), 40))
        for name, views in (("DatumA", [top]), ("Flatness", [top]), ("Position", [turned]),
                            ("Dia", [turned]), ("Note", [top, turned])):
            doc.getObject(name).Views = views
        doc.recompute()
        bad = [o.Name for o in doc.Objects if getattr(o, "Condition", "Ok") != "Ok"]
        self.assertEqual(bad, [])

    def export(self, keep, objs=None):
        path = os.path.join(self.dir, "part-%d.step" % keep)
        Import.export(objs or [self.body], path, keepPlacement=keep)
        back = App.newDocument("PMIExportBack")
        self.addCleanup(App.closeDocument, back.Name)
        Import.insert(path, back.Name)
        back.recompute()
        return path, back

    @staticmethod
    def key(o):
        for prop in ("Letter", "Characteristic", "Type"):
            if hasattr(o, prop):
                return (o.TypeId, str(getattr(o, prop)))
        return (o.TypeId, "")

    def test_it_writes_ap242_and_reads_back_the_same(self):
        path, back = self.export(False)
        self.assertIn("AP242", schema_of(path))
        with open(path) as f:
            text = f.read()
        # The graphics in the callouts' blue, not OCCT's white
        self.assertIn("TESSELLATED_ANNOTATION_OCCURRENCE", text)
        self.assertNotIn("COLOUR_RGB('',1.,1.,1.)", text)
        self.assertEqual(sorted(d.Letter for d in objects(back, "Datum")), ["A", "B", "C"])
        tols = []
        for gt in objects(back, "GeometricTolerance"):
            frame = list(gt.DatumSystem.Compartments) if gt.DatumSystem else []
            tols.append((gt.Characteristic, round(gt.Value, 6), gt.ZoneShape,
                         gt.MaterialCondition, frame))
        self.assertEqual(sorted(tols), [
            ("Flatness", 0.05, "None", "None", []),
            ("Position", 0.1, "Diameter", "Maximum", ["A", "B", "C(M)"]),
        ])
        dims = [(d.Type, round(d.Nominal, 4), round(d.Upper, 6), round(d.Lower, 6))
                for d in objects(back, "Dimension")]
        self.assertEqual(sorted(dims), [("Diameter", 6.0, 0.05, -0.02),
                                        ("Distance", 40.0, 0.0, 0.0)])
        notes = objects(back, "Note")
        self.assertEqual([n.Label for n in notes], ["BREAK EDGES"])
        self.assertTrue(notes[0].Feature)
        bad = [o.Name for o in back.Objects if getattr(o, "Condition", "Ok") != "Ok"]
        self.assertEqual(bad, [])

    def test_written_as_the_schema_has_it(self):
        """What a checker of the file (NIST's SFA) asks of it: a tolerance's
        bounds typed; a perpendicularity to a datum the entity alone, not a
        complex; a common datum's list a list, its members' modifiers typed;
        a zone defined as runout or projected only when it is, a projected one
        with its length; an angle in degrees, its bounds typed; each graphic
        named -- and read back the same"""
        from PMITests.TestModel import BOTTOM, HOLE, LEFT, face_at

        doc, pset = self.doc, self.doc.getObject("PMI")
        solid = self.body.SolidBodies[0]

        def add(kind, name, **props):
            o = doc.addObject("PMI::" + kind, name)
            pset.addObject(o)
            for k, v in props.items():
                setattr(o, k, v)
            return o

        def feat(p):
            return add("TolerancedFeature", "Feature",
                       References=[(solid, [face_at(solid.Shape, p)])])

        add("GeometricTolerance", "Square", Characteristic="Perpendicularity", Value=0.02,
            Feature=feat(LEFT), DatumSystem=add("DatumReferenceFrame", "ToA",
                                                Compartments=["A"]))
        add("GeometricTolerance", "Projected", Characteristic="Position", Value=0.05,
            ZoneShape="Diameter", Modifiers=["P"], ProjectedLength=12.5,
            Feature=feat(HOLE), DatumSystem=add("DatumReferenceFrame", "Common",
                                                Compartments=["A-B(M)"]))
        add("Dimension", "Corner", Type="Angle", Features=[feat(LEFT), feat(BOTTOM)],
            Upper=0.5, Lower=-0.5)
        doc.recompute()
        bad = [o.Name for o in doc.Objects if getattr(o, "Condition", "Ok") != "Ok"]
        self.assertEqual(bad, [])
        path, back = self.export(False)
        with open(path) as f:
            text = re.sub(r"\s+", "", f.read())
        # Typed bounds: LENGTH_MEASURE(0.05), not a bare number
        self.assertFalse(re.findall(r"=MEASURE_WITH_UNIT\([-0-9.E+]+,", text))
        self.assertIn("=PERPENDICULARITY_TOLERANCE(", text)
        self.assertNotIn("PERPENDICULARITY_TOLERANCE()", text)
        self.assertRegex(text, r"COMMON_DATUM_LIST\(\(#\d+(,#\d+)+\)\)")
        # A modifier is a member of a SELECT: typed, not the bare enumeration
        self.assertIn("(SIMPLE_DATUM_REFERENCE_MODIFIER(.MAXIMUM_MATERIAL_REQUIREMENT.))", text)
        self.assertNotRegex(text, r"DATUM_REFERENCE_ELEMENT\([^;]*,\(\.[A-Z_]+\.")
        # The angle in degrees, the unit says so: not 90 radians
        unit = re.search(r"POSITIVE_PLANE_ANGLE_MEASURE\(90\.0*\),(#\d+)", text)
        self.assertTrue(unit)
        self.assertRegex(text, re.escape(unit.group(1)) + r"=\(CONVERSION_BASED_UNIT\('DEGREE',")
        self.assertIn("=PLANE_ANGLE_MEASURE_WITH_UNIT(PLANE_ANGLE_MEASURE(0.5),", text)
        # Named as the recommended practice names it
        self.assertIn("=ANGULAR_LOCATION('angle',", text)
        self.assertNotIn("RUNOUT_ZONE_DEFINITION", text)
        self.assertRegex(text, r"PROJECTED_ZONE_DEFINITION\(#\d+,\(\),#\d+,#\d+\)")
        self.assertNotIn("TESSELLATED_ANNOTATION_OCCURRENCE(''", text)
        # Each graphic's set named by what it presents, one of the names of
        # the recommended practice (8.4), by which a checker tells it
        sets = set(re.findall(r"TESSELLATED_GEOMETRIC_SET\('([^']*)'", text))
        self.assertEqual(sets, {"datum", "flatness", "position", "perpendicularity",
                                "diameterdimension", "lineardimension", "angulardimension",
                                "note"})
        # Each occurrence named, uniquely
        occs = re.findall(r"TESSELLATED_ANNOTATION_OCCURRENCE\('([^']*)'", text)
        self.assertEqual(len(occs), len(set(occs)))
        # Each associated with what it is about: the semantic link -- the
        # note's to its words, a semantic text -- and one per shape aspect
        # (the length's two faces, the angle's)
        links = re.findall(r"DRAUGHTING_MODEL_ITEM_ASSOCIATION\('([^']*)'", text)
        semantic = links.count("PMIrepresentationtopresentationlink")
        self.assertEqual(semantic, len(occs))
        self.assertEqual(len(links) - semantic, len(occs) + 1 + 1)
        self.assertIn("=PROPERTY_DEFINITION('semantictext',", text)
        self.assertIn("=DESCRIPTIVE_REPRESENTATION_ITEM('Note','BREAKEDGES')", text)
        got = {gt.Characteristic: gt for gt in objects(back, "GeometricTolerance")}
        projected = [gt for gt in objects(back, "GeometricTolerance")
                     if gt.ProjectedLength.Value > 0]
        self.assertEqual([round(gt.ProjectedLength.Value, 6) for gt in projected], [12.5])
        self.assertEqual(list(projected[0].DatumSystem.Compartments), ["A-B(M)"])
        self.assertEqual(round(got["Perpendicularity"].Value, 6), 0.02)
        self.assertEqual(list(got["Perpendicularity"].DatumSystem.Compartments), ["A"])
        angle = [d for d in objects(back, "Dimension") if d.Type == "Angle"]
        self.assertEqual([(round(d.Nominal.Value, 6), round(d.Upper, 6),
                           round(d.Lower, 6)) for d in angle], [(90, 0.5, -0.5)])

    def test_in_the_files_length_unit(self):
        """Written in inches (Preferences, STEP, Unit: the writer's unit), the
        GD&T is in inches as the shapes are: the position's 0.1 mm a value of
        0.1/25.4, not 0.1 inch -- and read back the same as written in mm:
        the values, the graphics where they were, the views"""
        unit = Part.exportUnits()["write.step.unit"]
        self.addCleanup(Part.exportUnits, unit)
        Part.exportUnits("MM")
        _, mm = self.export(False)
        Part.exportUnits("INCH")
        path = os.path.join(self.dir, "part-inch.step")
        Import.export([self.body], path, keepPlacement=False)
        inch = App.newDocument("PMIExportInch")
        self.addCleanup(App.closeDocument, inch.Name)
        Import.insert(path, inch.Name)
        inch.recompute()
        with open(path) as f:
            text = re.sub(r"\s+", "", f.read())
        self.assertIn("CONVERSION_BASED_UNIT('INCH'", text)
        # The inch's 25.4 typed, as the schema has a measure's value: SFA
        # cannot tell an untyped one's select, LENGTH_UNIT(25.4) or bare 25.4
        self.assertIn("LENGTH_MEASURE_WITH_UNIT(LENGTH_MEASURE(25.4),", text)
        # The values, not a curve style's line width (0.1, as NIST's files have it)
        values = [float(v) for v in re.findall(r"LENGTH_MEASURE\(([-0-9.E+]+)\)",
                                               re.sub(r"CURVE_STYLE\([^;]*;", "", text))]
        self.assertTrue([v for v in values if abs(v - 0.1 / 25.4) < 1e-9], values)
        self.assertFalse([v for v in values if abs(v - 0.1) < 1e-9], values)
        self.assertSameGdt(mm, inch)
        self.assertSameViews(mm, inch)

        def graphics(d):
            return sorted((o.Label, o.ImportedGraphic.BoundBox) for o in d.Objects
                          if hasattr(o, "ImportedGraphic") and not o.ImportedGraphic.isNull())

        ga, gb = graphics(mm), graphics(inch)
        self.assertEqual([x[0] for x in ga], [x[0] for x in gb])
        self.assertTrue(ga)
        for (name, a), (_, b) in zip(ga, gb):
            for x, y in ((a.XMin, b.XMin), (a.YMax, b.YMax), (a.ZMax, b.ZMax),
                         (a.DiagonalLength, b.DiagonalLength)):
                self.assertAlmostEqual(x, y, delta=1e-6, msg=name)

    def test_views_written_and_read_back(self):
        origs = {v.Label: v for v in objects(self.doc, "View")}

        def shown(view, doc):
            return sorted(self.key(o) for o in doc.Objects
                          if hasattr(o, "Views") and view in o.Views)

        for keep in (False, True):
            with self.subTest(keepPlacement=keep):
                path, back = self.export(keep)
                with open(path) as f:
                    text = f.read()
                # The clipped one a camera clipping, the other a plain one
                self.assertEqual(text.count("CAMERA_MODEL_D3("), 1)
                self.assertEqual(text.count("CAMERA_MODEL_D3_MULTI_CLIPPING("), 1)
                views = {v.Label: v for v in objects(back, "View")}
                self.assertEqual(sorted(views), sorted(origs))
                # A camera is the document's: kept placed, the part is where
                # it was; not, it is at the origin, and the camera with it
                to_back = App.Placement() if keep else self.body.Placement.inverse()
                for name, orig in origs.items():
                    v = views[name]
                    want = to_back.multiply(orig.Camera)
                    # The same camera: turned the same, looking along the
                    # same line (where on it is the file's eye, not ours)
                    for axis in (V(1, 0, 0), V(0, 1, 0), V(0, 0, 1)):
                        self.assertLess((want.Rotation.multVec(axis)
                                         - v.Camera.Rotation.multVec(axis)).Length, 1e-9, name)
                    z = want.Rotation.multVec(V(0, 0, 1))
                    off = v.Camera.Base - want.Base
                    self.assertLess((off - z * off.dot(z)).Length, 1e-6, name)
                    self.assertEqual(v.Perspective, orig.Perspective, name)
                    self.assertEqual(v.Objects, [], name)
                    self.assertEqual(shown(v, back), shown(orig, self.doc), name)
                    # The same clipping: the plane the same, facing the same way
                    self.assertEqual(v.Clip, orig.Clip, name)
                    if orig.Clip:
                        cut = to_back.multiply(orig.ClipPlane)
                        n = cut.Rotation.multVec(V(0, 0, 1))
                        self.assertLess((v.ClipPlane.Rotation.multVec(V(0, 0, 1)) - n).Length,
                                        1e-9, name)
                        self.assertLess(abs((v.ClipPlane.Base - cut.Base).dot(n)), 1e-6, name)
                # A height kept; fitting all, the window the part fills
                self.assertAlmostEqual(views["Top"].Height.Value, 60, places=9)
                self.assertGreater(views["Turned"].Height.Value, 20)

    def test_placed_and_drawn_where_they_were(self):
        origs = {self.key(o): o for o in self.doc.Objects
                 if hasattr(o, "Plane") and o.TypeId != "PMI::View"}
        for keep in (False, True):
            with self.subTest(keepPlacement=keep):
                _, back = self.export(keep)
                # The part's own coordinates, or the document's
                frame = self.body.Placement if keep else App.Placement()
                seen = 0
                for o in back.Objects:
                    if not hasattr(o, "Plane") or o.TypeId == "PMI::View":
                        continue
                    orig = origs[self.key(o)]
                    seen += 1
                    want = frame.multiply(orig.Plane)
                    self.assertLess((want.Base - o.Plane.Base).Length, 1e-6, o.Label)
                    z = V(0, 0, 1)
                    self.assertLess((want.Rotation.multVec(z)
                                     - o.Plane.Rotation.multVec(z)).Length, 1e-9, o.Label)
                    drawn = orig.Presentation.copy()
                    drawn.transformShape(frame.toMatrix())
                    a, b = drawn.BoundBox, o.ImportedGraphic.BoundBox
                    for x, y in ((a.XMin, b.XMin), (a.YMin, b.YMin), (a.ZMin, b.ZMin),
                                 (a.XMax, b.XMax), (a.YMax, b.YMax), (a.ZMax, b.ZMax)):
                        self.assertAlmostEqual(x, y, 6, o.Label)
                self.assertEqual(seen, len(origs))

    def test_without_pmi_the_schema_is_left_alone(self):
        path, _ = self.export(False)
        self.assertIn("AP242", schema_of(path))
        plain = self.doc.addObject("Part::Box", "Plain")
        self.doc.recompute()
        other = os.path.join(self.dir, "plain.step")
        Import.export([plain], other)
        self.assertNotIn("AP242", schema_of(other))


class TestStepRoundTrip(Base):
    """CTC-01 read, written and read again: what it reads the same"""

    def test_ctc01(self):
        doc = self.read(CTC01)
        again = self.write_and_read(doc)
        self.assertSameGdt(doc, again)
        self.assertSameViews(doc, again)
        self.assertEqual(self.sound(again), [])
