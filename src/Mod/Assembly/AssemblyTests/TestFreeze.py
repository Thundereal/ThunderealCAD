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

"""A frozen assembly.

docs/AssemblyObjectModel.md sec 3.1 and 4.4. Freezing keeps the compound
of the components in the Parts group, stops solving, and lets a document
linking to the assembly restore that shape and the elements without the
components.
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


class TestFreeze(unittest.TestCase):
    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.tmp = tempfile.mkdtemp(prefix="asmfreeze")
        self.doc = App.newDocument("FreezeChild")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Sub")

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        for name in list(App.listDocuments()):
            if name.startswith("Freeze"):
                App.closeDocument(name)
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ------------------------------------------------------------ building

    def box(self, name, placement=None, assembly=None):
        box = (assembly or self.assembly).newObject("Part::Box", name)
        if placement:
            box.Placement = placement
        return box

    def grounded(self, obj, assembly=None):
        joints = (assembly or self.assembly).getGroup("Joints", True)
        ground = joints.newObject("Assembly::GroundedJointPython", "GroundedJoint")
        JointObject.GroundedJoint(ground, obj)
        return ground

    def partsGroup(self, assembly=None):
        groups = [
            o
            for o in (assembly or self.assembly).Group
            if o.isDerivedFrom("App::AuxGroup") and o.Role == "Parts"
        ]
        self.assertLessEqual(len(groups), 1)
        return groups[0] if groups else None

    def twoBoxes(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        self.grounded(a)
        self.doc.recompute()
        return a, b

    # ------------------------------------------------------------ freezing

    def testFreezingKeepsTheVisibleComponentsAsOneShape(self):
        self.twoBoxes()
        hidden = self.box("Hidden", plc(0, 50, 0))
        self.doc.recompute()
        hidden.Visibility = False
        self.assertIsNone(self.partsGroup())
        self.assembly.Freeze = True
        parts = self.partsGroup()
        self.assertEqual(parts.TypeId, "Assembly::PartsGroup")
        self.assertEqual(len(parts.Shape.Solids), 2)
        self.assertAlmostEqual(parts.Shape.BoundBox.XMax, 40)
        self.assertAlmostEqual(parts.Shape.BoundBox.YMax, 10)
        # The assembly's shape is the stored one
        shape = Part.getShape(self.assembly)
        self.assertEqual(len(shape.Solids), 2)
        self.assertTrue(shape.BoundBox.isInside(App.Vector(35, 5, 5)))

    def testAFrozenAssemblyIsNotSolved(self):
        a, b = self.twoBoxes()
        joint = self.assembly.makeJoint("Fixed", (a, "Face2"), (b, "Face1"))
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        solved = b.Placement
        self.assembly.Freeze = True
        b.Placement = plc(70, 20, 0)
        self.assertEqual(self.assembly.solve(), 0)
        self.assertTrue(b.Placement.isSame(plc(70, 20, 0), 1e-9))
        self.assembly.Freeze = False
        self.assertEqual(self.assembly.solve(), 0)
        self.assertTrue(b.Placement.isSame(solved, 1e-7))
        self.assertIsNotNone(joint)

    def testTheShapeIsTakenAgainOnlyOnRefresh(self):
        _, b = self.twoBoxes()
        self.assembly.Freeze = True
        b.Placement = plc(100, 0, 0)
        self.doc.recompute()
        parts = self.partsGroup()
        self.assertAlmostEqual(parts.Shape.BoundBox.XMax, 40)
        self.assembly.refreshFrozenShape()
        self.assertAlmostEqual(parts.Shape.BoundBox.XMax, 110)

    def testUnfreezingLeavesTheGroupWithoutTheShape(self):
        self.twoBoxes()
        self.assembly.Freeze = True
        self.assembly.Freeze = False
        parts = self.partsGroup()
        self.assertIsNotNone(parts)
        self.assertTrue(parts.Shape.isNull())
        # The assembly's shape is no longer the stored one
        self.assertNotEqual(self.assembly.getSubObjects(), (parts.Name + ".",))

    def testAPlainPartsGroupGivesWay(self):
        self.twoBoxes()
        plain = self.doc.addObject("App::PartAuxGroup", "Parts")
        plain.Role = "Parts"
        self.assembly.Group = self.assembly.Group + [plain]
        plain._Owner = self.assembly
        name = plain.Name
        self.assembly.Freeze = True
        parts = self.partsGroup()
        self.assertEqual(parts.TypeId, "Assembly::PartsGroup")
        self.assertIsNone(self.doc.getObject(name))
        self.assertEqual(len(parts.Shape.Solids), 2)

    def testTheFrozenStateIsSaved(self):
        self.twoBoxes()
        self.assembly.Freeze = True
        path = os.path.join(self.tmp, "FreezeChild.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        assembly = self.doc.getObject("Sub")
        self.assertTrue(assembly.Freeze)
        self.assertEqual(len(self.partsGroup(assembly).Shape.Solids), 2)

    # ------------------------------------------------------------ a parent

    def parentOf(self, freeze):
        """A parent document whose assembly links to this one's, with a joint
        on an element of it; both saved and closed, the parent reopened"""
        a, b = self.twoBoxes()
        self.assembly.makeJoint("Fixed", (a, "Face2"), (b, "Face1"))
        self.doc.recompute()

        parent = App.newDocument("FreezeParent")
        top = parent.addObject("Assembly::AssemblyObject", "Top")
        link = top.newObject("App::Link", "SubLink")
        link.LinkedObject = self.assembly
        other = self.box("Other", plc(0, 0, 40), top)
        self.grounded(other, top)
        joint = top.makeJoint("Fixed", (link, "A.Face6"), (other, "Face5"))
        parent.recompute()
        self.assertTrue(joint.Binding1.Shape.Faces)
        self.assembly.Freeze = freeze
        # Saved once: the stored shapes borrow sub-shapes from the files of
        # the components, which a partial load does not restore
        self.doc.saveAs(os.path.join(self.tmp, "FreezeChild.FCStd"))
        parentPath = os.path.join(self.tmp, "FreezeParent.FCStd")
        parent.saveAs(parentPath)
        App.closeDocument(parent.Name)
        App.closeDocument(self.doc.Name)

        parent = App.openDocument(parentPath)
        self.doc = App.getDocument("FreezeChild")
        return parent

    def testAParentRestoresAFrozenAssemblysShapeAndElementsOnly(self):
        parent = self.parentOf(True)
        child = self.doc
        self.assertTrue(child.Partial)
        sub = child.getObject("Sub")
        self.assertNotIn("Partial", sub.State)
        self.assertTrue(sub.Freeze)
        parts = self.partsGroup(sub)
        self.assertNotIn("Partial", parts.State)
        self.assertEqual(len(parts.Shape.Solids), 2)
        # The components and the joints are created, not restored
        self.assertIn("Partial", child.getObject("A").State)
        self.assertIn("Partial", child.getObject("B").State)
        self.assertTrue(child.getObject("A").Shape.isNull())
        # The elements keep their copies
        elements = sub.Elements
        self.assertNotIn("Partial", elements.State)
        self.assertTrue(elements.Group)
        for element in elements.Group:
            self.assertNotIn("Partial", element.State)
            self.assertFalse(element.Shape.isNull())
        # The link shows the stored shape
        link = parent.getObject("SubLink")
        self.assertEqual(len(Part.getShape(link).Solids), 2)
        # The parent's joint still solves on the element
        top = parent.getObject("Top")
        parent.recompute()
        self.assertEqual(top.solve(), 0)

    def testAParentLoadsAnAssemblyNotFrozenInFull(self):
        self.parentOf(False)
        child = self.doc
        self.assertFalse(child.Partial)
        self.assertNotIn("Partial", child.getObject("A").State)
        self.assertFalse(child.getObject("A").Shape.isNull())


if __name__ == "__main__":
    unittest.main()
