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
import shutil
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


# The NIST set's tolerances per unit: (file, characteristic, basis, size, second side), in mm
PER_UNIT = (
    ("nist_ctc_03_asme1_ap242-e2.stp", "Flatness", "Rectangle", 6.35, 6.35),
    ("nist_ftc_07_asme1_ap242-e2.stp", "Flatness", "Circle", 25.4, 0.0),
    ("nist_stc_07_asme1_ap242-e3.stp", "Flatness", "Circle", 25.4, 0.0),
    ("nist_ftc_10_asme1_ap242-e2.stp", "Straightness", "Length", 15.0, 0.0),
    ("nist_stc_10_asme1_ap242-e2.stp", "Straightness", "Length", 15.0, 0.0),
)

# The NIST set's hole depths, written as 'thickness' sizes: (file, depths)
DEPTHS = (
    ("nist_stc_06_asme1_ap242-e3.stp", [7.9375, 12.7, 12.7, 14.2875]),
    ("nist_stc_10_asme1_ap242-e2.stp", [2.0, 6.0, 6.0, 6.0, 10.0, 10.0, 27.5, 35.0]),
)


def edition(path):
    """A NIST file whatever its edition: nist_ctc_04_asme1_ap242-e1.stp is
    -e2 in a newer zip, ftc_08's -e1-tg -e4-tg (dwin, p9). The one named if it
    is there, else the one other edition of it"""
    if os.path.exists(path):
        return path
    folder, name = os.path.split(path)
    pattern = re.sub(r"-e\d+", "-e*", name, count=1)
    found = sorted(glob.glob(os.path.join(folder, pattern)))
    return found[-1] if found else path


def supplemental_group(obj):
    """Whether obj is in a part's supplemental geometry"""
    return any(getattr(g, "SupplementalGeometry", False) for g in obj.InList)


