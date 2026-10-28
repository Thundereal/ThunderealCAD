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

"""The assembly's Joints, Simulations and Views aux groups, and Auto as its
group mode (docs/AssemblyObjectModel.md sec 2, sec 3, step 4e).

The groups are views of the assembly's own Group by role, in place of
upstream's JointGroup, SimulationGroup and ViewGroup, which owned their
members; upstream's groups in a file become them when it is opened.
"""

import os
import tempfile

import FreeCAD as App

import JointObject
import UtilsAssembly

from AssemblyTests.TestJoints import JointTestBase, Z, plc


class TestAssemblyGroups(JointTestBase):
    def setUp(self):
        super().setUp()
        # The base makes upstream's JointGroup; an assembly the workbench
        # makes now has the aux group
        self.doc.removeObject(self.jointgroup.Name)
        self.jointgroup = self.assembly.getGroup("Joints", True)

    def reopen(self):
        path = os.path.join(tempfile.mkdtemp(), "AssemblyGroups.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.assembly = self.doc.getObject("Assembly")
        self.jointgroup = UtilsAssembly.getJointGroup(self.assembly)

    def pair(self):
        a = self.box("A")
        b = self.box("B", plc(30, 5, 2, Z, 20))
        self.doc.recompute()
        return a, b

    # ------------------------------------------------------------ tests

    def testTheJointsGroupIsAnAuxGroupOfTheAssembly(self):
        group = self.jointgroup
        self.assertEqual(group.TypeId, "Assembly::AssemblyGroup")
        self.assertEqual(group.Role, "Joints")
        self.assertEqual(group.getPropertyByName("_Owner"), self.assembly)
        self.assertIs(UtilsAssembly.getJointGroup(self.assembly), group)
        self.assertIs(self.assembly.getGroup("Joints"), group)
        self.assertIsNone(self.assembly.getGroup("Views"))

    def testWhatTheWorkbenchMakesIsShownByRole(self):
        a, b = self.pair()
        ground = self.jointgroup.newObject("Assembly::GroundedJointPython", "GroundedJoint")
        JointObject.GroundedJoint(ground, a)
        joint = self.assembly.makeJoint("Revolute", (a, "Face6"), (b, "Face5"))
        rg = self.jointgroup.newObject("Assembly::RigidGroupPython", "RigidGroupJoint")
        JointObject.RigidGroupJoint(rg, [a, b])
        for obj in (ground, joint, rg):
            self.assertIn(obj, self.assembly.Group, obj.Name)
            self.assertIn(obj, self.jointgroup.Group, obj.Name)
        # The components and the joint's element bindings are not joints
        for obj in [a, b, joint.Binding1, joint.Binding2]:
            self.assertNotIn(obj, self.jointgroup.Group, obj.Name)

    def testAnUpstreamJointJoinsWhenItGetsItsProxy(self):
        """As upstream's commands and scripts make one: the object first,
        then its proxy, which gives it its role"""
        a, b = self.pair()
        ground = self.ground(a)
        self.assertEqual(ground.TypeId, "App::FeaturePython")
        self.assertIn(ground, self.jointgroup.Group)
        j = self.joint("Revolute", (a, ["Face6"]), plc(5, 5, 10), (b, ["Face5"]), plc(5, 5, 0))
        self.assertIn(j, self.jointgroup.Group)

    def testSolvesThroughTheAuxGroup(self):
        a, b = self.pair()
        ground = self.jointgroup.newObject("Assembly::GroundedJointPython", "GroundedJoint")
        JointObject.GroundedJoint(ground, a)
        joint = self.assembly.makeJoint("Revolute", (a, "Face6"), (b, "Face5"))
        self.solve()
        f1 = joint.getFrame(1)
        f2 = joint.getFrame(2)
        self.assertVecClose(f1.Base, f2.Base)
        self.assertTrue(a.Placement.isSame(App.Placement(), 1e-12))

    def testSimulationsAndViews(self):
        import CommandCreateSimulation
        import CommandCreateView

        a, b = self.pair()
        joint = self.assembly.makeJoint("Revolute", (a, "Face6"), (b, "Face5"))
        sims = UtilsAssembly.getSimulationGroup(self.assembly)
        self.assertEqual((sims.TypeId, sims.Role), ("Assembly::AssemblyGroup", "Simulations"))
        sim = sims.newObject("Assembly::SimulationPython", "Simulation")
        CommandCreateSimulation.Simulation(sim)
        motion = self.assembly.newObject("Assembly::MotionPython", "Motion")
        CommandCreateSimulation.Motion(motion, "Angular", joint, "36*time")
        sim.Group = [motion]
        views = UtilsAssembly.getViewGroup(self.assembly)
        self.assertEqual((views.TypeId, views.Role), ("Assembly::AssemblyGroup", "Views"))
        view = views.newObject("Assembly::ExplodedViewPython", "Exploded View")
        CommandCreateView.ExplodedView(view)
        self.assertEqual(sims.Group, [sim])
        self.assertEqual(views.Group, [view])
        # A motion is its simulation's, in no group of the assembly's
        for group in (sims, views, self.jointgroup):
            self.assertNotIn(motion, group.Group)
        ground = self.jointgroup.newObject("Assembly::GroundedJointPython", "GroundedJoint")
        JointObject.GroundedJoint(ground, a)
        sim.TimeStepOutput = 0.1
        self.doc.recompute()
        # The builder finds the motion and the joints through the new groups
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assertEqual(self.assembly.numberOfFrames(), 12)

    def testDeletingAGroupKeepsWhatItShows(self):
        a, b = self.pair()
        joint = self.assembly.makeJoint("Revolute", (a, "Face6"), (b, "Face5"))
        self.doc.removeObject(self.jointgroup.Name)
        self.assertIn(joint, self.assembly.Group)
        self.assertIsNone(self.assembly.getGroup("Joints"))
        group = self.assembly.getGroup("Joints", True)
        self.assertEqual(group.Group, [joint])

    def testUpstreamGroupsBecomeAuxGroupsOnOpen(self):
        """In their order, which the assembly's now carries"""
        self.doc.removeObject(self.jointgroup.Name)
        legacy = self.assembly.newObject("Assembly::JointGroup", "Joints")
        legacy.Label = "My joints"
        self.jointgroup = legacy
        a, b = self.pair()
        c = self.box("C", plc(0, 30, 0))
        self.doc.recompute()
        ground = self.ground(a)
        j1 = self.joint("Revolute", (a, ["Face6"]), plc(5, 5, 10), (b, ["Face5"]), plc(5, 5, 0))
        j2 = self.joint("Fixed", (a, ["Face4"]), plc(5, 10, 5), (c, ["Face3"]), plc(5, 0, 5))
        # The user's order, not the one they were made in
        legacy.Group = [j2, ground, j1]
        order = [o.Label for o in legacy.Group]
        sims = self.assembly.newObject("Assembly::SimulationGroup", "Simulations")
        views = self.assembly.newObject("Assembly::ViewGroup", "Exploded Views")
        self.assertEqual((sims.Group, views.Group), ([], []))

        self.reopen()
        types = [o.TypeId for o in self.assembly.Group]
        for legacy in ("JointGroup", "SimulationGroup", "ViewGroup"):
            self.assertNotIn("Assembly::" + legacy, types)
        group = self.jointgroup
        self.assertEqual(group.TypeId, "Assembly::AssemblyGroup")
        self.assertEqual(group.Label, "My joints")
        self.assertEqual([o.Label for o in group.Group], order)
        self.assertEqual(self.assembly.getGroup("Simulations").Role, "Simulations")
        self.assertEqual(self.assembly.getGroup("Views").Role, "Views")
        self.solve()

    def testThePartsGroupTakesTheComponentsOnly(self):
        """Not what is the joints', a simulation's, a view's, or another group's"""
        import CommandCreateSimulation
        import CommandCreateView

        a, b = self.pair()
        joint = self.assembly.makeJoint("Revolute", (a, "Face6"), (b, "Face5"))
        sim = UtilsAssembly.getSimulationGroup(self.assembly).newObject(
            "Assembly::SimulationPython", "Simulation"
        )
        CommandCreateSimulation.Simulation(sim)
        motion = self.assembly.newObject("Assembly::MotionPython", "Motion")
        CommandCreateSimulation.Motion(motion, "Angular", joint, "36*time")
        sim.Group = [motion]
        view = UtilsAssembly.getViewGroup(self.assembly).newObject(
            "Assembly::ExplodedViewPython", "Exploded View"
        )
        CommandCreateView.ExplodedView(view)
        step = self.assembly.newObject("Assembly::ExplodedViewStepPython", "Move")
        CommandCreateView.ExplodedViewStep(step, 0)
        view.Group = [step]
        self.assembly.newObject("Assembly::SnapshotGroup", "Snapshots")
        # As the assembly's context menu makes it (Gui::ViewProviderPart)
        parts = self.doc.addObject("App::PartAuxGroup", "Parts")
        parts.Role = "Parts"
        self.assembly.Group = self.assembly.Group + [parts]
        parts._Owner = self.assembly
        self.assertEqual(parts.Group, [a, b])

    def testAutoGroupMode(self):
        """A body in another part is added as an owned link, not taken"""
        self.assertEqual(self.assembly.GroupMode, "Auto")
        part = self.doc.addObject("App::Part", "Part")
        box = part.newObject("Part::Box", "Elsewhere")
        self.doc.recompute()
        self.assembly.addObject(box)
        self.assertIn(box, part.Group)
        self.assertNotIn(box, self.assembly.Group)
        links = [o for o in self.assembly.Group if o.isDerivedFrom("App::Link")]
        self.assertEqual([o.LinkedObject for o in links], [box])
