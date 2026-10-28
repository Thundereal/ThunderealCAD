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

"""A spring's ends on elements (docs/AssemblyDynamics.md, D1's leftovers).

An end on an element is the origin of the element's frame, as a joint's side
is, through an element binding the spring owns; the element itself may be
shared. Its Component and Point are then output, following the geometry at
each recompute.
"""

import os
import shutil
import tempfile
import unittest

import FreeCAD as App

import CommandCreateSpring


def plc(x=0, y=0, z=0):
    return App.Placement(App.Vector(x, y, z), App.Rotation())


class EndsBase(unittest.TestCase):
    """A base box and a block above it in an assembly"""

    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("TestSpringEnds")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.base = self.box("Base")
        self.block = self.box("Block", plc(0, 0, 20))
        self.doc.recompute()

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        App.closeDocument(self.doc.Name)

    def box(self, name, placement=None):
        b = self.assembly.newObject("Part::Box", name)
        if placement is not None:
            b.Placement = placement
        return b

    def spring(self):
        spring = CommandCreateSpring.newSpring(self.assembly)
        return spring

    def assertVec(self, a, b, tol=1e-9):
        self.assertLess((a - b).Length, tol, "%r != %r" % (a, b))

    def bindings(self):
        return [o for o in self.doc.Objects if o.isDerivedFrom("Part::ElementBinding")]


