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

"""Part::Element and Part::ElementLink in an App::Part.

An element is an interface a container declares, kept in the container's
Elements group (held by its Elements property, reached as "$$Elements.");
an element link is how a container uses an element of a child, or its own.
Both keep a copy of the geometry in the container's coordinate system.
See docs/AssemblyObjectModel.md sec 4 and 6.
"""

import os
import tempfile
import unittest

import FreeCAD
import Part


class ElementTest(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument("PartElementTest")

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def makePart(self, name, at=(0, 0, 0)):
        """An App::Part holding a box, placed at 'at'"""
        part = self.doc.addObject("App::Part", name)
        part.Placement.Base = FreeCAD.Vector(*at)
        box = self.doc.addObject("Part::Box", name + "Box")
        part.addObject(box)
        return part, box

    def assertSameFace(self, shape, face, placement=None):
        """'shape' is 'face' moved by 'placement'"""
        self.assertEqual(len(shape.Faces), 1)
        expected = face.copy()
        if placement is not None:
            expected.Placement = placement.multiply(expected.Placement)
        self.assertAlmostEqual(shape.Area, expected.Area, places=6)
        self.assertTrue(shape.Faces[0].CenterOfMass.isEqual(expected.CenterOfMass, 1e-7))

    def testElementIsInTheElementsGroup(self):
        part, box = self.makePart("Part")
        element = Part.makeElement(part, box, "Face6", "Top")
        group = part.Elements
        self.assertIsNotNone(group)
        self.assertEqual(group.Role, "Elements")
        self.assertEqual(group.Group, [element])
        self.assertNotIn(group, part.Group)
        self.assertEqual(element.Label, "Top")
        self.assertFalse(element.Visibility)
        # The group and its element are in the part's coordinate system
        self.assertEqual(group.getParentGeoFeatureGroup(), part)
        self.assertEqual(element.getParentGeoFeatureGroup(), part)

    def testElementIsReachedThroughTheProperty(self):
        part, box = self.makePart("Part")
        element = Part.makeElement(part, box, "Face6", "Top")
        part.Elements.Label = "Renamed"
        self.assertEqual(part.getSubObject("$$Elements.$Top.", retType=1), element)

    def testElementCopiesItsTarget(self):
        part, box = self.makePart("Part", (1, 2, 3))
        box.Placement.Base = FreeCAD.Vector(5, 0, 0)
        element = Part.makeElement(part, box, "Face6", "Top")
        self.doc.recompute()
        # In the part's coordinate system: the box's own placement only
        self.assertSameFace(element.Shape, box.Shape.Face6)

    def testElementFollowsItsTarget(self):
        part, box = self.makePart("Part")
        element = Part.makeElement(part, box, "Face6", "Top")
        self.doc.recompute()
        box.Length = 20
        self.doc.recompute()
        self.assertSameFace(element.Shape, box.Shape.Face6)

    def testDetachedElementKeepsItsCopy(self):
        part, box = self.makePart("Part")
        element = Part.makeElement(part, box, "Face6", "Top")
        self.doc.recompute()
        area = element.Shape.Area
        element.Detach = True
        box.Length = 20
        self.doc.recompute()
        self.assertAlmostEqual(element.Shape.Area, area, places=6)

    def testLabelsAreUniqueAmongSiblingsOnly(self):
        a, boxA = self.makePart("A")
        b, boxB = self.makePart("B")
        first = Part.makeElement(a, boxA, "Face1", "PinEnd")
        other = Part.makeElement(b, boxB, "Face1", "PinEnd")
        second = Part.makeElement(a, boxA, "Face2", "PinEnd")
        self.assertEqual(first.Label, "PinEnd")
        self.assertEqual(other.Label, "PinEnd")
        self.assertNotEqual(second.Label, "PinEnd")
        self.assertTrue(second.Label.startswith("PinEnd"))

    def testElementReexportsAChildsElement(self):
        pin, box = self.makePart("Pin")
        pinEnd = Part.makeElement(pin, box, "Face6", "End")
        axis = self.doc.addObject("App::Part", "Axis")
        axis.GroupMode = "Link"
        link = axis.addObject(pin)[0]
        link.Placement.Base = FreeCAD.Vector(0, 0, 50)
        shaftEnd = Part.makeElement(axis, link, "$$Elements.$End.", "ShaftEnd")
        self.doc.recompute()
        self.assertSameFace(shaftEnd.Shape, box.Shape.Face6, link.Placement)
        self.assertEqual(axis.getSubObject("$$Elements.$ShaftEnd.", retType=1), shaftEnd)

    def testElementLinkUsesAChildsElement(self):
        pin, box = self.makePart("Pin")
        Part.makeElement(pin, box, "Face6", "End")
        rotor = self.doc.addObject("App::Part", "Rotor")
        rotor.GroupMode = "Link"
        link = rotor.addObject(pin)[0]
        link.Placement.Base = FreeCAD.Vector(10, 0, 0)
        elementLink = Part.makeElementLink(rotor, link, "$$Elements.$End.")
        self.doc.recompute()
        self.assertIn(elementLink, rotor.Group)
        self.assertSameFace(elementLink.Shape, box.Shape.Face6, link.Placement)

    def testElementLinkUsesAConvenienceElement(self):
        rotor, box = self.makePart("Rotor")
        Part.makeElement(rotor, box, "Face1", "_Element")
        elementLink = Part.makeElementLink(rotor, rotor.Elements, "$_Element.")
        self.doc.recompute()
        self.assertSameFace(elementLink.Shape, box.Shape.Face1)

    def testRelabelUpdatesTheElementLink(self):
        pin, box = self.makePart("Pin")
        end = Part.makeElement(pin, box, "Face6", "End")
        rotor = self.doc.addObject("App::Part", "Rotor")
        rotor.GroupMode = "Link"
        link = rotor.addObject(pin)[0]
        elementLink = Part.makeElementLink(rotor, link, "$$Elements.$End.")
        end.Label = "Tip"
        self.assertEqual(elementLink.Element[1], ["$$Elements.$Tip."])
        pin.Elements.Label = "Whatever"
        self.assertEqual(elementLink.Element[1], ["$$Elements.$Tip."])

    def testDeletingThePartDeletesItsElements(self):
        part, box = self.makePart("Part")
        element = Part.makeElement(part, box, "Face6", "Top")
        group, name = part.Elements.Name, element.Name
        self.doc.removeObject(part.Name)
        self.assertIsNone(self.doc.getObject(group))
        self.assertIsNone(self.doc.getObject(name))

    def testDeletingTheGroupDeletesItsElements(self):
        part, box = self.makePart("Part")
        element = Part.makeElement(part, box, "Face6", "Top")
        name = element.Name
        self.doc.removeObject(part.Elements.Name)
        self.assertIsNone(self.doc.getObject(name))
        self.assertIsNone(part.Elements)

    def testSaveAndRestore(self):
        part, box = self.makePart("Part", (1, 0, 0))
        Part.makeElement(part, box, "Face6", "Top")
        self.doc.recompute()
        path = os.path.join(tempfile.mkdtemp(), "Elements.FCStd")
        self.doc.saveAs(path)
        FreeCAD.closeDocument(self.doc.Name)
        self.doc = FreeCAD.openDocument(path)
        part = self.doc.getObject("Part")
        box = self.doc.getObject("PartBox")
        element = part.getSubObject("$$Elements.$Top.", retType=1)
        self.assertIsNotNone(element)
        self.assertEqual(element.TypeId, "Part::Element")
        self.assertSameFace(element.Shape, box.Shape.Face6)
        self.assertEqual(element.getParentGeoFeatureGroup(), part)

    def testGroupIsReachedByNameAndLabel(self):
        # The paths the tree makes name the group, which is in no Group
        part, box = self.makePart("Part")
        element = Part.makeElement(part, box, "Face6", "Top")
        group = part.Elements
        self.assertEqual(part.getSubObject(group.Name + ".$Top.", retType=1), element)
        self.assertEqual(part.getSubObject("$" + group.Label + ".$Top.", retType=1), element)

    def testAutoVisibilityOffMakesThemVisible(self):
        params = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Part")
        old = params.GetBool("AutoElementVisibility", True)
        params.SetBool("AutoElementVisibility", False)
        try:
            part, box = self.makePart("Part")
            element = Part.makeElement(part, box, "Face6", "Top")
            link = Part.makeElementLink(part, part.Elements, "$Top.")
            self.assertTrue(element.Visibility)
            self.assertTrue(link.Visibility)
        finally:
            params.SetBool("AutoElementVisibility", old)

    def testAutoVisibilityHidesThem(self):
        part, box = self.makePart("Part")
        element = Part.makeElement(part, box, "Face6", "Top")
        link = Part.makeElementLink(part, part.Elements, "$Top.")
        self.assertFalse(element.Visibility)
        self.assertFalse(link.Visibility)

    def topVertex(self, box):
        """The name of a vertex of the box's top face, and the vertex"""
        for i, v in enumerate(box.Shape.Vertexes):
            if abs(v.Point.z - box.Height.Value) < 1e-9 and v.Point.x < 1e-9 and v.Point.y < 1e-9:
                return "Vertex%d" % (i + 1), v
        self.fail("no top vertex")

    def testElementLinkVertexDrivesTheOffset(self):
        part, box = self.makePart("Part")
        Part.makeElement(part, box, "Face6", "Top")
        link = Part.makeElementLink(part, part.Elements, "$Top.")
        self.doc.recompute()
        # The top face's frame is at its centre, z up; the vertex at (0, 0, 10)
        name, _ = self.topVertex(box)
        link.Vertex = (box, [name])
        self.doc.recompute()
        self.assertTrue(link.VertexPosition.isEqual(FreeCAD.Vector(-5, -5, 0), 1e-9))
        self.assertTrue(link.Offset.Base.isEqual(FreeCAD.Vector(-5, -5, 0), 1e-9))

        # The user's share of the offset stays as the vertex moves
        link.Offset = link.Offset.multiply(FreeCAD.Placement(FreeCAD.Vector(0, 0, 3), FreeCAD.Rotation()))
        box.Length = 20
        self.doc.recompute()
        self.assertTrue(link.VertexPosition.isEqual(FreeCAD.Vector(-10, -5, 0), 1e-9))
        self.assertTrue(link.Offset.Base.isEqual(FreeCAD.Vector(-10, -5, 3), 1e-9))

        # No vertex: its share is taken back out
        link.Vertex = None
        self.doc.recompute()
        self.assertTrue(link.Offset.Base.isEqual(FreeCAD.Vector(0, 0, 3), 1e-9))
        self.assertTrue(link.VertexPosition.isEqual(FreeCAD.Vector(), 1e-9))

    def testCopiesFollowThroughAPart(self):
        # The parts and groups between a link and its geometry do not change
        # when the geometry does: the copies still follow it
        part, box = self.makePart("Part")
        outer = self.doc.addObject("App::Part", "Outer")
        outer.addObject(part)
        Part.makeElement(part, box, "Face6", "Top")
        through = Part.makeElement(outer, part, "%s.Face6" % box.Name, "Through")
        reexport = Part.makeElement(outer, part, "$$Elements.$Top.", "Again")
        link = Part.makeElementLink(outer, outer.Elements, "$Through.")
        self.doc.recompute()
        box.Length = 20
        self.doc.recompute()
        for obj in (through, reexport, link):
            self.assertSameFace(obj.Shape, box.Shape.Face6)

    def testElementLinkVertexMustBeAVertex(self):
        part, box = self.makePart("Part")
        Part.makeElement(part, box, "Face6", "Top")
        link = Part.makeElementLink(part, part.Elements, "$Top.")
        link.Vertex = (box, ["Edge1"])
        self.doc.recompute()
        self.assertIn("Invalid", link.State)

    def testReplacedComponentKeepsItsLabelledElements(self):
        # Replacing a component: its element links follow the labels the
        # new one declares (docs/AssemblyObjectModel.md sec 4.5)
        pin, box = self.makePart("Pin")
        Part.makeElement(pin, box, "Face6", "End")
        other, otherBox = self.makePart("Other")
        otherBox.Height = 20
        Part.makeElement(other, otherBox, "Face6", "End")
        rotor = self.doc.addObject("App::Part", "Rotor")
        rotor.GroupMode = "Link"
        link = rotor.addObject(pin)[0]
        elementLink = Part.makeElementLink(rotor, link, "$$Elements.$End.")
        self.doc.recompute()
        link.LinkedObject = other
        self.doc.recompute()
        self.assertSameFace(elementLink.Shape, otherBox.Shape.Face6)

    def makeReplaceable(self):
        """A link to a box in a part, and an element on the box's Face1
        through the link"""
        box = self.doc.addObject("Part::Box", "Box")
        outer = self.doc.addObject("App::Part", "Outer")
        link = outer.newObject("App::Link", "Link")
        link.LinkedObject = box
        self.doc.recompute()
        element = Part.makeElement(outer, link, "Face1", "Side")
        self.doc.recompute()
        return outer, link, element

    def testReplacedComponentIsSearchedForTheCopy(self):
        # The same face at another index of the new component
        outer, link, element = self.makeReplaceable()
        face = element.Shape.copy()
        feature = self.doc.addObject("Part::Feature", "Reordered")
        box = Part.makeBox(10, 10, 10)
        feature.Shape = Part.Solid(Part.Shell(list(reversed(box.Faces))))
        link.LinkedObject = feature
        self.doc.recompute()
        self.assertEqual(element.Target[1], ["Face6"])
        self.assertNotIn("Invalid", element.State)
        self.assertSameFace(element.Shape, face)

    def testReplacedComponentWithoutTheCopyKeepsTheName(self):
        outer, link, element = self.makeReplaceable()
        taller = self.doc.addObject("Part::Box", "Taller")
        taller.Height = 20
        link.LinkedObject = taller
        self.doc.recompute()
        self.assertEqual(element.Target[1], ["Face1"])
        self.assertNotIn("Invalid", element.State)
        self.assertSameFace(element.Shape, taller.Shape.Face1)