class Base(unittest.TestCase):
    def read(self, path):
        path = edition(path)
        doc = App.newDocument("PMIStep")
        self.addCleanup(App.closeDocument, doc.Name)
        Import.insert(path, doc.Name)
        doc.recompute()
        return doc

    def assertSameGdt(self, a, b):
        """What two documents read of GD&T is the same: the datums, the
        tolerances with their frames, zones and modifiers, the dimensions (a
        nominal measured on the model again, to 1e-3) with theirs, the datum
        targets and the notes"""

        def gdt(d):
            letters = sorted(x.Letter for x in objects(d, "Datum"))
            tols = sorted((gt.Characteristic, round(gt.Value, 6),
                           list(gt.DatumSystem.Compartments) if gt.DatumSystem else [],
                           round(gt.ProjectedLength.Value, 6), gt.ZoneShape,
                           gt.MaterialCondition, sorted(gt.Modifiers), gt.UnitBasis,
                           bool(gt.Composite))
                          for gt in objects(d, "GeometricTolerance"))
            dims = sorted((x.Type, round(x.Upper, 6), round(x.Lower, 6), x.Limits, x.Reference,
                           x.Basic, x.Fit, x.Nominal.Value, sorted(x.Modifiers), x.DecimalPlaces)
                          for x in objects(d, "Dimension"))
            targets = sorted((t.TargetType, t.Number) for t in objects(d, "DatumTarget"))
            return letters, tols, dims, targets, len(objects(d, "Note"))

        ga, gb = gdt(a), gdt(b)
        self.assertEqual(ga[0], gb[0])
        self.assertEqual(ga[1], gb[1])
        self.assertEqual([x[:7] + x[8:] for x in ga[2]], [x[:7] + x[8:] for x in gb[2]])
        for x, y in zip(ga[2], gb[2]):
            self.assertAlmostEqual(x[7], y[7], delta=1e-3)
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
        """Exported as a user does, and read back: an imported file's App::Part,
        its solid and what is beside it (supplemental geometry) -- else the
        sets' parts"""
        parts = []
        for s in objects(doc, "AnnotationSet"):
            part = s.Part
            # Its outermost App::Part: a user exports the whole (CTC-02's surfaces are a
            # group in it)
            group = part.getParentGeoFeatureGroup() if part else None
            while group and group.TypeId == "App::Part":
                part = group
                group = part.getParentGeoFeatureGroup()
            if part and part not in parts:
                parts.append(part)
        # Gone when the test is: 11 MB a NIST file, 3.7 GB of /tmp in two days
        # left behind, when it filled the writer could not open the file
        folder = tempfile.mkdtemp(prefix="pmi-roundtrip-")
        self.addCleanup(shutil.rmtree, folder, True)
        path = self.written = os.path.join(folder, "again.step")
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
            self.assertFalse(n.ImportedGraphic.isNull(), n.Name)
        # Their words, where the file gives a graphic its 'semantic text' beside
        # the geometry it is on: two labels', 'A' and 'B'
        self.assertEqual(sorted(t for n in notes for t in n.Text), ["A", "B"])
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
    # Two texts and four sizes the file gives only as graphics: two of the
    # sizes, diameters of datum features given no value, were lost (dwin, p15)
    "notes": 6,
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
        "G": [("CircularCurve", 1)],
        "H": [("CircularCurve", 1)],
        "J": [("Line", 1), ("Line", 2)],
        "K": [("Line", 1), ("Line", 2)],
    },
    # Areas, each a datum_target of its own face, with no feature for datum
    # target relationship: read as none before (dwin, p15)
    "nist_ftc_10_asme1_ap242-e2.stp": {
        "K": [("Area", 1), ("Area", 2)],
        "L": [("Area", 1), ("Area", 2)],
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
        self.assertTrue(files, "no AP242 file in FC_PMI_NIST")
        for path in files:
            with self.subTest(os.path.basename(path)):
                doc = self.read(path)
                again = self.write_and_read(doc)
                self.assertSameGdt(doc, again)
                self.assertSameViews(doc, again)
                self.assertEqual(len([b for b in self.sound(again) if " Broken: " in b]), 0)
                self.assertWrittenClean(os.path.basename(path))
                self.assertSameGraphics(path, self.written)
                self.assertPartsStyled(path, self.written)
                self.assertBodyColoured(path, self.written)
                self.assertSameAssembly(path, self.written)
                self.assertSameGraphicForms(path, self.written)
                self.assertSameAspects(path, self.written)
                # A product's semantic text written as a note's, not as an attribute
                # (SFA: invalid representation.name 'semantic text', dwin p21)
                with open(self.written, errors="replace") as f:
                    self.assertNotIn("REPRESENTATION('semantic text'", f.read())

    def assertSameAspects(self, original, written):
        """The composite shape aspects a tolerance or a dimension is on, as the
        file has them: a pattern of features a composite group shape aspect
        'pattern of features' of a composite shape aspect 'unified geometric
        item' a hole, named as the annotation (NIST CTC-04's 'Position.2', 6
        holes of 2 faces each), or a datum's feature itself (FTC-07's sizes on
        datum B's) -- kind, name, description and members, each leaf counted
        by its faces"""
        # Not yet as the file has them: a PMI on centres of symmetry, a size that is its own
        # datum feature (STC-07); a datum's own feature grouped, of hole occurrences (STC-09)
        known = r"nist_(stc_07|stc_09)_"
        if re.match(known, os.path.basename(original)):
            return

        def trees(path):
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", " ", f.read())
            text = text[text.index("DATA;"):]
            ents = dict(re.findall(r"#(\d+) ?= ?([A-Z_(][^;]*?(?:'[^']*'[^;']*?)*);", text))
            ref = lambda v: re.findall(r"#(\d+)", v)
            kinds = {
                "COMPOSITE_SHAPE_ASPECT": "C",
                "COMPOSITE_GROUP_SHAPE_ASPECT": "G",
                "DATUM_FEATURE": "D",
                "BASIC_ROUND_HOLE_OCCURRENCE": "H",
            }

            def kind(v):
                # a datum feature of several shapes is a complex one: a composite too
                if v.startswith("("):
                    if "DATUM_FEATURE()" in v and "COMPOSITE_GROUP_SHAPE_ASPECT()" in v:
                        return "F"
                    for part, k in (
                        ("DATUM_FEATURE()", "D"),
                        ("COMPOSITE_GROUP_SHAPE_ASPECT()", "G"),
                        ("COMPOSITE_SHAPE_ASPECT()", "C"),
                    ):
                        if part in v:
                            return k
                    return None
                return kinds.get(re.match(r"[A-Z_]*", v).group(0))
            members, faces = collections.defaultdict(list), collections.Counter()
            users = collections.defaultdict(set)
            for k, v in ents.items():
                for r in ref(v):
                    users[r].add(k)
                if re.match(r"SHAPE_ASPECT_RELATIONSHIP ?\(", v):
                    members[ref(v)[-2]].append(ref(v)[-1])
                elif re.match(r"(GEOMETRIC_ITEM_SPECIFIC|ITEM_IDENTIFIED_REPRESENTATION)_USAGE", v):
                    faces[ref(v)[0]] += len(ref(v)) - 2

            def tree(k):
                own, subs = 0, []
                for m in members[k]:
                    if kind(ents.get(m, "")):
                        subs.append(tree(m))
                    else:
                        own += faces[m]
                s = re.findall(r"'([^']*)'", ents[k]) + ["", ""]
                return (kind(ents[k]), s[0], s[1], own, tuple(sorted(subs)))

            def annotation(u):
                v = ents[u]
                head = v if v.startswith("(") else v.split("(")[0]
                if "TOLERANCE" in head:
                    return True
                # a dimension given no value is read as a graphic alone
                valued = (ents[c].startswith("DIMENSIONAL_CHARACTERISTIC_REP") for c in users[u])
                return re.search(r"DIMENSIONAL_|ANGULAR_", head) is not None and any(valued)

            return collections.Counter(
                tree(k) for k, v in ents.items()
                if kind(v) and any(annotation(u) for u in users[k]))

        self.assertEqual(trees(written), trees(original))

    def assertSameAssembly(self, original, written):
        """An assembly where the file has one and none where it has none: a
        product of several shapes is one product (CTC-02 nested its surfaces),
        several free products are free again (FTC-09's four; SFA's 'contains
        Assemblies', dwin p19)"""

        def usages(path):
            with open(path, errors="replace") as f:
                return len(re.findall(r"NEXT_ASSEMBLY_USAGE_OCCURRENCE\s*\(", f.read()))

        self.assertEqual(usages(written), usages(original))

    def assertSameGraphicForms(self, original, written):
        """A graphic given as curves -- an annotation curve occurrence of
        polylines and circles, NIST CTC-05's 22 -- written as curves again,
        any other tessellated (SFA's 'Graphic PMI (polyline)', dwin p20)"""

        def curves(path):
            with open(path, errors="replace") as f:
                return len(re.findall(r"ANNOTATION_CURVE_OCCURRENCE\s*\(", f.read()))

        def drawn(path):
            """The polylines, their points, and the circles of the curve sets
            annotations draw (CTC-05: 573, 6965, 77; ours had 4.7 times the
            points). Not of the two sets NIST overrides a style of (a part of
            its rectangular regions, logged)"""
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", "", f.read())
            ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
            lines = points = circles = 0
            for v in ents.values():
                if not v.startswith("ANNOTATION_CURVE_OCCURRENCE("):
                    continue
                v = ents[re.findall(r"#(\d+)", v)[-1]]
                for i in re.findall(r"#(\d+)", v):
                    if ents[i].startswith("POLYLINE("):
                        lines += 1
                        points += len(re.findall(r"#\d+", ents[i]))
                    elif ents[i].startswith("CIRCLE("):
                        circles += 1
            return lines, points, circles

        self.assertEqual(curves(written), curves(original))
        if curves(original):
            self.assertEqual(drawn(written), drawn(original))

    def assertPartsStyled(self, original, written):
        """Each solid and surface model the file gives a colour is written
        with one (dwin, p17: since one product of several shapes was written
        as one, its children's colours had been lost in 12 files). The
        supplemental items are styled as many times, each by one style alone
        (SFA's colour of supplemental geometry reads a single style)"""

        def styles(path):
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", "", f.read())
            ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
            typ = lambda i: ents.get(i, "").split("(")[0]
            items = set()
            for v in ents.values():
                if v.startswith("CONSTRUCTIVE_GEOMETRY_REPRESENTATION("):
                    items |= set(re.findall(r"#(\d+)", v)[:-1])
            parts, supplemental, several = collections.Counter(), 0, 0
            for v in ents.values():
                if not re.match(r"(OVER_RIDING_)?STYLED_ITEM\(", v):
                    continue
                refs = re.findall(r"#(\d+)", v)
                if refs[-1] in items:
                    supplemental += 1
                    psas = [r for r in refs[:-1] if typ(r) == "PRESENTATION_STYLE_ASSIGNMENT"]
                    several += any(len(re.findall(r"#\d+", ents[r])) > 1 for r in psas)
                elif typ(refs[-1]) in ("MANIFOLD_SOLID_BREP", "SHELL_BASED_SURFACE_MODEL"):
                    parts[typ(refs[-1])] += 1
            return parts, supplemental, several

        (a, a_sup, _), (b, b_sup, b_several) = styles(original), styles(written)
        self.assertEqual(sorted((a - b).items()), [])
        self.assertEqual(b_several, 0)
        self.assertGreaterEqual(b_sup, min(a_sup, 1) if a_sup else 0)

    def assertBodyColoured(self, original, written):
        """A solid written in the colour the file gives it, and no more of its
        faces styled than the file styles: a part read with a colour per face
        had kept the default grey as its own, and every face then differed
        from it (STC-09: 163 face colours written where the file has 5)"""

        def colours(path):
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", "", f.read())
            ents = dict(re.findall(r"#(\d+)=([^;]*);", text))

            def fill(ref):
                seen, todo = set(), [ref]
                while todo:
                    k = todo.pop()
                    v = ents.get(k, "")
                    if k in seen or v.startswith("CURVE_STYLE("):
                        continue
                    seen.add(k)
                    if v.startswith("FILL_AREA_STYLE_COLOUR("):
                        c = ents[re.findall(r"#(\d+)", v)[-1]]
                        rgb = re.findall(r"[-0-9.E+]+(?=[,)])", c.split(",", 1)[-1])
                        # In steps of 1/255: what a colour of 8 bits a channel can say
                        return tuple(round(float(x) * 255) for x in rgb) if rgb else c
                    todo += re.findall(r"#(\d+)", v)

            solids, faces = [], 0
            for v in ents.values():
                if not re.match(r"(OVER_RIDING_)?STYLED_ITEM\(", v):
                    continue
                refs = re.findall(r"#(\d+)", v)
                item = refs[-2] if v.startswith("OVER") else refs[-1]
                kind = ents.get(item, "").split("(")[0]
                if kind == "MANIFOLD_SOLID_BREP":
                    solids.append(fill(refs[0]))
                elif kind == "ADVANCED_FACE":
                    faces += 1
            return sorted(solids, key=str), faces

        (a, a_faces), (b, b_faces) = colours(original), colours(written)
        if a:  # FTC-09 styles each face and not the solid
            self.assertEqual(b, a)
        self.assertLessEqual(b_faces, a_faces)

    def assertSameGraphics(self, original, written):
        """The graphics written are the file's, kind and name, each as many
        times -- what SFA's graphic PMI coverage counts -- and so are its
        placeholders and their links (dwin, p15: every file's table equal to
        NIST's). A curve occurrence of the file is a tessellated one written:
        of the same kind and name"""

        def graphics(path):
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", "", f.read())
            ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
            found = collections.Counter()
            for v in ents.values():
                m = re.match(r"(TESSELLATED_ANNOTATION_OCCURRENCE|ANNOTATION_CURVE_OCCURRENCE"
                             r"|ANNOTATION_FILL_AREA_OCCURRENCE|ANNOTATION_OCCURRENCE"
                             r"|ANNOTATION_PLACEHOLDER_OCCURRENCE\w*)"
                             r"\('([^']*)',\([^)]*\),#(\d+)", v)
                if m:
                    k = re.search(r"REPRESENTATION_ITEM\('([^']*)'\)|^\w+\('([^']*)'",
                                  ents.get(m.group(3), ""))
                    kind = (k.group(1) or k.group(2)) if k else ""
                    found[("placeholder" if "PLACEHOLDER" in m.group(1) else "graphic", kind,
                           m.group(2))] += 1
                elif v.startswith("DRAUGHTING_MODEL_ITEM_ASSOCIATION_WITH_PLACEHOLDER("):
                    found[("link", "", "")] += 1
            return found

        a, b = graphics(original), graphics(written)
        self.assertEqual((sorted((a - b).items()), sorted((b - a).items())), ([], []))

    def assertWrittenClean(self, name):
        """What SFA reported of the file written (dwin, p6): no occurrence
        without its name (a label of a counterbore sign, CTC-04), no empty
        semantic text (a note's empty line, STC-07); a graphic on nothing
        associated with nothing, as NIST's CTC-05 has its two regions. And
        (p7) no occurrence named in \\X2\\ alone, which SFA reads as no name,
        nor a callout in no annotation plane"""
        with open(self.written, errors="replace") as f:
            raw = f.read()
        text = re.sub(r"\s+", "", raw)
        self.assertNotIn("TESSELLATED_ANNOTATION_OCCURRENCE('',", text)
        # On the file as written: a text of a space (FTC-06's Text.8, as NIST
        # gives it) is not empty
        self.assertEqual(
            re.findall(r"DESCRIPTIVE_REPRESENTATION_ITEM\(\s*'[^']*'\s*,\s*''\s*\)", raw), []
        )
        names = re.findall(r"TESSELLATED_ANNOTATION_OCCURRENCE\('([^']*)'", text)
        self.assertEqual(
            [n for n in names if not re.search(r"[A-Za-z0-9]", re.sub(r"\\X2\\.*?\\X0\\", "", n))],
            [],
        )
        ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
        planed = set()
        for v in ents.values():
            if v.startswith("ANNOTATION_PLANE("):
                planed.update(re.findall(r"#(\d+)", v))
        self.assertEqual(
            [k for k, v in ents.items() if v.startswith("DRAUGHTING_CALLOUT(") and k not in planed],
            [],
        )
        if "ctc_05" not in name:
            return
        used = collections.Counter()
        for k, v in ents.items():
            if not v.startswith("DRAUGHTING_MODEL_ITEM_ASSOCIATION"):
                used.update(set(re.findall(r"#(\d+)", v)))
        empty = []
        for k, v in ents.items():
            m = re.match(r"DRAUGHTING_MODEL_ITEM_ASSOCIATION\('[^']*','[^']*',#(\d+),", v)
            if m and ents.get(m.group(1), "").startswith("SHAPE_ASPECT(") \
                    and not used[m.group(1)]:
                empty.append(k)
        self.assertEqual(empty, [])

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
                path = edition(os.path.join(nist, name))
                again = self.write_and_read(self.read(path))
                theirs, ours = kinds(path), kinds(self.written)
                if name.endswith("-tg.stp"):
                    self.assertEqual(ours, theirs)
                else:
                    self.assertEqual(set(ours) - set(theirs), set())
                    self.assertIn("label", ours)
                # Of those read with a graphic: a composite frame's lower segment
                # is drawn in its upper's, written with none
                kept = [c.ImportedGraphicType for c in again.Objects
                        if hasattr(c, "ImportedGraphicType") and not c.ImportedGraphic.isNull()]
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
        # One text, as the file has it: its lines broken as the file breaks them
        with open(self.written, errors="replace") as f:
            written = re.sub(r"\s+", "", f.read())
        self.assertIn("SPECIFIED):\\X\\0A", written)

    def test_names_whatever_the_locale(self):
        """A graphic's name is ASCII under a locale whose letters include
        the bytes of UTF-8 (Windows' cp1252, dwin, SFA p8): CTC-04's label
        of a counterbore sign alone named 'Text.13', not '\\X2\\2334\\X0\\'.
        Skipped where no such locale is installed"""
        import locale

        before = locale.setlocale(locale.LC_CTYPE)
        for name in (".1252", "en_US.ISO-8859-1", "en_US.iso88591", "de_DE.ISO-8859-1"):
            try:
                locale.setlocale(locale.LC_CTYPE, name)
                break
            except locale.Error:
                continue
        else:
            self.skipTest("no Latin-1 locale")
        self.addCleanup(locale.setlocale, locale.LC_CTYPE, before)
        doc = self.read(os.path.join(os.environ["FC_PMI_NIST"], "nist_ctc_04_asme1_ap242-e1.stp"))
        self.write_and_read(doc)
        self.assertWrittenClean("nist_ctc_04_asme1_ap242-e1.stp")

    def test_a_placeholder_written_again(self):
        """An annotation placeholder (AP242 ed2: where a reader that draws PMI
        itself puts its text) is kept as its kind and written again, a callout
        of its own linked to its PMI: STC-07's kinds and names, each as the
        file has it -- what SFA counts its graphics by (dwin, p12: 'general
        tolerance' 1 for 7, 'note' none for 2)"""
        nist = os.path.join(os.environ["FC_PMI_NIST"], "nist_stc_07_asme1_ap242-e3.stp")

        def kinds(path):
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", "", f.read())
            sets = dict(re.findall(r"#(\d+)=GEOMETRIC_SET\('([^']*)'", text))
            found = re.findall(r"ANNOTATION_PLACEHOLDER_OCCURRENCE\w*\('([^']*)',\([^)]*\),#(\d+),",
                               text)
            return sorted({(sets[s], name) for name, s in found})

        doc = self.read(nist)
        self.assertEqual(sorted({o.ImportedPlaceholder[0] for o in doc.Objects
                                 if getattr(o, "ImportedPlaceholder", [])}),
                         ["datum", "general tolerance", "linear dimension", "note"])
        self.write_and_read(doc)
        self.assertEqual(kinds(self.written), kinds(nist))
        self.assertEqual(len(kinds(nist)), 22)
        # A file with placeholders says it is of AP242's second edition, where
        # they are from (dwin, p13: SFA tells off one identified as the first)
        self.assertIn("442 3 1 4", schema_of(self.written))

    def test_a_notes_words_beside_the_geometry_it_is_on(self):
        """A graphic linked both to the geometry it is on and to its words (a
        'semantic text') is read with both: CTC-02's general notes, on the
        product itself, have their words -- read by the first link alone, they
        were lost, and with them the note's placeholder, which had nothing to
        be linked by (dwin, p13: 45 for 46)"""
        nist = os.path.join(os.environ["FC_PMI_NIST"], "nist_ctc_02_asme1_ap242-e2.stp")

        def placeholders(path):
            with open(path, errors="replace") as f:
                return len(re.findall(r"=\s*ANNOTATION_PLACEHOLDER_OCCURRENCE", f.read()))

        doc = self.read(nist)
        general = [n for n in objects(doc, "Note") if n.Label == "Text.1"]
        self.assertEqual(len(general), 1)
        self.assertTrue(general[0].Text[0].startswith("NOTES (UNLESS OTHERWISE SPECIFIED)"))
        self.assertGreater(len(general[0].Text), 5)
        self.assertEqual(list(general[0].ImportedPlaceholder), ["note", "Text.1"])
        again = self.write_and_read(doc)
        self.assertEqual(placeholders(self.written), placeholders(nist))
        back = [n for n in objects(again, "Note") if n.Label == "Text.1"]
        self.assertEqual([list(n.Text) for n in back], [list(general[0].Text)])
        # As SFA has it (dwin, p14). A line break in the 8-bit directive, as
        # NIST's: in UTF-16 the text counts as Unicode. And a file of the
        # second edition -- one with placeholders -- connects each view's
        # camera to its draughting model by a model_geometric_view, the lack
        # of which is a syntax error there
        with open(self.written, errors="replace") as f:
            raw = f.read()
        self.assertIn("\\X\\0A", raw)
        self.assertNotIn("\\X2\\000A", raw)
        with open(nist, errors="replace") as f:
            views = f.read().count("MODEL_GEOMETRIC_VIEW(")
        text = re.sub(r"\s+", "", raw)
        ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
        linked = [re.findall(r"#(\d+)", v) for v in ents.values()
                  if v.startswith("MODEL_GEOMETRIC_VIEW(")]
        self.assertEqual(len(linked), views)
        self.assertEqual(len(linked), len(objects(again, "View")))
        for camera, model in linked:
            self.assertTrue(ents[camera].startswith("CAMERA_MODEL_D3"), ents[camera][:40])
            self.assertIn("DRAUGHTING_MODEL", ents[model][:60])
            self.assertIn("#" + camera, ents[model])

    def test_a_line_break_said_is_no_word(self):
        """FTC-08 gives its general note's lines as items with one of '/n'
        between each two: line breaks said, not words of the note (dwin, p14)"""
        nist = os.path.join(os.environ["FC_PMI_NIST"], "nist_ftc_08_asme1_ap242-e2.stp")
        doc = self.read(nist)
        notes = [n for n in objects(doc, "Note") if n.Text]
        self.assertTrue(notes)
        self.assertEqual([n.Label for n in notes if "/n" in n.Text], [])
        general = [n for n in notes if n.Text[0].startswith("NOTES (UNLESS OTHERWISE")]
        self.assertEqual(len(general), 1)
        self.assertGreater(len(general[0].Text), 5)
        self.write_and_read(doc)
        with open(self.written, errors="replace") as f:
            self.assertNotIn("/n", "".join(re.findall(
                r"DESCRIPTIVE_REPRESENTATION_ITEM\('[^']*','([^']*)'\)", f.read())))

    def test_the_supplemental_geometry_is_read_with_the_pmi(self):
        """The curves a file's PMI is on (FTC-10's, a thread's rims) are read
        with its PMI and not without: a file read as its shapes alone has
        its part alone"""
        nist = os.path.join(os.environ["FC_PMI_NIST"], "nist_ftc_10_asme1_ap242-e2.stp")

        def shapes(doc):
            """The part's shapes: not its supplemental geometry's"""
            return [o.Name for o in doc.Objects
                    if o.isDerivedFrom("Part::Feature") and not o.Shape.isNull()
                    and not supplemental_group(o)]

        doc = self.read(nist)
        with_pmi = shapes(doc)
        groups = [o for o in doc.Objects if getattr(o, "SupplementalGeometry", False)]
        self.assertEqual(len(groups), 1)
        self.assertEqual(len([o for o in groups[0].Group if o.TypeId == "Part::Feature"]), 26)
        params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Import")
        self.addCleanup(params.RemBool, "ReadPMI")
        params.SetBool("ReadPMI", False)
        doc = self.read(nist)
        self.assertEqual([o.Name for o in doc.Objects if o.TypeId.startswith("PMI::")], [])
        self.assertEqual([o for o in doc.Objects if getattr(o, "SupplementalGeometry", False)],
                         [])
        # The curves are their own objects or none: the solid and what else the file has stay
        self.assertEqual(len(shapes(doc)), len(with_pmi))
        self.assertEqual(sum(len(o.Shape.Solids) for o in doc.Objects
                             if o.isDerivedFrom("Part::Feature")), 1)

    def test_a_value_format_of_the_value_written(self):
        """Written in inches, a value's format counts the digits of the value
        as written, not of the millimetres it is kept in: STC-06's 12 in is an
        'NR2 2.2' as NIST's is, not the 'NR2 3.2' of 304.8 (dwin, p12: SFA
        says it 'specifies more digits than are necessary')"""
        nist = os.path.join(os.environ["FC_PMI_NIST"], "nist_stc_06_asme1_ap242-e3.stp")

        def formats(path):
            with open(path, errors="replace") as f:
                return sorted(set(re.findall(r"VALUE_FORMAT_TYPE_QUALIFIER\('([^']*)'", f.read())))

        unit = Part.exportUnits()["write.step.unit"]
        self.addCleanup(Part.exportUnits, unit)
        Part.exportUnits("INCH")
        self.write_and_read(self.read(nist))
        self.assertEqual(formats(self.written), formats(nist))
        self.assertIn("NR2 2.2", formats(self.written))

    def test_a_graphic_by_its_placeholder(self):
        """A graphic linked to what it presents by an association with a
        placeholder (AP242 ed2), its only semantic link in FTC-06 and STC-08:
        each dimension has its graphic, not a note of its own beside it. And
        where a named association gives the graphic, the placeholder's (a
        callout of the placeholder alone) does not take its place: STC-06. A
        graphic is a PMI's only when that PMI is read and holds it. A
        tolerance of no geometry is of the whole part, all over it ('Geometrical
        Tolerance.1' of FTC-06 and STC-08, a general profile on a shape aspect
        that names no face): read as one, on a feature that is the part, with
        its graphic (dwin, p12: one profile short)"""
        nist = os.environ["FC_PMI_NIST"]
        general = "Geometrical Tolerance.1"
        for name, notes, whole in (("nist_ftc_06_asme1_ap242-e2.stp", 7, 1),
                                   ("nist_stc_06_asme1_ap242-e3.stp", 2, 0),
                                   ("nist_stc_08_asme1_ap242-e3.stp", 3, 1)):
            with self.subTest(name):
                doc = self.read(os.path.join(nist, name))
                dims = objects(doc, "Dimension")
                self.assertTrue(dims)
                self.assertEqual([d.Label for d in dims if d.ImportedGraphic.isNull()], [])
                self.assertEqual(len(objects(doc, "Note")), notes)
                of_all = [t for t in objects(doc, "GeometricTolerance")
                          if t.Feature.Kind == "WholePart"]
                self.assertEqual([(t.Characteristic, t.ImportedGraphicName, t.Condition,
                                   t.ImportedGraphic.isNull()) for t in of_all],
                                 [("ProfileOfSurface", general, "Ok", False)] * whole)
        # A placeholder to a PMI not read, or read with no graphic, takes no graphic away:
        # CTC-03's datum feature 6, which no tolerance cites, stays a graphic alone (dwin, p9)
        doc = self.read(os.path.join(nist, "nist_ctc_03_asme1_ap242-e2.stp"))
        self.assertIn("Simple Datum.6", [n.Label for n in objects(doc, "Note")])

    def test_a_placeholder_is_of_the_graphic_of_its_name(self):
        """STC-07 presents 32 radii by one graphic, '32X R', named 'Linear
        Size.13': 31 of them are linked only to a placeholder of that name,
        where a reader writes the PMI -- all 32 at the one place, the
        graphic's. The radius the graphic is linked to is linked to four more
        (its oriented dimensions): each of the five is kept, none lost. The
        '32X R' is the 31's: the first of them holds it, the others draw
        nothing of their own, and are written linked to it, so SFA counts one
        linear dimension, not 31 radial ones (dwin, p10). One edited is
        drawn, the graphic stays"""
        doc = self.read(os.path.join(os.environ["FC_PMI_NIST"],
                                     "nist_stc_07_asme1_ap242-e3.stp"))
        notes = {n.Label: n for n in objects(doc, "Note")}
        for name in ("Oriented Linear Dimension.2", "Oriented Linear Dimension.3",
                     "Oriented Linear Dimension.4"):
            self.assertIn(name, notes)
            self.assertFalse(notes[name].ImportedGraphic.isNull(), name)
        self.assertNotIn("Linear Size.13", notes)
        held = [d for d in objects(doc, "Dimension") if d.ImportedGraphicName == "Linear Size.13"]
        self.assertEqual(len(held), 31)
        hosts = [d for d in held if not d.ImportedGraphic.isNull()]
        self.assertEqual(len(hosts), 1)
        host = hosts[0]
        self.assertFalse(host.Presentation.isNull())
        alone = [d for d in held if d.ImportedGraphic.isNull()]
        self.assertEqual(len(alone), 30)
        for d in alone:
            self.assertEqual(d.Type, "Radius")
            self.assertTrue(d.UseImported)
            self.assertEqual(d.ImportedGraphicType, "linear dimension")
            self.assertEqual(d.ImportedWith, host, d.Label)
            self.assertTrue(d.Presentation.isNull(), d.Label)
        # The others with a graphic are named as the file names them too
        drawn = [c for c in doc.Objects if getattr(c, "UseImported", False)
                 and not c.ImportedGraphic.isNull()]
        self.assertTrue(drawn)
        self.assertEqual([c.Label for c in drawn if not c.ImportedGraphicName], [])
        # Edited: ours, drawn and named as ours; the graphic of the rest stays
        alone[0].Upper = alone[0].Upper.Value + 0.1
        doc.recompute()
        self.assertFalse(alone[0].UseImported)
        self.assertFalse(alone[0].Presentation.isNull())
        self.assertTrue(host.UseImported)
        self.assertTrue(alone[1].UseImported)
        again = self.write_and_read(doc)
        with open(self.written, errors="replace") as f:
            text = re.sub(r"\s+", "", f.read())
        ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
        kinds = collections.Counter()
        callout = None
        for k, v in ents.items():
            m = re.match(r"TESSELLATED_ANNOTATION_OCCURRENCE\('([^']*)',\(#\d+\),#(\d+)\)", v)
            if m:
                kinds[(m.group(1), re.match(r"\w+\('([^']*)'", ents[m.group(2)]).group(1))] += 1
            # The graphic's callout, not a placeholder's of that name
            m = re.match(r"DRAUGHTING_CALLOUT\('LinearSize.13',\(#(\d+)", v)
            if m and ents[m.group(1)].startswith("TESSELLATED_ANNOTATION_OCCURRENCE("):
                callout = k
        self.assertEqual(kinds[("LinearSize.13", "lineardimension")], 1)
        self.assertEqual(kinds[(re.sub(r"\s", "", alone[0].Label), "radialdimension")], 1)
        # The one that holds it and the 29 drawn in it, each linked to the one
        # callout as what it presents
        links = [v for v in ents.values()
                 if v.startswith("DRAUGHTING_MODEL_ITEM_ASSOCIATION('PMIrepresentationto")
                 and v.endswith(",#%s)" % callout)]
        self.assertEqual(len(links), 30)
        # Read again: the 30 hold that graphic, one drawing it
        back = [d for d in objects(again, "Dimension")
                if d.ImportedGraphicName == "Linear Size.13"]
        self.assertEqual(len(back), 30)
        self.assertEqual(len([d for d in back if d.Presentation.isNull()]), 29)
        self.assertWrittenClean("nist_stc_07_asme1_ap242-e3.stp")

    def test_composite_frames_as_the_file_has_them(self):
        """A composite frame's lower segment is related to its upper by a
        geometric_tolerance_relationship 'composite', in 11 of the files: each
        read as its upper's lower segment, of its characteristic. NIST draws
        both in the upper's graphic: while that is drawn the lower draws
        nothing of its own. Written again, related as read"""
        nist = os.environ["FC_PMI_NIST"]
        for name, count in (("nist_ctc_04_asme1_ap242-e1.stp", 2),
                            ("nist_ftc_06_asme1_ap242-e2.stp", 4),
                            ("nist_ftc_07_asme1_ap242-e2.stp", 6),
                            ("nist_ftc_08_asme1_ap242-e2.stp", 6),
                            ("nist_ftc_09_asme1_ap242-e1.stp", 2),
                            ("nist_ftc_10_asme1_ap242-e2.stp", 3),
                            ("nist_stc_06_asme1_ap242-e3.stp", 4),
                            ("nist_stc_07_asme1_ap242-e3.stp", 6),
                            ("nist_stc_08_asme1_ap242-e3.stp", 4),
                            ("nist_stc_09_asme1_ap242-e3.stp", 2),
                            ("nist_stc_10_asme1_ap242-e2.stp", 3)):
            with self.subTest(name):
                doc = self.read(edition(os.path.join(nist, name)))
                lowers = [g for g in objects(doc, "GeometricTolerance") if g.Composite]
                self.assertEqual(len(lowers), count)
                for g in lowers:
                    upper = g.Composite
                    self.assertEqual(upper.Characteristic, g.Characteristic, g.Label)
                    self.assertFalse(upper.Composite)
                    self.assertNotEqual(g.Condition, "Invalid", (g.Label, g.ConditionMessage))
                    if upper.UseImported and not upper.ImportedGraphic.isNull():
                        self.assertTrue(g.UseImported, g.Label)
                        self.assertTrue(g.Presentation.isNull(), g.Label)
                again = self.write_and_read(doc)
                with open(self.written, errors="replace") as f:
                    text = re.sub(r"\s+", "", f.read())
                self.assertEqual(text.count("=GEOMETRIC_TOLERANCE_RELATIONSHIP('composite'"),
                                 count)
                back = [g for g in objects(again, "GeometricTolerance") if g.Composite]
                self.assertEqual(len(back), count)

    def test_features_named_as_stc_07_names_them(self):
        """STC-07 names its holes' surfaces through an item identified
        representation usage, a set of faces, beside a 'semantic text'
        property; its positions are on centres of symmetry of those, an axis
        drawn in its supplemental geometry, grouped as patterns. Each is read:
        the positions on the holes' faces, the profile on datum feature A's
        surfaces, the flatness per unit area. Datums B and C are holes that
        are their own diameters (dimensional_size_with_datum_feature): each a
        datum on its faces and a diameter. All 22 of the file's tolerances"""
        doc = self.read(os.path.join(os.environ["FC_PMI_NIST"],
                                     "nist_stc_07_asme1_ap242-e3.stp"))
        gts = objects(doc, "GeometricTolerance")
        kinds = collections.Counter(g.Characteristic for g in gts)
        self.assertEqual(len(gts), 22, kinds)
        self.assertEqual(kinds["Position"], 9, kinds)
        self.assertEqual(kinds["ProfileOfSurface"], 10, kinds)
        for g in gts:
            self.assertEqual(g.Condition, "Ok", (g.Label, g.ConditionMessage))
            subs = [s for r in g.Feature.References for s in r[1]]
            self.assertTrue(subs and all(s.startswith("Face") for s in subs), (g.Label, subs))
        letters = sorted(d.Letter for d in objects(doc, "Datum"))
        self.assertEqual(letters, ["A", "B", "C", "D", "E"])
        for d in objects(doc, "Datum"):
            self.assertTrue(d.Feature and d.Feature.References, d.Letter)
        holes = [d for d in objects(doc, "Dimension")
                 if d.Type == "Diameter" and abs(d.Nominal.Value - 6.35) < 1e-6]
        self.assertGreaterEqual(len(holes), 2)

    def test_a_tolerance_per_unit(self):
        """A flatness per unit area -- any 0.25 x 0.25 in of the face (CTC-03),
        any circle of 1 in (FTC-07, STC-07) -- and a straightness per unit
        length (FTC-10, STC-10): read with its unit, written and read again
        the same"""
        nist = os.environ["FC_PMI_NIST"]
        for name, kind, basis, size, size2 in PER_UNIT:
            with self.subTest(name):
                doc = self.read(os.path.join(nist, name))

                def per_unit(doc):
                    return sorted((g.Characteristic, g.UnitBasis, round(g.UnitSize.Value, 4),
                                   round(g.UnitSize2.Value, 4))
                                  for g in objects(doc, "GeometricTolerance")
                                  if g.UnitBasis != "None")

                self.assertEqual(per_unit(doc), [(kind, basis, size, size2)])
                gt = [g for g in objects(doc, "GeometricTolerance") if g.UnitBasis != "None"][0]
                self.assertEqual(gt.Condition, "Ok", gt.ConditionMessage)
                # With its graphic (CTC-03's was lost in p9, dwin)
                self.assertFalse(gt.ImportedGraphic.isNull())
                self.assertEqual(per_unit(self.write_and_read(doc)), [(kind, basis, size, size2)])

    def test_a_holes_depth(self):
        """A 'thickness' of a hole is its depth: STC-06's four (a hole, a
        pattern of four, two counterbores given on their floors alone),
        STC-10's eight (counterbores to their floors, holes to their drill
        points, patterns of them), read as the file gives them, measured Ok,
        written and read again the same. Each is drawn in its hole's graphic:
        STC-06 presents a hole's diameter and depth by one, STC-10 gives a
        depth none of its own (dwin, SFA p11)"""
        nist = os.environ["FC_PMI_NIST"]
        for name, want in DEPTHS:
            with self.subTest(name):
                doc = self.read(os.path.join(nist, name))

                def depths(doc):
                    return sorted(round(d.Nominal.Value, 4) for d in objects(doc, "Dimension")
                                  if d.Type == "Depth")

                self.assertEqual(depths(doc), want)
                for d in objects(doc, "Dimension"):
                    if d.Type == "Depth":
                        self.assertEqual(d.Condition, "Ok", (d.Label, d.ConditionMessage))
                        self.assertTrue(d.ImportedWith, d.Label)
                        self.assertIn(d.ImportedWith.Type, ("Diameter", "Radius"), d.Label)
                        self.assertTrue(d.Presentation.isNull(), d.Label)
                self.assertEqual(depths(self.write_and_read(doc)), want)

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

            def on_solid(obj, sub):
                if not sub:
                    # The whole part
                    return bool(obj.Shape.Solids)
                el = obj.Shape.getElement(sub)
                return any(el.isPartner(x) for solid in obj.Shape.Solids
                           for x in (solid.Faces if sub.startswith("Face") else solid.Edges))

            for o in objects(doc, "Dimension") + objects(doc, "GeometricTolerance"):
                feats = o.Features if hasattr(o, "Features") else [o.Feature]
                refs = [(r[0], sub) for f in feats if f for r in f.References for sub in r[1]]
                # surfaces beside the solid, on a PMI on the solid (FTC-10's extracted
                # surfaces, beside the solid's faces they copy), are not curves
                solid = [on_solid(obj, sub) for obj, sub in refs]
                if all(solid) or any(solid) and all(
                        ok or obj.Shape.Faces for ok, (obj, sub) in zip(solid, refs)):
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


    def test_holes_as_the_file_defines_them(self):
        """A file's hole features (AP242 ed3: NIST STC-09 -e4's 8 basic_round_hole
        of 25 occurrences) read, each occurrence a hole on its faces, with its
        definition's diameter, tolerance and whether it goes through, and where
        it is; written again as many, and read back the same (SFA's 'This file
        contains: Holes', dwin p20)"""
        path = edition(os.path.join(os.environ["FC_PMI_NIST"],
                                    "nist_stc_09_asme1_ap242-e4.stp"))

        def occurrences(path):
            with open(path, errors="replace") as f:
                text = f.read()
            return (len(re.findall(r"BASIC_ROUND_HOLE_OCCURRENCE\s*\(", text)),
                    len(re.findall(r"BASIC_ROUND_HOLE\s*\(", text)))

        want, defs = occurrences(path)
        if not want:
            self.skipTest("this edition of STC-09 defines no holes")
        doc = self.read(path)

        def holes(doc):
            return sorted((h.OccurrenceName, h.DefinitionName, round(h.Diameter.Value, 4),
                           round(h.DiameterLower.Value, 4), round(h.DiameterUpper.Value, 4),
                           h.Through, tuple(round(x, 3) for x in h.Position.Base))
                          for h in objects(doc, "Hole"))

        read = holes(doc)
        self.assertEqual(len(read), want)
        self.assertEqual(len({h[1] for h in read}), defs)
        for h in objects(doc, "Hole"):
            self.assertTrue(h.Feature and h.Feature.References, h.Label)
            self.assertEqual(h.Feature.Condition, "Ok", h.Label)
        again = self.write_and_read(doc)
        self.assertEqual(occurrences(self.written), (want, defs))
        self.assertEqual(holes(again), read)

    def test_uuids_kept(self):
        """The UUIDs a file identifies its product, GD&T and holes by (AP242
        ed4 uuid_attribute: NIST STC-09 -e4's v5_uuid_attribute) kept on their
        objects and written again, one each, none twice; its faces' and edges'
        are not kept (docs/GDandT.md sec 10.26; SFA's 'contains UUIDs')"""
        path = edition(os.path.join(os.environ["FC_PMI_NIST"],
                                    "nist_stc_09_asme1_ap242-e4.stp"))
        with open(path, errors="replace") as f:
            if "UUID_ATTRIBUTE" not in f.read():
                self.skipTest("this edition of STC-09 has no UUIDs")

        def kept(doc):
            return {(o.TypeId, v[1]) for o in doc.Objects
                    for v in [getattr(o, "StepUUID", None)] if v}

        doc = self.read(path)
        read = kept(doc)
        types = {t for t, _ in read}
        for t in ("PMI::Hole", "PMI::GeometricTolerance", "PMI::Dimension"):
            self.assertIn(t, types)
        again = self.write_and_read(doc)
        with open(self.written, errors="replace") as f:
            text = re.sub(r"\s+", "", f.read())
        ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
        targets = collections.Counter()
        for v in ents.values():
            if re.match(r"\w*UUID_ATTRIBUTE\(", v):
                targets.update(re.findall(r"#(\d+)", v))
        self.assertEqual(sum(targets.values()), len(read))
        self.assertEqual([t for t, n in targets.items() if n > 1], [])
        self.assertEqual(kept(again), read)

    def test_a_products_attributes_and_validation(self):
        """A product's user defined attributes (CTC-03's 'CAGE Code', CTC-05's
        'FILESIZE', FTC-08's 'PartNumber') are kept on its object and written
        again, each by name; a part the file gives geometric validation
        properties is written with them, computed of its shape (SFA's 'This
        file contains: Properties', dwin p20)"""
        nist = os.environ["FC_PMI_NIST"]

        def properties(path):
            with open(path, errors="replace") as f:
                text = f.read()
            return collections.Counter(
                re.findall(r"PROPERTY_DEFINITION\s*\(\s*'([^']*)'", text))

        def lengths(path):
            """The attributes given as lengths (STC-06's, in inches)"""
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", "", f.read())
            ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
            names = set()
            for value in ents.values():
                if not value.startswith("PROPERTY_DEFINITION_REPRESENTATION("):
                    continue
                pd, rep = re.findall(r"#(\d+)", value)[:2]
                if "'userdefinedattribute'" not in ents[pd]:
                    continue
                for item in re.findall(r"#(\d+)", ents[rep].split("),")[0]):
                    if "LENGTH_MEASURE(" in ents.get(item, ""):
                        names.add(re.findall(r"'([^']*)'", ents[pd])[0])
            return names

        for name in ("nist_ctc_03_asme1_ap242-e2.stp", "nist_ctc_05_asme1_ap242-e1.stp",
                     "nist_ftc_08_asme1_ap242-e4-tg.stp", "nist_stc_06_asme1_ap242-e3.stp"):
            path = edition(os.path.join(nist, name))
            with self.subTest(os.path.basename(path)):
                want = properties(path)
                attributes = {k for k in want if k.strip() and "validation" not in k
                              and k != "semantic text"}
                self.assertTrue(attributes)
                doc = self.read(path)
                kept = set()
                for o in doc.Objects:
                    for prop in ("StepAttributes", "StepIntegerAttributes",
                                 "StepRealAttributes"):
                        kept |= set(getattr(o, prop, {}).keys())
                self.assertEqual(kept, attributes)
                self.write_and_read(doc)
                got = properties(self.written)
                self.assertEqual({k for k in got if k in attributes}, attributes)
                self.assertEqual("geometric validation property" in got,
                                 "geometric validation property" in want)
                # Each described as NIST does (OCCT wrote '$': SetDescription set no flag)
                with open(self.written, errors="replace") as f:
                    described = set(re.findall(
                        r"PROPERTY_DEFINITION\s*\(\s*'([^']*)'\s*,\s*'user defined attribute'",
                        f.read()))
                self.assertEqual(attributes - described, set())
                # Lengths written as lengths (STC-06's 130), in mm as the file is
                self.assertEqual(lengths(self.written), lengths(path))
        self.assertTrue(lengths(path))
        part = [o for o in doc.Objects if "p1" in getattr(o, "StepRealAttributes", {})][0]
        self.assertEqual(part.StepAttributeMeasures["p1"], "LENGTH_MEASURE")
        self.assertAlmostEqual(float(part.StepRealAttributes["p1"]), 1.25 * 25.4)

    def test_what_the_file_hides_is_hidden(self):
        """What a file does not show -- PMI graphics an INVISIBILITY lists
        (STC-08's 29, STC-09's 75), their callouts (FTC-09's 44) and
        placeholders (STC-06's 19), a surface beside the solid whose styles
        it lists (STC-08's, CTC-02's) -- is read hidden and written hidden
        again, each graphic as NIST hides it"""
        nist = os.environ["FC_PMI_NIST"]

        def hidden(path):
            """The annotation occurrences an invisibility lists, and how many
            styles of surface models"""
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", "", f.read())
            ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
            typ = lambda i: ents.get(i, "").split("(")[0]
            name = lambda i: (re.findall(r"'([^']*)'", ents[i]) or [""])[0]
            graphics, surfaces = collections.Counter(), 0
            for v in ents.values():
                if not v.startswith("INVISIBILITY("):
                    continue
                for r in re.findall(r"#(\d+)", v):
                    # A placeholder hidden with its PMI (STC-06's 19), a callout hidden
                    # whole (FTC-09's 44), each by name
                    if "PLACEHOLDER_OCCURRENCE" in typ(r):
                        graphics["placeholder", name(r)] += 1
                    elif typ(r) == "DRAUGHTING_CALLOUT":
                        graphics["callout", name(r)] += 1
                    elif "ANNOTATION" in typ(r) and "OCCURRENCE" in typ(r):
                        graphics["graphic", ""] += 1
                    elif "STYLED_ITEM" in typ(r):
                        refs = re.findall(r"#(\d+)", ents[r])
                        surfaces += typ(refs[-1]) == "SHELL_BASED_SURFACE_MODEL"
            return graphics, surfaces

        for name in ("nist_stc_08_asme1_ap242-e3.stp", "nist_stc_09_asme1_ap242-e4.stp",
                     "nist_ctc_02_asme1_ap242-e2.stp", "nist_ftc_09_asme1_ap242-e1.stp",
                     "nist_stc_06_asme1_ap242-e3.stp"):
            path = edition(os.path.join(nist, name))
            with self.subTest(os.path.basename(path)):
                graphics, surfaces = hidden(path)
                doc = self.read(path)
                if graphics:
                    self.assertTrue([o for o in doc.Objects if o.isDerivedFrom("PMI::Callout")
                                     and not o.Visibility])
                shells = [o for o in doc.Objects if o.TypeId == "Part::Feature"
                          and not o.Shape.Solids and o.Shape.Faces and not supplemental_group(o)]
                # A surface model of its own: hidden whole (STC-09's are one compound
                # with others, of which an object hides no part)
                if surfaces and shells:
                    self.assertFalse(shells[0].Visibility)
                self.write_and_read(doc)
                got_graphics, got_surfaces = hidden(self.written)
                self.assertEqual(got_graphics, graphics)
                if shells:
                    self.assertEqual(bool(got_surfaces), bool(surfaces))

    def test_supplemental_geometry_as_the_file_has_it(self):
        """A part's supplemental geometry -- its constructive geometry
        representations: planes, cylinders, axis systems, sets of curves --
        read as a group each of datum planes, coordinate systems and shapes,
        named as the file names them, and written again as NIST has it: the
        part one product, each group one representation of the same items,
        GD&T on a thread's curves on their set (CTC-04, FTC-06, FTC-10,
        STC-08; docs/GDandT.md sec 10.26)"""
        nist = os.environ["FC_PMI_NIST"]

        def entities(path):
            with open(path, errors="replace") as f:
                text = f.read()
            ents = {}
            for m in re.finditer(r"#(\d+)\s*=\s*(.*?);\s*(?=#\d+\s*=|ENDSEC)", text, re.S):
                ents[m.group(1)] = re.sub(r"\s+", " ", m.group(2))
            return ents

        def typ(ents, i):
            return ents[i].split("(")[0].strip()

        def written(path):
            """Each representation's items, (type, name) sorted; how many
            usages are of its items; the products assembled"""
            ents = entities(path)
            reps = []
            cgr = set()
            for k, v in ents.items():
                if typ(ents, k) == "CONSTRUCTIVE_GEOMETRY_REPRESENTATION":
                    cgr.add(k)
                    refs = re.findall(r"#(\d+)", v)[:-1]
                    reps.append(sorted((typ(ents, i), re.findall(r"'([^']*)'", ents[i])[0])
                                       for i in refs))
            usages = sum(1 for k, v in ents.items()
                         if typ(ents, k) == "GEOMETRIC_ITEM_SPECIFIC_USAGE"
                         and set(re.findall(r"#(\d+)", v)) & cgr)
            nauo = sum(1 for k in ents if typ(ents, k) == "NEXT_ASSEMBLY_USAGE_OCCURRENCE")
            items = set()
            for k in cgr:
                items |= set(re.findall(r"#(\d+)", ents[k])[:-1])
            styled = sum(1 for k, v in ents.items()
                         if "STYLED_ITEM" in typ(ents, k)
                         and (re.findall(r"#(\d+)", v) or [None])[-1] in items)
            return sorted(reps), usages, nauo, styled

        kinds = {"Part::DatumPlane": "PLANE", "Part::LocalCoordinateSystem": "AXIS2_PLACEMENT_3D"}

        def read(doc):
            """Each group's items as written: (type, name) sorted"""
            got = []
            for g in doc.Objects:
                if not getattr(g, "SupplementalGeometry", False):
                    continue
                items = []
                for o in g.Group:
                    kind = kinds.get(o.TypeId)
                    if not kind:
                        kind = ("CYLINDRICAL_SURFACE" if o.Shape.Faces else "GEOMETRIC_CURVE_SET")
                    items.append((kind, o.StepName))
                got.append(sorted(items))
            return sorted(got)

        for name, usages in (("nist_ctc_04_asme1_ap242-e1.stp", 8),
                             ("nist_ftc_06_asme1_ap242-e2.stp", 0),
                             ("nist_ftc_10_asme1_ap242-e2.stp", 6),
                             ("nist_stc_08_asme1_ap242-e3.stp", 0)):
            with self.subTest(name):
                path = edition(os.path.join(nist, name))
                want, nist_usages, _, nist_styled = written(path)
                # The usages of supplemental items an edition has (CTC-04 -e1 8, FTC-10 -e2 6)
                self.assertEqual(bool(nist_usages), bool(usages))
                doc = self.read(path)
                self.assertEqual(read(doc), want)
                again = self.write_and_read(doc)
                reps, got_usages, nauo, styled = written(self.written)
                self.assertEqual(reps, want)
                # Each item drawn in its colour as NIST's are (white: SFA says it may not show)
                self.assertEqual(styled, nist_styled)
                self.assertEqual(nauo, 0, "the part is one product")
                # Each PMI on a thread's curves on their sets: NIST's, or twice where a
                # tolerance and a dimension share them
                self.assertGreaterEqual(got_usages, nist_usages)
                self.assertEqual(bool(got_usages), bool(nist_usages))
                self.assertEqual(read(again), want)
    def test_supplemental_layers_as_the_file_has_them(self):
        """Each item of supplemental geometry on the layer the file puts it on,
        and hidden where the file hides it -- by its layer (FTC-07's 13,
        STC-06's 16) or its style -- read so (StepLayer, StepHidden; a set of
        curves not shown) and written so again, item for item"""
        nist = os.environ["FC_PMI_NIST"]

        def layered(path):
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", "", f.read())
            ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
            items, hidden, layers, styles = [], set(), {}, {}
            for k, v in ents.items():
                if v.startswith("CONSTRUCTIVE_GEOMETRY_REPRESENTATION("):
                    items += re.findall(r"#(\d+)", v)[:-1]
                elif v.startswith("INVISIBILITY("):
                    hidden |= set(re.findall(r"#(\d+)", v))
            for k, v in ents.items():
                if v.startswith("PRESENTATION_LAYER_ASSIGNMENT("):
                    for i in re.findall(r"#(\d+)", v):
                        layers[i] = (tuple(re.findall(r"'([^']*)'", v)[:2]), k in hidden)
                elif v.startswith("STYLED_ITEM("):
                    i = re.findall(r"#(\d+)", v)[-1]
                    styles[i] = styles.get(i, False) or k in hidden
            got = collections.Counter()
            for i in items:
                layer = layers.get(i)
                got[ents[i].split("(")[0], (re.findall(r"'([^']*)'", ents[i]) or [""])[0],
                    layer[0] if layer else None,
                    bool(layer and layer[1]) or bool(styles.get(i))] += 1
            return got

        for name in ("nist_ctc_04_asme1_ap242-e1.stp", "nist_ftc_07_asme1_ap242-e2.stp",
                     "nist_ftc_10_asme1_ap242-e2.stp", "nist_stc_06_asme1_ap242-e3.stp",
                     "nist_stc_09_asme1_ap242-e4.stp"):
            path = edition(os.path.join(nist, name))
            with self.subTest(os.path.basename(path)):
                want = layered(path)
                if not want:  # STC-09 -e3 (2021) has no supplemental geometry
                    continue
                doc = self.read(path)
                hidden = [o for o in doc.Objects if getattr(o, "StepHidden", False)]
                self.assertEqual(len(hidden), sum(n for k, n in want.items() if k[3]))
                for o in hidden:
                    self.assertFalse(o.Visibility)
                self.write_and_read(doc)
                self.assertEqual(layered(self.written), want)

    def test_cited_documents_as_the_file_has_them(self):
        """The documents a part's GD&T cites -- the standards it follows: an
        applied document reference of an additional context of its product
        definition, its role, the product or formation the document is
        equivalent to (FTC-10's 'ASME Y14.41-2003', STC-07's four, CTC-05's
        'ISO 16792'; SFA lists the file's standards by them) -- read
        (StepDocuments) and written again as they were"""
        nist = os.environ["FC_PMI_NIST"]

        def cited(path):
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", " ", f.read())
            ents = dict(re.findall(r"#(\d+)\s*=\s*([^;]*);", text))
            ref = lambda v: re.findall(r"#(\d+)", v)
            words = lambda v: tuple(w.strip() for w in re.findall(r"'([^']*)'", v))
            got = collections.Counter()
            for k, v in ents.items():
                if not v.startswith("APPLIED_DOCUMENT_REFERENCE("):
                    continue
                doc = ref(v)[0]
                role = [words(ents[ref(w)[0]]) for w in ents.values()
                        if w.startswith("ROLE_ASSOCIATION(") and ref(w)[1] == k]
                equal = [ents[ref(w)[1]] for w in ents.values()
                         if w.startswith("DOCUMENT_PRODUCT_EQUIVALENCE(") and ref(w)[0] == doc]
                # A formation's product too: SFA takes the standards from both ids
                equal += [ents[ref(e)[-1]] for e in equal
                          if e.startswith("PRODUCT_DEFINITION_FORMATION(")]
                contexts = [words(ents[ref(ents[i])[0]]) + words(ents[i])
                            for i in ref(v)[1:]]
                got[words(ents[doc]) + words(ents[ref(ents[doc])[-1]]) + words(v),
                    tuple(role), tuple(words(e)[:2] for e in equal), tuple(contexts)] += 1
            return got

        for name in ("nist_ctc_05_asme1_ap242-e1.stp", "nist_stc_07_asme1_ap242-e3.stp",
                     "nist_ftc_07_asme1_ap242-e2.stp", "nist_ftc_10_asme1_ap242-e2.stp",
                     "nist_stc_09_asme1_ap242-e4.stp"):
            path = edition(os.path.join(nist, name))
            with self.subTest(os.path.basename(path)):
                want = cited(path)
                if not want:  # an edition citing none
                    continue
                doc = self.read(path)
                self.assertEqual(sum(len(getattr(o, "StepDocuments", [])) for o in doc.Objects),
                                 sum(want.values()))
                self.write_and_read(doc)
                self.assertEqual(cited(self.written), want)

    def test_view_supplemental_geometry_as_the_file_has_it(self):
        """The supplemental geometry each saved view shows -- a 'supplemental
        geometry subset' mapped into its draughting model, STC-06's twelve --
        read as the view's objects beside the part's solids and written so
        again, item for item, with the part the view maps"""
        nist = os.environ["FC_PMI_NIST"]

        def subsets(path):
            with open(path, errors="replace") as f:
                text = re.sub(r"\s+", "", f.read())
            ents = dict(re.findall(r"#(\d+)=([^;]*);", text))
            ref = lambda v: re.findall(r"#(\d+)", v)
            described = {ref(v)[-1] for v in ents.values()
                         if v.startswith("DESCRIPTION_ATTRIBUTE('supplementalgeometrysubset'")}
            got = {}
            for v in ents.values():
                if "DRAUGHTING_MODEL" not in v:
                    continue
                m = re.search(r"(?:DRAUGHTING_MODEL|REPRESENTATION)\('([^']*)',\(([^)]*)\)", v)
                if not m or not any(ents[i].startswith("CAMERA_MODEL") for i in ref(m.group(2))):
                    continue
                items, shapes = collections.Counter(), 0
                for i in ref(m.group(2)):
                    if not ents[i].startswith("MAPPED_ITEM("):
                        continue
                    rep = ref(ents[ref(ents[i])[0]])[-1]
                    if rep not in described:
                        shapes += 1
                        continue
                    for j in ref(ents[rep])[:-1]:
                        items[ents[j].split("(")[0],
                              (re.findall(r"'([^']*)'", ents[j]) or [""])[0]] += 1
                got[m.group(1)] = (shapes, sorted(items.items()))
            return got

        path = edition(os.path.join(nist, "nist_stc_06_asme1_ap242-e3.stp"))
        want = subsets(path)
        self.assertEqual(sum(1 for s, i in want.values() if i), 12)
        doc = self.read(path)
        listed = {v.Label: v.Objects for v in objects(doc, "View")}
        for name, (_, items) in want.items():
            if items:
                supplemental = [o for o in listed[name] if not getattr(o, "Shape", None)
                                or not o.Shape.Solids]
                self.assertEqual(len(supplemental), sum(n for _, n in items), name)
        self.write_and_read(doc)
        self.assertEqual(subsets(self.written), want)

    def test_basic_as_the_file_gives_it(self):
        """CTC-04's 75 and 10 are basic -- a 'dimensional note' that is
        'theoretical' -- read so and written so (SFA's [75], [dia10])"""
        nist = os.environ["FC_PMI_NIST"]
        doc = self.read(os.path.join(nist, "nist_ctc_04_asme1_ap242-e1.stp"))

        def basic(d):
            return sorted(round(x.Nominal.Value, 6) for x in objects(d, "Dimension") if x.Basic)

        self.assertEqual(basic(doc), [10.0, 75.0])
        self.assertEqual(basic(self.write_and_read(doc)), basic(doc))
        with open(self.written, errors="replace") as f:
            written = re.sub(r"\s+", "", f.read())
        self.assertEqual(
            written.count("DESCRIPTIVE_REPRESENTATION_ITEM('dimensionalnote','theoretical')"), 2)

    def test_a_fit_as_the_file_gives_it(self):
        """FTC-10's three holes, 3.50 G6: the fit read -- its grade given
        with its variance, 'G6', none of its own -- and written as STEP has a
        class of tolerance, its grade apart, IT6 as 6, read back the same"""
        nist = os.environ["FC_PMI_NIST"]
        doc = self.read(os.path.join(nist, "nist_ftc_10_asme1_ap242-e2.stp"))

        def fits(d):
            return sorted((round(x.Nominal.Value, 6), x.Fit) for x in objects(d, "Dimension")
                          if x.Fit)

        self.assertEqual(fits(doc), [(3.5, "G6")] * 3)
        again = self.write_and_read(doc)
        self.assertEqual(fits(again), fits(doc))
        with open(self.written, errors="replace") as f:
            written = re.sub(r"\s+", "", f.read())
        self.assertEqual(written.count("LIMITS_AND_FITS('G','hole','6',"), 3)

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
        # circle (its K), rectangles (CTC-05's C, D), lines (FTC-06's J, K),
        # circular curves (its G, H: a circle that is no area), and FTC-10's
        # areas, which are datums with a feature of their own too
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
                    # On a face of the part, its centre on it; an area is
                    # its faces, of no centre
                    self.assertTrue(t.Feature, t.Label)
                    if t.TargetType == "Area":
                        continue
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
        self.addCleanup(shutil.rmtree, self.dir, True)
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

    def test_what_an_audit_found_lost(self):
        """Written and read again (audit, 2026-11-23): deviations keep their
        signs -- a shaft's 6 -0.01/-0.03 was read a range, a +0.03/+0.01 came
        back +0.03/-0.01; limits stay where they are; a profile all around is
        all around; a composite frame's lower segment is its upper's"""
        doc = self.doc
        pset = doc.getObject("PMI")

        def add(kind, name, **props):
            o = doc.addObject("PMI::" + kind, name)
            pset.addObject(o)
            for k, v in props.items():
                setattr(o, k, v)
            return o

        position = doc.getObject("Position")
        add("GeometricTolerance", "Lower", Characteristic="Position", Value=0.02,
            ZoneShape="Diameter", Feature=position.Feature, DatumSystem=position.DatumSystem,
            Composite=position)
        add("GeometricTolerance", "Around", Characteristic="ProfileOfSurface", Value=0.3,
            Feature=doc.getObject("Flatness").Feature, DatumSystem=position.DatumSystem,
            Modifiers=["AllAround"])
        dia = doc.getObject("Dia")
        for n, (upper, lower, limits) in enumerate(((-0.01, -0.03, False), (0.03, 0.01, False),
                                                    (0.05, 0.0, True))):
            with self.subTest(upper=upper, lower=lower, limits=limits):
                dia.Upper, dia.Lower, dia.Limits = upper, lower, limits
                doc.recompute()
                self.assertEqual([o.Name for o in doc.Objects
                                  if getattr(o, "Condition", "Ok") != "Ok"], [])
                back = self.export(bool(n % 2))[1]
                got = [d for d in objects(back, "Dimension") if d.Type == "Diameter"]
                self.assertEqual(len(got), 1)
                self.assertAlmostEqual(got[0].Nominal.Value, 6, 6)
                self.assertAlmostEqual(got[0].Upper.Value, upper, 6)
                self.assertAlmostEqual(got[0].Lower.Value, lower, 6)
                self.assertEqual(got[0].Limits, limits)
                tols = objects(back, "GeometricTolerance")
                lowers = [t for t in tols if t.Composite]
                self.assertEqual([round(t.Value.Value, 6) for t in lowers], [0.02])
                self.assertAlmostEqual(lowers[0].Composite.Value.Value, 0.1, 6)
                self.assertEqual([list(t.Modifiers) for t in tols
                                  if t.Characteristic == "ProfileOfSurface"], [["AllAround"]])

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

    def test_a_tolerance_of_the_whole_part(self):
        """A tolerance all over the part -- a general profile, NIST's
        'Geometrical Tolerance.1' -- is on a feature that is the whole part,
        no face of it named: written on the product's shape itself, with no
        geometry, and read back so"""
        import PMI

        pset = self.doc.PMI
        solid = self.body.SolidBodies[0]
        whole = PMI.feature(pset, solid, [], "WholePart")
        self.assertEqual(whole.Kind, "WholePart")
        self.assertIs(PMI.feature(pset, solid, [], "WholePart"), whole)
        with self.assertRaises(Exception):
            PMI.feature(pset, solid, ["Face1"], "WholePart")
        gt = self.doc.addObject("PMI::GeometricTolerance", "General")
        pset.addObject(gt)
        gt.Characteristic, gt.Value = "ProfileOfSurface", 0.5
        gt.Feature, gt.DatumSystem = whole, self.doc.DRF
        self.doc.recompute()
        self.assertEqual(whole.Condition, "Ok", whole.ConditionMessage)
        self.assertEqual(len(whole.Shape.Faces), len(solid.Shape.Faces))
        self.assertEqual(gt.Condition, "Ok", gt.ConditionMessage)
        path, doc = self.export(False)
        with open(path) as f:
            text = re.sub(r"\s+", "", f.read())
        shapes = re.findall(r"#(\d+)=PRODUCT_DEFINITION_SHAPE\(", text)
        on = re.findall(r"GEOMETRIC_TOLERANCE\('[^']*','[^']*',#\d+,#(\d+)\)", text)
        self.assertEqual(len([t for t in on if t in shapes]), 1, (on, shapes))
        back = [t for t in objects(doc, "GeometricTolerance") if t.Feature.Kind == "WholePart"]
        self.assertEqual([(t.Characteristic, t.Value) for t in back],
                         [("ProfileOfSurface", 0.5)])
        self.assertEqual(list(back[0].DatumSystem.Compartments), ["A", "B", "C(M)"])
        self.assertEqual(len(objects(doc, "GeometricTolerance")),
                         len(objects(self.doc, "GeometricTolerance")))
        self.assertSameGdt(self.doc, doc)

    def test_two_parts_each_with_a_datum_a(self):
        """Two parts written to one file, each with its own datum A and a
        tolerance to it: each part's A is written with its own feature and
        read back on its own part, its tolerance citing it (by the letter
        alone the second part's was taken for the first's, and lost)"""
        import PMI
        from PMITests.TestModel import BOTTOM, TOP, face_at

        other = self.doc.addObject("Part::Box", "Other")
        other.Length, other.Width, other.Height = 30, 30, 30
        other.Placement = App.Placement(V(-200, 0, 0), App.Rotation())
        self.doc.recompute()
        oset = PMI.getSet(other, True)
        bottom = PMI.feature(oset, other, [face_at(other.Shape, V(-185, 15, 0))])
        top = PMI.feature(oset, other, [face_at(other.Shape, V(-185, 15, 30))])
        datum = self.doc.addObject("PMI::Datum", "OtherA")
        oset.addObject(datum)
        datum.Letter, datum.Feature = "A", bottom
        par = self.doc.addObject("PMI::GeometricTolerance", "OtherParallel")
        oset.addObject(par)
        par.Characteristic, par.Value, par.Feature = "Parallelism", 0.2, top
        par.DatumSystem = PMI.frame(oset, ["A"])
        self.doc.recompute()
        self.assertEqual(par.Condition, "Ok", par.ConditionMessage)
        path = os.path.join(self.dir, "two-parts.step")
        Import.export([self.body, other], path)
        doc = App.newDocument("PMITwoParts")
        self.addCleanup(App.closeDocument, doc.Name)
        Import.insert(path, doc.Name)
        doc.recompute()
        sets = [s for s in objects(doc, "AnnotationSet") if s.Group]
        self.assertEqual(len(sets), 2)
        for s in sets:
            letters = sorted(o.Letter for o in s.Group if o.TypeId == "PMI::Datum")
            self.assertIn("A", letters, s.Label)
            for o in s.Group:
                if o.TypeId in ("PMI::Datum", "PMI::GeometricTolerance"):
                    self.assertEqual(o.Condition, "Ok", (o.Label, o.ConditionMessage))
        self.assertEqual(sorted(len([o for o in s.Group if o.TypeId == "PMI::Datum"])
                                for s in sets), [1, 3])
        pars = [t for t in objects(doc, "GeometricTolerance")
                if t.Characteristic == "Parallelism"]
        self.assertEqual(len(pars), 1)
        self.assertEqual(list(pars[0].DatumSystem.Compartments), ["A"])
        mine = [o for o in pars[0].getParentGroup().Group if o.TypeId == "PMI::Datum"]
        self.assertEqual([d.Letter for d in mine], ["A"])
        self.assertSameGdt(self.doc, doc)

    def test_a_file_read_without_its_pmi(self):
        """Preferences, Import-Export, STEP, 'Import PMI': off, a file is
        read as its shapes alone -- no annotation, and none of the
        supplemental geometry its PMI is on"""
        path, doc = self.export(False)
        self.assertTrue(objects(doc, "AnnotationSet"))
        params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Import")
        self.addCleanup(params.RemBool, "ReadPMI")
        params.SetBool("ReadPMI", False)
        plain = App.newDocument("PMINotRead")
        self.addCleanup(App.closeDocument, plain.Name)
        Import.insert(path, plain.Name)
        plain.recompute()
        self.assertEqual([o.Name for o in plain.Objects if o.TypeId.startswith("PMI::")], [])
        self.assertTrue([o for o in plain.Objects if o.isDerivedFrom("Part::Feature")])

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
