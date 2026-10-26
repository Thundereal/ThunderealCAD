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


"""C++ joints in a flexible sub-assembly.

docs/AssemblyObjectModel.md sec 7.1. The instance of a flexible
sub-assembly (Assembly::AssemblyLink) mirrors its assembly's joints onto its
own links to the components. A C++ joint is mirrored as one, with element
links of its own on the instance's links, and convenience elements of its
own: copied, its element links were the assembly's, and took the joint out
of it.
"""

import os
import tempfile
import unittest

import FreeCAD as App

import JointObject


def plc(x=0, y=0, z=0, rot=0):
    return App.Placement(App.Vector(x, y, z), App.Rotation(App.Vector(0, 0, 1), rot))


class TestFlexibleSubAssembly(unittest.TestCase):
    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("FlexibleSubAssembly")
        self.build()

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        App.closeDocument(self.doc.Name)

    def ground(self, assembly, obj, name):
        joints = assembly.getGroup("Joints", True)
        ground = joints.newObject("Assembly::GroundedJointPython", name)
        JointObject.GroundedJoint(ground, obj)

    def build(self):
        """A sub-assembly of two boxes on a C++ revolute, and a parent with a
        flexible instance of it, one of whose parts the parent grounds"""
        doc = self.doc
        self.sub = doc.addObject("Assembly::AssemblyObject", "Sub")
        a = self.sub.newObject("Part::Box", "A")
        b = self.sub.newObject("Part::Box", "B")
        b.Placement = plc(30, 5, 2, 20)
        self.ground(self.sub, a, "GroundA")
        doc.recompute()
        self.joint = self.sub.makeJoint("Revolute", (a, "Face6"), (b, "Face5"))
        doc.recompute()
        self.assertEqual(self.sub.solve(), 0)

        self.top = doc.addObject("Assembly::AssemblyObject", "Top")
        self.inst = self.top.newObject("Assembly::AssemblyLink", "Inst")
        self.inst.LinkedObject = self.sub
        self.inst.Placement = plc(0, 50, 0)
        doc.recompute()
        self.inst.Rigid = False
        doc.recompute()

    def local(self, name):
        return next(
            o for o in self.inst.Group if o.TypeId == "App::Link" and o.LinkedObject.Name == name
        )

    def assertSubIntact(self):
        # The assembly keeps its joint and the joint its element links
        self.assertIn(self.joint, self.sub.Group)
        for i in (1, 2):
            link = getattr(self.joint, "ElementLink%d" % i)
            self.assertIn(link, self.sub.Group)
            self.assertNotIn(link, self.inst.Group)

    def solveInParent(self):
        a, b = self.local("A"), self.local("B")
        self.ground(self.top, a, "GroundLocalA")
        self.doc.recompute()
        b.Placement = b.Placement.multiply(plc(0, 0, 5, 30))
        self.doc.recompute()
        self.assertEqual(self.top.solve(), 0)
        # B back on A's top face, its bottom face's centre on A's top's
        rel = a.Placement.inverse().multiply(b.Placement)
        centre = rel.multVec(App.Vector(5, 5, 0))
        self.assertTrue(centre.isEqual(App.Vector(5, 5, 10), 1e-6), rel)

    def testTheJointIsMirroredWithElementLinksOfItsOwn(self):
        self.assertSubIntact()
        mirror = next(o for o in self.inst.Group if o.isDerivedFrom("Assembly::Joint"))
        self.assertEqual(mirror.TypeId, "Assembly::Joint")
        self.assertEqual(mirror.JointType, "Revolute")
        self.assertEqual(mirror.getComponent(1), self.local("A"))
        self.assertEqual(mirror.getComponent(2), self.local("B"))
        for i in (1, 2):
            link = getattr(mirror, "ElementLink%d" % i)
            self.assertIn(link, self.inst.Group)
        # The convenience elements are the instance's own
        self.assertEqual(len(self.inst.Elements.Group), 2)
        for element in self.inst.Elements.Group:
            self.assertIn(element.Target[0], (self.local("A"), self.local("B")))
        self.assertNotIn("Invalid", mirror.State)

    def testTheMirrorSolvesInTheParent(self):
        self.solveInParent()
        self.assertSubIntact()

    def testRigidAgainLeavesNothing(self):
        self.inst.Rigid = True
        self.doc.recompute()
        self.assertFalse([o for o in self.inst.Group
                          if o.isDerivedFrom("Part::ElementLink")
                          or o.isDerivedFrom("Assembly::Joint")])
        group = self.inst.Elements
        self.assertTrue(group is None or not group.Group)
        self.assertSubIntact()

    def testSavedAndRestored(self):
        path = os.path.join(tempfile.mkdtemp(), "FlexibleSubAssembly.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.sub = self.doc.getObject("Sub")
        self.top = self.doc.getObject("Top")
        self.inst = self.doc.getObject("Inst")
        self.joint = next(o for o in self.sub.Group if o.isDerivedFrom("Assembly::Joint"))
        self.assertEqual(
            len([o for o in self.inst.Group if o.isDerivedFrom("Assembly::Joint")]), 1
        )
        self.solveInParent()
        self.assertSubIntact()

    def onBase(self):
        """A box the parent grounds, a part of the instance placed on it by a
        parent's joint"""
        base = self.top.newObject("Part::Box", "Base")
        base.Length = base.Width = 40
        self.ground(self.top, base, "GroundBase")
        # A flexible instance's placement went into its parts' when it turned
        # flexible: theirs are in the parent's coordinate system
        self.assertTrue(self.inst.Placement.isIdentity())
        self.doc.recompute()
        return base

    def assertOnBase(self):
        # A centred on the base's top, B on A's by the mirrored revolute
        def at(name, local):
            return self.inst.Placement.multiply(self.local(name).Placement).multVec(local)

        for name, z in (("A", 10), ("B", 20)):
            where = at(name, App.Vector(5, 5, 0))
            self.assertTrue(where.isEqual(App.Vector(20, 20, z), 1e-6), (name, where))

    def testAParentJointOnAPartOfTheInstance(self):
        base = self.onBase()
        joint = self.top.makeJoint("Fixed", (self.local("A"), "Face5"), (base, "Face6"))
        self.doc.recompute()
        # Through the instance, the parent's child, to the part
        self.assertEqual(joint.getComponent(1), self.local("A"))
        self.assertEqual(self.top.solve(), 0)
        self.assertEqual(self.top.getSolveStatus()["DoF"], 1)
        self.doc.recompute()
        self.assertOnBase()
        self.assertNotIn("Invalid", self.top.State)
        # Rigid, the instance is the component, the path the same
        self.inst.Rigid = True
        self.doc.recompute()
        self.assertEqual(joint.getComponent(1), self.inst)
        self.inst.Rigid = False
        self.doc.recompute()
        self.assertEqual(joint.getComponent(1), self.local("A"))

    def testPythonJointsConvertInAndOnTheInstance(self):
        import AssemblyMigrate

        # The sub-assembly's joint and a parent's on the instance's part, as
        # upstream's joints
        App.closeDocument(self.doc.Name)
        self.doc = App.newDocument("FlexibleSubAssembly")
        doc = self.doc
        self.sub = doc.addObject("Assembly::AssemblyObject", "Sub")
        a = self.sub.newObject("Part::Box", "A")
        b = self.sub.newObject("Part::Box", "B")
        self.ground(self.sub, a, "GroundA")
        doc.recompute()
        old = self.sub.getGroup("Joints").newObject("App::FeaturePython", "Revolute")
        JointObject.Joint(old, JointObject.JointTypes.index("Revolute"))
        old.Proxy.setJointConnectors(old, [[a, ["Face6", "Face6"]], [b, ["Face5", "Face5"]]])
        self.top = doc.addObject("Assembly::AssemblyObject", "Top")
        self.inst = self.top.newObject("Assembly::AssemblyLink", "Inst")
        self.inst.LinkedObject = self.sub
        doc.recompute()
        self.inst.Rigid = False
        doc.recompute()
        base = self.onBase()
        fixed = self.top.getGroup("Joints").newObject("App::FeaturePython", "Fixed")
        JointObject.Joint(fixed, JointObject.JointTypes.index("Fixed"))
        fixed.Proxy.setJointConnectors(
            fixed, [[self.local("A"), ["Face5", "Face5"]], [base, ["Face6", "Face6"]]]
        )
        doc.recompute()

        report = AssemblyMigrate.convertDocument(doc, force=True)
        doc.recompute()
        self.assertTrue(all(new is not None for _, new, _ in report), report)
        self.joint = next(o for o in self.sub.Group if o.isDerivedFrom("Assembly::Joint"))
        mirror = next(o for o in self.inst.Group if o.isDerivedFrom("Assembly::Joint"))
        self.assertEqual(mirror.TypeId, "Assembly::Joint")
        parent = next(o for o in self.top.Group if o.isDerivedFrom("Assembly::Joint"))
        # The task may have put the grounded side first
        self.assertEqual(
            {parent.getComponent(1), parent.getComponent(2)}, {self.local("A"), base}
        )
        self.assertEqual(self.top.solve(), 0)
        doc.recompute()
        self.assertOnBase()
        self.assertSubIntact()