class TestSpringEnds(EndsBase):
    # ------------------------------------------------------------ ends

    def testEndsOnFaces(self):
        spring = self.spring()
        binding = spring.setEnd(1, self.base, "Face6")
        spring.setEnd(2, self.block, "Face5")
        self.doc.recompute()
        self.assertEqual(binding.TypeId, "Part::ElementBinding")
        self.assertEqual(spring.Binding1, binding)
        # The spring's, a child of the assembly
        self.assertIn(binding, self.assembly.Group)
        self.assertEqual(spring.Component1, self.base)
        self.assertEqual(spring.Component2, self.block)
        self.assertVec(spring.Point1, App.Vector(5, 5, 10))
        self.assertVec(spring.Point2, App.Vector(5, 5, 0))
        self.assertVec(spring.getEndPoint(2), App.Vector(5, 5, 20))
        self.assertEqual(spring.getEnd(2)[0], self.block)
        # Output while bound
        self.assertIn("ReadOnly", spring.getPropertyStatus("Point1"))
        self.assertIn("ReadOnly", spring.getPropertyStatus("Component1"))
        self.assertNotIn("Invalid", spring.State)

    def testEndFollowsAResize(self):
        spring = self.spring()
        spring.setEnd(1, self.base, "Face6")
        self.doc.recompute()
        self.base.Height = 25
        self.doc.recompute()
        self.assertVec(spring.Point1, App.Vector(5, 5, 25))
        self.assertNotIn("Touched", spring.State)

    def testEndFollowsAFaceTheModelRenames(self):
        # A body in a part, the end on its top face; a fillet renames the
        # face beyond what the link follows, and the element finds it
        part = self.assembly.newObject("App::Part", "Sub")
        body = part.newObject("PartDesign::Body", "Body")
        pdbox = body.newObject("PartDesign::AdditiveBox", "PDBox")
        self.doc.recompute()
        spring = self.spring()
        spring.setEnd(1, part, "Body.Face6")
        spring.setEnd(2, self.block, "Face5")
        self.doc.recompute()
        self.assertVec(spring.Point1, App.Vector(5, 5, 10))
        # Declared in the part, which declares elements
        self.assertEqual(spring.Binding1.Element[0], part)
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        edges = [
            "Edge%d" % (i + 1)
            for i, edge in enumerate(pdbox.Shape.Edges)
            if abs(edge.CenterOfMass.z - 10) < 1e-7
        ]
        fillet.Base = (pdbox, edges)
        fillet.Radius = 1
        self.doc.recompute()
        self.assertNotIn("Invalid", spring.State)
        self.assertVec(spring.Point1, App.Vector(5, 5, 10))
        pdbox.Height = 30
        self.doc.recompute()
        self.assertVec(spring.Point1, App.Vector(5, 5, 30))

    def testEndAtAVertex(self):
        spring = self.spring()
        spring.setEnd(1, self.base, "Face6", "Vertex2")
        self.doc.recompute()
        self.assertVec(spring.Point1, self.base.Shape.Vertex2.Point)

    def testOffsetAlongTheNormal(self):
        spring = self.spring()
        spring.setEnd(1, self.base, "Face6")
        self.doc.recompute()
        spring.Binding1.Offset = plc(0, 0, 3)
        self.doc.recompute()
        self.assertVec(spring.Point1, App.Vector(5, 5, 13))

    def testFollowsTheComponentWhenItMoves(self):
        spring = self.spring()
        spring.setEnd(2, self.block, "Face5")
        self.doc.recompute()
        # As a solve moves it: no recompute
        self.block.Placement = plc(7, 0, 40)
        self.assertVec(spring.getEndPoint(2), App.Vector(12, 5, 40))

    def testRepickDeletesTheOldBinding(self):
        spring = self.spring()
        old = spring.setEnd(1, self.base, "Face6").Name
        spring.setEnd(1, self.base, "Face1")
        self.doc.recompute()
        self.assertIsNone(self.doc.getObject(old))
        self.assertEqual(len(self.bindings()), 1)

    def testFreeKeepsTheEndWhereItIs(self):
        spring = self.spring()
        name = spring.setEnd(1, self.base, "Face6").Name
        self.doc.recompute()
        spring.clearEnd(1)
        self.assertIsNone(spring.Binding1)
        self.assertIsNone(self.doc.getObject(name))
        self.assertEqual(spring.Component1, self.base)
        self.assertVec(spring.Point1, App.Vector(5, 5, 10))
        self.assertNotIn("ReadOnly", spring.getPropertyStatus("Point1"))
        # A free point no longer follows the face
        self.base.Height = 25
        self.doc.recompute()
        self.assertVec(spring.Point1, App.Vector(5, 5, 10))

    def testTwoSpringsShareTheElement(self):
        a = self.spring()
        b = self.spring()
        a.setEnd(1, self.base, "Face6")
        b.setEnd(1, self.base, "Face6")
        self.doc.recompute()
        elements = self.assembly.Elements.Group
        self.assertEqual(len(elements), 1)
        # A binding each, on the one element
        self.assertNotEqual(a.Binding1, b.Binding1)
        self.assertEqual(a.Binding1.Element, b.Binding1.Element)
        # Each one's offset is its own
        a.Binding1.Offset = plc(0, 0, 2)
        self.doc.recompute()
        self.assertVec(a.Point1, App.Vector(5, 5, 12))
        self.assertVec(b.Point1, App.Vector(5, 5, 10))
        element = elements[0].Name
        self.doc.removeObject(a.Name)
        self.assertIsNotNone(self.doc.getObject(element))
        self.assertEqual(len(self.bindings()), 1)
        self.doc.removeObject(b.Name)
        self.assertIsNone(self.doc.getObject(element))
        self.assertEqual(self.bindings(), [])

    def testDeleteAndUndo(self):
        self.doc.UndoMode = 1
        spring = self.spring()
        spring.setEnd(1, self.base, "Face6")
        spring.setEnd(2, self.block, "Face5")
        self.doc.recompute()
        name = spring.Name
        self.doc.openTransaction("delete")
        self.doc.removeObject(name)
        self.doc.commitTransaction()
        self.assertEqual(self.bindings(), [])
        self.doc.undo()
        spring = self.doc.getObject(name)
        self.assertIsNotNone(spring)
        self.assertIsNotNone(spring.Binding1)
        self.assertEqual(len(self.bindings()), 2)
        self.doc.recompute()
        self.assertVec(spring.getEndPoint(2), App.Vector(5, 5, 20))

    def testMakeSpringTakesEitherKindOfEnd(self):
        spring = CommandCreateSpring.makeSpring(
            self.assembly, (self.base, "Face6"), (self.block, App.Vector(5, 5, 0))
        )
        self.assertIsNotNone(spring.Binding1)
        self.assertIsNone(spring.Binding2)
        self.assertAlmostEqual(spring.FreeLength.Value, 10, places=9)

    def testSaveAndRestore(self):
        spring = self.spring()
        spring.setEnd(1, self.base, "Face6")
        self.doc.recompute()
        name = spring.Name
        tmp = tempfile.mkdtemp(prefix="springends")
        try:
            path = os.path.join(tmp, "SpringEnds.FCStd")
            self.doc.saveAs(path)
            App.closeDocument(self.doc.Name)
            self.doc = App.openDocument(path)
            spring = self.doc.getObject(name)
            self.assertIsNotNone(spring.Binding1)
            self.assertIn("ReadOnly", spring.getPropertyStatus("Point1"))
            base = self.doc.getObject("Base")
            base.Height = 15
            self.doc.recompute()
            self.assertVec(spring.Point1, App.Vector(5, 5, 15))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class TestLoadEnd(EndsBase):
    """An applied load's frame on an element, owned as a spring's end is"""

    def newLoad(self):
        import CommandCreateLoad

        return CommandCreateLoad.newLoad(self.assembly)

    def testFrameOnAFace(self):
        load = self.newLoad()
        binding = load.setEnd(self.base, "Face6")
        self.doc.recompute()
        self.assertEqual(load.Binding, binding)
        self.assertEqual(load.Component, self.base)
        self.assertIn("ReadOnly", load.getPropertyStatus("Frame"))
        self.assertVec(load.getEndPlacement().Base, App.Vector(5, 5, 10))
        # Along the face's normal, in the frame's axes
        self.assertVec(load.getDirectionNow(), App.Vector(0, 0, 1))
        self.base.Height = 25
        self.doc.recompute()
        self.assertVec(load.getEndPlacement().Base, App.Vector(5, 5, 25))

    def testDirectionTurnsWithThePartOrNot(self):
        load = self.newLoad()
        load.setEnd(self.block, "Face6")
        load.Direction = App.Vector(1, 0, 0)
        self.doc.recompute()
        # As a solve turns the block, without a recompute
        self.block.Placement = App.Placement(
            App.Vector(0, 0, 20), App.Rotation(App.Vector(0, 0, 1), 90)
        )
        self.assertVec(load.getDirectionNow(), App.Vector(0, 1, 0))
        load.DirectionAxes = "Assembly"
        self.assertVec(load.getDirectionNow(), App.Vector(1, 0, 0))

    def testTorqueTakesATorqueUnit(self):
        load = self.newLoad()
        load.Kind = "Torque"
        load.Magnitude = "2 N*m"
        self.assertAlmostEqual(load.Magnitude.getValueAs("N*m").Value, 2, places=9)

    def testFreeKeepsTheFrame(self):
        load = self.newLoad()
        name = load.setEnd(self.base, "Face6").Name
        self.doc.recompute()
        load.clearEnd()
        self.assertIsNone(load.Binding)
        self.assertIsNone(self.doc.getObject(name))
        self.assertEqual(load.Component, self.base)
        self.assertVec(load.getEndPlacement().Base, App.Vector(5, 5, 10))

    def testDeletingTheLoadDeletesItsBinding(self):
        load = self.newLoad()
        load.setEnd(self.base, "Face6")
        spring = self.spring()
        spring.setEnd(1, self.base, "Face6")
        self.doc.recompute()
        element = self.assembly.Elements.Group[0].Name
        self.doc.removeObject(load.Name)
        # The spring's binding, and the element it shares, stay
        self.assertEqual(len(self.bindings()), 1)
        self.assertIsNotNone(self.doc.getObject(element))


