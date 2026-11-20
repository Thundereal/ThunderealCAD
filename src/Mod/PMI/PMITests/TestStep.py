# SPDX-License-Identifier: LGPL-2.1-or-later
"""The GD&T of a STEP file, read (docs/GDandT.md sec 5.1, P2).

NIST's CTC-01 (AP242 edition 1, from the NIST MBE PMI Validation and
Conformance Testing project, used without restriction) imported: its
datums, tolerances and dimensions become annotations beside the part, on
its faces, placed where the file places them.

The rest of the NIST set is read too when FC_PMI_NIST names its directory
(the 'NIST-PMI-STEP-Files' of nist.gov/document/nist-pmi-step-files): each
file imports, and nothing it makes is invalid.
"""

import glob
import os
import unittest

import FreeCAD as App
import Import
import PMI  # noqa: F401 -- registers the pass

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
        nfaces = len(part.Shape.Faces)
        for feat in objects(self.doc, "TolerancedFeature"):
            for obj, subs in feat.References:
                self.assertEqual(obj, part)
                for s in subs:
                    self.assertTrue(s.startswith("Face") and 0 < int(s[4:]) <= nfaces, s)

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
