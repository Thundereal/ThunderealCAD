# SPDX-License-Identifier: LGPL-2.1-or-later
"""A file this build has migrated and saved does not ask for the migration
again; one whose migration is still owed does.

The smoke corpus, 2026-11-27 (finding A5): 37 of 105 documents, opened,
migrated, saved and opened again, asked for the migration recompute again.
A shape read from a file written before element maps is marked "?", and
the save wrote the "?" back for every shape the recompute had not replaced:
a Part::Feature, which has no execute; a datum, or a Draft object, that
leaves an unchanged shape alone. Every open asked again, though nothing
would change. The "?" is now written back only while the object still
carries MigrationPending: a recompute that ran and succeeded clears it;
answering No, recomputing only some objects, or a recompute that fails
leaves it.
"""

import os
import re
import shutil
import tempfile
import unittest
import zipfile

import FreeCAD
import Part


def pending(doc):
    return sorted(o.Name for o in doc.Objects if "MigrationPending" in o.State)


def versions(path):
    """Each object's saved ElementMap attribute"""
    with zipfile.ZipFile(path) as z:
        xml = z.read("Document.xml").decode("utf-8")
    data = xml[xml.index("<ObjectData"):]
    out = {}
    for m in re.finditer(r'<Object name="([^"]+)"(.*?)</Object>', data, re.S):
        v = re.search(r'<Part [^>]*ElementMap="([^"]*)"', m.group(2))
        if v:
            out[m.group(1)] = v.group(1)
    return out


class MigrationVersionTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="migration")
        doc = FreeCAD.newDocument("Migration")
        doc.SaveSchemaVersion = 4  # each object's shape element of its own
        doc.BlobCompression = "Deflate"
        plain = doc.addObject("Part::Feature", "Plain")  # no execute: nothing remakes it
        plain.Shape = Part.makeBox(1, 2, 3)
        doc.addObject("Part::Box", "Prim")
        broken = doc.addObject("Part::Cut", "Broken")  # no tool: its recompute fails
        broken.Base = doc.Prim
        doc.recompute()
        made = os.path.join(self.dir, "made.FCStd")
        doc.saveAs(made)
        FreeCAD.closeDocument(doc.Name)
        # What a file of a release before element maps says of its shapes
        self.legacy = os.path.join(self.dir, "legacy.FCStd")
        with zipfile.ZipFile(made) as zin, zipfile.ZipFile(self.legacy, "w") as zout:
            for item in zin.infolist():
                data = zin.read(item.filename)
                if item.filename == "Document.xml":
                    text = data.decode("utf-8")
                    text = re.sub(r' HasherIndex="\d+"', "", text)
                    text = re.sub(r' SaveHasher="\d+"', "", text)
                    text = re.sub(r'ElementMap="[^"]*"', 'ElementMap="?"', text)
                    data = text.encode("utf-8")
                zout.writestr(item, data)

    def tearDown(self):
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        shutil.rmtree(self.dir, ignore_errors=True)

    def resaved(self, name, before_save):
        doc = FreeCAD.openDocument(self.legacy)
        self.assertEqual(pending(doc), ["Broken", "Plain", "Prim"], "the legacy file asks")
        before_save(doc)
        path = os.path.join(self.dir, name)
        doc.saveAs(path)
        FreeCAD.closeDocument(doc.Name)
        doc = FreeCAD.openDocument(path)
        asked = pending(doc)
        FreeCAD.closeDocument(doc.Name)
        return asked, versions(path)

    def testAMigratedFileDoesNotAskAgain(self):
        asked, saved = self.resaved("migrated.FCStd", lambda doc: doc.recompute())
        # Plain's shape was not replaced -- it has no execute -- and is settled
        self.assertNotEqual(saved["Plain"], "?")
        self.assertNotEqual(saved["Prim"], "?")
        # The one whose migration failed is still owed it, and asks
        self.assertEqual(saved["Broken"], "?")
        self.assertEqual(asked, ["Broken"])

    def testAnsweringNoStillAsksNextTime(self):
        asked, saved = self.resaved("declined.FCStd", lambda doc: None)
        self.assertEqual(asked, ["Broken", "Plain", "Prim"])
        self.assertEqual(set(saved.values()), {"?"})

    def testAPartialRecomputeSettlesOnlyWhatItMade(self):
        asked, saved = self.resaved("partial.FCStd", lambda doc: doc.recompute([doc.Plain]))
        self.assertNotEqual(saved["Plain"], "?")
        self.assertEqual(saved["Prim"], "?")
        self.assertEqual(asked, ["Broken", "Prim"])