class TestBushingEnd(EndsBase):
    """A bushing's frame on an element, as a load's is (FramedItem)"""

    def newBushing(self):
        import CommandCreateBushing

        return CommandCreateBushing.newBushing(self.assembly)

    def testFrameOnAFace(self):
        bushing = self.newBushing()
        binding = bushing.setEnd(self.base, "Face6")
        self.doc.recompute()
        self.assertEqual(bushing.Binding, binding)
        self.assertEqual(bushing.Component, self.base)
        self.assertIn("ReadOnly", bushing.getPropertyStatus("Component"))
        self.assertVec(bushing.getEndPlacement().Base, App.Vector(5, 5, 10))
        self.base.Height = 25
        self.doc.recompute()
        self.assertVec(bushing.getEndPlacement().Base, App.Vector(5, 5, 25))

    def testNewValues(self):
        """A new bushing's values, in the internal units"""
        bushing = self.newBushing()
        self.assertVec(bushing.TranslationalStiffness, App.Vector(1e6, 1e6, 1e6))
        self.assertVec(bushing.RotationalStiffness, App.Vector(1e8, 1e8, 1e8))
        self.assertVec(bushing.Preload, App.Vector())

    def testFreeKeepsTheFrame(self):
        bushing = self.newBushing()
        name = bushing.setEnd(self.base, "Face6").Name
        self.doc.recompute()
        bushing.clearEnd()
        self.assertIsNone(bushing.Binding)
        self.assertIsNone(self.doc.getObject(name))
        self.assertVec(bushing.getEndPlacement().Base, App.Vector(5, 5, 10))

    def testDeletingTheBushingDeletesItsBinding(self):
        bushing = self.newBushing()
        bushing.setEnd(self.base, "Face6")
        self.doc.recompute()
        self.doc.removeObject(bushing.Name)
        self.assertEqual(len(self.bindings()), 0)


