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

    def oscillator(self, damping=0.0, listed=True, onFaces=False):
        """A 10 mm cube on a vertical slider over a grounded one, a spring
        from the grounded one's top to its bottom, free at 4 mm; started at
        2 mm, without gravity: it swings about 4 mm by 2. With onFaces, the
        spring's ends are on the faces rather than points."""
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
        self.assertEqual(len(header), 7)
        self.assertEqual(header[0], "Time (s)")
        self.assertEqual(header[1], "Revolute: Reaction force X (N)")
        self.assertEqual(header[6], "Revolute: Reaction torque Z (N m)")
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
        check leaves the last solve's status as it was; a study whose frames
        are gone is not stale."""
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

        # Another study's run: this one has no frames now
        sim.TimeEnd = 0.03
        self.assertTrue(stale())
        other, result = self.simulate(0.02, 1e-2, AnalysisType="Kinematic")
        self.assertEqual(result, 0)
        self.assertFalse(sim.ResultsStale)
        self.assertFalse(self.assembly.isSimulationStale(sim))
        rod.Length = 90
        self.doc.recompute()
        self.assertFalse(other.ResultsStale)

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
