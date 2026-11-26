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

    def test_contents_follow_their_parents_looks(self):
        # docs/GDandT.md sec 10.25, I11 (c): a content is a piece of its
        # parent's shape, drawn as it is drawn there -- then and after any
        # change of the parent's looks -- until it is given a look of its own
        def rgb(obj):
            return {tuple(round(c, 3) for c in m.DiffuseColor[:3]) for m in obj.ShapeAppearance}

        def faces(obj):
            looks = [tuple(round(c, 3) for c in m.DiffuseColor[:3]) for m in obj.ShapeAppearance]
            return looks * len(obj.Shape.Faces) if len(looks) == 1 else looks

        def drawn(owner, box):
            # What the parent draws of one box's six faces
            return faces(owner)[6 * box : 6 * box + 6]

        red, green, blue, yellow = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), (1.0, 1.0, 0.0)
        feat = self.doc.addObject("Part::Feature", "Comp")
        feat.Shape = Part.makeCompound(
            [Part.makeBox(1, 1, 1), Part.makeBox(1, 1, 1, App.Vector(3, 0, 0))]
        )
        feat.ShapeColor = red
        feat.addProperty("App::PropertyLinkList", "ShapeContents", "ShapeContent")
        path = os.path.join(self.dir, "looks.FCStd")
        self.doc.saveAs(path)
        first, second = feat.ShapeContents
        self.assertEqual((rgb(first), rgb(second)), ({red}, {red}))

        # The parent's colour changed: the contents with it
        feat.ShapeColor = green
        self.assertEqual((rgb(first), rgb(second)), ({green}, {green}))

        # One face of the parent's, in the second box: that face of the second content
        # (A material from the list each: one repeated is one object)
        looks = [feat.ShapeAppearance[0] for _ in feat.Shape.Faces]
        mat = looks[8]
        mat.DiffuseColor = blue + (0.0,)
        looks[8] = mat
        feat.ShapeAppearance = looks
        self.assertEqual(rgb(first), {green})
        self.assertEqual(faces(second)[2], blue)
        self.assertEqual(faces(second), drawn(feat, 1))

        # A content given a colour of its own keeps it: the other follows on
        first.ShapeColor = yellow
        self.assertFalse(first.MapFaceColor)
        feat.ShapeColor = red
        self.assertEqual(rgb(first), {yellow})
        self.assertIn(red, drawn(feat, 1))
        self.assertEqual(faces(second), drawn(feat, 1))
        shown = faces(second)

        self.doc.save()
        names = (first.Name, second.Name)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        feat = self.doc.getObject("Comp")
        first, second = (self.doc.getObject(n) for n in names)
        self.assertEqual(rgb(first), {yellow})
        self.assertEqual(faces(second), shown)
        # And still followed after the reopen
        feat.ShapeColor = green
        feat.ShapeAppearance = [feat.ShapeAppearance[0]]
        self.assertEqual(rgb(first), {yellow})
        self.assertEqual(faces(second), drawn(feat, 1))
        self.assertEqual(rgb(second), rgb(feat))

if __name__ == "__main__":
    unittest.main()
