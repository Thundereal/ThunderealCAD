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

    def topEdges(self, shape, z):
        return [
            "Edge%d" % (i + 1)
            for i, edge in enumerate(shape.Edges)
            if abs(edge.CenterOfMass.z - z) < 1e-7
        ]

    def elementOnTheBody(self):
        """A part holding the body, and an element of the part on the
        body's top face: the body's result, a moving target"""
        part = self.doc.addObject("App::Part", "Part")
        part.addObject(self.body)
        element = Part.makeElement(part, self.body, "Face6", "Top")
        self.doc.recompute()
        return part, element

    def testElementFollowsItsFaceByTheNamingHistory(self):
        # A fillet renames the face beyond what the link follows: the
        # element finds what it became (docs/AssemblyObjectModel.md sec 4.5)
        part, element = self.elementOnTheBody()
        fillet = self.body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (self.box, self.topEdges(self.box.Shape, 10))
        fillet.Radius = 1
        self.doc.recompute()
        self.assertNotIn("Invalid", element.State)
        self.assertNotIn("?", element.Target[1][0])
        self.assertAlmostEqual(element.Shape.Area, 64, places=6)
        self.assertAlmostEqual(element.Shape.CenterOfMass.z, 10, places=6)

    def testElementWithItsFaceGoneIsAnErrorOfItsOwn(self):
        # Nothing became the face: the element says so, and an element link
        # to it keeps its copy
        part, element = self.elementOnTheBody()
        elementLink = Part.makeElementLink(part, part.Elements, "$Top.")
        self.doc.recompute()
        cut = self.body.newObject("PartDesign::SubtractiveBox", "Cut")
        cut.Placement.Base = FreeCAD.Vector(-1, -1, 5)
        cut.Length = cut.Width = 12
        self.doc.recompute()
        self.assertIn("Invalid", element.State)
        self.assertNotIn("Invalid", elementLink.State)
        self.assertAlmostEqual(elementLink.Shape.Area, 100, places=6)
        self.assertAlmostEqual(elementLink.Shape.CenterOfMass.z, 10, places=6)


class TestSolidBodyElements(unittest.TestCase):
    """A body with SolidBodies shows each solid's elements in that solid
    body; the body's Elements group still owns them all (sec 5.3)"""

    def setUp(self):
        self.doc = FreeCAD.newDocument("PartDesignTestSolidBodyElements")
        self.body = self.doc.addObject("PartDesign::Body", "Body")
        self.a = self.box("Box")
        self.b = self.box("Box001", 100, newSolid=True)
        self.doc.recompute()
        self.solidA, self.solidB = self.body.SolidBodies

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def box(self, name, x=0.0, newSolid=False):
        feat = self.body.newObjectAt("PartDesign::AdditiveBox", name)
        feat.Placement.Base = FreeCAD.Vector(x, 0, 0)
        if newSolid:
            feat.NewSolid = True
        return feat

    def view(self, solid):
        views = [
            o
            for o in self.body.Group
            if o.isDerivedFrom("App::AuxGroup") and getattr(o, "_Owner", None) == solid
        ]
        self.assertLessEqual(len(views), 1)
        return views[0] if views else None

    def testNoViewsWithoutElements(self):
        self.assertIsNone(self.view(self.solidA))
        self.assertIsNone(self.view(self.solidB))

    def testEachSolidShowsItsElements(self):
        a = Part.makeElement(self.body, self.a, "Face6", "A")
        b = Part.makeElement(self.body, self.b, "Face6", "B")
        self.assertEqual(self.body.Elements.Group, [a, b])
        self.assertEqual(self.view(self.solidA).Group, [a])
        self.assertEqual(self.view(self.solidB).Group, [b])
        self.assertEqual(self.view(self.solidA).Role, "Elements")
        # The path does not name the solid
        self.assertEqual(self.body.getSubObject("$$Elements.$B.", retType=1), b)

    def testAnElementOfNoSolidStaysWithTheBody(self):
        plane = self.body.newObject("PartDesign::Plane", "DatumPlane")
        datum = Part.makeElement(self.body, plane, "", "Datum")
        self.assertEqual(self.body.Elements.Group, [datum])
        self.assertEqual(self.view(self.solidA).Group, [])
        self.assertEqual(self.view(self.solidB).Group, [])

    def testRetargetMovesTheElement(self):
        a = Part.makeElement(self.body, self.a, "Face6", "A")
        a.Target = (self.b, ["Face6"])
        self.assertEqual(self.view(self.solidA).Group, [])
        self.assertEqual(self.view(self.solidB).Group, [a])

    def testViewsGoWithTheSolids(self):
        Part.makeElement(self.body, self.a, "Face6", "A")
        viewB = self.view(self.solidB).Name
        # Back to one solid: the second solid body goes, and its view
        self.b.NewSolid = False
        self.doc.recompute()
        self.assertIsNone(self.doc.getObject(viewB))

    def testViewsGoWithTheElementsGroup(self):
        Part.makeElement(self.body, self.a, "Face6", "A")
        views = [self.view(self.solidA).Name, self.view(self.solidB).Name]
        self.doc.removeObject(self.body.Elements.Name)
        for name in views:
            self.assertIsNone(self.doc.getObject(name))

    def testDeletingTheBodyTakesEverything(self):
        Part.makeElement(self.body, self.a, "Face6", "A")
        Part.makeElement(self.body, self.b, "Face6", "B")
        self.body.removeObjectsFromDocument()
        self.doc.removeObject(self.body.Name)
        self.doc.recompute()
        left = [o.TypeId for o in self.doc.Objects]
        self.assertNotIn("Part::Element", left)
        self.assertNotIn("App::PartAuxGroup", left)

    def testUndoAndRedoAnElement(self):
        self.doc.UndoMode = 1
        self.doc.openTransaction("element")
        a = Part.makeElement(self.body, self.a, "Face6", "A")
        self.doc.commitTransaction()
        name = a.Name
        self.doc.undo()
        self.assertIsNone(self.doc.getObject(name))
        self.doc.redo()
        a = self.doc.getObject(name)
        self.assertIsNotNone(a)
        self.assertEqual(self.view(self.solidA).Group, [a])
