# SPDX-License-Identifier: LGPL-2.1-or-later
"""Flexible bodies (docs/AssemblyDynamics.md, D4b step 3): a FlexibleBody of a
component, its mesh and interfaces made for it (FlexibleBodies.prepare()),
and a Dynamic run bending it."""

import math
import unittest

import FreeCAD as App
import JointObject

import FlexibleBodies

# As TestDynamics': the complete elliptic integral of the first kind at
# k = sin(45 deg)
K_45 = 1.8540746773013719
G = 9806.65  # mm/s^2


def plc(x=0.0, y=0.0, z=0.0, axis=None, angle=0.0):
    rot = App.Rotation(axis, angle) if axis is not None else App.Rotation()
    return App.Placement(App.Vector(x, y, z), rot)


class TestFlexibleBody(unittest.TestCase):
    def setUp(self):
        import AssemblyApp

        if "Chrono" not in AssemblyApp.getAvailableSolvers():
            self.skipTest("the Chrono backend is not built")
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("TestFlexibleBody")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.getGroup("Joints", True)

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        App.closeDocument(self.doc.Name)

    def box(self, name, size, at):
        b = self.assembly.newObject("Part::Box", name)
        b.Length, b.Width, b.Height = size
        b.Placement.Base = App.Vector(*at)
        return b

    def pendulum(self):
        """TestDynamics' rod, 100 x 10 x 10, hinged at one end about the y
        axis through (10, 5, 5) to a grounded block, lying along x"""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        rod = self.box("Rod", (100, 10, 10), (10, 0, 0))
        self.doc.recompute()
        hinge = self.joints.newObject("App::FeaturePython", "Revolute")
        JointObject.Joint(hinge, JointObject.JointTypes.index("Revolute"))
        hinge.Detach1 = True
        hinge.Detach2 = True
        hinge.Reference1 = (base, ["Face2"])
        hinge.Reference2 = (rod, ["Face1"])
        hinge.Placement1 = plc(10, 5, 5, App.Vector(1, 0, 0), -90)
        hinge.Placement2 = plc(0, 5, 5, App.Vector(1, 0, 0), -90)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        return rod, hinge

    def study(self, end, step):
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        self.doc.recompute()
        return sim

    def test_mesh_and_interfaces(self):
        """The rod's mesh, its interface on the hinge's face, kept until the
        size changes"""
        rod, hinge = self.pendulum()
        sim = self.study(0.01, 1e-3)
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        self.assertIn(flexible, sim.Elements)
        self.assertTrue(FlexibleBodies.prepare(flexible))
        nodes = flexible.Nodes
        self.assertGreater(len(nodes), 100)
        self.assertEqual(len(flexible.Tetrahedra) % 10, 0)
        # In the rod's own coordinates: 100 x 10 x 10 from its corner
        xs = [p.x for p in nodes]
        self.assertAlmostEqual(min(xs), 0.0, places=6)
        self.assertAlmostEqual(max(xs), 100.0, places=6)
        self.assertEqual(flexible.InterfaceFrames, [hinge.FullName])
        held = flexible.InterfaceNodes
        self.assertEqual(held[-1], -1)
        self.assertEqual(held.count(-1), 1)
        # Its nodes those of the end face x = 0, every one of them
        on = [n for n in held if n >= 0]
        self.assertEqual(sorted(on), [i for i, p in enumerate(nodes) if abs(p.x) < 1e-6])
        # Kept, then made again for another size
        self.assertFalse(FlexibleBodies.prepare(flexible))
        flexible.MeshSize = 4.0
        self.assertTrue(FlexibleBodies.prepare(flexible))
        self.assertNotEqual(len(flexible.Nodes), len(nodes))

    def test_welded_block(self):
        """A block welded to the flexible rod's free end is held at an
        interface of its own, not grouped with the rod: the rod still bends
        (its modal coordinates reported), the block at its end throughout"""
        rod, hinge = self.pendulum()
        block = self.box("Block", (5, 10, 10), (110, 0, 0))
        self.doc.recompute()
        weld = self.joints.newObject("App::FeaturePython", "Fixed")
        JointObject.Joint(weld, JointObject.JointTypes.index("Fixed"))
        weld.Detach1 = True
        weld.Detach2 = True
        weld.Reference1 = (rod, ["Face2"])
        weld.Reference2 = (block, ["Face1"])
        weld.Placement1 = plc(100, 5, 5)
        weld.Placement2 = plc(0, 5, 5)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        sim = self.study(0.05, 1e-3)
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        flexible.Modes = 4
        FlexibleBodies.prepare(flexible)
        self.assertEqual(sorted(flexible.InterfaceFrames), sorted([hinge.FullName, weld.FullName]))
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        results = self.assembly.getSimulationResults()
        modal = [c for c in results["Channels"] if c["Quantity"] == "ModalCoordinates"]
        self.assertEqual(len(modal), 1)
        moved = False
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            tip = rod.Placement.multVec(App.Vector(100, 5, 5))
            mount = block.Placement.multVec(App.Vector(0, 5, 5))
            self.assertLess((tip - mount).Length, 0.01, i)
            moved = moved or tip.z < 4.0
        self.assertTrue(moved, "the rod did not swing")

    def test_flexible_pendulum(self):
        """Let go level, the flexible rod swings down as the rigid one does,
        its centre at the bottom in a quarter of the period, the hinge
        holding; the run reports its modal coordinates"""
        rod, hinge = self.pendulum()
        sim = self.study(0.2, 1e-3)
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        flexible.Modes = 6
        FlexibleBodies.prepare(flexible)
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        results = self.assembly.getSimulationResults()
        modal = [c for c in results["Channels"] if c["Quantity"] == "ModalCoordinates"]
        self.assertEqual(len(modal), 1)
        self.assertEqual(len(modal[0]["Values"]), len(results["Times"]))
        self.assertEqual(len(modal[0]["Values"][0]), 6)

        inertiaPerMass = (100**2 + 10**2) / 12 + 50**2
        quarter = math.sqrt(inertiaPerMass / (G * 50)) * K_45
        crossed = None
        previous = None
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            time = i * 1e-3
            centre = rod.Placement.multVec(App.Vector(50, 5, 5))
            end = rod.Placement.multVec(App.Vector(0, 5, 5))
            # The hinge holds, but for the rod's bending there: microns
            self.assertLess((end - App.Vector(10, 5, 5)).Length, 0.01, time)
            angle = math.degrees(math.atan2(5 - centre.z, centre.x - 10))
            if previous and previous[1] < 90 <= angle and crossed is None:
                t0, a0 = previous
                crossed = t0 + (time - t0) * (90 - a0) / (angle - a0)
            previous = (time, angle)
        self.assertIsNotNone(crossed, "the rod did not swing down")
        self.assertAlmostEqual(crossed, quarter, delta=quarter * 2e-3)
