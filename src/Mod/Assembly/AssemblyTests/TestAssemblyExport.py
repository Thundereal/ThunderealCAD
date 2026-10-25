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

"""What an assembly's shape and export are made of.

docs/AssemblyObjectModel.md sec 3.1. The components, and the user's plain
groups of them, whatever refers to them: a joint, its element links, the
grounding and the groups name a component without taking its place.
ExportMode "Child Query" lets the user leave a component out, or put one in.
"""

import os
import shutil
import tempfile
import unittest

import FreeCAD as App
import Part

import JointObject


def plc(x=0, y=0, z=0):
    return App.Placement(App.Vector(x, y, z), App.Rotation())


class TestAssemblyExport(unittest.TestCase):
    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.tmp = tempfile.mkdtemp(prefix="asmexport")
        self.doc = App.newDocument("AsmExport")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.a = self.assembly.newObject("Part::Box", "A")
        self.b = self.assembly.newObject("Part::Box", "B")
        self.b.Placement = plc(30, 0, 0)
        self.cylinder = self.doc.addObject("Part::Cylinder", "Cylinder")
        self.cylinder.Visibility = False
        self.link = self.assembly.newObject("App::Link", "Link")
        self.link.LinkedObject = self.cylinder
        self.link.Placement = plc(0, 40, 0)
        joints = self.assembly.getGroup("Joints", True)
        ground = joints.newObject("Assembly::GroundedJointPython", "GroundedJoint")
        JointObject.GroundedJoint(ground, self.a)
        self.doc.recompute()
        self.assembly.makeJoint("Fixed", (self.a, "Face2"), (self.b, "Face1"))
        self.doc.recompute()

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        App.closeDocument(self.doc.Name)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def exported(self):
        return [s.rstrip(".") for s in self.assembly.getSubObjects()]

    def solids(self, obj=None):
        return len(Part.getShape(obj or self.assembly).Solids)

    def testTheShapeIsTheComponents(self):
        # The grounding, the joint and its element links all refer to A, B
        self.assertEqual(self.exported(), ["A", "B", "Link"])
        self.assertEqual(self.solids(), 3)

    def testAHiddenComponentIsLeftOut(self):
        self.b.Visibility = False
        self.doc.recompute()
        self.assertEqual(self.solids(), 2)

    def testAPlainGroupGoesForWhatItHolds(self):
        group = self.assembly.newObject("App::DocumentObjectGroup", "UserGroup")
        d = self.assembly.newObject("Part::Box", "D")
        d.Placement = plc(0, 80, 0)
        group.addObject(d)
        self.doc.recompute()
        self.assertIn(d, self.assembly.Group)
        self.assertEqual(self.exported(), ["A", "B", "Link", "UserGroup"])
        self.assertEqual(self.solids(), 4)

    def testChildQueryStartsFromTheComponentsAndTakesTheUsersChoice(self):
        self.assembly.ExportMode = "Child Query"
        self.assertEqual(self.exported(), ["A", "B", "Link"])
        for obj in self.assembly.Group:
            expected = obj.Name in ("A", "B", "Link")
            self.assertEqual(obj.Group_EnableExport, expected, obj.Name)
        self.b.Group_EnableExport = False
        joint = self.doc.getObject("Fixed")
        joint.Group_EnableExport = True
        self.assertEqual(self.exported(), ["A", "Link", "Fixed"])
        self.assertEqual(self.solids(), 2)

    def testALinkToTheAssemblyHasTheComponents(self):
        link = self.doc.addObject("App::Link", "AssemblyLink")
        link.LinkedObject = self.assembly
        self.doc.recompute()
        self.assertEqual(self.solids(link), 3)

    def testStepExportHasTheComponents(self):
        import Import

        path = os.path.join(self.tmp, "assembly.step")
        Import.export([self.assembly], path)
        self.assertEqual(len(Part.read(path).Solids), 3)
