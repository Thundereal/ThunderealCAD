# SPDX-License-Identifier: LGPL-2.1-or-later

"""A shape not read since its document was opened is loaded on first use
(docs/DocumentLoad.md sec 14). Overwriting it dropped the unread value before the
transaction took a copy of what was there, so undoing the recompute that changed it
put back a null shape -- with the object still marked up to date."""

import os
import tempfile
import unittest

import FreeCAD as App


class DeferredShapeUndoTest(unittest.TestCase):
    def setUp(self):
        doc = App.newDocument("DeferredShapeUndo")
        box = doc.addObject("Part::Box", "Box")
        tool = doc.addObject("Part::Cylinder", "Cyl")
        tool.Radius = 2
        cut = doc.addObject("Part::Cut", "Cut")
        cut.Base = box
        cut.Tool = tool
        doc.recompute()
        self.volume = cut.Shape.Volume
        self.path = os.path.join(tempfile.mkdtemp(), "deferredshapeundo.FCStd")
        doc.saveAs(self.path)
        App.closeDocument(doc.Name)
        self.doc = App.openDocument(self.path)

    def tearDown(self):
        App.closeDocument(self.doc.Name)
        os.remove(self.path)

    def lengthen(self):
        doc = self.doc
        doc.UndoMode = 1
        doc.openTransaction("Length")
        doc.Box.Length = 20
        doc.recompute()
        doc.commitTransaction()
        return doc.Cut.Shape.Volume

    def testUndoPutsBackAnUnreadShape(self):
        changed = self.lengthen()
        self.assertGreater(changed, self.volume + 1)
        self.doc.undo()
        shape = self.doc.Cut.Shape
        self.assertFalse(shape.isNull())
        self.assertAlmostEqual(shape.Volume, self.volume, places=6)
        self.assertNotIn("Touched", self.doc.Cut.State)

    def testRedoAfterUndoOfAnUnreadShape(self):
        changed = self.lengthen()
        self.doc.undo()
        self.doc.redo()
        self.assertAlmostEqual(self.doc.Cut.Shape.Volume, changed, places=6)

    def testRecomputeOutsideTheTransaction(self):
        # The recompute after the commit is a transaction of its own (the
        # repro the bug was found with).
        doc = self.doc
        doc.UndoMode = 1
        doc.openTransaction("Length")
        doc.Box.Length = 20
        doc.commitTransaction()
        doc.recompute()
        doc.undo()
        shape = doc.Cut.Shape
        self.assertFalse(shape.isNull())
        self.assertAlmostEqual(shape.Volume, self.volume, places=6)
