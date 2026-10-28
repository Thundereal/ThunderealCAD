# SPDX-License-Identifier: LGPL-2.1-or-later
"""Dynamics (docs/AssemblyDynamics.md, phase D1): a Dynamic simulation of an
unpowered mechanism under gravity, against the closed form."""

import math
import unittest

import FreeCAD as App
import JointObject

# The complete elliptic integral of the first kind at k = sin(45 deg): a
# pendulum released from horizontal takes sqrt(I / (m g d)) times it to swing
# down to the bottom
K_45 = 1.8540746773013719
G = 9806.65  # mm/s^2


def plc(x=0.0, y=0.0, z=0.0, axis=None, angle=0.0):
    rot = App.Rotation(axis, angle) if axis is not None else App.Rotation()
    return App.Placement(App.Vector(x, y, z), rot)


class TestDynamics(unittest.TestCase):
    def setUp(self):
        import AssemblyApp

        if "Chrono" not in AssemblyApp.getAvailableSolvers():
            self.skipTest("the Chrono backend is not built")
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("TestDynamics")
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

    def joint(self, kind, ref1, plc1, ref2, plc2):
        j = self.joints.newObject("App::FeaturePython", kind)
        JointObject.Joint(j, JointObject.JointTypes.index(kind))
        j.Detach1 = True
        j.Detach2 = True
        j.Reference1 = ref1
        j.Reference2 = ref2
        j.Placement1 = plc1
        j.Placement2 = plc2
        return j

    def pendulum(self):
        """A rod 100 x 10 x 10 hinged at one end to a grounded block, about
        the y axis through (10, 5, 5), lying horizontal along x."""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        rod = self.box("Rod", (100, 10, 10), (10, 0, 0))
        self.doc.recompute()
        # z along y: a quarter turn about x
        self.joint(
            "Revolute",
            (base, ["Face2"]),
            plc(10, 5, 5, App.Vector(1, 0, 0), -90),
            (rod, ["Face1"]),
            plc(0, 5, 5, App.Vector(1, 0, 0), -90),
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        return rod

    def simulate(self, end, step, **props):
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        for name, value in props.items():
            setattr(sim, name, value)
        self.doc.recompute()
        return sim, self.assembly.generateSimulation(sim)

    def frames(self, rod, step):
        """Each frame's time, the rod's centre of mass and its hinge end."""
        out = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            out.append(
                (
                    i * step,
                    rod.Placement.multVec(App.Vector(50, 5, 5)),
                    rod.Placement.multVec(App.Vector(0, 5, 5)),
                )
            )
        return out

    def test_pendulum_period(self):
        """Released from horizontal, the rod's centre of mass swings down to
        the bottom in a quarter of the period of a pendulum let go at 90 deg."""
        rod = self.pendulum()
        _, result = self.simulate(0.2, 1e-3)
        self.assertEqual(result, 0)
        frames = self.frames(rod, 1e-3)
        self.assertEqual(len(frames), 201)
        # About the hinge: the rod's own about its centre, and 50 mm across
        inertiaPerMass = (100**2 + 10**2) / 12 + 50**2
        quarter = math.sqrt(inertiaPerMass / (G * 50)) * K_45
        # The angle below the horizontal, through 90 deg at the bottom
        crossed = None
        previous = None
        lowest = 1e9
        for time, centre, hinge in frames:
            # The hinge holds
            self.assertLess((hinge - App.Vector(10, 5, 5)).Length, 1e-6, time)
            angle = math.degrees(math.atan2(5 - centre.z, centre.x - 10))
            lowest = min(lowest, centre.z)
            if previous and previous[1] < 90 <= angle and crossed is None:
                t0, a0 = previous
                crossed = t0 + (time - t0) * (90 - a0) / (angle - a0)
            previous = (time, angle)
        self.assertIsNotNone(crossed, "the rod did not swing down")
        self.assertAlmostEqual(crossed, quarter, delta=quarter * 5e-4)
        # It swings through the bottom, 50 below the hinge
        self.assertAlmostEqual(lowest, 5 - 50, delta=0.05)

    def test_without_gravity(self):
        """With gravity off nothing moves."""
        rod = self.pendulum()
        _, result = self.simulate(0.05, 1e-2, GravityEnabled=False)
        self.assertEqual(result, 0)
        for time, centre, _ in self.frames(rod, 1e-2):
            self.assertLess((centre - App.Vector(60, 5, 5)).Length, 1e-9, time)

    def test_motion_refused(self):
        """A motion is not run by a dynamic analysis yet: refused."""
        self.pendulum()
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        motion = sim.newObject("Assembly::Motion", "Motion")
        motion.Joint = [o for o in self.joints.Group if o.Name.startswith("Revolute")][0]
        motion.MotionType = "Angular"
        motion.Formula = "time"
        self.doc.recompute()
        self.assertNotEqual(self.assembly.generateSimulation(sim), 0)
