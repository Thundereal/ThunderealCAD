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

"""Joints on the elements of a link array, and the multiplied joint.

docs/AssemblyObjectModel.md sec 7.4. An element of an expanded link array
is a component of its own; a multiplied joint pairs each element of the
array its first side is on with an edge of its second side's component
like the one picked, as Assembly3's multiplied constraint does, keeping
an instance joint per pair.
"""

import os
import tempfile
import unittest

import FreeCAD as App
import Part

import JointObject

HOLES = ((10, 10), (50, 10), (10, 50), (50, 50))


def circleEdge(shape, center):
    for i, e in enumerate(shape.Edges):
        if e.Curve.TypeId == "Part::GeomCircle" and e.Curve.Center.isEqual(center, 1e-6):
            return "Edge%d" % (i + 1)
    raise AssertionError("no circle at %s" % center)


class TestMultiplyJoint(unittest.TestCase):
    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("MultiplyJoint")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.getGroup("Joints", True)

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        for name in list(App.listDocuments()):
            if name.startswith("MultiplyJoint"):
                App.closeDocument(name)

    # ------------------------------------------------------------ building

    def plate(self):
        shape = Part.makeBox(60, 60, 5)
        for x, y in HOLES:
            shape = shape.cut(Part.makeCylinder(2, 5, App.Vector(x, y, 0)))
        plate = self.assembly.newObject("Part::Feature", "Plate")
        plate.Shape = shape
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedJoint")
        JointObject.GroundedJoint(ground, plate)
        return plate

    def pins(self, count, placements=None):
        pin = self.doc.addObject("Part::Cylinder", "Pin")
        pin.Radius = 2
        pin.Height = 15
        array = self.assembly.newObject("App::Link", "Pins")
        array.LinkedObject = pin
        array.ElementCount = count
        array.ShowElement = True
        self.doc.recompute()
        for i, element in enumerate(array.ElementList):
            p = placements[i] if placements else App.Vector(100 + 20 * i, 0, 20)
            element.Placement = App.Placement(p, App.Rotation())
        self.doc.recompute()
        self.pinEdge = circleEdge(pin.Shape, App.Vector(0, 0, 0))
        return array

    def multiplied(self, plate, array, jtype="Revolute", **props):
        hole = circleEdge(plate.Shape, App.Vector(10, 10, 5))
        joint = self.assembly.makeJoint(jtype, (array.ElementList[0], self.pinEdge), (plate, hole))
        joint.Multiply = True
        for k, v in props.items():
            setattr(joint, k, v)
        self.doc.recompute()
        return joint

    def seated(self, array):
        """The hole each element sits in, by its index in HOLES"""
        res = []
        for element in array.ElementList:
            p = element.Placement.Base
            match = [i for i, (x, y) in enumerate(HOLES) if App.Vector(x, y, 5).isEqual(p, 1e-6)]
            res.append(match[0] if match else None)
        return res

    # ------------------------------------------------------------ tests

    def testAnArrayElementIsAComponent(self):
        plate = self.plate()
        array = self.pins(3)
        hole = circleEdge(plate.Shape, App.Vector(50, 10, 5))
        # Picked through the array, and as the element itself
        j1 = self.assembly.makeJoint(
            "Revolute", (array, array.ElementList[1].Name + "." + self.pinEdge), (plate, hole)
        )
        self.assertEqual(j1.getComponent(1), array.ElementList[1])
        hole2 = circleEdge(plate.Shape, App.Vector(10, 50, 5))
        j2 = self.assembly.makeJoint(
            "Revolute", (array.ElementList[2], self.pinEdge), (plate, hole2)
        )
        self.assertEqual(j2.getComponent(1), array.ElementList[2])
        first = array.ElementList[0].Placement
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        self.assertEqual(self.seated(array)[1:], [1, 2])
        # The element no joint holds stays where it is
        self.assertTrue(array.ElementList[0].Placement.isSame(first, 1e-9))

    def testPinsInHoles(self):
        plate = self.plate()
        array = self.pins(1)
        joint = self.multiplied(plate, array)
        # AutoCount grows the array to the four holes
        self.assertEqual(array.ElementCount, 4)
        instances = list(joint._Instances)
        self.assertEqual(len(instances), 4)
        self.assertEqual(sorted(i._Instance for i in instances), [0, 1, 2, 3])
        # The joint stands for them in the Joints group
        self.assertIn(joint, self.joints.Group)
        for instance in instances:
            self.assertNotIn(instance, self.joints.Group)
            self.assertEqual(instance.JointType, "Revolute")
        self.assertEqual(self.assembly.solve(), 0)
        seated = self.seated(array)
        # The element picked in the hole picked, each in its own
        self.assertEqual(seated[0], 0)
        self.assertEqual(sorted(seated), [0, 1, 2, 3])

    def testThePairsAreKept(self):
        plate = self.plate()
        array = self.pins(1)
        joint = self.multiplied(plate, array)
        self.assertEqual(self.assembly.solve(), 0)
        seated = self.seated(array)
        # Every pin far off and shuffled: each goes back to its own hole
        for i, element in enumerate(reversed(array.ElementList)):
            element.Placement = App.Placement(App.Vector(200 + 30 * i, 100, 40), App.Rotation())
        joint.touch()
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        self.assertEqual(self.seated(array), seated)

    def testWithoutAutoCount(self):
        plate = self.plate()
        array = self.pins(2)
        joint = self.multiplied(plate, array, AutoCount=False)
        self.assertEqual(array.ElementCount, 2)
        self.assertEqual(len(joint._Instances), 2)
        self.assertEqual(self.assembly.solve(), 0)
        self.assertEqual(self.seated(array)[0], 0)

    def testTheJointsFollowItsValues(self):
        plate = self.plate()
        array = self.pins(1)
        joint = self.multiplied(plate, array)
        joint.Suppressed = True
        self.assertTrue(all(i.Suppressed for i in joint._Instances))

    def testMultiplyOffAndDeleting(self):
        plate = self.plate()
        array = self.pins(1)
        joint = self.multiplied(plate, array)
        names = [i.Name for i in joint._Instances]
        joint.Multiply = False
        self.doc.recompute()
        self.assertEqual(list(joint._Instances), [])
        self.assertEqual([n for n in names if self.doc.getObject(n)], [])
        joint.Multiply = True
        self.doc.recompute()
        names = [i.Name for i in joint._Instances]
        self.assertEqual(len(names), 4)
        self.doc.removeObject(joint.Name)
        self.assertEqual([n for n in names if self.doc.getObject(n)], [])

    def testReopened(self):
        plate = self.plate()
        array = self.pins(1)
        self.multiplied(plate, array)
        self.assertEqual(self.assembly.solve(), 0)
        seated = self.seated(array)
        path = os.path.join(tempfile.mkdtemp(), "MultiplyJoint.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.assembly = self.doc.getObject("Assembly")
        joint = [
            j for j in self.doc.Objects if j.isDerivedFrom("Assembly::Joint") and j.Multiply
        ][0]
        self.assertEqual(len(joint._Instances), 4)
        array = self.doc.getObject("Pins")
        for element in array.ElementList:
            element.Placement = App.Placement(App.Vector(0, 0, 30), App.Rotation())
        self.assertEqual(self.assembly.solve(), 0)
        self.assertEqual(self.seated(array), seated)
