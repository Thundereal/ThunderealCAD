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


"""Replacing a component of an assembly.

docs/AssemblyObjectModel.md sec 4.5. The link to the component is pointed
at another container declaring the same element labels; the element bindings
follow the labels, and the joint is solved on the new geometry.
"""

import unittest

import FreeCAD as App
import Part

import JointObject


def plc(x=0, y=0, z=0):
    return App.Placement(App.Vector(x, y, z), App.Rotation())


class TestReplaceComponent(unittest.TestCase):
    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("ReplaceComponent")

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        App.closeDocument(self.doc.Name)

    def grounded(self, assembly, obj):
        joints = assembly.getGroup("Joints", True)
        ground = joints.newObject("Assembly::GroundedJointPython", "GroundedJoint")
        JointObject.GroundedJoint(ground, obj)

    def testTheJointIsSolvedOnTheReplacement(self):
        doc = self.doc
        sub = doc.addObject("Assembly::AssemblyObject", "Sub")
        box = sub.newObject("Part::Box", "Box")
        self.grounded(sub, box)
        top = doc.addObject("Assembly::AssemblyObject", "Top")
        base = top.newObject("Part::Box", "Base")
        self.grounded(top, base)
        link = top.newObject("App::Link", "SubLink")
        link.LinkedObject = sub
        link.Placement = plc(50, 0, 0)
        doc.recompute()
        top.makeJoint("Fixed", (link, "Box.Face5"), (base, "Face6"))
        doc.recompute()
        self.assertEqual(top.solve(), 0)
        doc.recompute()
        solved = link.Placement
        # The joint declared its side on the child, by a label
        self.assertEqual(len(sub.Elements.Group), 1)
        label = sub.Elements.Group[0].Label

        # Another child with that label, its box 5 higher in it
        other = doc.addObject("Assembly::AssemblyObject", "Other")
        otherBox = other.newObject("Part::Box", "Box")
        otherBox.Placement = plc(0, 0, 5)
        self.grounded(other, otherBox)
        Part.makeElement(other, otherBox, "Face5", label)
        doc.recompute()

        link.LinkedObject = other
        doc.recompute()
        self.assertEqual(top.solve(), 0)
        doc.recompute()
        expected = solved.Base - App.Vector(0, 0, 5)
        self.assertTrue(link.Placement.Base.isEqual(expected, 1e-6), link.Placement)
        for obj in doc.Objects:
            self.assertNotIn("Invalid", obj.State, obj.FullName)
