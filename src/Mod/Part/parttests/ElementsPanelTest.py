# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
# *                                                                         *
# *   This file is part of FreeCAD.                                         *
# *                                                                         *
# *   FreeCAD is free software: you can redistribute it and/or modify it    *
# *   under the terms of the GNU Lesser General Public License as           *
# *   published by the Free Software Foundation, either version 2.1 of the  *
# *   License, or (at your option) any later version.                       *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful, but        *
# *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
# *   Lesser General Public License for more details.                       *
# *                                                                         *
# *   You should have received a copy of the GNU Lesser General Public      *
# *   License along with FreeCAD. If not, see                               *
# *   <https://www.gnu.org/licenses/>.                                      *
# *                                                                         *
# ***************************************************************************

"""The Elements task panel (Part_EditElements, PartElements/TaskElements.py).

GUI only: it drives the panel the way its buttons do, with the 3D selection
set through Gui.Selection.
"""

import unittest

import FreeCAD
import FreeCADGui
import Part


class ElementsPanelTest(unittest.TestCase):
    def setUp(self):
        import PartElements.TaskElements as TaskElements

        self.TaskElements = TaskElements
        self.doc = FreeCAD.newDocument("ElementsPanelTest")
        self.part = self.doc.addObject("App::Part", "Part")
        self.box = self.part.newObject("Part::Box", "Box")
        self.doc.recompute()
        FreeCADGui.Selection.clearSelection()

    def tearDown(self):
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
        FreeCADGui.Selection.clearSelection()
        FreeCAD.closeDocument(self.doc.Name)

    def select(self, *subs, root=None):
        FreeCADGui.Selection.clearSelection()
        root = root or self.part
        for sub in subs:
            FreeCADGui.Selection.addSelection(self.doc.Name, root.Name, sub)

    def open(self, obj=None):
        return self.TaskElements.edit(obj or self.part)

    def targets(self, container=None):
        container = container or self.part
        return [
            (e.Target[0].Name, list(e.Target[1]))
            for e in self.TaskElements.elementsOf(container)
        ]

    def rowOf(self, panel, element):
        for i in range(panel.list.topLevelItemCount()):
            item = panel.list.topLevelItem(i)
            if item.text(0) == element.Label:
                return item
        self.fail("no row for %s" % element.Label)

    def testAddFromTheSelection(self):
        panel = self.open()
        self.assertFalse(panel.addButton.isEnabled())
        self.select("Box.Face6", "Box.Face1")
        self.assertTrue(panel.addButton.isEnabled())
        panel.add()
        self.assertEqual(self.targets(), [("Box", ["Face6"]), ("Box", ["Face1"])])
        self.assertEqual(panel.list.topLevelItemCount(), 2)

    def testReplaceAndRemove(self):
        panel = self.open()
        self.select("Box.Face6")
        element = panel.add()[0]
        self.rowOf(panel, element).setSelected(True)
        self.select("Box.Face5")
        self.assertTrue(panel.replaceButton.isEnabled())
        panel.replace()
        self.assertEqual(self.targets(), [("Box", ["Face5"])])
        self.rowOf(panel, element).setSelected(True)
        panel.remove()
        self.assertEqual(self.targets(), [])

    def testRenameInTheList(self):
        panel = self.open()
        self.select("Box.Face6")
        element = panel.add()[0]
        self.rowOf(panel, element).setText(0, "Top")
        self.assertEqual(element.Label, "Top")

    def testCancelUndoesAndOkKeeps(self):
        panel = self.open()
        self.select("Box.Face6")
        panel.add()
        panel.reject()
        self.assertEqual(self.targets(), [])
        panel = self.open()
        self.select("Box.Face6")
        panel.add()
        panel.accept()
        self.assertEqual(self.targets(), [("Box", ["Face6"])])

    def testOnABodysFeatureAndItsOwnShape(self):
        body = self.doc.addObject("PartDesign::Body", "Body")
        feature = body.newObject("PartDesign::AdditiveBox", "Pad")
        self.doc.recompute()
        panel = self.open(body)
        # A pick on the feature, and one on the body's own shape, which the
        # selection names by the body's mapped name: both are stored on the
        # feature by plain name
        self.select(feature.Name + ".Face6", "Face5", root=body)
        panel.add()
        self.assertEqual(self.targets(body), [("Pad", ["Face6"]), ("Pad", ["Face5"])])

    def testAChildsElementIsTakenWhole(self):
        outer = self.doc.addObject("App::Part", "Outer")
        outer.addObject(self.part)
        end = Part.makeElement(self.part, self.box, "Face6", "End")
        self.doc.recompute()
        panel = self.open(outer)
        self.select(
            "%s.%s.%s.Face1" % (self.part.Name, self.part.Elements.Name, end.Name), root=outer
        )
        panel.add()
        self.assertEqual(self.targets(outer), [("Part", ["$$Elements.$End."])])

    def testCommandOpensItFromTheGroup(self):
        Part.makeElement(self.part, self.box, "Face6", "Top")
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(self.doc.Name, self.part.Elements.Name)
        FreeCADGui.runCommand("Part_EditElements")
        dialog = FreeCADGui.Control.activeDialog()
        self.assertTrue(dialog)
        self.assertEqual(self.TaskElements.getContainer(self.part.Elements), self.part)
