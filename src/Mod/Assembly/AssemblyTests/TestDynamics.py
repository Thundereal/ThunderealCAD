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

    def test_nothing_to_move(self):
        """A dynamic run whose joints tie nothing to the ground has nothing to
        move: it runs, its frames all where the parts are (it crashed:
        Chrono's solve of an empty system)"""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        a = self.box("A", (10, 10, 10), (30, 0, 0))
        b = self.box("B", (10, 10, 10), (50, 0, 0))
        self.doc.recompute()
        self.joint("Revolute", (a, ["Face2"]), plc(10, 5, 5), (b, ["Face1"]), plc(0, 5, 5))
        self.doc.recompute()
        _, result = self.simulate(0.05, 1e-2)
        self.assertEqual(result, 0)
        self.assertEqual(self.assembly.numberOfFrames(), 6)

    def test_without_gravity(self):
        """With gravity off nothing moves."""
        rod = self.pendulum()
        _, result = self.simulate(0.05, 1e-2, GravityEnabled=False)
        self.assertEqual(result, 0)
        for time, centre, _ in self.frames(rod, 1e-2):
            self.assertLess((centre - App.Vector(60, 5, 5)).Length, 1e-9, time)

    def driven(self, control, formula, analysis="Dynamic", gravity=True, reactions=False):
        """The pendulum's hinge driven by a motion; the rod's turn about it
        (rad, unsigned) at each frame of 0.2 s by 0.01 s, or None if the run
        was refused."""
        rod = self.pendulum()
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = analysis
        sim.Reactions = reactions
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

    def test_start_driven(self):
        """The rod driven from level at 3 t + t^2 rad: at the start it turns
        at 3 rad/s and speeds up at 2 rad/s^2. Its centre, 50 mm out, at
        -9 r along it and 2 r across; the hinge's force on it m (a - g).
        The start's values are the constraints solved at it, the motion's
        own acceleration and the turn's with them; they were taken back
        from the run's smallest steps."""
        self.assertIsNotNone(
            self.driven("Position", "3*time + time*time", reactions=True)
        )
        mass = 100 * 10 * 10 * 2.7e-6
        r = 50.0
        accel = self.channel("Rod", "Acceleration")[0]
        self.assertAlmostEqual(accel.x, -9.0 * r, delta=1e-6 * 9.0 * r)
        self.assertAlmostEqual(accel.y, 0.0, delta=1e-6 * 9.0 * r)
        self.assertAlmostEqual(abs(accel.z), 2.0 * r, delta=1e-6 * 9.0 * r)
        self.assertAlmostEqual(
            self.channel("Rod", "AngularAcceleration")[0].Length, 2.0, delta=1e-6
        )
        want = App.Vector(-9.0 * r, 0, math.copysign(2.0 * r, accel.z) + G) * mass
        force = self.channel("Revolute", "Force")[0]
        self.assertLess(min((force - want).Length, (force + want).Length), 1e-6 * want.Length, force)
        # The motor holds its weight's moment, less I alpha where it turns
        # down with it
        inertia = mass * ((100.0**2 + 10.0**2) / 12.0 + r * r)
        torque = mass * G * r + math.copysign(inertia * 2.0, accel.z)
        motor = self.channel("Revolute", "MotorTorque")[0]
        self.assertAlmostEqual(abs(motor), torque, delta=1e-6 * torque)

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

    def oscillator(self, damping=0.0, listed=True, onFaces=False, sensors=()):
        """A 10 mm cube on a vertical slider over a grounded one, a spring
        from the grounded one's top to its bottom, free at 4 mm; started at
        2 mm, without gravity: it swings about 4 mm by 2. With onFaces, the
        spring's ends are on the faces rather than points. sensors: each a
        dict of newSensor()'s arguments but the study and the source, the
        spring, made in the study first."""
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
        if onFaces:
            spring.setEnd(1, base, "Face6")
            spring.setEnd(2, block, "Face5")
        else:
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
        if sensors:
            import CommandCreateSimulation

            for sensor in sensors:
                CommandCreateSimulation.newSensor(sim, spring, **sensor)
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        heights = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            heights.append((i * 1e-4, block.Placement.Base.z))
        return heights

    def test_sensor_stops(self):
        """A sensor on the spring's length rising above its free length, 4
        mm, stops the run where it does, a quarter period in -- the time
        located, not the next frame's: the last frame is there, the length
        4 mm (docs/AssemblyDynamics.md sec 5.2's Sensor, P15)."""
        self.oscillator(sensors=[{"quantity": "SpringLength", "threshold": 4.0}])
        quarter = math.pi / 2 * math.sqrt(self.MASS / self.K)
        results = self.assembly.getSimulationResults()
        events = results["Events"]
        self.assertEqual(len(events), 1, events)
        self.assertTrue(events[0]["Stopped"])
        self.assertEqual(events[0]["Object"].Name, "Sensor")
        self.assertAlmostEqual(events[0]["Time"], quarter, delta=quarter * 1e-5)
        self.assertAlmostEqual(events[0]["Value"], 4.0, delta=1e-6)
        self.assertAlmostEqual(results["Times"][-1], events[0]["Time"], delta=1e-12)
        self.assertEqual(len(results["Times"]), int(events[0]["Time"] / 1e-4) + 2)
        self.assertAlmostEqual(self.channel("Spring", "SpringLength")[-1], 4.0, delta=1e-6)

    def test_sensor_marks(self):
        """Marking, the run goes on to its end, the times noted: rising
        above 4 mm a quarter period in and once a period after, falling
        below it three quarters in, each where the length is 4 mm."""
        self.oscillator(
            sensors=[
                {"quantity": "SpringLength", "threshold": 4.0, "action": "Mark"},
                {"quantity": "SpringLength", "threshold": 4.0, "action": "Mark", "compare": "Below"},
            ]
        )
        period = 2 * math.pi * math.sqrt(self.MASS / self.K)
        results = self.assembly.getSimulationResults()
        self.assertAlmostEqual(results["Times"][-1], 0.3, delta=1e-9)
        events = results["Events"]
        expected = sorted(
            [(period / 4 + n * period, "Sensor") for n in range(10) if period / 4 + n * period < 0.3]
            + [
                (3 * period / 4 + n * period, "Sensor001")
                for n in range(10)
                if 3 * period / 4 + n * period < 0.3
            ]
        )
        self.assertEqual([e["Name"] for e in events], [name for _, name in expected])
        for event, (time, _) in zip(events, expected):
            self.assertFalse(event["Stopped"])
            self.assertAlmostEqual(event["Time"], time, delta=period * 1e-4)
            self.assertAlmostEqual(event["Value"], 4.0, delta=1e-6)

    def test_sensor_on_nothing_reported(self):
        """A sensor on a quantity its source does not report does nothing,
        and says so; the run goes to its end."""
        self.oscillator(sensors=[{"quantity": "ContactForce", "threshold": 1.0}])
        results = self.assembly.getSimulationResults()
        self.assertEqual(results["Events"], [])
        self.assertAlmostEqual(results["Times"][-1], 0.3, delta=1e-9)

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

    def test_spring_holds_fixed_pair(self):
        """A cube a spring alone holds over the grounded one, without
        gravity, and a second fixed on top of it: nothing joins the pair to
        the ground, and their joint is the run's all the same -- they swing
        as one, between 12 and 16 by half a period of sqrt(2 m / k), the top
        one 10 above the other. Left out, the top one stayed where it was
        and the other swung alone."""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        block = self.box("Block", (10, 10, 10), (0, 0, 12))
        top = self.box("Top", (10, 10, 10), (0, 0, 22))
        self.doc.recompute()
        self.joint("Fixed", (block, ["Face6"]), plc(5, 5, 10), (top, ["Face5"]), plc(5, 5, 0))
        physics = self.assembly.getGroup("Physics", True)
        spring = physics.newObject("Assembly::SpringDamper", "Spring")
        spring.Component1 = base
        spring.Point1 = App.Vector(5, 5, 10)
        spring.Component2 = block
        spring.Point2 = App.Vector(5, 5, 0)
        spring.Stiffness = "%g N/m" % self.K
        spring.FreeLength = 4.0
        _, result = self.simulate(0.3, 1e-4, GravityEnabled=False, Elements=[spring])
        self.assertEqual(result, 0)
        heights = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            heights.append((i * 1e-4, block.Placement.Base.z))
            self.assertAlmostEqual(top.Placement.Base.z - block.Placement.Base.z, 10.0, delta=1e-6)
        half = math.pi * math.sqrt(2 * self.MASS / self.K)
        crossed = self.crossings(heights, 14.0)
        self.assertGreaterEqual(len(crossed), 3)
        for a, b in zip(crossed, crossed[1:]):
            self.assertAlmostEqual(b - a, half, delta=half * 1e-3)
        self.assertAlmostEqual(max(z for _, z in heights), 16.0, delta=1e-3)

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

    def test_spring_ends_on_faces(self):
        """Its ends on the faces, the cube swings as with the points."""
        points = self.oscillator()
        App.closeDocument(self.doc.Name)
        self.setUp()
        faces = self.oscillator(onFaces=True)
        self.assertEqual(len(faces), len(points))
        for (time, a), (_, b) in zip(points, faces):
            self.assertAlmostEqual(a, b, delta=1e-9, msg=time)

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

    def channelOf(self, obj, quantity):
        """The values of the last run's channel of obj and the quantity"""
        results = self.assembly.getSimulationResults()
        found = [c for c in results["Channels"] if c["Object"] == obj and c["Quantity"] == quantity]
        self.assertEqual(len(found), 1, [(c["Name"], c["Quantity"]) for c in results["Channels"]])
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

    def test_body_motion(self):
        """The rod's motion, which its inertia loads are made of
        (docs/AssemblyDynamics.md, D4a): its centre of mass's acceleration,
        its turn about the hinge's axis, y, and the turn's rate, from the
        energy it has lost and gravity's torque about the hinge."""
        rod = self.pendulum()
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        accels = self.channel("Rod", "Acceleration")
        spins = self.channel("Rod", "AngularVelocity")
        rates = self.channel("Rod", "AngularAcceleration")
        mass = 100 * 10 * 10 * 2.7e-6
        d = 50.0
        inertia = mass * ((100**2 + 10**2) / 12 + d**2)
        frames = self.frames(rod, 1e-2)
        for (time, centre, _), accel, spin, rate in zip(frames, accels, spins, rates):
            theta = math.atan2(5 - centre.z, centre.x - 10)
            omega = math.sqrt(2 * mass * G * d * math.sin(max(theta, 0.0)) / inertia)
            alpha = mass * G * d * math.cos(theta) / inertia
            e = App.Vector(math.cos(theta), 0, -math.sin(theta))
            t = App.Vector(-math.sin(theta), 0, -math.cos(theta))
            expected = t * (alpha * d) - e * (omega * omega * d)
            self.assertLess((accel - expected).Length, G * 1e-3, time)
            # Down from +x towards -z: a turn about +y
            self.assertLess((spin - App.Vector(0, omega, 0)).Length, 1e-3 * math.sqrt(G / d), time)
            self.assertLess((rate - App.Vector(0, alpha, 0)).Length, 1e-3 * G / d, time)

    def balanced(self, component, frames, force, length):
        """The free body of component at each of frames (MotionLoads,
        docs/AssemblyDynamics.md D4a step 2) balances: its residual within
        1e-6 of force, and of force times length about its centre; nothing
        missing. The free bodies, in frame order."""
        import MotionLoads

        bodies = []
        for frame in frames:
            body = MotionLoads.loadsOn(self.assembly, component, frame)
            residual, moment = body["Residual"]
            self.assertLess(residual.Length, 1e-6 * force, frame)
            self.assertLess(moment.Length, 1e-6 * force * length, frame)
            self.assertEqual(body["Missing"], [])
            bodies.append(body)
        return bodies

    def test_loads_on_pendulum(self):
        """The rod's free body: the hinge's reaction at the hinge, (0, 5, 5)
        in the rod's coordinates, the opposite of what the run reports on
        the grounded block, its first reference; gravity and the inertia
        balance it. The rod is left where it was."""
        rod = self.pendulum()
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        before = App.Placement(rod.Placement)
        forces = self.channel("Revolute", "Force")
        weight = 100 * 10 * 10 * 2.7e-6 * G
        bodies = self.balanced(rod, range(0, 21, 4), weight, 100.0)
        self.assertTrue(rod.Placement.isSame(before, 1e-12))
        for body in bodies:
            frame = body["Frame"]
            self.assertEqual(len(body["Loads"]), 1)
            hinge = body["Loads"][0]
            self.assertEqual(hinge["Kind"], "Joint")
            self.assertEqual(hinge["Reference"][0], rod)
            self.assertLess((hinge["Point"] - App.Vector(0, 5, 5)).Length, 1e-6, frame)
            self.assertAlmostEqual(hinge["Force"].Length, forces[frame].Length, delta=1e-9 * weight)
            self.assertLess((body["CenterOfMass"] - App.Vector(50, 5, 5)).Length, 1e-9)

    def test_loads_on_middle_link(self):
        """A double pendulum's first rod, between two hinges -- the first
        reference of one, the second of the other: its free body
        balances."""
        rod = self.pendulum()
        second = self.box("Second", (100, 10, 10), (110, 0, 0))
        self.doc.recompute()
        self.joint(
            "Revolute",
            (rod, ["Face2"]),
            plc(100, 5, 5, App.Vector(1, 0, 0), -90),
            (second, ["Face1"]),
            plc(0, 5, 5, App.Vector(1, 0, 0), -90),
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        weight = 100 * 10 * 10 * 2.7e-6 * G
        for body in self.balanced(rod, range(0, 21, 4), 2 * weight, 100.0):
            self.assertEqual(len(body["Loads"]), 2)
            points = sorted((l["Point"].x for l in body["Loads"]))
            self.assertAlmostEqual(points[0], 0.0, delta=1e-6)
            self.assertAlmostEqual(points[1], 100.0, delta=1e-6)
        # And the second rod's, on one hinge
        self.balanced(second, range(0, 21, 4), weight, 100.0)

    def test_loads_to_fem(self):
        """The double pendulum's first rod at 80 ms as a static stress
        analysis (MotionLoads.makeAnalysis, D4a step 4): held at the hinge
        with the larger force through a rigid coupling on its end face, the
        other hinge's load through a distributing one, the body motion
        load. Solved, where CalculiX is found, what holds it there is the
        run's reaction at that hinge within 0.5%, less the body load on the
        held face's own nodes: the loads balance in the analysis as in the
        run."""
        import os
        import shutil
        import tempfile

        import MotionLoads

        rod = self.pendulum()
        second = self.box("Second", (100, 10, 10), (110, 0, 0))
        self.doc.recompute()
        self.joint(
            "Revolute",
            (rod, ["Face2"]),
            plc(100, 5, 5, App.Vector(1, 0, 0), -90),
            (second, ["Face1"]),
            plc(0, 5, 5, App.Vector(1, 0, 0), -90),
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        analysis, body, held = MotionLoads.makeAnalysis(self.assembly, rod, 8, maxSize=4.0)
        kinds = sorted(o.Proxy.Type if hasattr(o, "Proxy") else o.TypeId for o in analysis.Group)
        self.assertIn("Fem::ConstraintBodyMotion", kinds)
        couplings = [o for o in analysis.Group if o.TypeId == "Fem::ConstraintRigidBody"]
        self.assertEqual(sorted(c.Coupling for c in couplings), ["Distributing", "Rigid"])
        hold = [c for c in couplings if c.Coupling == "Rigid"][0]
        load = [c for c in couplings if c.Coupling == "Distributing"][0]
        # Each on its end face of the copy, which stands where the rod does
        self.assertEqual(hold.References[0][1], ("Face1",) if held["Point"].x < 50 else ("Face2",))
        self.assertEqual(len(load.References[0][1]), 1)
        mesh = [o for o in analysis.Group if o.TypeId == "Fem::FemMeshShapeNetgenObject"][0]
        self.assertGreater(mesh.FemMesh.VolumeCount, 0)

        from femtools import ccxtools

        solver = [o for o in analysis.Group if o.TypeId == "Fem::FemSolverObjectPython"][0]
        work = tempfile.mkdtemp(prefix="asm_loadsfem_")
        try:
            fea = ccxtools.FemToolsCcx(analysis, solver, test_mode=True)
            fea.update_objects()
            fea.setup_working_dir(work)
            self.assertEqual(fea.check_prerequisites(), "")
            fea.write_inp_file()
            binary = (
                App.ParamGet("User parameter:BaseApp/Preferences/Mod/Fem/Ccx")
                .GetString("ccxBinaryPath", "")
                .strip()
            )
            binary = shutil.which(binary) if binary else shutil.which("ccx")
            if not binary:
                self.skipTest("no CalculiX binary")
            fea.setup_ccx(binary)
            fea.start_ccx()
            with open(os.path.splitext(fea.inp_file_name)[0] + ".dat") as f:
                text = f.read()
        finally:
            shutil.rmtree(work, ignore_errors=True)

        def total(node):
            lines = text.splitlines()
            title = "total force (fx,fy,fz) for set %s_%s" % (hold.Name.upper(), node)
            for k, line in enumerate(lines):
                if line.strip().startswith(title):
                    return App.Vector(*[float(v) for v in lines[k + 2].split()])
            self.fail("no %s in the results" % title)

        place = rod.getGlobalPlacement()
        force = place.Rotation.multVec(held["Force"]) * 1e-3
        torque = place.Rotation.multVec(held["Torque"]) * 1e-3
        # What CalculiX reports holding it leaves out the body load on the
        # held face's own nodes, which goes into the coupling without
        # passing through the part: 0.6% of its weight at 4 mm, 0.3% at 2
        weight = body["Mass"] * G * 1e-3
        self.assertLess((total("REFNODE") - force).Length, 5e-3 * force.Length + 1e-2 * weight)
        scale = (force.Length + weight) * 100.0
        self.assertLess((total("ROTNODE") - torque).Length, 5e-3 * scale)

    def test_section_depth(self):
        """The margin the stress away from the loaded faces leaves: about a
        section's depth, the shortest span through the point along the
        part's principal axes -- a bar's thinner side, at its middle or at
        its end; across a bore, wall to wall; never less than the mesh
        size"""
        import numpy as np

        import MotionLoads

        # A bar 120 x 10 x 8, its nodes 1 mm apart, turned off the axes
        grid = np.mgrid[0:121, 0:11, 0:9].reshape(3, -1).T.astype(float)
        turn = App.Rotation(App.Vector(1, 2, 3), 35).toMatrix()
        m = np.array([[turn.A[4 * r + c] for c in range(3)] for r in range(3)])
        coords = grid @ m.T
        centred = coords - coords.mean(axis=0)
        axes = np.linalg.eigh(centred.T @ centred)[1].T

        def depth(point, size=1.0, nodes=coords):
            return MotionLoads._sectionDepth(nodes, axes, m @ np.array(point), size)

        self.assertAlmostEqual(depth((60, 10, 4)), 8.0, delta=1e-9)
        self.assertAlmostEqual(depth((0, 5, 4)), 8.0, delta=1e-9)
        self.assertAlmostEqual(depth((60, 10, 4), size=12.0), 12.0)
        # A bore of 3 mm round its end, along its 8 mm: from the bore's
        # axis, the line along it is in the bore, and across it the bar's
        # 10 mm depth, wall to wall
        bored = [p for p in grid if (p[0] - 5) ** 2 + (p[1] - 5) ** 2 > 9]
        nodes = np.array(bored) @ m.T
        self.assertAlmostEqual(depth((5, 5, 4), nodes=nodes), 10.0, delta=1e-9)

    def test_loads_to_fem_frames_jobs(self):
        """The frames' CalculiX runs several at once, each in a folder of its
        own: the series the same as one at a time, to the last digit; a
        stop asked for at a frame leaves those started before it."""
        import MotionLoads

        if not MotionLoads.ccxBinary():
            self.skipTest("no CalculiX binary")
        rod = self.pendulum()
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        frames = list(range(0, 21, 3))
        one = MotionLoads.solveFrames(self.assembly, rod, frames, maxSize=4.0, jobs=1)
        three = MotionLoads.solveFrames(self.assembly, rod, frames, maxSize=4.0, jobs=3)
        self.assertEqual(list(three.Frames), frames)
        for name in ("VonMises", "VonMisesAt", "VonMisesAway", "FemHeldForce", "FemHeldTorque"):
            self.assertEqual(list(getattr(three, name)), list(getattr(one, name)), name)

        stopped = MotionLoads.solveFrames(
            self.assembly, rod, frames, maxSize=4.0, jobs=3, progress=lambda k, n: k < 4
        )
        self.assertEqual(list(stopped.Frames), frames[:4])
        self.assertEqual(list(stopped.VonMises), list(one.VonMises)[:4])

    def test_modal_response(self):
        """A mode's dynamic part through loads linear between frames, in
        closed form (MotionLoads._modalResponse): y = q - c, q'' + 2 z w q'
        + w^2 q = w^2 c, at rest at the first frame (q = c, q' = 0) --
        against the same integrated by RK4 at a thousandth of the shortest
        period, undamped and damped, to 1e-8 of c's largest. Loads that
        stand still leave none."""
        import numpy as np

        import MotionLoads

        omega = 2.0 * math.pi * np.array([1.0, 3.7, 11.0])
        times = np.array([0.0, 0.3, 0.35, 0.9, 1.0, 1.6, 2.5])
        c = np.array(
            [
                [0.0, 1.0, 1.0, -0.5, 2.0, 0.0, 0.3],
                [1.0, 0.2, -1.0, 0.0, 0.5, 0.5, 0.5],
                [0.5, 0.5, 1.5, 0.2, -0.7, 0.1, 0.0],
            ]
        )
        grid = np.linspace(0.0, 2.5, 251)
        h = 1e-3 / 11.0

        def load(t):
            k = min(max(np.searchsorted(times, t, side="right") - 1, 0), len(times) - 2)
            w = (t - times[k]) / (times[k + 1] - times[k])
            return (1.0 - w) * c[:, k] + w * c[:, k + 1]

        for zeta in (0.0, 0.05):
            got = MotionLoads._modalResponse(times, c, omega, zeta, grid)

            def rate(t, q, v):
                return v, omega**2 * (load(t) - q) - 2.0 * zeta * omega * v

            q, v = c[:, 0].copy(), np.zeros(3)
            t = 0.0
            want = np.zeros_like(got)
            want[:, 0] = q - load(0.0)
            for j in range(1, len(grid)):
                n = int(round((grid[j] - t) / h))
                dt = (grid[j] - t) / n
                for _ in range(n):
                    k1 = rate(t, q, v)
                    k2 = rate(t + dt / 2, q + dt / 2 * k1[0], v + dt / 2 * k1[1])
                    k3 = rate(t + dt / 2, q + dt / 2 * k2[0], v + dt / 2 * k2[1])
                    k4 = rate(t + dt, q + dt * k3[0], v + dt * k3[1])
                    q = q + dt / 6 * (k1[0] + 2 * k2[0] + 2 * k3[0] + k4[0])
                    v = v + dt / 6 * (k1[1] + 2 * k2[1] + 2 * k3[1] + k4[1])
                    t += dt
                want[:, j] = q - load(grid[j])
            self.assertLess(np.abs(got - want).max(), 1e-8 * np.abs(c).max(), zeta)
        still = MotionLoads._modalResponse(times, np.ones_like(c), omega, 0.0, grid)
        self.assertEqual(np.abs(still).max(), 0.0)

    def test_held_modes(self):
        """The rod's modes held at its hinge, as To FEM's vibration takes
        them (MotionLoads._Modes): orthogonal through the mass of its
        10-node tetrahedra as built here (_tet10Mass, the nodes by where
        they are), to 1e-4 (1.3e-5) -- a mass or a node order wrong is not
        -- and the first, the rod clamped at its end, the beam's closed
        form 1.875^2 / (2 pi) sqrt(E I / (rho A)) / L^2 to 1% (0.15%). Its
        static state at the start, falling, made of its modes by its
        shares of them (coordinate()): what they leave of it under 1e-4 of
        it, by the mass (9.3e-6)."""
        import os
        import shutil
        import tempfile

        import numpy as np
        from femtools import ccxtools

        import MotionLoads

        if not MotionLoads.ccxBinary():
            self.skipTest("no CalculiX binary")
        rod = self.pendulum()
        _, result = self.simulate(0.01, 1e-2)
        self.assertEqual(result, 0)
        analysis, body, _ = MotionLoads.makeAnalysis(self.assembly, rod, 0, maxSize=4.0)
        solver = [o for o in analysis.Group if o.isDerivedFrom("Fem::FemSolverObjectPython")][0]
        solver.AnalysisType = "frequency"
        solver.EigenmodesCount = 8
        mesh = [o for o in analysis.Group if o.TypeId == "Fem::FemMeshShapeNetgenObject"][0]
        material = [o for o in analysis.Group if o.Name.startswith("LoadsMaterial")][0]
        self.doc.recompute()
        work = tempfile.mkdtemp(prefix="heldmodes_")
        try:
            fea = ccxtools.FemToolsCcx(analysis, solver, test_mode=True)
            fea.update_objects()
            fea.setup_working_dir(work)
            fea.setup_ccx(MotionLoads.ccxBinary())
            fea.write_inp_file()
            self.assertEqual(fea.start_ccx(), 0)
            modes = MotionLoads._Modes(os.path.splitext(fea.inp_file_name)[0], mesh.FemMesh)
            solver.AnalysisType = "static"
            self.doc.recompute()
            fea.write_inp_file()
            self.assertEqual(fea.start_ccx(), 0)
            blocks = MotionLoads._frdFields(os.path.splitext(fea.inp_file_name)[0] + ".frd")
        finally:
            shutil.rmtree(work, ignore_errors=True)
        n = len(modes.frequencies)
        self.assertEqual(n, 8)
        gram = np.array([[modes.inner(a, b) for b in modes.shapes] for a in modes.shapes])
        scale = np.sqrt(np.outer(np.diag(gram), np.diag(gram)))
        off = np.abs(gram / scale - np.eye(n)).max()
        self.assertLess(off, 1e-4)
        card = material.Material
        e = App.Units.Quantity(card["YoungsModulus"]).getValueAs("MPa").Value
        rho = body["Mass"] / (100.0 * 10.0 * 10.0) * 1e-3  # t/mm^3
        beam = 1.875**2 / (2 * math.pi) * math.sqrt(e * 10.0**2 / 12.0 / rho) / 100.0**2
        self.assertAlmostEqual(modes.frequencies[0], beam, delta=0.01 * beam)
        # The static state by its shares of the modes
        u = modes.align([b for b in blocks if b[0] == "DISP"][0])[:, :3]
        c = modes.coordinate(u)
        rebuilt = np.tensordot(c, modes.nodal, axes=1)
        whole = modes.inner(u[modes.index], u[modes.index])
        rest = u - rebuilt
        self.assertLess(modes.inner(rest[modes.index], rest[modes.index]) / whole, 1e-4)

    def test_held_modes_free(self):
        """The rod's modes held as its hinge holds it (_holdAsJoint), free
        to turn about the hinge's axis, the global y: the turn is no mode of
        them, each a share of it under 1e-4 (4.5e-6); the first across the
        hinge's axis still the rod clamped at its end, 1.875^2 / (2 pi)
        sqrt(E I / (rho A)) / L^2 to 1% (0.15%), and the first about it the
        rod pinned at its end, 3.927^2 instead of 1.875^2, to 5% (2.4% under
        it: the rod is 10 sections long, and shear takes its share). Its
        static state held stiff, made of them by its shares of them: what
        they leave of it is the turn but for 1e-5 of it, by the mass
        (8.7e-7)."""
        import os
        import shutil
        import tempfile

        import numpy as np
        from femtools import ccxtools

        import MotionLoads

        if not MotionLoads.ccxBinary():
            self.skipTest("no CalculiX binary")
        rod = self.pendulum()
        _, result = self.simulate(0.01, 1e-2)
        self.assertEqual(result, 0)
        analysis, body, held = MotionLoads.makeAnalysis(self.assembly, rod, 0, maxSize=4.0)
        solver = [o for o in analysis.Group if o.isDerivedFrom("Fem::FemSolverObjectPython")][0]
        mesh = [o for o in analysis.Group if o.TypeId == "Fem::FemMeshShapeNetgenObject"][0]
        hold = [o for o in analysis.Group if o.Name.startswith("Hold")][0]
        material = [o for o in analysis.Group if o.Name.startswith("LoadsMaterial")][0]
        place = rod.getGlobalPlacement()
        free, kind = MotionLoads._holdAsJoint({"Placement": place}, hold, held)
        self.assertEqual(kind, "Turn")
        self.assertEqual(len(free), 1)
        axis, point = free[0]
        y = place.Rotation.multVec(App.Vector(0, 1, 0))
        self.assertAlmostEqual(abs(App.Vector(*axis).dot(y)), 1.0, delta=1e-9)
        solver.AnalysisType = "frequency"
        solver.EigenmodesCount = 8 + len(free)
        self.doc.recompute()
        work = tempfile.mkdtemp(prefix="heldfree_")
        try:
            fea = ccxtools.FemToolsCcx(analysis, solver, test_mode=True)
            fea.update_objects()
            fea.setup_working_dir(work)
            fea.setup_ccx(MotionLoads.ccxBinary())
            fea.write_inp_file()
            with open(fea.inp_file_name) as f:
                self.assertIn("*TRANSFORM, NSET=%s_RotNode" % hold.Name, f.read())
            self.assertEqual(fea.start_ccx(), 0)
            base = os.path.splitext(fea.inp_file_name)[0]
            modes = MotionLoads._Modes(base, mesh.FemMesh, free, 8)
            MotionLoads._holdStiff(hold)
            solver.AnalysisType = "static"
            self.doc.recompute()
            fea.write_inp_file()
            with open(fea.inp_file_name) as f:
                self.assertNotIn("*TRANSFORM", f.read())
            self.assertEqual(fea.start_ccx(), 0)
            blocks = MotionLoads._frdFields(os.path.splitext(fea.inp_file_name)[0] + ".frd")
        finally:
            shutil.rmtree(work, ignore_errors=True)
        self.assertEqual(len(modes.frequencies), 8)
        # The turn about the hinge, by the mass
        at = np.array([tuple(mesh.FemMesh.Nodes[n]) for n in modes.ids])
        turn = np.cross(np.array(axis), at - np.array(point))[modes.index]
        turn /= math.sqrt(modes.inner(turn, turn))
        shares = [
            abs(modes.inner(turn, s)) / math.sqrt(n) for s, n in zip(modes.shapes, modes.norms)
        ]
        self.assertLess(max(shares), 1e-4)
        card = material.Material
        e = App.Units.Quantity(card["YoungsModulus"]).getValueAs("MPa").Value
        rho = body["Mass"] / (100.0 * 10.0 * 10.0) * 1e-3  # t/mm^3
        root = math.sqrt(e * 10.0**2 / 12.0 / rho) / (2 * math.pi) / 100.0**2
        clamped, pinned = 1.8751**2 * root, 3.9266**2 * root
        # A mode that moves along the hinge's axis bends across it, held
        # stiff so; one that does not bends about it, pinned
        along = [float(np.abs(s @ np.array(axis)).max() / np.abs(s).max()) for s in modes.shapes]
        first = [f for f, a in zip(modes.frequencies, along) if a > 0.5][0]
        about = [f for f, a in zip(modes.frequencies, along) if a < 0.1][0]
        self.assertAlmostEqual(first, clamped, delta=0.01 * clamped)
        self.assertAlmostEqual(about, pinned, delta=0.05 * pinned)
        # The static state held stiff, by its shares of them
        u = modes.align([b for b in blocks if b[0] == "DISP"][0])[:, :3]
        c = modes.coordinate(u)
        rest = (u - np.tensordot(c, modes.nodal, axes=1))[modes.index]
        whole = modes.inner(u[modes.index], u[modes.index])
        rest = rest - modes.inner(turn, rest) * turn
        self.assertLess(modes.inner(rest, rest) / whole, 1e-5)

    def test_loads_to_fem_vibration(self):
        """To FEM's frames with the part's own vibration (D4a): the pendulum
        swinging, its loads changing slowly against its periods -- 1.2 ms
        at most, the frames 10 ms apart -- rings by little, its stress at
        each frame within 1e-3 (1.4e-5; 1.2e-4 with its modes held stiff)
        of the largest of the frames' static ones; its modes free to turn on
        its hinge, the first in its swing's plane at 3.5 kHz; the series'
        samples between the frames, at the frames the same; the analysis
        left at the largest sample, whose result is that field."""
        import MotionLoads

        if not MotionLoads.ccxBinary():
            self.skipTest("no CalculiX binary")
        rod = self.pendulum()
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        frames = list(range(21))
        series = MotionLoads.solveFrames(
            self.assembly, rod, frames, maxSize=4.0, dynamic=True, damping=0.02
        )
        self.assertTrue(series.Dynamic)
        self.assertEqual(len(series.ModeFrequencies), MotionLoads.MODES)
        self.assertEqual(series.ModesFree, "Turn")
        self.assertEqual(list(series.ModeFrequencies), sorted(series.ModeFrequencies))
        largest = max(series.StaticVonMises)
        for k, frame in enumerate(frames):
            self.assertLess(
                abs(series.VonMises[k] - series.StaticVonMises[k]), 1e-3 * largest, frame
            )
        # The samples: each frame's among them, its value the frame's;
        # enough between them for the highest mode, at most SAMPLES
        samples = list(series.SampleTimes)
        self.assertEqual(samples, sorted(samples))
        for k, time in enumerate(series.Times):
            j = samples.index(time)
            self.assertEqual(series.SampleVonMises[j], series.VonMises[k])
        self.assertEqual(len(samples), MotionLoads.SAMPLES * 20 + 1)
        analysis = [o for o in series.InList if o.isDerivedFrom("Fem::FemAnalysis")][0]
        results = [o for o in analysis.Group if o.isDerivedFrom("Fem::FemResultObject")]
        self.assertEqual(len(results), 1)
        peak = max(series.SampleVonMises)
        self.assertAlmostEqual(max(results[0].vonMises), peak, delta=1e-4 * peak)
        channels = MotionLoads.stressChannels(self.assembly)
        self.assertEqual([c["Quantity"] for c in channels], ["VonMises", "VonMisesAway"])
        self.assertEqual(channels[0]["SampleTimes"], samples)
        self.assertEqual(channels[0]["Samples"], list(series.SampleVonMises))
        self.assertEqual(channels[0]["Static"], list(series.StaticVonMises))

    def test_loads_to_fem_frames(self):
        """The double pendulum's first rod solved at five frames of its run
        (MotionLoads.solveFrames, D4a step 5): one analysis, one mesh, held
        at one hinge throughout; at each frame what holds it there is the
        run's reaction, as test_loads_to_fem checks at one; the analysis
        left at the frame of the largest stress, with that result; the
        series a channel of the run's results, at the frames solved, and
        not of another run of the study."""
        import CommandCreateSimulation
        import MotionLoads

        if not MotionLoads.ccxBinary():
            self.skipTest("no CalculiX binary")
        rod = self.pendulum()
        second = self.box("Second", (100, 10, 10), (110, 0, 0))
        self.doc.recompute()
        self.joint(
            "Revolute",
            (rod, ["Face2"]),
            plc(100, 5, 5, App.Vector(1, 0, 0), -90),
            (second, ["Face1"]),
            plc(0, 5, 5, App.Vector(1, 0, 0), -90),
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        sim, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        frames = [0, 5, 10, 15, 20]
        calls = []

        def progress(k, n):
            calls.append((k, n))

        series = MotionLoads.solveFrames(
            self.assembly, rod, frames + [10], maxSize=4.0, progress=progress
        )
        self.assertEqual(calls, [(k, 5) for k in range(5)])
        self.assertEqual(list(series.Frames), frames)
        self.assertTrue(series.Component == rod)
        analysis = [o for o in series.InList if o.isDerivedFrom("Fem::FemAnalysis")][0]
        meshes = [o for o in analysis.Group if o.TypeId == "Fem::FemMeshShapeNetgenObject"]
        self.assertEqual(len(meshes), 1)
        couplings = [o for o in analysis.Group if o.TypeId == "Fem::ConstraintRigidBody"]
        self.assertEqual(sorted(c.Coupling for c in couplings), ["Distributing", "Rigid"])
        results = [o for o in analysis.Group if o.isDerivedFrom("Fem::FemResultObject")]
        self.assertEqual(len(results), 1)

        body = MotionLoads.loadsOn(self.assembly, rod, 0)
        weight = body["Mass"] * G * 1e-3
        box = rod.Shape.BoundBox
        for k, frame in enumerate(frames):
            run, fem = series.HeldForce[k], series.FemHeldForce[k]
            self.assertLess((fem - run).Length, 5e-3 * run.Length + 1e-2 * weight, frame)
            scale = (run.Length + weight) * 100.0
            self.assertLess(
                (series.FemHeldTorque[k] - series.HeldTorque[k]).Length, 5e-3 * scale, frame
            )
            self.assertGreater(series.VonMises[k], 0.0)
            at = series.VonMisesAt[k]
            # In the rod's own coordinates, those of its shape before its
            # Placement: 100 along x
            self.assertTrue(-1e-6 <= at.x <= box.XLength + 1e-6, at)
            self.assertTrue(-1e-6 <= at.y <= box.YLength + 1e-6, at)
        # Left at its largest, which its result holds
        peak = max(series.VonMises)
        self.assertAlmostEqual(max(results[0].vonMises), peak, delta=1e-6 * peak)

        # Away from the loaded faces: the rod's section's depth (10 mm, more
        # than the mesh size) off both end faces, where the hold and the
        # other hinge's coupling are, and no more
        size = 10.0
        for k, frame in enumerate(frames):
            away, at = series.VonMisesAway[k], series.VonMisesAwayAt[k]
            self.assertLessEqual(away, series.VonMises[k] + 1e-12, frame)
            self.assertGreater(away, 0.0, frame)
            self.assertTrue(size - 1e-6 <= at.x <= box.XLength - size + 1e-6, at)
            if size <= series.VonMisesAt[k].x <= box.XLength - size:
                self.assertEqual(away, series.VonMises[k], frame)

        channels = MotionLoads.stressChannels(self.assembly)
        self.assertEqual([c["Quantity"] for c in channels], ["VonMises", "VonMisesAway"])
        values = channels[0]["Values"]
        self.assertEqual(len(values), self.assembly.numberOfFrames())
        for frame, value in enumerate(values):
            if frame in frames:
                self.assertEqual(value, series.VonMises[frames.index(frame)])
            else:
                self.assertIsNone(value)
        header, rows = CommandCreateSimulation.resultsTable(self.assembly)
        column = header.index("Rod: Largest von Mises stress (MPa)")
        self.assertEqual(rows[5][column], series.VonMises[1])
        self.assertIsNone(rows[1][column])

        # Run again with other loads: the series is not of this run
        sim.Gravity = App.Vector(0, 0, -2 * G)
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assertEqual(MotionLoads.stressChannels(self.assembly), [])

    def test_loads_on_spring(self):
        """The oscillator's block: the slider's reaction and the spring's
        force, along the slider, k (4 - length); no gravity."""
        self.oscillator()
        block = self.doc.getObject("Block")
        lengths = self.channel("Spring", "SpringLength")
        # N/m times mm is mN: k times the 2 mm it swings by
        for body in self.balanced(block, range(0, 3001, 500), self.K * 2.0, 10.0):
            spring = [l for l in body["Loads"] if l["Kind"] == "Spring"]
            self.assertEqual(len(spring), 1)
            want = self.K * (4.0 - lengths[body["Frame"]])
            self.assertAlmostEqual(spring[0]["Force"].z, want, delta=1e-9 * self.K)
            self.assertLess((spring[0]["Point"] - App.Vector(5, 5, 0)).Length, 1e-9)

    def doublePendulum(self):
        """The pendulum with a second rod hinged at its far end"""
        rod = self.pendulum()
        second = self.box("Second", (100, 10, 10), (110, 0, 0))
        self.doc.recompute()
        self.joint(
            "Revolute",
            (rod, ["Face2"]),
            plc(100, 5, 5, App.Vector(1, 0, 0), -90),
            (second, ["Face1"]),
            plc(0, 5, 5, App.Vector(1, 0, 0), -90),
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        return rod, second

    def test_loads_on_torsion(self):
        """The rod on a torsion spring at its hinge: in its free body the
        spring's torque about the hinge's axis, y, at the hinge, which the
        run reports; and the free body balances."""
        rod, spring = self.torsion(2000.0)
        _, result = self.simulate(0.2, 1e-2, Elements=[spring])
        self.assertEqual(result, 0)
        torques = self.channel("TorsionSpring", "SpringTorque")
        weight = 100 * 10 * 10 * 2.7e-6 * G
        largest = max(abs(t) for t in torques)
        self.assertGreater(largest, 0.1 * weight * 100.0)
        for body in self.balanced(rod, range(0, 21, 4), weight, 100.0):
            mine = [l for l in body["Loads"] if l["Kind"] == "TorsionSpring"]
            self.assertEqual(len(mine), 1)
            load = mine[0]
            self.assertTrue(load["Source"] == spring)
            self.assertEqual(load["Reference"][0], rod)
            self.assertLess((load["Point"] - App.Vector(0, 5, 5)).Length, 1e-6)
            self.assertEqual(load["Force"].Length, 0.0)
            torque = torques[body["Frame"]]
            self.assertAlmostEqual(load["Torque"].Length, abs(torque), delta=1e-9 * largest)
            # About the rod's own y, the hinge's axis
            self.assertAlmostEqual(abs(load["Torque"].y), abs(torque), delta=1e-9 * largest)

    def test_loads_on_applied_load(self):
        """A force on the double pendulum's second rod at its far end, the
        first rod its reaction: the run reports it; in the second rod's
        free body at its end, in the first's the opposite where the end was
        at the start, which stays on the first; both balance. And a torque
        on the first rod, the ground its reaction."""
        import CommandCreateLoad

        rod, second = self.doublePendulum()
        force = CommandCreateLoad.makeLoad(
            self.assembly,
            (second, App.Placement(App.Vector(100, 5, 5), App.Rotation())),
            magnitude="0.02 N",
            direction=App.Vector(0, 0, 1),
            reaction=rod,
        )
        force.Profile = "1 + time*10"
        torque = CommandCreateLoad.makeLoad(
            self.assembly,
            (rod, App.Placement(App.Vector(50, 5, 5), App.Rotation())),
            kind="Torque",
            magnitude="1 N*mm",
            direction=App.Vector(0, 1, 0),
        )
        _, result = self.simulate(0.2, 1e-2, Elements=[force, torque])
        self.assertEqual(result, 0)
        forces = self.channelOf(force, "AppliedForce")
        torques = self.channelOf(torque, "AppliedTorque")
        times = self.assembly.getSimulationResults()["Times"]
        for time, f, t in zip(times, forces, torques):
            # 20 mN (1 + 10 t), along the frame's z as the second rod turns
            self.assertAlmostEqual(f.Length, 20.0 * (1 + 10 * time), delta=1e-9, msg=time)
            self.assertAlmostEqual(t.Length, 1000.0, delta=1e-9, msg=time)
        weight = 100 * 10 * 10 * 2.7e-6 * G
        self.assembly.updateForFrame(0)
        start = rod.Placement.inverse().multVec(second.Placement.multVec(App.Vector(100, 5, 5)))
        for body in self.balanced(rod, range(0, 21, 4), 3 * weight, 100.0):
            frame = body["Frame"]
            mine = {l["Source"].Name: l for l in body["Loads"] if l["Kind"] == "Load"}
            self.assertEqual(sorted(mine), sorted([force.Name, torque.Name]))
            self.assertLess((mine[force.Name]["Point"] - start).Length, 1e-9)
            self.assertAlmostEqual(
                mine[force.Name]["Force"].Length, forces[frame].Length, delta=1e-9
            )
            self.assertEqual(mine[force.Name]["Torque"].Length, 0.0)
            self.assertAlmostEqual(mine[torque.Name]["Torque"].y, 1000.0, delta=1e-9)
            self.assertEqual(mine[torque.Name]["Force"].Length, 0.0)
        for body in self.balanced(second, range(0, 21, 4), 3 * weight, 100.0):
            mine = [l for l in body["Loads"] if l["Kind"] == "Load"]
            self.assertEqual(len(mine), 1)
            self.assertLess((mine[0]["Point"] - App.Vector(100, 5, 5)).Length, 1e-9)
            # Along the second rod's own z
            self.assertAlmostEqual(mine[0]["Force"].z, forces[body["Frame"]].Length, delta=1e-9)

    def test_loads_to_fem_torsion(self):
        """The double pendulum with a torsion spring on each hinge, its
        first rod solved at three frames: held at the hinge to the base,
        whose spring's torque the hold takes with the hinge's reaction; the
        other spring's through its hinge's coupling, one for the two. What
        holds the rod in the analysis is the run's there, as
        test_loads_to_fem_frames checks without the springs."""
        import MotionLoads

        if not MotionLoads.ccxBinary():
            self.skipTest("no CalculiX binary")
        rod, second = self.doublePendulum()
        hinges = sorted(
            (o for o in self.joints.Group if o.Name.startswith("Revolute")), key=lambda o: o.Name
        )
        physics = self.assembly.getGroup("Physics", True)
        springs = []
        for hinge in hinges:
            spring = physics.newObject("Assembly::TorsionSpring", "TorsionSpring")
            spring.Joint = hinge
            spring.Stiffness = 2000.0
            springs.append(spring)
        _, result = self.simulate(0.2, 1e-2, Elements=springs)
        self.assertEqual(result, 0)
        frames = [4, 12, 20]
        series = MotionLoads.solveFrames(self.assembly, rod, frames, hold=hinges[0], maxSize=4.0)
        analysis = [o for o in series.InList if o.isDerivedFrom("Fem::FemAnalysis")][0]
        couplings = [o for o in analysis.Group if o.TypeId == "Fem::ConstraintRigidBody"]
        self.assertEqual(sorted(c.Coupling for c in couplings), ["Distributing", "Rigid"])
        body = MotionLoads.loadsOn(self.assembly, rod, 0)
        weight = body["Mass"] * G * 1e-3
        torques = self.channelOf(springs[0], "SpringTorque")
        self.assertGreater(max(abs(torques[f]) for f in frames) * 1e-3, 0.05 * weight * 100.0)
        for k, frame in enumerate(frames):
            free = MotionLoads.loadsOn(self.assembly, rod, frame)
            joint = [l for l in free["Loads"] if l["Source"] == hinges[0]][0]
            spring = [l for l in free["Loads"] if l["Source"] == springs[0]][0]
            held = (joint["Torque"] + spring["Torque"]) * 1e-3
            self.assertLess((series.HeldTorque[k] - held).Length, 1e-9 * weight * 100.0, frame)
            run, fem = series.HeldForce[k], series.FemHeldForce[k]
            self.assertLess((fem - run).Length, 5e-3 * run.Length + 1e-2 * weight, frame)
            scale = (run.Length + weight) * 100.0
            self.assertLess(
                (series.FemHeldTorque[k] - series.HeldTorque[k]).Length, 5e-3 * scale, frame
            )

    def test_loads_on_rigid_group(self):
        """The pendulum's rod made of two boxes in a rigid group, Rod and Tip
        (50 x 10 x 10 on its end): the run reports the group's motion, under
        Rod's name. Each one's free body: its own centre's acceleration from
        the group's; what the other holds it with on the faces where they
        meet -- Rod's Face2, Tip's Face1 -- which balances it. Taken from each
        side apart, Rod's by the hinge's reaction, the two are equal and
        opposite; and the parts' m a sum to the group's. Tip, which the run
        reports no motion of, is taken to FEM, held where it meets Rod."""
        import JointObject
        import MotionLoads

        rod = self.pendulum()
        tip = self.box("Tip", (50, 10, 10), (110, 0, 0))
        group = self.joints.newObject("Assembly::RigidGroupPython", "RigidGroupJoint")
        JointObject.RigidGroupJoint(group, [rod, tip])
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        self.assertEqual(
            [p.Name for p in MotionLoads.movingParts(self.assembly)], ["Rod", "Tip"]
        )
        accels = self.channel("Rod", "Acceleration")
        weight = 150 * 10 * 10 * 2.7e-6 * G
        for frame in (0, 8, 16, 20):
            bodies = self.balanced(rod, [frame], weight, 150.0) + self.balanced(
                tip, [frame], weight, 150.0
            )
            sides = []
            masses = App.Vector()
            for body, part, face in zip(bodies, (rod, tip), ("Face2", "Face1")):
                (load,) = [l for l in body["Loads"] if l["Kind"] == "Group"]
                self.assertTrue(load["Source"] == group)
                self.assertEqual(load["Faces"], [face])
                # The faces they share, whole
                self.assertEqual(load["Radius"], 0.0)
                self.assembly.updateForFrame(frame)
                turn = part.Placement.Rotation
                point = part.Placement.multVec(load["Point"])
                sides.append((turn.multVec(load["Force"]), turn.multVec(load["Torque"]), point))
                masses += turn.multVec(body["Acceleration"]) * body["Mass"]
            (f1, t1, p1), (f2, t2, p2) = sides
            self.assertLess((p1 - p2).Length, 1e-6, frame)
            self.assertLess((f1 + f2).Length, 1e-6 * weight, frame)
            self.assertLess((t1 + t2).Length, 1e-6 * weight * 150.0, frame)
            total = bodies[0]["Mass"] + bodies[1]["Mass"]
            self.assertLess((masses - accels[frame] * total).Length, 1e-6 * weight, frame)
        if not MotionLoads.ccxBinary():
            return
        series = MotionLoads.solveFrames(self.assembly, tip, [8, 16], maxSize=4.0)
        self.assertTrue(series.Held == group)
        for k in range(2):
            run, fem = series.HeldForce[k], series.FemHeldForce[k]
            self.assertLess((fem - run).Length, 5e-3 * run.Length + 1e-2 * weight * 1e-3)
        self.assertEqual(len(MotionLoads.stressChannels(self.assembly)), 2)

    def test_loads_on_rigid_group_patch(self):
        """An arm 10 x 10 x 60 hanging under the rod's far end, the two in a
        rigid group: they share the arm's top face whole and a 10 x 10 patch
        of the rod's underside -- the rod's group load on that patch, a round
        of 5 sqrt(2) about its centre, not along the underside. Solved, the
        rod held at its hinge, the arm's load on the patch, the hold is the
        run's."""
        import JointObject
        import MotionLoads

        rod = self.pendulum()
        arm = self.box("Arm", (10, 10, 60), (100, 0, -60))
        group = self.joints.newObject("Assembly::RigidGroupPython", "RigidGroupJoint")
        JointObject.RigidGroupJoint(group, [rod, arm])
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        weight = 160 * 10 * 10 * 2.7e-6 * G
        (onRod,) = [
            l
            for l in self.balanced(rod, [10], weight, 150.0)[0]["Loads"]
            if l["Kind"] == "Group"
        ]
        self.assertEqual(onRod["Faces"], ["Face5"])
        self.assertLess((onRod["Point"] - App.Vector(95, 5, 0)).Length, 1e-6)
        self.assertAlmostEqual(onRod["Radius"], 50**0.5, delta=1e-6)
        (onArm,) = [
            l
            for l in self.balanced(arm, [10], weight, 150.0)[0]["Loads"]
            if l["Kind"] == "Group"
        ]
        self.assertEqual(onArm["Faces"], ["Face6"])
        self.assertEqual(onArm["Radius"], 0.0)
        if not MotionLoads.ccxBinary():
            return
        series = MotionLoads.solveFrames(self.assembly, rod, [10], maxSize=4.0)
        analysis = [o for o in series.InList if o.isDerivedFrom("Fem::FemAnalysis")][0]
        couplings = [o for o in analysis.Group if o.TypeId == "Fem::ConstraintRigidBody"]
        (patch,) = [c for c in couplings if c.Coupling == "Distributing"]
        self.assertEqual(patch.References[0][1], ("Face5",))
        self.assertAlmostEqual(patch.CouplingRadius.Value, 50**0.5, delta=1e-6)
        run, fem = series.HeldForce[0], series.FemHeldForce[0]
        self.assertLess((fem - run).Length, 5e-3 * run.Length + 1e-2 * weight * 1e-3)

    def test_loads_on_rigid_group_gap(self):
        """test_loads_on_rigid_group_patch's arm hung 1 mm under the rod,
        the two meeting nowhere: each held where it would meet the other
        across the gap -- the rod on the 10 x 10 patch of its underside
        over the arm, the arm on its top face -- not on the face nearest
        first found, the rod's end and the arm's side as near along their
        edges. Solved, the hold is the run's."""
        import JointObject
        import MotionLoads

        rod = self.pendulum()
        arm = self.box("Arm", (10, 10, 60), (100, 0, -61))
        group = self.joints.newObject("Assembly::RigidGroupPython", "RigidGroupJoint")
        JointObject.RigidGroupJoint(group, [rod, arm])
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        _, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        weight = 160 * 10 * 10 * 2.7e-6 * G
        (onRod,) = [
            l
            for l in self.balanced(rod, [10], weight, 150.0)[0]["Loads"]
            if l["Kind"] == "Group"
        ]
        self.assertEqual(onRod["Faces"], ["Face5"])
        self.assertLess((onRod["Point"] - App.Vector(95, 5, 0)).Length, 1e-6)
        self.assertAlmostEqual(onRod["Radius"], 50**0.5, delta=1e-6)
        self.assertAlmostEqual(onRod["Gap"], 1.0, delta=1e-9)
        (onArm,) = [
            l
            for l in self.balanced(arm, [10], weight, 150.0)[0]["Loads"]
            if l["Kind"] == "Group"
        ]
        self.assertEqual(onArm["Faces"], ["Face6"])
        self.assertEqual(onArm["Radius"], 0.0)
        self.assertAlmostEqual(MotionLoads.groupGap(self.assembly, arm), 1.0, delta=1e-9)
        if not MotionLoads.ccxBinary():
            return
        series = MotionLoads.solveFrames(self.assembly, rod, [10], maxSize=4.0)
        run, fem = series.HeldForce[0], series.FemHeldForce[0]
        self.assertLess((fem - run).Length, 5e-3 * run.Length + 1e-2 * weight * 1e-3)

    def test_loads_on_driven(self):
        """The rod turned by a motion about its hinge, time^2, a kinematic
        run with its reactions: its free body's hinge load holds the motor's
        torque with the reaction -- the motor is a link of its own, whose
        torque the reaction does not hold -- and balances."""
        self.assertIsNotNone(
            self.driven("Position", "time*time", analysis="Kinematic", reactions=True)
        )
        rod = self.doc.getObject("Rod")
        motor = self.channel("Revolute", "MotorTorque")
        weight = 100 * 10 * 10 * 2.7e-6 * G
        self.assertGreater(max(abs(t) for t in motor), 0.1 * weight * 100.0)
        frames = range(0, self.assembly.numberOfFrames(), 10)
        for body in self.balanced(rod, frames, weight, 100.0):
            (hinge,) = body["Loads"]
            self.assertEqual(hinge["Kind"], "Joint")

    def test_body_motion_kinematic(self):
        """A kinematic run with its reactions reports the rod's motion too:
        turned by time^2 about the hinge, 2 t rad/s at 2 rad/s^2, its centre
        of mass 50 mm out."""
        self.assertIsNotNone(
            self.driven("Position", "time*time", analysis="Kinematic", reactions=True)
        )
        results = self.assembly.getSimulationResults()
        accels = self.channel("Rod", "Acceleration")
        spins = self.channel("Rod", "AngularVelocity")
        rates = self.channel("Rod", "AngularAcceleration")
        for time, accel, spin, rate in zip(results["Times"], accels, spins, rates):
            omega = 2 * time
            self.assertAlmostEqual(spin.Length, omega, delta=1e-5, msg=time)
            self.assertAlmostEqual(abs(spin.y), spin.Length, delta=1e-9, msg=time)
            self.assertAlmostEqual(rate.Length, 2.0, delta=1e-4, msg=time)
            expected = 50 * math.hypot(2.0, omega * omega)
            self.assertAlmostEqual(accel.Length, expected, delta=1e-3 * expected, msg=time)

    def test_results_export(self):
        """The run's values to a CSV file and to a spreadsheet
        (docs/AssemblyDynamics.md sec 8.4): the time, then each channel's
        components in the panel's units, a row per frame, to full
        precision; nothing from a run that reported none."""
        import csv
        import os
        import tempfile

        import CommandCreateSimulation as Sim

        self.pendulum()
        _, result = self.simulate(0.05, 1e-2)
        self.assertEqual(result, 0)
        results = self.assembly.getSimulationResults()
        forces = self.channel("Revolute", "Force")
        torques = self.channel("Revolute", "Torque")
        path = os.path.join(tempfile.mkdtemp(), "results.csv")
        self.assertTrue(Sim.exportResultsCsv(self.assembly, path))
        with open(path, newline="", encoding="utf-8") as f:
            rows = list(csv.reader(f))
        os.remove(path)
        os.rmdir(os.path.dirname(path))
        header = rows[0]
        # The hinge's reaction, then the rod's motion
        self.assertEqual(len(header), 16)
        self.assertEqual(header[0], "Time (s)")
        self.assertEqual(header[1], "Revolute: Reaction force X (N)")
        self.assertEqual(header[6], "Revolute: Reaction torque Z (N m)")
        self.assertEqual(header[7], "Rod: Acceleration X (m/s^2)")
        self.assertEqual(header[15], "Rod: Angular acceleration Z (deg/s^2)")
        self.assertEqual(len(rows), 1 + len(results["Times"]))
        for k, row in enumerate(rows[1:]):
            self.assertEqual(float(row[0]), results["Times"][k])
            # mN to N, mN mm to N m, exactly as written
            self.assertEqual(float(row[3]), forces[k].z * 1e-3)
            self.assertEqual(float(row[5]), torques[k].y * 1e-6)

        sheet = Sim.exportResultsSheet(self.assembly)
        self.assertEqual(sheet.TypeId, "Spreadsheet::Sheet")
        self.assertEqual(sheet.get("A1"), "Time (s)")
        self.assertEqual(sheet.get("G1"), "Revolute: Reaction torque Z (N m)")
        last = len(results["Times"]) + 1
        self.assertAlmostEqual(sheet.get("A%d" % last), results["Times"][-1], places=12)
        self.assertAlmostEqual(sheet.get("D%d" % last), forces[-1].z * 1e-3, places=12)

        # A kinematic run reports nothing to export
        self.tearDown()
        self.setUp()
        self.assertIsNotNone(self.driven("Position", "time", analysis="Kinematic"))
        self.assertIsNone(Sim.resultsTable(self.assembly))
        self.assertFalse(Sim.exportResultsCsv(self.assembly, path))
        self.assertFalse(os.path.exists(path))

    def test_trace(self):
        """The rod's tip traced over the swing (docs/AssemblyDynamics.md sec
        8.5): a curve through its position at every frame, on the circle of
        100 mm about the hinge; traced riding on the rod itself, one point
        in the rod's coordinates; another study's run leaves it as it was,
        and its own study's run again makes it again."""
        import Part

        import SimulationTrace

        rod = self.pendulum()
        sim, result = self.simulate(0.2, 1e-2)
        self.assertEqual(result, 0)
        placements = self.assembly.getSimulationPlacements(rod)
        self.assertEqual(len(placements), self.assembly.numberOfFrames())
        self.assertEqual(self.assembly.getSimulationResults()["Simulation"], sim)
        tip = App.Vector(100, 5, 5)
        trace = SimulationTrace.newTrace(sim, rod, tip)
        self.assertIn(trace, sim.Group)
        # The study's, not a component
        import UtilsAssembly

        self.assertNotIn(trace, UtilsAssembly.getMovablePartsWithin(self.assembly))
        components = [c["Object"] for c in self.assembly.getMassProperties()["Components"]]
        self.assertEqual(sorted(o.Name for o in components), ["Base", "Rod"])
        self.assertEqual(len(trace.Shape.Edges), 1)
        edge = trace.Shape.Edges[0]
        hinge = App.Vector(10, 5, 5)
        for k, placement in enumerate(placements):
            point = placement.multVec(tip)
            self.assertLess(edge.distToShape(Part.Vertex(point))[0], 1e-7, k)
        for point in edge.discretize(50):
            self.assertAlmostEqual((point - hinge).Length, 100, delta=0.01)
        # It swung through more than a quarter turn
        self.assertLess(edge.BoundBox.ZMin, 5 - 99)

        riding = SimulationTrace.newTrace(sim, rod, tip, relative=rod)
        self.assertEqual(riding.Shape.ShapeType, "Vertex")
        self.assertLess((riding.Shape.Vertexes[0].Point - rod.Placement.multVec(tip)).Length, 1e-9)

        # Another study's run: the trace keeps its curve
        length = edge.Length
        other, result = self.simulate(0.05, 1e-2)
        self.assertEqual(result, 0)
        SimulationTrace.updateTraces(sim)
        trace.touch()
        trace.recompute()
        self.assertAlmostEqual(trace.Shape.Edges[0].Length, length, delta=1e-9)
        # Its own study again, shorter: the curve follows
        sim.TimeEnd = 0.1
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        SimulationTrace.updateTraces(sim)
        self.assertLess(trace.Shape.Edges[0].Length, length * 0.5)

    def test_trace_closed(self):
        """A full turn of the rod, driven: the tip's trace is a closed,
        periodic curve -- the start's frame (which a kinematic run may give
        twice) counted once."""
        import SimulationTrace

        self.assertIsNotNone(
            self.driven("Position", "6.283185307179586*time", analysis="Kinematic")
        )
        rod = self.doc.getObject("Rod")
        sim = self.assembly.getSimulationResults()["Simulation"]
        sim.TimeEnd = 1.0
        sim.TimeStepOutput = 0.05
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        trace = SimulationTrace.newTrace(sim, rod, App.Vector(100, 5, 5))
        edge = trace.Shape.Edges[0]
        self.assertTrue(edge.Curve.isPeriodic())
        self.assertTrue(edge.isClosed())
        self.assertAlmostEqual(edge.Length, 2 * math.pi * 100, delta=2 * math.pi * 100 * 1e-3)

    def waitForRun(self, until, limit=60.0):
        """Polls the run on the worker thread until 'until(progress)'"""
        import time

        start = time.monotonic()
        while True:
            progress = self.assembly.getSimulationProgress()
            if until(progress):
                return progress
            self.assertLess(time.monotonic() - start, limit, progress)
            time.sleep(0.01)

    def test_worker_run(self):
        """A run on a worker thread (docs/AssemblyDynamics.md sec 4) makes
        the frames a run here makes; its warnings reach a Python console
        observer from there; the assembly solves meanwhile."""
        rod = self.pendulum()
        base = self.doc.getObject("Base")
        # A second hinge on the axis: the run warns of its redundant rows
        self.joint(
            "Revolute",
            (base, ["Face2"]),
            plc(10, 0, 5, App.Vector(1, 0, 0), -90),
            (rod, ["Face1"]),
            plc(0, 0, 5, App.Vector(1, 0, 0), -90),
        )
        self.doc.recompute()
        sim, result = self.simulate(0.1, 1e-2)
        self.assertEqual(result, 0)
        here = [self.assembly.getSimulationPlacements(rod)[k] for k in range(11)]
        self.assembly.updateForFrame(0)

        class Warnings:
            def __init__(self):
                self.messages = []

            def __call__(self, notifier, msg, level):
                if level == "Warning":
                    self.messages.append(msg)

        warnings = Warnings()
        App.Console.AttachObserver(warnings)
        try:
            self.assertEqual(self.assembly.startSimulation(sim), 0)
            self.assertEqual(self.assembly.solve(), 0)
            progress = self.waitForRun(lambda p: p["Done"])
            self.assertFalse(progress["Running"])
            self.assertEqual(progress["Frames"], 11)
            self.assertAlmostEqual(progress["Time"], 0.1, places=12)
            self.assertEqual(self.assembly.finishSimulation(), 0)
        finally:
            App.Console.DetachObserver(warnings)
        self.assertTrue(any("redundant" in m for m in warnings.messages), warnings.messages)
        self.assertEqual(self.assembly.getSimulationResults()["Simulation"], sim)
        there = self.assembly.getSimulationPlacements(rod)
        self.assertEqual(len(there), 11)
        for k, (a, b) in enumerate(zip(here, there)):
            self.assertTrue(a.isSame(b, 1e-12), k)
        self.assertFalse(self.assembly.getSimulationProgress()["Done"])
        self.assertEqual(self.assembly.finishSimulation(), -1)

    def test_worker_cancel(self):
        """A long run on the worker thread, cancelled: it stops at its next
        frame, keeps none, and a run after it runs whole."""
        import time

        self.pendulum()
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        sim.TimeEnd = 1000
        sim.TimeStepOutput = 0.01
        self.doc.recompute()
        self.assertEqual(self.assembly.startSimulation(sim), 0)
        self.waitForRun(lambda p: p["Frames"] >= 3)
        start = time.monotonic()
        self.assembly.cancelSimulation()
        self.waitForRun(lambda p: p["Done"], limit=5.0)
        self.assertLess(time.monotonic() - start, 5.0)
        self.assertEqual(self.assembly.finishSimulation(), -7)
        self.assertEqual(self.assembly.numberOfFrames(), 0)
        self.assertIsNone(self.assembly.getSimulationResults()["Simulation"])
        sim.TimeEnd = 0.05
        self.doc.recompute()
        self.assertEqual(self.assembly.startSimulation(sim), 0)
        # A run here cancels the one going first
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assertEqual(self.assembly.numberOfFrames(), 6)

    def test_mass_properties_after_playback(self):
        """A played frame moves the rod without a recompute; the mass
        properties, which a dynamic run's inertias come from, are those of
        the rod where it is (the assembly's shape cache let go of)."""
        rod = self.pendulum()
        _, result = self.simulate(0.05, 1e-2)
        self.assertEqual(result, 0)
        for frame in (3, 5):
            self.assembly.updateForFrame(frame)
            props = [
                c for c in self.assembly.getMassProperties()["Components"] if c["Object"] == rod
            ][0]
            self.assertLess((props["CenterOfMass"] - rod.Shape.CenterOfMass).Length, 1e-9, frame)
        self.assertGreater((rod.Shape.CenterOfMass - App.Vector(60, 5, 5)).Length, 1)

    def test_stale_flag(self):
        """The stale flag (docs/AssemblyDynamics.md sec 8.4): a run's study is
        stale once the study, a joint or a part's shape changes, and fresh
        again once run; playing it, or moving a part, does not count; the
        check leaves the last solve's status as it was; another study's run
        leaves it stale, its results kept in the document; one that keeps
        nothing, its frames gone, is not stale."""
        rod = self.pendulum()
        sim, result = self.simulate(0.05, 1e-2)
        self.assertEqual(result, 0)

        def stale():
            self.doc.recompute()
            self.assertEqual(sim.ResultsStale, self.assembly.isSimulationStale(sim))
            return sim.ResultsStale

        self.assertFalse(stale())
        self.assembly.updateForFrame(3)
        self.assertFalse(stale())
        moved = rod.Placement
        moved.Base += App.Vector(0, 0, 0.5)
        rod.Placement = moved
        self.assertFalse(stale())

        sim.TimeEnd = 0.04
        self.assertTrue(stale())
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assertFalse(stale())

        joint = [o for o in self.joints.Group if o.Name.startswith("Revolute")][0]
        status = self.assembly.getSolveStatus()
        joint.Placement2 = plc(0, 5, 4, App.Vector(1, 0, 0), -90)
        self.assertTrue(stale())
        self.assertEqual(self.assembly.getSolveStatus(), status)
        joint.Placement2 = plc(0, 5, 5, App.Vector(1, 0, 0), -90)
        self.assertFalse(stale())

        # The rod's mass: a dynamic study's, not a kinematic one's
        rod.Length = 90
        self.assertTrue(stale())
        rod.Length = 100
        self.assertFalse(stale())

        # Another study's run: this one keeps its results in the document,
        # stale as they were
        sim.TimeEnd = 0.03
        self.assertTrue(stale())
        other, result = self.simulate(0.02, 1e-2, AnalysisType="Kinematic")
        self.assertEqual(result, 0)
        self.assertTrue(sim.ResultsStale)
        self.assertTrue(self.assembly.isSimulationStale(sim))
        rod.Length = 90
        self.doc.recompute()
        self.assertFalse(other.ResultsStale)

        # Keeping nothing, its frames gone with another's run: not stale
        sim.KeepResults = "None"
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        sim.TimeEnd = 0.02
        self.assertTrue(stale())
        _, result = self.simulate(0.02, 1e-2, AnalysisType="Kinematic")
        self.assertEqual(result, 0)
        self.assertFalse(sim.ResultsStale)
        self.assertFalse(self.assembly.isSimulationStale(sim))

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

    def dropped(
        self, restitution=0.0, analysis="Dynamic", end=0.2, step=1e-2, stop=None, effort=None
    ):
        """A 10 mm cube on a vertical slider, 4 mm over a grounded one, its
        travel's least 0: the stop the grounded one's top. Its heights.
        'stop': a JointStop's properties, the study's only element; 'effort':
        an Effort motion's formula on the slider (N)."""
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
        props = {}
        if stop is not None:
            physics = self.assembly.getGroup("Physics", True)
            element = physics.newObject("Assembly::JointStop", "JointStop")
            element.Joint = slider
            for name, value in stop.items():
                setattr(element, name, value)
            props["Elements"] = [element]
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = analysis
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        sim.Restitution = restitution
        for name, value in props.items():
            setattr(sim, name, value)
        if effort is not None:
            motion = sim.newObject("Assembly::Motion", "Motion")
            motion.Joint = slider
            motion.MotionType = "Linear"
            motion.Control = "Effort"
            motion.Formula = effort
        self.doc.recompute()
        result = self.assembly.generateSimulation(sim)
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

    # ------------------------------------------------------------ joint stops

    # A penalty stop for the dropped block: 1000 N/m, about a centimetre's
    # period on the 2.7 g block
    PENALTY = 1e3  # mN/mm, N/m

    def test_stop_penalty_rests(self):
        """On a penalty stop the block comes to rest m g / k into it, the
        stop carrying its weight in the slider's reaction; a static study
        settles it there too."""
        weight = self.MASS * G  # mN
        depth = weight / self.PENALTY
        for analysis, end in (("Dynamic", 0.3), ("Static", 0.0)):
            self.tearDown()
            self.setUp()
            stop = {"Mode": "Penalty", "Stiffness": self.PENALTY, "Damping": 1.0}
            heights = self.dropped(analysis=analysis, end=end, stop=stop)
            self.assertAlmostEqual(heights[-1][1], 10.0 - depth, delta=1e-6, msg=analysis)
            force = self.channel("Slider", "Force")[-1]
            self.assertLess(
                (force - App.Vector(0, 0, -weight)).Length, weight * 1e-5, msg=analysis
            )

    def test_stop_penalty_bounce(self):
        """Undamped, the penalty stop hands the block back its energy: dropped
        4 mm, it goes in as deep as m g (h + d) = k d^2 / 2 says and comes
        back to where it fell from."""
        weight = self.MASS * G * 1e-3  # N
        k = self.PENALTY
        h = 4e-3
        deepest = (weight + math.sqrt(weight * weight + 2 * k * weight * h)) / k * 1e3  # mm
        stop = {"Mode": "Penalty", "Stiffness": k, "Damping": 0.0}
        heights = self.dropped(end=0.08, step=1e-4, stop=stop)
        fall = math.sqrt(2 * 4 / G)
        lowest = min(z for _, z in heights)
        self.assertAlmostEqual(lowest, 10.0 - deepest, delta=deepest * 2e-3)
        after = [z for time, z in heights if fall + 0.01 < time < fall + 0.05]
        self.assertAlmostEqual(max(after), 14.0, delta=4.0 * 2e-3)

    def test_stop_own_restitution(self):
        """A rigid stop's own restitution, 1, over the study's, 0: the block
        bounces back to where it fell from."""
        heights = self.dropped(
            0.0, end=0.06, step=1e-4, stop={"Mode": "Rigid", "Restitution": 1.0}
        )
        fall = math.sqrt(2 * 4 / G)
        after = [z for time, z in heights if fall + 1e-3 < time < fall + 0.04]
        self.assertAlmostEqual(max(after) - 10.0, 4.0, delta=4.0 * 1e-2)

    def test_stop_off(self):
        """A stop Off lets the block fall through its limit, as if free."""
        heights = self.dropped(end=0.1, step=1e-2, stop={"Mode": "Off"})
        for time, z in heights:
            self.assertAlmostEqual(z, 14.0 - G * time * time / 2, delta=1e-3, msg=time)

    def test_stop_under_effort(self):
        """A study with a motion keeps the limits of the joints it does not
        hold: an Effort pushing the block down 1 N onto its penalty stop, a
        static study settles it (m g + 1 N) / k in, which the Effort's own
        joint's limit, dropped before, now stops."""
        weight = self.MASS * G  # mN
        stop = {"Mode": "Penalty", "Stiffness": self.PENALTY, "Damping": 1.0}
        ((_, z),) = self.dropped(analysis="Static", end=0.0, stop=stop, effort="-1")
        self.assertAlmostEqual(z, 10.0 - (weight + 1000.0) / self.PENALTY, delta=1e-6)

    def test_stop_penalty_rotation(self):
        """The pendulum's hinge limited to 30 deg below the horizontal by a
        penalty stop of 10 N m per radian: a static study settles the rod where
        the stop's torque k (a - 30 deg) holds its weight's m g L cos a."""
        rod = self.pendulum()
        hinge = [o for o in self.joints.Group if o.Name.startswith("Revolute")][0]
        hinge.EnableAngleMin = True
        hinge.EnableAngleMax = True
        hinge.AngleMin = -30.0
        hinge.AngleMax = 30.0
        physics = self.assembly.getGroup("Physics", True)
        stop = physics.newObject("Assembly::JointStop", "JointStop")
        stop.Joint = hinge
        stop.Mode = "Penalty"
        stop.TorsionalStiffness = "10 N*m"
        self.doc.recompute()
        _, result = self.simulate(0.0, 1e-2, AnalysisType="Static", Elements=[stop])
        self.assertEqual(result, 0)
        frames = self.frames(rod, 1e-2)
        angle = math.atan2(5 - frames[-1][1].z, frames[-1][1].x - 10)
        # m g L cos a = k (a - pi / 6), by Newton's iterations; SI
        torque = 100 * 10 * 10 * 2.7e-6 * G * 1e-3 * 0.05
        a = math.pi / 6
        for _ in range(20):
            a -= (10.0 * (a - math.pi / 6) - torque * math.cos(a)) / (
                10.0 + torque * math.sin(a)
            )
        self.assertAlmostEqual(math.degrees(angle), math.degrees(a), delta=1e-4)
        self.assertGreater(math.degrees(angle), 30.01)

    # ------------------------------------------------------------ torsion

    def torsion(self, stiffness, free=0.0, damping=0.0):
        """The pendulum with a torsion spring on its hinge (mN mm / rad, deg)"""
        rod = self.pendulum()
        hinge = [o for o in self.joints.Group if o.Name.startswith("Revolute")][0]
        physics = self.assembly.getGroup("Physics", True)
        spring = physics.newObject("Assembly::TorsionSpring", "TorsionSpring")
        spring.Joint = hinge
        spring.Stiffness = stiffness
        spring.FreeAngle = free
        spring.Damping = damping
        return rod, spring

    def test_torsion_period(self):
        """Without gravity, a torsion spring free 0.1 rad from where the rod
        starts swings it between 0 and 0.2 rad, by half a period of
        pi sqrt(I / k) about the hinge."""
        k = 1e4  # mN mm / rad
        rod, spring = self.torsion(k, math.degrees(0.1))
        _, result = self.simulate(1.0, 1e-4, GravityEnabled=False, Elements=[spring])
        self.assertEqual(result, 0)
        turns = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            turns.append((i * 1e-4, rod.Placement.Rotation.Angle))
        inertia = 100 * 10 * 10 * 2.7e-6 * ((100**2 + 10**2) / 12 + 50**2)  # kg mm^2
        half = math.pi * math.sqrt(inertia / k)
        crossed = self.crossings(turns, 0.1)
        self.assertGreaterEqual(len(crossed), 3)
        for a, b in zip(crossed, crossed[1:]):
            self.assertAlmostEqual(b - a, half, delta=half * 1e-3)
        self.assertAlmostEqual(max(a for _, a in turns), 0.2, delta=1e-4)
        # Its torque, -k (turn - 0.1), as the turn goes the spring's way
        torques = self.channel("TorsionSpring", "SpringTorque")
        angles = self.channel("TorsionSpring", "SpringAngle")
        start = angles[0]
        for torque, angle in zip(torques, angles):
            self.assertAlmostEqual(torque, -k * (angle - start - 0.1), delta=k * 1e-9)
        self.assertAlmostEqual(abs(max(angles, key=abs) - start), 0.2, delta=1e-4)

    def test_torsion_static(self):
        """A torsion spring free where the rod starts, level, holds it where
        k turn = m g d cos(turn) below level."""
        k = 2000.0
        rod, spring = self.torsion(k)
        _, result = self.simulate(0.0, 1e-2, AnalysisType="Static", Elements=[spring])
        self.assertEqual(result, 0)
        weight = 100 * 10 * 10 * 2.7e-6 * G * 50
        lo, hi = 0.0, math.pi / 2
        for _ in range(60):
            mid = (lo + hi) / 2
            if k * mid < weight * math.cos(mid):
                lo = mid
            else:
                hi = mid
        ((_, centre, _),) = self.frames(rod, 1e-2)
        below = math.atan2(5 - centre.z, centre.x - 10)
        self.assertAlmostEqual(below, lo, delta=1e-6)
        torque = self.channel("TorsionSpring", "SpringTorque")[0]
        self.assertAlmostEqual(abs(torque), weight * math.cos(lo), delta=weight * 1e-6)

    def test_torsion_on_wrong_joint(self):
        """A torsion spring on no revolute or cylindrical joint does nothing."""
        rod, spring = self.torsion(1e4, 30.0)
        spring.Joint = None
        _, result = self.simulate(0.05, 1e-2, GravityEnabled=False, Elements=[spring])
        self.assertEqual(result, 0)
        for time, centre, _ in self.frames(rod, 1e-2):
            self.assertLess((centre - App.Vector(60, 5, 5)).Length, 1e-9, time)

    # ------------------------------------------------------------ loads

    def slider(self, name="Block", x=0.0):
        """A 10 mm cube on a vertical slider over the grounded base, 2 mm
        above it, without gravity's run: what a load alone moves"""
        base = self.doc.getObject("Base")
        if base is None:
            base = self.box("Base", (40, 10, 10), (0, 0, 0))
            ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
            JointObject.GroundedJoint(ground, base)
        block = self.box(name, (10, 10, 10), (x, 0, 12))
        self.doc.recompute()
        self.joint(
            "Slider", (base, ["Face6"]), plc(x + 5, 5, 10), (block, ["Face5"]), plc(5, 5, 0)
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        return block

    def load(self, end, **props):
        import CommandCreateLoad

        load = CommandCreateLoad.makeLoad(self.assembly, end, **props)
        self.assertNotIn("Invalid", load.State)
        return load

    def heights(self, block, step):
        out = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            out.append((i * step, block.Placement.Base.z))
        return out

    def test_force_constant(self):
        """A constant force up the slider: z = z0 + F t^2 / (2 m), at a free
        point and on the block's top face alike."""
        accel = 1e4  # mm/s^2
        force = self.MASS * accel * 1e-3  # N
        for end in ("point", "face"):
            if end == "face":
                self.tearDown()
                self.setUp()
            block = self.slider()
            load = self.load(
                (block, App.Placement(App.Vector(5, 5, 10), App.Rotation()))
                if end == "point"
                else (block, "Face6"),
                magnitude="%r N" % force,
            )
            if end == "face":
                self.assertIsNotNone(load.Binding)
                self.assertTrue(load.getEndPlacement().Base.isEqual(App.Vector(5, 5, 22), 1e-9))
            _, result = self.simulate(0.1, 1e-2, GravityEnabled=False, Elements=[load])
            self.assertEqual(result, 0)
            for time, z in self.heights(block, 1e-2):
                self.assertAlmostEqual(z, 12 + 0.5 * accel * time * time, delta=1e-6, msg=end)

    def test_force_profile(self):
        """F0 sin(w t) from rest: z = z0 + (F0 / m) (t / w - sin(w t) / w^2),
        to second order in the step -- the load taken at each stage's time,
        not held over a step, which would be first order."""
        accel = 1e4
        w = 2 * math.pi * 10
        errors = []
        for step in (1e-3, 5e-4):
            if errors:
                self.tearDown()
                self.setUp()
            block = self.slider()
            load = self.load(
                (block, App.Placement(App.Vector(5, 5, 10), App.Rotation())),
                magnitude="%r N" % (self.MASS * accel * 1e-3),
                direction=App.Vector(0, 0, 1),
            )
            load.Profile = "sin(2*pi*10*time)"
            _, result = self.simulate(0.2, step, GravityEnabled=False, Elements=[load])
            self.assertEqual(result, 0)
            errors.append(
                max(
                    abs(z - 12 - accel * (time / w - math.sin(w * time) / (w * w)))
                    for time, z in self.heights(block, step)
                )
            )
        # Halving the step quarters the error (measured 4.006); a hundredth
        # of the swing about the drift, accel / w^2 (2.5 mm), at 1 ms
        self.assertGreater(errors[0] / errors[1], 3.5, errors)
        self.assertLess(errors[0], 1e-2 * accel / (w * w), errors)

    def rodAngles(self, rod, step):
        return [
            (time, math.atan2(5 - centre.z, centre.x - 10))
            for time, centre, _ in self.frames(rod, step)
        ]

    def test_torque(self):
        """A torque about the hinge's axis: theta = T t^2 / (2 I)."""
        rod = self.pendulum()
        inertia = 100 * 10 * 10 * 2.7e-6 * ((100**2 + 10**2) / 12 + 50**2) * 1e-6  # kg m^2
        torque = 200 * inertia  # N m: a turn of 1 rad at 0.1 s
        load = self.load(
            (rod, App.Placement(App.Vector(0, 5, 5), App.Rotation())),
            kind="Torque",
            magnitude="%r N*m" % torque,
            direction=App.Vector(0, 1, 0),
        )
        _, result = self.simulate(0.1, 1e-3, GravityEnabled=False, Elements=[load])
        self.assertEqual(result, 0)
        for time, angle in self.rodAngles(rod, 1e-3):
            self.assertAlmostEqual(angle, 100 * time * time, delta=1e-4, msg=time)

    def test_force_follows_the_part(self):
        """A force at the rod's tip across it: in the rod's axes it keeps its
        arm, theta = F L t^2 / (2 I); in the assembly's its arm shrinks as
        the rod turns, and the rod lags."""
        inertia = 100 * 10 * 10 * 2.7e-6 * ((100**2 + 10**2) / 12 + 50**2) * 1e-6
        force = 200 * inertia / 0.1  # N, at 0.1 m
        turns = {}
        for axes in ("Frame", "Assembly"):
            if axes == "Assembly":
                self.tearDown()
                self.setUp()
            rod = self.pendulum()
            load = self.load(
                (rod, App.Placement(App.Vector(100, 5, 5), App.Rotation())),
                magnitude="%r N" % force,
                direction=App.Vector(0, 0, -1),
            )
            load.DirectionAxes = axes
            _, result = self.simulate(0.1, 1e-3, GravityEnabled=False, Elements=[load])
            self.assertEqual(result, 0)
            turns[axes] = self.rodAngles(rod, 1e-3)
        for time, angle in turns["Frame"]:
            self.assertAlmostEqual(angle, 100 * time * time, delta=1e-4, msg=time)
        # Fixed in the assembly: theta'' = 200 cos(theta), integrated finely
        theta, rate, h = 0.0, 0.0, 1e-6
        expected = {}
        for i in range(100001):
            if i % 1000 == 0:
                expected[round(i * h, 9)] = theta

            def acc(a):
                return 200 * math.cos(a)

            k1v, k1x = acc(theta), rate
            k2v, k2x = acc(theta + 0.5 * h * k1x), rate + 0.5 * h * k1v
            k3v, k3x = acc(theta + 0.5 * h * k2x), rate + 0.5 * h * k2v
            k4v, k4x = acc(theta + h * k3x), rate + h * k3v
            theta += h * (k1x + 2 * k2x + 2 * k3x + k4x) / 6
            rate += h * (k1v + 2 * k2v + 2 * k3v + k4v) / 6
        for time, angle in turns["Assembly"]:
            self.assertAlmostEqual(angle, expected[round(time, 9)], delta=1e-4, msg=time)
        self.assertLess(turns["Assembly"][-1][1], turns["Frame"][-1][1] - 0.02)

    def test_force_reaction(self):
        """A force on one block with its reaction on another, each on its
        own slider: equal and opposite, z - z0 = +-F t^2 / (2 m)."""
        accel = 1e4
        a = self.slider("BlockA")
        b = self.slider("BlockB", x=20)
        load = self.load(
            (b, App.Placement(App.Vector(5, 5, 10), App.Rotation())),
            magnitude="%r N" % (self.MASS * accel * 1e-3),
            reaction=a,
        )
        _, result = self.simulate(0.1, 1e-2, GravityEnabled=False, Elements=[load])
        self.assertEqual(result, 0)
        for (time, za), (_, zb) in zip(self.heights(a, 1e-2), self.heights(b, 1e-2)):
            self.assertAlmostEqual(zb, 12 + 0.5 * accel * time * time, delta=1e-6, msg=time)
            self.assertAlmostEqual(za, 12 - 0.5 * accel * time * time, delta=1e-6, msg=time)

    def test_force_static(self):
        """A block on a spring, lifted by a force: it sags by (m g - F) / k."""
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
        lift = 0.25 * self.MASS * G * 1e-3  # N
        load = self.load((block, "Face6"), magnitude="%r N" % lift)
        _, result = self.simulate(
            0.0, 1e-2, AnalysisType="Static", Elements=[spring, load]
        )
        self.assertEqual(result, 0)
        self.assembly.updateForFrame(0)
        sag = 0.75 * self.MASS * G / self.K
        self.assertAlmostEqual(block.Placement.Base.z, 10 + 4 - sag, delta=1e-5)

    def test_load_bad_profile(self):
        """A profile that does not parse fails the run, naming the load."""
        block = self.slider()
        load = self.load(
            (block, App.Placement(App.Vector(5, 5, 10), App.Rotation())), magnitude="1 N"
        )
        load.Profile = "sin(time"
        _, result = self.simulate(0.1, 1e-2, GravityEnabled=False, Elements=[load])
        self.assertNotEqual(result, 0)

    def test_load_not_listed(self):
        """A load the study does not list does nothing."""
        block = self.slider()
        self.load((block, App.Placement(App.Vector(5, 5, 10), App.Rotation())), magnitude="1 N")
        _, result = self.simulate(0.1, 1e-2, GravityEnabled=False, Elements=[])
        self.assertEqual(result, 0)
        for time, z in self.heights(block, 1e-2):
            self.assertAlmostEqual(z, 12, delta=1e-9, msg=time)

    # ------------------------------------------------------------ bushings

    def bushing(self, end, other=None, **props):
        """A bushing with only the values given (vectors, internal units)"""
        import CommandCreateBushing

        bushing = CommandCreateBushing.makeBushing(self.assembly, end, other)
        for name in CommandCreateBushing.Values:
            setattr(bushing, name, props.get(name, App.Vector()))
        self.assertNotIn("Invalid", bushing.State)
        return bushing

    @staticmethod
    def stepResponse(time, w, zeta):
        """The unit step's response of an oscillator from rest"""
        wd = w * math.sqrt(1 - zeta * zeta)
        return 1 - math.exp(-zeta * w * time) * (
            math.cos(wd * time) + zeta / math.sqrt(1 - zeta * zeta) * math.sin(wd * time)
        )

    def test_bushing_step(self):
        """The block on its slider held to the ground by a bushing, free at
        the start and preloaded up: it rises to F / k as a step's response,
        undamped and at 20% of critical."""
        k = self.K * 10  # N/m
        force = 2e-3  # N: 2 mm at k
        w = math.sqrt(k / self.MASS)
        for zeta in (0.0, 0.2):
            if zeta:
                self.tearDown()
                self.setUp()
            block = self.slider()
            c = 2 * zeta * math.sqrt(k * self.MASS)  # kg/s
            bushing = self.bushing(
                (block, App.Placement(App.Vector(5, 5, 0), App.Rotation())),
                TranslationalStiffness=App.Vector(k, k, k),
                TranslationalDamping=App.Vector(c, c, c),
                Preload=App.Vector(0, 0, force * 1e3),
            )
            _, result = self.simulate(0.15, 1e-4, GravityEnabled=False, Elements=[bushing])
            self.assertEqual(result, 0)
            settled = force / k * 1e3  # mm
            for time, z in self.heights(block, 1e-4):
                self.assertAlmostEqual(
                    z,
                    12 + settled * self.stepResponse(time, w, zeta),
                    delta=settled * 2e-3,
                    msg=(zeta, time),
                )

    def test_bushing_turn(self):
        """A bushing at the hinge, stiff about it and preloaded with a
        torque: the rod turns to T / k as a step's response, at sqrt(k / I);
        the frame's y is the hinge's axis."""
        rod = self.pendulum()
        inertia = 100 * 10 * 10 * 2.7e-6 * ((100**2 + 10**2) / 12 + 50**2) * 1e-6  # kg m^2
        k = 400 * inertia  # N m / rad: w = 20 rad/s
        torque = 0.1 * k  # N m: 0.1 rad
        bushing = self.bushing(
            (rod, App.Placement(App.Vector(0, 5, 5), App.Rotation())),
            RotationalStiffness=App.Vector(0, k * 1e6, 0),
            PreloadTorque=App.Vector(0, torque * 1e6, 0),
        )
        _, result = self.simulate(0.4, 1e-3, GravityEnabled=False, Elements=[bushing])
        self.assertEqual(result, 0)
        for time, angle in self.rodAngles(rod, 1e-3):
            self.assertAlmostEqual(
                angle, 0.1 * self.stepResponse(time, 20.0, 0.0), delta=2e-4, msg=time
            )

    def test_bushing_between_parts(self):
        """A bushing from one block to another, each on its own slider: they
        part symmetrically, the gap at sqrt(2 k / m); the channel is the
        force on the first, in the assembly's axes."""
        k = self.K * 10
        force = 2e-3
        a = self.slider("BlockA")
        b = self.slider("BlockB", x=20)
        bushing = self.bushing(
            (b, App.Placement(App.Vector(5, 5, 0), App.Rotation())),
            other=a,
            TranslationalStiffness=App.Vector(k, k, k),
            Preload=App.Vector(0, 0, force * 1e3),
        )
        _, result = self.simulate(0.1, 1e-4, GravityEnabled=False, Elements=[bushing])
        self.assertEqual(result, 0)
        w = math.sqrt(2 * k / self.MASS)
        settled = force / k * 1e3
        forces = self.channel("Bushing", "BushingForce")
        heights = zip(self.heights(a, 1e-4), self.heights(b, 1e-4))
        for ((time, za), (_, zb)), f in zip(heights, forces):
            gap = settled * self.stepResponse(time, w, 0.0)
            self.assertAlmostEqual(zb, 12 + 0.5 * gap, delta=settled * 2e-3, msg=time)
            self.assertAlmostEqual(za, 12 - 0.5 * gap, delta=settled * 2e-3, msg=time)
            # mN: the preload less the stretch
            self.assertAlmostEqual(f[2], force * 1e3 - k * (zb - za), delta=force * 1e3 * 1e-6)
            self.assertAlmostEqual(f[0], 0, delta=1e-9)

    def test_bushing_static(self):
        """The block on its slider under gravity, held by a bushing: it sags
        by m g / k."""
        k = self.K * 10
        block = self.slider()
        bushing = self.bushing(
            (block, App.Placement(App.Vector(5, 5, 0), App.Rotation())),
            TranslationalStiffness=App.Vector(k, k, k),
        )
        _, result = self.simulate(0.0, 1e-2, AnalysisType="Static", Elements=[bushing])
        self.assertEqual(result, 0)
        self.assembly.updateForFrame(0)
        self.assertAlmostEqual(block.Placement.Base.z, 12 - self.MASS * G / k, delta=1e-5)

    # ------------------------------------------------------------ initial velocities

    def started(self, end, step, conditions, **props):
        """A Dynamic run with initial velocities: conditions, each a dict of
        newInitialCondition's arguments, made in the study first"""
        import CommandCreateSimulation

        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = props.pop("AnalysisType", "Dynamic")
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        for name, value in props.items():
            setattr(sim, name, value)
        for condition in conditions:
            CommandCreateSimulation.newInitialCondition(sim, **condition)
        self.doc.recompute()
        return sim, self.assembly.generateSimulation(sim)

    def test_initial_velocity_of_a_component(self):
        """The block on its slider started up at v0, under gravity:
        z = z0 + v0 t - g t^2 / 2."""
        block = self.slider()
        v0 = 100.0  # mm/s
        _, result = self.started(
            0.02, 1e-3, [dict(component=block, linear=App.Vector(0, 0, v0))]
        )
        self.assertEqual(result, 0)
        for time, z in self.heights(block, 1e-3):
            self.assertAlmostEqual(z, 12 + v0 * time - 0.5 * G * time * time, delta=1e-6)

    def test_initial_velocity_projected(self):
        """What the slider does not allow is taken out: across it and a turn
        go, along it stays."""
        block = self.slider()
        _, result = self.started(
            0.05,
            1e-2,
            [
                dict(
                    component=block,
                    linear=App.Vector(50, 30, 100),
                    angular=App.Vector(0, 0, 90),
                )
            ],
            GravityEnabled=False,
        )
        self.assertEqual(result, 0)
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            time = i * 1e-2
            plc = block.Placement
            self.assertAlmostEqual(plc.Base.z, 12 + 100 * time, delta=1e-6, msg=time)
            self.assertAlmostEqual(plc.Base.x, 0, delta=1e-9, msg=time)
            self.assertAlmostEqual(plc.Base.y, 0, delta=1e-9, msg=time)
            self.assertAlmostEqual(plc.Rotation.Angle, 0, delta=1e-9, msg=time)

    def speedRun(self, joint, formula, end, step):
        """The same joint driven by a Speed motion instead, from the start
        pose: a run starts where the parts are, and playing the last one
        left them at its end"""
        if self.assembly.numberOfFrames():
            self.assembly.updateForFrame(0)
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        sim.GravityEnabled = False
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        motion = self.assembly.newObject("Assembly::Motion", "Motion")
        motion.Joint = joint
        motion.MotionType = "Linear" if joint.JointType == "Slider" else "Angular"
        motion.Control = "Speed"
        motion.Formula = formula
        sim.Group = [motion]
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)

    def test_initial_rate_of_a_revolute(self):
        """A rate of 90 deg/s on the hinge, nothing else acting: the rod turns
        at it, as a Speed motion of the joint turns it."""
        rod = self.pendulum()
        joint = self.joints.Group[-1]
        _, result = self.started(
            0.1, 1e-2, [dict(joint=joint, rate=90.0)], GravityEnabled=False
        )
        self.assertEqual(result, 0)
        free = self.rodAngles(rod, 1e-2)
        self.speedRun(joint, "pi/2", 0.1, 1e-2)
        driven = self.rodAngles(rod, 1e-2)
        # A Speed motion's frames are up to 2e-6 s of its rate ahead
        # (measured: 1e-6 s at the start, 2e-6 s after)
        for (time, a), (_, b) in zip(free, driven):
            self.assertAlmostEqual(a, b, delta=1e-5, msg=time)
            self.assertAlmostEqual(abs(a), math.pi / 2 * time, delta=1e-6, msg=time)

    def test_initial_rate_of_a_slider(self):
        """A rate of 100 mm/s on the slider, as a Speed motion of it."""
        block = self.slider()
        joint = self.joints.Group[-1]
        _, result = self.started(
            0.05, 1e-2, [dict(joint=joint, rate="100 mm/s")], GravityEnabled=False
        )
        self.assertEqual(result, 0)
        free = self.heights(block, 1e-2)
        self.speedRun(joint, "100", 0.05, 1e-2)
        driven = self.heights(block, 1e-2)
        for (time, a), (_, b) in zip(free, driven):
            self.assertAlmostEqual(a, b, delta=3e-4, msg=time)
            self.assertAlmostEqual(abs(a - 12), 100 * time, delta=1e-6, msg=time)

    def test_motor_speed_from_where_it_is(self):
        """A Speed motion on a slider 2 mm open moves it from there, not from
        the joint's zero: Chrono's speed motor held the joint to the integral
        of its rate from 0, and pulled the block down 2 mm at the start."""
        block = self.slider()
        self.speedRun(self.joints.Group[-1], "100", 0.05, 1e-2)
        # Its frames up to 2e-6 s of the rate ahead, 2e-4 mm; the snap was 2 mm
        for time, z in self.heights(block, 1e-2):
            self.assertAlmostEqual(abs(z - 12), 100 * time, delta=3e-4, msg=time)

    def test_initial_velocity_turning_about_the_hinge(self):
        """The rod's own velocity, given as a turn about the hinge -- its
        origin's velocity w x (origin - hinge) and w -- needs no change: it
        turns at w, as the joint's rate of w turns it."""
        rod = self.pendulum()
        w = math.pi / 2
        # The origin (10, 0, 0), the hinge's axis along y through (10, 5, 5)
        v = App.Vector(0, w, 0).cross(App.Vector(0, -5, -5))
        _, result = self.started(
            0.1,
            1e-2,
            [dict(component=rod, linear=v, angular=App.Vector(0, 90, 0))],
            GravityEnabled=False,
        )
        self.assertEqual(result, 0)
        for time, angle in self.rodAngles(rod, 1e-2):
            self.assertAlmostEqual(angle, w * time, delta=1e-6, msg=time)

    def test_initial_velocity_static_ignored(self):
        """A static study starts and ends at rest: the block sags m g / k."""
        k = self.K * 10
        block = self.slider()
        bushing = self.bushing(
            (block, App.Placement(App.Vector(5, 5, 0), App.Rotation())),
            TranslationalStiffness=App.Vector(k, k, k),
        )
        _, result = self.started(
            0.0,
            1e-2,
            [dict(component=block, linear=App.Vector(0, 0, 500))],
            AnalysisType="Static",
            Elements=[bushing],
        )
        self.assertEqual(result, 0)
        self.assembly.updateForFrame(0)
        self.assertAlmostEqual(block.Placement.Base.z, 12 - self.MASS * G / k, delta=1e-5)

    def test_redundant_hinges(self):
        """The rod on a second hinge on the same axis (docs/AssemblyDynamics.md
        sec 6.1): the run takes out the equations that repeat the first's,
        swings as on one hinge, and flags both hinges' reactions not unique;
        together they are the one hinge's."""
        rod = self.pendulum()
        _, result = self.simulate(0.1, 1e-2)
        self.assertEqual(result, 0)
        single = self.frames(rod, 1e-2)
        channels = self.assembly.getSimulationResults()["Channels"]
        alone = [c for c in channels if c["Quantity"] == "Force"]
        self.assertEqual(len(alone), 1)
        self.assertTrue(alone[0]["Unique"])
        base = self.doc.getObject("Base")
        self.joint(
            "Revolute",
            (base, ["Face2"]),
            plc(10, 0, 5, App.Vector(1, 0, 0), -90),
            (rod, ["Face1"]),
            plc(0, 0, 5, App.Vector(1, 0, 0), -90),
        )
        self.doc.recompute()
        self.assembly.updateForFrame(0)
        _, result = self.simulate(0.1, 1e-2)
        self.assertEqual(result, 0)
        both = self.frames(rod, 1e-2)
        self.assertEqual(len(both), len(single))
        for (time, centre, _), (_, centre2, _) in zip(single, both):
            self.assertLess((centre - centre2).Length, 1e-6, time)
        # It swings
        self.assertLess(single[-1][1].z, 5 - 10)
        channels = self.assembly.getSimulationResults()["Channels"]
        forces = [c for c in channels if c["Quantity"] == "Force"]
        self.assertEqual(len(forces), 2)
        for c in channels:
            if c["Quantity"] in ("Force", "Torque"):
                self.assertFalse(c["Unique"], c["Name"])
        weight = 100 * 10 * 10 * 2.7e-6 * G
        for k, force in enumerate(alone[0]["Values"]):
            together = forces[0]["Values"][k] + forces[1]["Values"][k]
            self.assertLess((together - force).Length, weight * 1e-6, k)

    def test_kinematic_reactions(self):
        """A kinematic run with its reactions (docs/AssemblyDynamics.md sec
        6.3): the rod driven from horizontal by theta = 2 t^2 takes the
        motor torque I alpha - m g d cos(theta) about the hinge, and the
        hinge holds it with m (a - g), a its centre of mass's from the
        prescribed motion."""
        turns = self.driven("Position", "2*time*time", analysis="Kinematic", reactions=True)
        self.assertIsNotNone(turns)
        rod = self.doc.getObject("Rod")
        frames = self.frames(rod, 1e-2)
        forces = self.channel("Revolute", "Force")
        torques = self.channel("Revolute", "MotorTorque")
        mass = 100 * 10 * 10 * 2.7e-6
        d = 50.0
        inertia = mass * ((100**2 + 10**2) / 12 + d**2)  # kg mm^2, about the hinge
        weight = mass * G
        # Which way it turns: theta below the horizontal, as the rod reads it
        last = frames[-1][1]
        sign = 1.0 if 5 - last.z > 0 else -1.0
        for (time, centre, _), force, torque in zip(frames, forces, torques):
            theta = math.atan2(5 - centre.z, centre.x - 10)
            self.assertAlmostEqual(theta, sign * 2 * time * time, delta=1e-9, msg=time)
            omega = sign * 4 * time
            alpha = sign * 4.0
            e = App.Vector(math.cos(theta), 0, -math.sin(theta))
            t = App.Vector(-math.sin(theta), 0, -math.cos(theta))
            accel = t * (alpha * d) - e * (omega * omega * d)
            onRod = accel * mass + App.Vector(0, 0, weight)
            self.assertLess((force + onRod).Length, weight * 1e-6, time)
            want = inertia * alpha - weight * d * math.cos(theta)
            self.assertAlmostEqual(abs(torque), abs(want), delta=weight * d * 1e-6, msg=time)

    def sliderCrank(self, analysis, reactions=False):
        """A crank 20 mm long about z through the origin, a rod 60 mm to a
        block sliding along x, all in the plane z = 0 (a closed loop: its
        out-of-plane equations are redundant); the crank driven at 10
        rad/s, no gravity. The crank's motor torque at each frame of 0.2 s
        by 0.01 s, or None if the run failed."""
        ground = self.box("Ground", (10, 10, 10), (-5, -5, -20))
        fixed = self.joints.newObject("Assembly::GroundedJointPython", "GroundedGround")
        JointObject.GroundedJoint(fixed, ground)
        crank = self.box("Crank", (20, 4, 4), (0, -2, -2))
        rod = self.box("Rod", (60, 4, 4), (20, -2, -2))
        block = self.box("Block", (10, 10, 10), (75, -5, -5))
        self.doc.recompute()
        self.joint("Revolute", (ground, ["Face6"]), plc(5, 5, 20), (crank, ["Face1"]), plc(0, 2, 2))
        self.joint("Revolute", (crank, ["Face2"]), plc(20, 2, 2), (rod, ["Face1"]), plc(0, 2, 2))
        self.joint("Revolute", (rod, ["Face2"]), plc(60, 2, 2), (block, ["Face1"]), plc(5, 5, 5))
        along = App.Vector(0, 1, 0)
        self.joint(
            "Slider",
            (ground, ["Face6"]),
            plc(85, 5, 20, along, 90),
            (block, ["Face1"]),
            plc(5, 5, 5, along, 90),
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = analysis
        sim.Reactions = reactions
        sim.GravityEnabled = False
        sim.TimeEnd = 0.2
        sim.TimeStepOutput = 0.01
        motion = sim.newObject("Assembly::Motion", "Motion")
        motion.Joint = [o for o in self.joints.Group if o.Name == "Revolute"][0]
        motion.MotionType = "Angular"
        motion.Formula = "10*time"
        self.doc.recompute()
        if self.assembly.generateSimulation(sim) != 0:
            return None
        return self.channel("Revolute", "MotorTorque")

    def test_kinematic_reactions_slider_crank(self):
        """The slider-crank's motor torque from the kinematic run's inverse
        dynamics is the torque a Dynamic run of the same Position motion
        takes, to the dynamic run's accuracy."""
        inverse = self.sliderCrank("Kinematic", reactions=True)
        self.assertIsNotNone(inverse)
        self.tearDown()
        self.setUp()
        forward = self.sliderCrank("Dynamic")
        self.assertIsNotNone(forward)
        self.assertEqual(len(inverse), len(forward))
        largest = max(abs(v) for v in inverse)
        self.assertGreater(largest, 1.0)
        for k, (a, b) in enumerate(zip(inverse, forward)):
            self.assertAlmostEqual(a, b, delta=largest * 1e-3, msg=k)

    def test_kinematic_reactions_need_every_freedom(self):
        """A kinematic run that asks for its reactions with a freedom left
        undriven runs, and reports none."""
        rod = self.pendulum()
        sim, result = self.simulate(0.05, 1e-2, AnalysisType="Kinematic", Reactions=True)
        self.assertEqual(result, 0)
        self.assertEqual(self.assembly.numberOfFrames(), 6)
        results = self.assembly.getSimulationResults()
        self.assertEqual(results["Channels"], [])
        for time, centre, _ in self.frames(rod, 1e-2):
            self.assertLess((centre - App.Vector(60, 5, 5)).Length, 1e-9, time)

    def test_bushing_not_listed(self):
        """A bushing the study does not list does nothing."""
        block = self.slider()
        self.bushing(
            (block, App.Placement(App.Vector(5, 5, 0), App.Rotation())),
            TranslationalStiffness=App.Vector(1, 1, 1),
            Preload=App.Vector(0, 0, 1),
        )
        _, result = self.simulate(0.1, 1e-2, GravityEnabled=False, Elements=[])
        self.assertEqual(result, 0)
        for time, z in self.heights(block, 1e-2):
            self.assertAlmostEqual(z, 12, delta=1e-9, msg=time)
