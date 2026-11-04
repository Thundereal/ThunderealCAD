# SPDX-License-Identifier: LGPL-2.1-or-later
"""Joint friction (docs/AssemblyDynamics.md sec 2.2, 5.2, phase D2): Coulomb's
friction in a Slider or a Revolute joint, its load the joint's reaction
across its axis or a preload -- against the closed forms of a block thrown
along a rail, a block on a slope, and a wheel spinning down on its axle."""

import math
import unittest

import FreeCAD as App
import JointObject

G = 9806.65  # mm/s^2
DENSITY = 2.7e-6  # kg/mm^3, the default aluminium
MU_S, MU_D = 0.3, 0.25  # a new friction's
V_FRICTION = 10.16  # mm/s, from which the dynamic coefficient holds


def plc(x=0.0, y=0.0, z=0.0, axis=None, angle=0.0):
    rot = App.Rotation(axis, angle) if axis is not None else App.Rotation()
    return App.Placement(App.Vector(x, y, z), rot)


class TestJointFriction(unittest.TestCase):
    def setUp(self):
        import AssemblyApp

        if "Chrono" not in AssemblyApp.getAvailableSolvers():
            self.skipTest("the Chrono backend is not built")
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("TestJointFriction")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.getGroup("Joints", True)

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        App.closeDocument(self.doc.Name)

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

    def grounded(self, size, at):
        base = self.assembly.newObject("Part::Box", "Base")
        base.Length, base.Width, base.Height = size
        base.Placement.Base = App.Vector(*at)
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        return base

    def friction(self, joint, **props):
        import CommandCreateSpring

        friction = CommandCreateSpring.makeJointFriction(self.assembly, joint)
        for name, value in props.items():
            setattr(friction, name, value)
        self.doc.recompute()
        return friction

    def simulate(self, friction, end, step, gravity=(0, 0, -G), linear=None, angular=None):
        import CommandCreateSimulation

        sims = self.assembly.getGroup("Simulations", True)
        sim = sims.newObject("Assembly::Simulation", "Simulation")
        sim.AnalysisType = "Dynamic"
        sim.TimeEnd = end
        sim.TimeStepOutput = step
        sim.Elements = [friction]
        if gravity is None:
            sim.GravityEnabled = False
        else:
            sim.Gravity = App.Vector(*gravity)
        if linear is not None or angular is not None:
            CommandCreateSimulation.newInitialCondition(
                sim, component=self.moving, linear=linear, angular=angular
            )
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        return self.assembly.getSimulationResults()

    def channel(self, quantity):
        channels = self.assembly.getSimulationResults()["Channels"]
        found = [c for c in channels if c["Quantity"] == quantity]
        self.assertEqual(len(found), 1, channels)
        return found[0]["Values"]

    def rail(self):
        """A 10 mm cube on a 100 mm rail along x, its slider's axis x; the
        cube's and its friction's"""
        base = self.grounded((100, 20, 10), (-50, 0, 0))
        block = self.assembly.newObject("Part::Box", "Block")
        block.Placement.Base = App.Vector(-5, 5, 10)
        self.doc.recompute()
        along = App.Vector(0, 1, 0)  # z to x
        slider = self.joint(
            "Slider",
            (base, ["Face6"]),
            plc(50, 10, 10, along, 90),
            (block, ["Face5"]),
            plc(5, 5, 0, along, 90),
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        self.moving = block
        return block, self.friction(slider)

    def xs(self, part):
        out = []
        for k in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(k)
            out.append(part.Placement.Base.x)
        return out

    def test_slide_slows(self):
        """A block thrown along its rail at 200 mm/s slows at mu_d g, the
        rail carrying its weight: x = v0 t - mu_d g t^2 / 2 while it slides
        faster than the friction velocity, the friction mu_d m g against it;
        it stops short of v0^2 / (2 mu_d g) by the slow end's lower friction
        at most"""
        block, friction = self.rail()
        step = 1e-3
        self.simulate(friction, 0.1, step, linear=App.Vector(200, 0, 0))
        a = MU_D * G
        weight = 1000 * DENSITY * G  # mN
        xs = self.xs(block)
        forces = self.channel("FrictionForce")
        sliding = 0
        for k, x in enumerate(xs):
            t = k * step
            if 200 - a * t > 2 * V_FRICTION:
                sliding += 1
                self.assertAlmostEqual(x + 5, 200 * t - a * t * t / 2, delta=1e-3, msg=t)
                if k > 0:
                    self.assertAlmostEqual(abs(forces[k]), MU_D * weight, delta=weight * 1e-4)
        self.assertGreater(sliding, 60)
        stop = 200 * 200 / (2 * a)
        self.assertLess(xs[-1] + 5, stop + 0.2)
        self.assertGreater(xs[-1] + 5, stop - 0.2)
        # Stopped: no slip, no friction
        self.assertLess(abs(xs[-1] - xs[-2]), 1e-4)

    def test_slope(self):
        """On a rail tilted 30 deg, steeper than mu_d, the block slides down
        at g (sin a - mu_d cos a); at 10 deg, gentler than mu_s, the
        friction holds it -- creeping below the stiction velocity, as a
        regularised friction does"""
        for degrees, held in ((30.0, False), (10.0, True)):
            with self.subTest(degrees=degrees):
                self.tearDown()
                self.setUp()
                block, friction = self.rail()
                a = math.radians(degrees)
                gravity = (-G * math.sin(a), 0, -G * math.cos(a))
                step = 1e-3
                self.simulate(friction, 0.1, step, gravity=gravity)
                xs = self.xs(block)
                if held:
                    self.assertLess(abs(xs[-1] - xs[0]), 0.1 * 0.1, xs[-5:])
                    continue
                down = G * (math.sin(a) - MU_D * math.cos(a))
                for k, x in enumerate(xs):
                    t = k * step
                    # Once it slides faster than the friction velocity
                    if down * t > 3 * V_FRICTION:
                        self.assertAlmostEqual(
                            x - xs[0], -down * t * t / 2, delta=0.02 + 0.02 * down * t * t, msg=t
                        )

    def wheel(self):
        """A disc of radius 20 mm on a revolute about z at its centre; its
        friction's"""
        base = self.grounded((100, 100, 10), (-50, -50, -20))
        wheel = self.assembly.newObject("Part::Cylinder", "Wheel")
        wheel.Radius = 20
        wheel.Height = 10
        self.doc.recompute()
        axle = self.joint(
            "Revolute", (base, ["Face6"]), plc(50, 50, 20), (wheel, ["Face3"]), plc(0, 0, 0)
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        self.moving = wheel
        return wheel, axle

    def angles(self, part):
        """Its turn about z at each frame, unwrapped, rad"""
        out = []
        for k in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(k)
            r = part.Placement.Rotation
            angle = r.Angle * (1.0 if r.Axis.z >= 0 else -1.0)
            if out:
                while angle - out[-1] > math.pi:
                    angle -= 2 * math.pi
                while angle - out[-1] < -math.pi:
                    angle += 2 * math.pi
            out.append(angle)
        return out

    def spin(self, angles, w0, slow, step):
        """Each frame's turn against w0 t - slow t^2 / 2 while it turns at
        the pin's radius faster than the friction velocity"""
        checked = 0
        for k, angle in enumerate(angles):
            t = k * step
            if (w0 - slow * t) * 5 > 2 * V_FRICTION:
                checked += 1
                self.assertAlmostEqual(angle, w0 * t - slow * t * t / 2, delta=2e-4, msg=t)
        self.assertGreater(checked, 50)
        stop = w0 * w0 / (2 * slow)
        self.assertAlmostEqual(angles[-1], stop, delta=0.01 * stop)

    def test_pin_slows_wheel(self):
        """A wheel spinning on its axle, its weight across the axle (gravity
        in its plane), slows at mu_d R m g / I, R the pin's radius (5 mm);
        the friction torque mu_d R m g"""
        wheel, axle = self.wheel()
        friction = self.friction(axle)
        self.assertFalse(friction.getPropertyStatus("PinRadius").count("Hidden"))
        self.assertIn("Hidden", friction.getPropertyStatus("PreloadForce"))
        w0 = 4 * math.pi
        slow = MU_D * 5 * G * 2 / 400.0  # mu_d R m g / (m r^2 / 2)
        step = 1e-3
        self.simulate(
            friction, 0.3, step, gravity=(-G, 0, 0), angular=App.Vector(0, 0, math.degrees(w0))
        )
        self.spin(self.angles(wheel), w0, slow, step)
        mass = math.pi * 400 * 10 * DENSITY
        torques = self.channel("FrictionTorque")
        self.assertAlmostEqual(abs(torques[20]), MU_D * 5 * mass * G, delta=mass * G * 1e-3)

    def test_preload_torque(self):
        """Without gravity the axle carries nothing, and only a preload
        torque T slows the wheel: at mu_d T / I"""
        wheel, axle = self.wheel()
        friction = self.friction(axle, PreloadTorque=1000.0)  # mN mm, 1 N mm
        w0 = 4 * math.pi
        mass = math.pi * 400 * 10 * DENSITY
        inertia = mass * 400 / 2  # kg mm^2
        slow = MU_D * 1000.0 / inertia  # mN mm / (kg mm^2) = 1 / s^2
        step = 1e-3
        self.simulate(friction, 0.5, step, gravity=None, angular=App.Vector(0, 0, math.degrees(w0)))
        self.spin(self.angles(wheel), w0, slow, step)
