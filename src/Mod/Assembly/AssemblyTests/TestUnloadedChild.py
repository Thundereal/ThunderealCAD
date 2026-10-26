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

"""A joint on a child assembly that is not loaded.

docs/AssemblyObjectModel.md sec 4.4. The element link keeps its copy and
says it is unresolved, not broken; the link to the child is still a
component, and the joint is solved on the frame it last had relative to it.
The child closing and coming back is followed.
"""

import os
import shutil
import tempfile
import unittest

import FreeCAD as App

import JointObject


def plc(x=0, y=0, z=0):
    return App.Placement(App.Vector(x, y, z), App.Rotation())


class TestUnloadedChild(unittest.TestCase):
    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.tmp = tempfile.mkdtemp(prefix="asmunloaded")
        self.childPath = os.path.join(self.tmp, "UnloadedChild.FCStd")
        self.parentPath = os.path.join(self.tmp, "UnloadedParent.FCStd")
        self.build()

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        for name in list(App.listDocuments()):
            if name.startswith("Unloaded"):
                App.closeDocument(name)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def grounded(self, assembly, obj):
        joints = assembly.getGroup("Joints", True)
        ground = joints.newObject("Assembly::GroundedJointPython", "GroundedJoint")
        JointObject.GroundedJoint(ground, obj)

    def build(self):
        """A child assembly with a box, and a parent whose assembly links to
        it, with a joint between the child's top face and a grounded base;
        solved, saved and closed"""
        child = App.newDocument("UnloadedChild")
        sub = child.addObject("Assembly::AssemblyObject", "Sub")
        box = sub.newObject("Part::Box", "Box")
        self.grounded(sub, box)
        child.recompute()
        child.saveAs(self.childPath)

        parent = App.newDocument("UnloadedParent")
        top = parent.addObject("Assembly::AssemblyObject", "Top")
        base = top.newObject("Part::Box", "Base")
        self.grounded(top, base)
        link = top.newObject("App::Link", "SubLink")
        link.LinkedObject = sub
        link.Placement = plc(50, 0, 0)
        parent.recompute()
        top.makeJoint("Fixed", (link, "Box.Face5"), (base, "Face6"))
        parent.recompute()
        self.assertEqual(top.solve(), 0)
        parent.recompute()
        self.solved = link.Placement
        child.save()
        parent.saveAs(self.parentPath)
        App.closeDocument(parent.Name)
        App.closeDocument(child.Name)

    def openParent(self):
        parent = App.openDocument(self.parentPath)
        return parent, parent.getObject("Top"), parent.getObject("SubLink")

    def childLink(self, parent, link):
        """The element link on the child's element"""
        links = [
            o
            for o in parent.Objects
            if o.isDerivedFrom("Part::ElementLink") and o.Element[0] == link
        ]
        self.assertEqual(len(links), 1)
        return links[0]

    def assertSolvesBack(self, parent, top, link):
        link.Placement = plc(80, 30, 0)
        parent.recompute()
        self.assertEqual(top.solve(), 0)
        self.assertTrue(link.Placement.isSame(self.solved, 1e-6), link.Placement)

    def testAJointSolvesOnAChildThatIsMissing(self):
        os.rename(self.childPath, self.childPath + ".moved")
        parent, top, link = self.openParent()
        self.assertIsNone(link.LinkedObject)
        elementLink = self.childLink(parent, link)
        self.assertTrue(elementLink.Unresolved)
        self.assertFalse(elementLink.Shape.isNull())
        self.assertNotIn("Invalid", elementLink.State)
        # Recomputed, it keeps the copy and is no error
        elementLink.touch()
        parent.recompute()
        self.assertTrue(elementLink.Unresolved)
        self.assertFalse(elementLink.Shape.isNull())
        self.assertNotIn("Invalid", elementLink.State)
        self.assertNotIn("Error", elementLink.State)
        self.assertSolvesBack(parent, top, link)

    def testTheChildClosingAndComingBackIsFollowed(self):
        parent, top, link = self.openParent()
        self.assertIsNotNone(link.LinkedObject)
        elementLink = self.childLink(parent, link)
        self.assertFalse(elementLink.Unresolved)
        App.closeDocument("UnloadedChild")
        self.assertIsNone(link.LinkedObject)
        self.assertTrue(elementLink.Unresolved)
        self.assertSolvesBack(parent, top, link)
        App.openDocument(self.childPath)
        self.assertIsNotNone(link.LinkedObject)
        self.assertFalse(elementLink.Unresolved)
        parent.recompute()
        self.assertFalse(elementLink.Unresolved)
        self.assertSolvesBack(parent, top, link)

    def testAnElementNotThereIsStillAnError(self):
        parent, top, link = self.openParent()
        elementLink = self.childLink(parent, link)
        elementLink.Element = (link, "$$Elements.$NoSuchElement.")
        parent.recompute()
        self.assertFalse(elementLink.Unresolved)
        self.assertIn("Invalid", elementLink.State)


if __name__ == "__main__":
    unittest.main()
