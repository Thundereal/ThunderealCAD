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

"""Upstream's groundings, rigid groups, simulations, motions, exploded views,
steps and snapshots converted to their C++ twins when a file is opened.

See AssemblyMigrate.py (TWINS) and docs/AssemblyObjectModel.md sec 7.2. Each
test builds upstream's Python objects as its commands make them, saves, and
opens the file again: the objects must be the C++ twins, with the same
values, in the same groups, and do what they did.
"""

import os
import tempfile

import FreeCAD as App

import CommandCreateSimulation
import CommandCreateSnapshot
import CommandCreateView
import JointObject
import UtilsAssembly

from AssemblyTests.TestJoints import JointTestBase, Z, plc


class TestMigrateTwins(JointTestBase):
    def setUp(self):
        super().setUp()
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._convert = self.params.GetBool("ConvertJointsOnOpen", True)
        self.params.SetBool("ConvertJointsOnOpen", True)

    def tearDown(self):
        self.params.SetBool("ConvertJointsOnOpen", self._convert)
        super().tearDown()

    # ------------------------------------------------------------ helpers

    def reopen(self):
        path = os.path.join(tempfile.mkdtemp(), "MigrateTwins.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        self.assembly = self.doc.getObject("Assembly")
        self.jointgroup = self.doc.getObject("Joints")

    def mechanism(self):
        """A grounded box and one turning on it by a revolute joint"""
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        self.ground(a)
        rev = self.joint(
            "Revolute",
            (a, ["Face6", "Face6"]),
            plc(5, 5, 10),
            (b, ["Face5", "Face5"]),
            plc(5, 5, 0),
        )
        self.doc.recompute()
        return a, b, rev

    def simulation(self, motions):
        """Upstream's simulation and motions, as its task makes them"""
        group = UtilsAssembly.getSimulationGroup(self.assembly)
        sim = group.newObject("App::FeaturePython", "Simulation")
        CommandCreateSimulation.Simulation(sim)
        for joint, mtype, formula in motions:
            m = self.assembly.newObject("App::FeaturePython", "Motion")
            CommandCreateSimulation.Motion(m, mtype, joint, formula)
            sim.Group = sim.Group + [m]
        return sim

    def explodedView(self, steps):
        """Upstream's exploded view and steps, as its task makes them"""
        group = UtilsAssembly.getViewGroup(self.assembly)
        view = group.newObject("App::FeaturePython", "Exploded View")
        CommandCreateView.ExplodedView(view)
        for subs, move, typeIndex in steps:
            step = self.assembly.newObject("App::FeaturePython", "Move")
            CommandCreateView.ExplodedViewStep(step, typeIndex)
            step.MovementTransform = move
            step.References = [self.assembly, subs]
            view.Group = view.Group + [step]
        return view

    def exploded(self, view):
        """Where the view puts each part it moves, by name"""
        placements, _lines = view.Proxy._calculateExplodedPlacements(view)
        return {o.Name: p for o, p in placements.items()}

    # ------------------------------------------------------------ tests

    def testSimulationAndMotions(self):
        _a, _b, rev = self.mechanism()
        sim = self.simulation([(rev, "Angular", "36*time")])
        sim.bTimeEnd = 2.0
        sim.cTimeStepOutput = 0.1
        sim.setExpression("jFramesPerSecond", "24")
        sim.Label = "Turn"
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        frames = self.assembly.numberOfFrames()

        self.reopen()
        sims = UtilsAssembly.getSimulationGroup(self.assembly).Group
        self.assertEqual(len(sims), 1)
        sim = sims[0]
        self.assertEqual(sim.TypeId, "Assembly::SimulationPython")
        self.assertEqual(type(sim.Proxy).__name__, "Simulation")
        self.assertEqual(sim.Label, "Turn")
        # Under the C++ simulation's names
        self.assertFalse(hasattr(sim, "bTimeEnd"))
        self.assertAlmostEqual(sim.TimeEnd.Value, 2.0)
        self.assertAlmostEqual(sim.TimeStepOutput.Value, 0.1)
        self.assertIn(("FramesPerSecond", "24"), sim.ExpressionEngine)
        self.assertEqual(sim.FramesPerSecond, 24)
        self.assertEqual(len(sim.Group), 1)
        motion = sim.Group[0]
        self.assertEqual(motion.TypeId, "Assembly::MotionPython")
        self.assertEqual(motion.MotionType, "Angular")
        self.assertEqual(motion.Formula, "36*time")
        # The joint was converted first; the motion drives the new one
        self.assertTrue(motion.Joint.isDerivedFrom("Assembly::Joint"))
        self.assertIn(motion, self.assembly.Group)
        self.assertEqual(motion.Proxy.getSimulation(motion), sim)
        self.assertEqual(sim.Proxy.getAssembly(sim), self.assembly)
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assertEqual(self.assembly.numberOfFrames(), frames)

    def testTwoMotionsOnOneJoint(self):
        a = self.box("A")
        b = self.box("B", plc(30, 0, 0))
        self.ground(a)
        cyl = self.joint(
            "Cylindrical",
            (a, ["Face6", "Face6"]),
            plc(5, 5, 10),
            (b, ["Face5", "Face5"]),
            plc(5, 5, 0),
        )
        self.doc.recompute()
        sim = self.simulation([(cyl, "Angular", "36*time"), (cyl, "Linear", "2*time")])
        sim.cTimeStepOutput = 0.1
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assembly.updateForFrame(self.assembly.numberOfFrames() - 1)
        before = self.doc.getObject("B").Placement

        self.reopen()
        sim = UtilsAssembly.getSimulationGroup(self.assembly).Group[0]
        self.assertEqual([m.MotionType for m in sim.Group], ["Angular", "Linear"])
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assembly.updateForFrame(self.assembly.numberOfFrames() - 1)
        self.assertTrue(self.doc.getObject("B").Placement.isSame(before, 1e-6))

    def testExplodedView(self):
        self.mechanism()
        c = self.box("C", plc(0, 30, 0))
        self.doc.recompute()
        view = self.explodedView(
            [
                (["B.", "C."], plc(0, 0, 20), 0),
                (["C."], plc(15, 0, 0, Z, 30), 0),
            ]
        )
        view.Label = "Apart"
        self.doc.recompute()
        before = self.exploded(view)
        self.assertEqual(set(before), {"B", c.Name})

        self.reopen()
        views = UtilsAssembly.getViewGroup(self.assembly).Group
        self.assertEqual(len(views), 1)
        view = views[0]
        self.assertEqual(view.TypeId, "Assembly::ExplodedViewPython")
        self.assertEqual(view.Label, "Apart")
        self.assertEqual(
            [s.TypeId for s in view.Group], ["Assembly::ExplodedViewStepPython"] * 2
        )
        first, second = view.Group
        self.assertEqual(first.References, (self.assembly, ["B.", "C."]))
        self.assertEqual(second.MoveType, "Normal")
        self.assertTrue(second.MovementTransform.isSame(plc(15, 0, 0, Z, 30), 1e-12))
        after = self.exploded(view)
        self.assertEqual(set(after), set(before))
        for name, p in before.items():
            self.assertTrue(after[name].isSame(p, 1e-9), name)

    def testSnapshot(self):
        _a, b, _rev = self.mechanism()
        group = UtilsAssembly.getSnapshotGroup(self.assembly)
        snap = group.newObject("App::FeaturePython", "Snapshot")
        CommandCreateSnapshot.Snapshot(snap)
        snap.SolveOnActivation = False
        captured = dict(zip([o.Name for o in snap.Components], snap.Placements))
        self.assertIn("B", captured)

        self.reopen()
        snaps = UtilsAssembly.getSnapshotGroup(self.assembly).Group
        self.assertEqual(len(snaps), 1)
        snap = snaps[0]
        self.assertEqual(snap.TypeId, "Assembly::SnapshotPython")
        self.assertFalse(snap.SolveOnActivation)
        self.assertEqual(dict(zip([o.Name for o in snap.Components], snap.Placements)), captured)
        b = self.doc.getObject("B")
        b.Placement = plc(99, 0, 0)
        snap.Proxy.restoreState(snap)
        self.assertTrue(b.Placement.isSame(captured["B"], 1e-12))

    def testWhatLinkedAMotionFollowsIt(self):
        _a, _b, rev = self.mechanism()
        sim = self.simulation([(rev, "Angular", "36*time")])
        other = self.doc.addObject("App::FeaturePython", "Other")
        other.addProperty("App::PropertyLinkGlobal", "Driver", "Test")
        other.Driver = sim.Group[0]
        self.reopen()
        motion = UtilsAssembly.getSimulationGroup(self.assembly).Group[0].Group[0]
        self.assertEqual(motion.TypeId, "Assembly::MotionPython")
        self.assertEqual(self.doc.getObject("Other").Driver, motion)

    def testExpressionsNamingARenamedProperty(self):
        """Another object's expression on upstream's name of a simulation's
        property names the C++ one's after, and the simulation's own"""
        _a, _b, rev = self.mechanism()
        sim = self.simulation([(rev, "Angular", "36*time")])
        sim.bTimeEnd = 2.0
        sim.setExpression("cTimeStepOutput", "bTimeEnd / 20")
        other = self.doc.addObject("App::FeaturePython", "Other")
        other.addProperty("App::PropertyFloat", "Twice", "Test")
        other.setExpression("Twice", "%s.bTimeEnd * 2" % sim.Name)
        self.doc.recompute()
        self.assertAlmostEqual(other.Twice, 4.0)

        self.reopen()
        sim = UtilsAssembly.getSimulationGroup(self.assembly).Group[0]
        other = self.doc.getObject("Other")
        sim.TimeEnd = 3.0
        self.doc.recompute()
        self.assertAlmostEqual(other.Twice, 6.0)
        self.assertAlmostEqual(sim.TimeStepOutput.Value, 0.15)
        self.assertIn(sim.Name + ".TimeEnd", dict(other.ExpressionEngine)["Twice"])

    def testUpstreamNamesRestoreIntoTheCppSimulation(self):
        """A file whose C++ simulation has upstream's names restores them"""
        import zipfile

        group = UtilsAssembly.getSimulationGroup(self.assembly)
        sim = group.newObject("Assembly::SimulationPython", "Simulation")
        CommandCreateSimulation.Simulation(sim)
        sim.TimeEnd = 2.5
        sim.FramesPerSecond = 12
        name = sim.Name
        path = os.path.join(tempfile.mkdtemp(), "UpstreamNames.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)

        # Each property as the build before the rename wrote it. Python's
        # zipfile reads no zstd, which the blob history is in; the file
        # opens without it
        edited = path + ".edited"
        with zipfile.ZipFile(path) as src, zipfile.ZipFile(edited, "w") as dst:
            for item in src.infolist():
                try:
                    data = src.read(item.filename)
                except NotImplementedError:
                    continue
                if item.filename == "Document.xml":
                    for new, old in CommandCreateSimulation.SIMULATION_NAMES.items():
                        data = data.replace(
                            b'name="%s"' % new.encode(), b'name="%s"' % old.encode()
                        )
                    self.assertIn(b'name="bTimeEnd"', data)
                dst.writestr(item, data)
        os.replace(edited, path)

        self.doc = App.openDocument(path)
        sim = self.doc.getObject(name)
        self.assertAlmostEqual(sim.TimeEnd.Value, 2.5)
        self.assertEqual(sim.FramesPerSecond, 12)
        self.assertFalse(hasattr(sim, "bTimeEnd"))

    def testGroundingAndRigidGroup(self):
        a, b, _rev = self.mechanism()
        c = self.box("C", plc(30, 20, 0))
        rg = self.jointgroup.newObject("App::FeaturePython", "RigidGroupJoint")
        JointObject.RigidGroupJoint(rg, [b, c])
        rg.Label = "Pair"
        self.solve()
        stored = list(rg.RigidPlacements)
        poses = {o.Name: o.Placement for o in (a, b, c)}

        self.reopen()
        grounded = [o for o in self.jointgroup.Group if hasattr(o, "ObjectToGround")]
        self.assertEqual([o.TypeId for o in grounded], ["Assembly::GroundedJointPython"])
        self.assertEqual(type(grounded[0].Proxy).__name__, "GroundedJoint")
        self.assertEqual(grounded[0].ObjectToGround, self.doc.getObject("A"))
        groups = [o for o in self.jointgroup.Group if hasattr(o, "ObjectsToRigidGroup")]
        self.assertEqual([o.TypeId for o in groups], ["Assembly::RigidGroupPython"])
        rg = groups[0]
        self.assertEqual(rg.Label, "Pair")
        self.assertEqual([o.Name for o in rg.ObjectsToRigidGroup], ["B", "C"])
        self.assertFalse(rg.Suppressed)
        self.assertEqual(len(rg.RigidPlacements), len(stored))
        for got, want in zip(rg.RigidPlacements, stored):
            self.assertTrue(got.isSame(want, 1e-12))
        self.assertEqual(
            [o.TypeId for o in self.jointgroup.Group if o.TypeId == "App::FeaturePython"], []
        )
        self.solve()
        for name, p in poses.items():
            self.assertTrue(self.doc.getObject(name).Placement.isSame(p, 1e-6), name)
        # Its part's placement locked again: a solve keeps the grounding
        # (syncGroundedJoints deletes one whose part is free)
        self.assertIn("ReadOnly", self.doc.getObject("A").getPropertyStatus("Placement"))
        self.assertEqual(
            [o.Name for o in self.jointgroup.Group if hasattr(o, "ObjectToGround")],
            [grounded[0].Name],
        )

    def testPreferenceOff(self):
        _a, _b, rev = self.mechanism()
        self.simulation([(rev, "Angular", "36*time")])
        self.params.SetBool("ConvertJointsOnOpen", False)
        self.reopen()
        sim = UtilsAssembly.getSimulationGroup(self.assembly).Group[0]
        self.assertEqual(sim.TypeId, "App::FeaturePython")
        self.assertEqual(sim.Group[0].TypeId, "App::FeaturePython")
