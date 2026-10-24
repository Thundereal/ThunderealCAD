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

"""Elements a Body declares (docs/AssemblyObjectModel.md sec 5).

The Body holds its Elements group through its Elements property, outside
its Group. An element's target is a child of the body: a feature, a sketch,
a datum. The element recomputes before the body and after its target, so
the body's dependency on its Elements group makes no cycle.
"""

import unittest

import FreeCAD
import Part


class TestBodyElement(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument("PartDesignTestBodyElement")
        self.body = self.doc.addObject("PartDesign::Body", "Body")
        self.box = self.body.newObject("PartDesign::AdditiveBox", "Box")
        self.doc.recompute()

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def testBodyHoldsItsElementsGroup(self):
        element = Part.makeElement(self.body, self.box, "Face6", "Top")
        group = self.body.Elements
        self.assertEqual(group.TypeId, "PartDesign::AuxGroup")
        self.assertEqual(group.Role, "Elements")
        self.assertEqual(group.Group, [element])
        self.assertNotIn(group, self.body.Group)
        self.assertEqual(element.getParentGeoFeatureGroup(), self.body)
        self.assertEqual(self.body.getSubObject("$$Elements.$Top.", retType=1), element)

    def testElementFollowsAFeatureWithoutACycle(self):
        element = Part.makeElement(self.body, self.box, "Face6", "Top")
        self.doc.recompute()
        self.box.Height = 30
        self.doc.recompute()
        for obj in (self.body, self.box, element):
            self.assertNotIn("Touched", obj.State, obj.Name)
            self.assertNotIn("Invalid", obj.State, obj.Name)
        face = self.box.Shape.Face6
        self.assertAlmostEqual(element.Shape.Area, face.Area, places=6)
        self.assertTrue(element.Shape.Faces[0].CenterOfMass.isEqual(face.CenterOfMass, 1e-7))

    def testElementsGroupStaysOutOfTheAuxViews(self):
        sketches = self.doc.addObject("PartDesign::AuxGroup", "Sketches")
        sketches.Role = "Sketches"
        self.body.Group = [sketches] + self.body.Group
        sketches._Body = self.body
        Part.makeElement(self.body, self.box, "Face6", "Top")
        sketch = self.body.newObject("Sketcher::SketchObject", "Sketch")
        self.assertEqual(sketches.Group, [sketch])
        # Only the body's own changes refresh a view; the Elements group is no view
        self.assertEqual(len(self.body.Elements.Group), 1)

    def testDeletingTheBodyDeletesItsElements(self):
        element = Part.makeElement(self.body, self.box, "Face6", "Top")
        group, name = self.body.Elements.Name, element.Name
        self.body.removeObjectsFromDocument()
        self.doc.removeObject(self.body.Name)
        self.assertIsNone(self.doc.getObject(group))
        self.assertIsNone(self.doc.getObject(name))
