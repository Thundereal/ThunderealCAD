# SPDX-License-Identifier: LGPL-2.1-or-later

"""A Part::Feature's shape contents expanded by the save itself (Feature::beforeSave): the
contents are objects added while the document's objects are being prepared for the save.
Document::Save walked its object array by iterator then, and 42 contents added (NIST STC-09's
compound) moved the array under it: the save crashed."""

import os
import tempfile
import unittest

import FreeCAD as App
import Part


class ShapeContentsSaveTest(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("ShapeContentsSave")
        self.dir = tempfile.mkdtemp(prefix="shapecontents")

    def tearDown(self):
        for name in list(App.listDocuments()):
            if name.startswith("ShapeContentsSave"):
                App.closeDocument(name)

    def test_contents_added_by_the_save(self):
        n = 60
        boxes = [Part.makeBox(1, 1, 1, App.Vector(2 * i, 0, 0)) for i in range(n)]
        feat = self.doc.addObject("Part::Feature", "Comp")
        feat.Shape = Part.makeCompound(boxes)
        self.doc.recompute()
        feat.addProperty("App::PropertyLinkList", "ShapeContents", "ShapeContent")
        path = os.path.join(self.dir, "contents.FCStd")
        self.doc.saveAs(path)
        self.assertEqual(len(feat.ShapeContents), n)
        self.assertTrue(all(len(c.Shape.Faces) == 6 for c in feat.ShapeContents))
        # Each one written: read again, each has its shape from its parent's
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        feat = self.doc.getObject("Comp")
        self.assertEqual(len(feat.ShapeContents), n)
        self.assertTrue(all(len(c.Shape.Faces) == 6 for c in feat.ShapeContents))


if __name__ == "__main__":
    unittest.main()