class TestSpringEndOnAnUnloadedChild(unittest.TestCase):
    """An end on a child assembly that is not loaded: the element binding
    keeps its copy, and the end stays where it last was on the link, as a
    joint's side does (TestUnloadedChild)"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="springunloaded")
        self.childPath = os.path.join(self.tmp, "SpringChild.FCStd")
        self.parentPath = os.path.join(self.tmp, "SpringParent.FCStd")
        child = App.newDocument("SpringChild")
        sub = child.addObject("Assembly::AssemblyObject", "Sub")
        sub.newObject("Part::Box", "Box")
        child.recompute()
        child.saveAs(self.childPath)

        parent = App.newDocument("SpringParent")
        top = parent.addObject("Assembly::AssemblyObject", "Top")
        base = top.newObject("Part::Box", "Base")
        link = top.newObject("App::Link", "SubLink")
        link.LinkedObject = sub
        link.Placement = plc(50, 0, 0)
        parent.recompute()
        spring = CommandCreateSpring.newSpring(top)
        spring.setEnd(1, link, "Box.Face6")
        spring.setEnd(2, base, "Face6")
        parent.recompute()
        self.springName = spring.Name
        self.assertTrue(spring.Point1.isEqual(App.Vector(5, 5, 10), 1e-9))
        parent.saveAs(self.parentPath)
        App.closeDocument(parent.Name)
        App.closeDocument(child.Name)

    def tearDown(self):
        for name in list(App.listDocuments()):
            if name.startswith("Spring"):
                App.closeDocument(name)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def testTheEndStaysOnTheLink(self):
        os.rename(self.childPath, self.childPath + ".moved")
        parent = App.openDocument(self.parentPath)
        link = parent.getObject("SubLink")
        spring = parent.getObject(self.springName)
        self.assertIsNone(link.LinkedObject)
        self.assertTrue(spring.Binding1.Unresolved)
        spring.touch()
        parent.recompute()
        self.assertNotIn("Invalid", spring.State)
        self.assertTrue(spring.Point1.isEqual(App.Vector(5, 5, 10), 1e-9), spring.Point1)
        link.Placement = plc(80, 30, 0)
        self.assertTrue(spring.getEndPoint(1).isEqual(App.Vector(85, 35, 10), 1e-9))


if __name__ == "__main__":
    unittest.main()
