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

    def driven(self, control, formula, analysis="Dynamic", gravity=True):
        """The pendulum's hinge driven by a motion; the rod's turn about it
        (rad, unsigned) at each frame of 0.2 s by 0.01 s, or None if the run
        was refused."""
        rod = self.pendulum()
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = analysis
        sim.GravityEnabled = gravity
        sim.TimeEnd = 0.2
        sim.TimeStepOutput = 0.01
        motion = sim.newObject("Assembly::Motion", "Motion")
        motion.Joint = [o for o in self.joints.Group if o.Name.startswith("Revolute")][0]
        motion.MotionType = "Angular"
        motion.Control = control
        motion.Formula = formula
        self.doc.recompute()
        if self.assembly.generateSimulation(sim) != 0:
            return None
        turns = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            turns.append((i * 0.01, rod.Placement.Rotation.Angle))
        return turns

    def test_motor_position(self):
        """A Position motion holds the angle to its formula, against gravity."""
        turns = self.driven("Position", "time")
        self.assertIsNotNone(turns)
        for time, angle in turns:
            self.assertAlmostEqual(angle, time, delta=1e-6, msg=time)

    def test_motor_speed(self):
        """A Speed motion turns at its rate, against gravity."""
        turns = self.driven("Speed", "2")
        self.assertIsNotNone(turns)
        for time, angle in turns:
            self.assertAlmostEqual(angle, 2 * time, delta=1e-4, msg=time)

    def test_motor_effort(self):
        """An Effort motion's torque, 1 mN m, accelerates the rod by tau / I
        about the hinge (no gravity): half of it times the time squared."""
        turns = self.driven("Effort", "0.001", gravity=False)
        self.assertIsNotNone(turns)
        mass = 100 * 10 * 10 * 2.7e-6  # kg, the default aluminium
        inertia = mass * ((100**2 + 10**2) / 12 + 50**2) * 1e-6  # kg m^2
        accel = 0.001 / inertia
        for time, angle in turns[1:]:
            self.assertAlmostEqual(angle, 0.5 * accel * time * time, delta=1e-4 * accel, msg=time)

    def test_kinematic_refuses_effort(self):
        """Speed and Effort are a dynamic study's: a kinematic one refuses."""
        self.assertIsNone(self.driven("Effort", "0.001", analysis="Kinematic"))

    # ------------------------------------------------------------ springs

    MASS = 1000 * 2.7e-6  # a 10 mm cube of the default aluminium, kg
    K = 10.0  # N/m, the same number in mN/mm

    def oscillator(self, damping=0.0, listed=True):
        """A 10 mm cube on a vertical slider over a grounded one, a spring
        from the grounded one's top to its bottom, free at 4 mm; started at
        2 mm, without gravity: it swings about 4 mm by 2."""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        block = self.box("Block", (10, 10, 10), (0, 0, 12))
        self.doc.recompute()
        self.joint("Slider", (base, ["Face6"]), plc(5, 5, 10), (block, ["Face5"]), plc(5, 5, 0))
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        physics = self.assembly.getGroup("Physics", True)
        spring = physics.newObject("Assembly::SpringDamper", "Spring")
        spring.Component1 = base
        spring.Point1 = App.Vector(5, 5, 10)
        spring.Component2 = block
        spring.Point2 = App.Vector(5, 5, 0)
        spring.Stiffness = "%g N/m" % self.K
        spring.FreeLength = 4.0
        spring.Damping = damping
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        sim.GravityEnabled = False
        sim.TimeEnd = 0.3
        sim.TimeStepOutput = 1e-4
        if listed:
            sim.Elements = [spring]
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        heights = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            heights.append((i * 1e-4, block.Placement.Base.z))
        return heights

    @staticmethod
    def crossings(heights, level):
        """The times the height passes the level, linearly between frames."""
        out = []
        for (t0, z0), (t1, z1) in zip(heights, heights[1:]):
            if (z0 - level) * (z1 - level) < 0:
                out.append(t0 + (t1 - t0) * (level - z0) / (z1 - z0))
        return out

    def test_spring_period(self):
        """It swings between 12 and 16 by half a period of sqrt(m / k)."""
        heights = self.oscillator()
        half = math.pi * math.sqrt(self.MASS / self.K)
        crossed = self.crossings(heights, 14.0)
        self.assertGreaterEqual(len(crossed), 3)
        for a, b in zip(crossed, crossed[1:]):
            self.assertAlmostEqual(b - a, half, delta=half * 1e-3)
        # Up to 16, the free length's far side
        self.assertAlmostEqual(max(z for _, z in heights), 16.0, delta=1e-3)
        self.assertAlmostEqual(min(z for _, z in heights), 12.0, delta=1e-6)

    def test_damper_decrement(self):
        """Damped at 5% of critical, each swing is smaller than the last by
        the logarithmic decrement 2 pi z / sqrt(1 - z^2)."""
        zeta = 0.05
        damping = 2 * zeta * math.sqrt(self.K * self.MASS)  # kg/s
        heights = self.oscillator(damping)
        peaks = [
            z - 14.0
            for (_, a), (_, z), (_, b) in zip(heights, heights[1:], heights[2:])
            if z > a and z >= b and z > 14.0
        ]
        self.assertGreaterEqual(len(peaks), 2)
        decrement = 2 * math.pi * zeta / math.sqrt(1 - zeta * zeta)
        for a, b in zip(peaks, peaks[1:]):
            self.assertAlmostEqual(math.log(a / b), decrement, delta=decrement * 1e-2)

    def test_spring_not_listed(self):
        """A spring the study does not list does nothing."""
        heights = self.oscillator(listed=False)
        for time, z in heights:
            self.assertAlmostEqual(z, 12.0, delta=1e-9, msg=time)

    # ------------------------------------------------------------ reactions

    def channel(self, prefix, quantity):
        """The values of the last run's channel of the object named prefix...
        and the quantity."""
        results = self.assembly.getSimulationResults()
        found = [
            c
            for c in results["Channels"]
            if c["Name"].startswith(prefix) and c["Quantity"] == quantity
        ]
        self.assertEqual(len(found), 1, [(c["Name"], c["Quantity"]) for c in results["Channels"]])
        self.assertEqual(len(found[0]["Values"]), len(results["Times"]))
        return found[0]["Values"]

    def test_hinge_reaction(self):
        """The hinge's reaction on the grounded block, its first reference:
        the opposite of what holds the swinging rod, m (a - g), with a its
        centre of mass's from the angle -- the energy it has lost, alpha
        from gravity's torque; no torque about the hinge."""
        rod = self.pendulum()
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        forces = self.channel("Revolute", "Force")
        torques = self.channel("Revolute", "Torque")
        mass = 100 * 10 * 10 * 2.7e-6
        d = 50.0
        inertia = mass * ((100**2 + 10**2) / 12 + d**2)  # kg mm^2, about the hinge
        weight = mass * G  # mN
        # At the start, still: the hinge carries what the fall does not
        self.assertAlmostEqual(forces[0].z, -weight * (1 - mass * d * d / inertia), delta=weight * 1e-4)
        for (time, centre, _), force, torque in zip(self.frames(rod, 1e-2), forces, torques):
            theta = math.atan2(5 - centre.z, centre.x - 10)
            omega2 = 2 * mass * G * d * math.sin(theta) / inertia
            alpha = mass * G * d * math.cos(theta) / inertia
            # Along the rod, and the way it swings
            e = App.Vector(math.cos(theta), 0, -math.sin(theta))
            t = App.Vector(-math.sin(theta), 0, -math.cos(theta))
            accel = t * (alpha * d) - e * (omega2 * d)
            onRod = accel * mass + App.Vector(0, 0, weight)
            self.assertLess((force + onRod).Length, weight * 1e-3, time)
            self.assertLess(torque.Length, weight * d * 1e-6, time)

    def test_motor_torque(self):
        """A Speed motion holding the rod still at horizontal applies its
        weight's torque about the hinge, m g d; an Effort motion of that torque
        holds it there."""
        self.assertIsNotNone(self.driven("Speed", "0"))
        torques = self.channel("Revolute", "MotorTorque")
        mass = 100 * 10 * 10 * 2.7e-6
        holding = mass * G * 50  # mN mm
        for torque in torques[1:]:
            self.assertAlmostEqual(abs(torque), holding, delta=holding * 1e-4)
        self.tearDown()
        self.setUp()
        # In N m
        turns = self.driven("Effort", repr(torques[-1] * 1e-6))
        self.assertIsNotNone(turns)
        for time, angle in turns:
            self.assertLess(angle, 1e-4, time)
        efforts = self.channel("Revolute", "MotorTorque")
        for effort in efforts:
            self.assertAlmostEqual(effort, torques[-1], delta=abs(torques[-1]) * 1e-9)

    def test_spring_force(self):
        """A spring reports its length, and its force: k times how far it is
        from its free length, pushing apart when shorter."""
        heights = self.oscillator()
        lengths = self.channel("Spring", "SpringLength")
        forces = self.channel("Spring", "SpringForce")
        for (time, z), length, force in zip(heights, lengths, forces):
            self.assertAlmostEqual(length, z - 10.0, delta=1e-9, msg=time)
            self.assertAlmostEqual(force, -self.K * (length - 4.0), delta=1e-9, msg=time)

    def test_kinematic_reports_nothing(self):
        """A kinematic run reports no channels."""
        self.assertIsNotNone(self.driven("Position", "time", analysis="Kinematic"))
        results = self.assembly.getSimulationResults()
        self.assertEqual(results["Channels"], [])
        self.assertEqual(len(results["Times"]), self.assembly.numberOfFrames())

    def test_rack_pinion_reaction(self):
        """A rack falling along x drives a pinion on a grounded block; the
        rack-and-pinion joint, held by the neutral equations, pushes back on
        the rack (its first reference) by m (a - g), a = g m / (m + I / r^2)."""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        rack = self.box("Rack", (60, 6, 6), (0, 0, 10))
        pinion = self.assembly.newObject("Part::Cylinder", "Pinion")
        pinion.Radius = 10
        pinion.Height = 5
        pinion.Placement.Base = App.Vector(0, 20, 10)
        self.doc.recompute()
        along_x = App.Rotation(App.Vector(0, 1, 0), 90)  # local z -> global x
        self.joint(
            "Slider",
            (base, ["Face6"]),
            App.Placement(App.Vector(0, 0, 10), along_x),
            (rack, ["Face5"]),
            App.Placement(App.Vector(), along_x),
        )
        self.joint("Revolute", (base, ["Face6"]), plc(0, 20, 10), (pinion, ["Face3"]), plc())
        rp = self.joint(
            "RackPinion",
            (rack, ["Face5"]),
            App.Placement(App.Vector(), along_x),
            (pinion, ["Face1"]),
            plc(),
        )
        rp.Distance = 10.0
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        _, result = self.simulate(0.1, 1e-2, Gravity=App.Vector(-G, 0, 0))
        self.assertEqual(result, 0)
        density = 2.7e-6
        massRack = 60 * 6 * 6 * density
        massPinion = math.pi * 10**2 * 5 * density
        inertia = massPinion * 10**2 / 2  # about its axis
        accel = G * massRack / (massRack + inertia / 10**2)
        # It falls by that
        self.assembly.updateForFrame(self.assembly.numberOfFrames() - 1)
        self.assertAlmostEqual(rack.Placement.Base.x, -0.5 * accel * 0.1**2, delta=1e-3)
        forces = self.channel("RackPinion", "Force")
        expected = massRack * (G - accel)
        for force in forces:
            self.assertAlmostEqual(force.x, expected, delta=expected * 1e-4)

    # ------------------------------------------------------------ statics

    def test_static_pendulum(self):
        """Static, from horizontal: the rod hangs straight down from the
        hinge, which holds the block's weight off it."""
        rod = self.pendulum()
        _, result = self.simulate(0.0, 1e-2, AnalysisType="Static")
        self.assertEqual(result, 0)
        self.assertEqual(self.assembly.numberOfFrames(), 1)
        (_, centre, hinge), = self.frames(rod, 1e-2)
        self.assertLess((hinge - App.Vector(10, 5, 5)).Length, 1e-6)
        self.assertLess((centre - App.Vector(10, 5, 5 - 50)).Length, 1e-4)
        weight = 100 * 10 * 10 * 2.7e-6 * G
        force = self.channel("Revolute", "Force")[0]
        self.assertLess((force - App.Vector(0, 0, -weight)).Length, weight * 1e-6)

    def test_static_spring(self):
        """A block on a vertical slider over a spring sags by its weight
        over the stiffness, m g / k, from the spring's free length."""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        block = self.box("Block", (10, 10, 10), (0, 0, 14))
        self.doc.recompute()
        self.joint("Slider", (base, ["Face6"]), plc(5, 5, 10), (block, ["Face5"]), plc(5, 5, 0))
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        physics = self.assembly.getGroup("Physics", True)
        spring = physics.newObject("Assembly::SpringDamper", "Spring")
        spring.Component1 = base
        spring.Point1 = App.Vector(5, 5, 10)
        spring.Component2 = block
        spring.Point2 = App.Vector(5, 5, 0)
        spring.Stiffness = "%g N/m" % self.K
        spring.FreeLength = 4.0
        _, result = self.simulate(0.0, 1e-2, AnalysisType="Static", Elements=[spring])
        self.assertEqual(result, 0)
        self.assembly.updateForFrame(0)
        sag = self.MASS * G / self.K
        self.assertAlmostEqual(block.Placement.Base.z, 10 + 4 - sag, delta=1e-5)
        self.assertAlmostEqual(
            self.channel("Spring", "SpringForce")[0], self.MASS * G, delta=self.MASS * G * 1e-6
        )

    def test_static_effort(self):
        """An Effort motion of half the rod's weight times its arm holds it
        where gravity's torque is that: 60 deg below horizontal."""
        mass = 100 * 10 * 10 * 2.7e-6
        holding = mass * G * 50 * 1e-6  # N m
        # The sign that holds the rod up (test_motor_torque)
        self.assertIsNotNone(self.driven("Speed", "0"))
        sign = math.copysign(1.0, self.channel("Revolute", "MotorTorque")[-1])
        self.tearDown()
        self.setUp()
        turns = self.driven("Effort", repr(sign * 0.5 * holding), analysis="Static")
        self.assertIsNotNone(turns)
        for time, angle in turns:
            self.assertAlmostEqual(math.degrees(angle), 60.0, delta=1e-3, msg=time)

    def test_static_refuses_speed(self):
        """A Speed motion holds nothing still: a static study refuses it."""
        self.assertIsNone(self.driven("Speed", "1", analysis="Static"))

    def test_static_no_equilibrium(self):
        """A block on a vertical slider with nothing under it has no
        equilibrium: the study says so rather than settle."""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        block = self.box("Block", (10, 10, 10), (0, 0, 14))
        self.doc.recompute()
        self.joint("Slider", (base, ["Face6"]), plc(5, 5, 10), (block, ["Face5"]), plc(5, 5, 0))
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        _, result = self.simulate(0.0, 1e-2, AnalysisType="Static")
        self.assertNotEqual(result, 0)

    # ------------------------------------------------------------ stops

    def dropped(self, restitution=0.0, analysis="Dynamic", end=0.2, step=1e-2):
        """A 10 mm cube on a vertical slider, 4 mm over a grounded one, its
        travel's least 0: the stop the grounded one's top. Its heights."""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        block = self.box("Block", (10, 10, 10), (0, 0, 14))
        self.doc.recompute()
        slider = self.joint(
            "Slider", (base, ["Face6"]), plc(5, 5, 10), (block, ["Face5"]), plc(5, 5, 0)
        )
        slider.EnableLengthMin = True
        slider.LengthMin = 0.0
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        _, result = self.simulate(end, step, AnalysisType=analysis, Restitution=restitution)
        self.assertEqual(result, 0)
        heights = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            heights.append((i * step, block.Placement.Base.z))
        return heights

    def test_stop_holds(self):
        """Dropped on its stop, the block stays on it, the stop carrying its
        weight in the slider's reaction."""
        heights = self.dropped()
        fall = math.sqrt(2 * 4 / G)
        for time, z in heights:
            self.assertGreaterEqual(z, 10 - 1e-6, time)
            if time > fall + 0.02:
                self.assertAlmostEqual(z, 10.0, delta=1e-6, msg=time)
        weight = self.MASS * G
        force = self.channel("Slider", "Force")[-1]
        self.assertLess((force - App.Vector(0, 0, -weight)).Length, weight * 1e-6)

    def test_stop_bounce(self):
        """With a restitution of 1 the block bounces back to where it fell
        from; of 0.5, to a quarter of that height."""
        for restitution, back in ((1.0, 4.0), (0.5, 1.0)):
            self.tearDown()
            self.setUp()
            heights = self.dropped(restitution, end=0.06, step=1e-4)
            fall = math.sqrt(2 * 4 / G)
            after = [z for time, z in heights if fall + 1e-3 < time < fall + 0.04]
            self.assertAlmostEqual(max(after) - 10.0, back, delta=back * 1e-2, msg=restitution)

    def test_stop_static(self):
        """A static study settles the block on its stop."""
        ((_, z),) = self.dropped(analysis="Static", end=0.0)
        self.assertAlmostEqual(z, 10.0, delta=1e-6)

    def test_stop_rotation(self):
        """The pendulum's hinge limited to 30 deg either way of where it
        starts: it swings down onto the stop and stays, dynamic and static."""
        for analysis, end in (("Dynamic", 0.3), ("Static", 0.0)):
            self.tearDown()
            self.setUp()
            rod = self.pendulum()
            hinge = [o for o in self.joints.Group if o.Name.startswith("Revolute")][0]
            hinge.EnableAngleMin = True
            hinge.EnableAngleMax = True
            hinge.AngleMin = -30.0
            hinge.AngleMax = 30.0
            self.doc.recompute()
            _, result = self.simulate(end, 1e-2, AnalysisType=analysis)
            self.assertEqual(result, 0, analysis)
            frames = self.frames(rod, 1e-2)
            for time, centre, _ in frames:
                angle = math.degrees(math.atan2(5 - centre.z, centre.x - 10))
                self.assertLess(angle, 30 + 1e-4, (analysis, time))
            angle = math.degrees(math.atan2(5 - frames[-1][1].z, frames[-1][1].x - 10))
            self.assertAlmostEqual(angle, 30.0, delta=1e-4, msg=analysis)
