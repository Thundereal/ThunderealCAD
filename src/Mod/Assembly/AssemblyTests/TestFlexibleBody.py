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

    def cantilever(self, modes):
        """The rod fixed by its end to the grounded block -- not taken onto
        the ground, flexible -- run 20 ms under gravity, damped hard: the
        rod, its flexible body and the run's results"""
        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        rod = self.box("Rod", (100, 10, 10), (10, 0, 0))
        self.doc.recompute()
        clamp = self.joints.newObject("App::FeaturePython", "Fixed")
        JointObject.Joint(clamp, JointObject.JointTypes.index("Fixed"))
        clamp.Detach1 = True
        clamp.Detach2 = True
        clamp.Reference1 = (base, ["Face2"])
        clamp.Reference2 = (rod, ["Face1"])
        clamp.Placement1 = plc(10, 5, 5)
        clamp.Placement2 = plc(0, 5, 5)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        sim = self.study(0.02, 1e-4)
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        flexible.Modes = modes
        flexible.Damping = 0.5
        FlexibleBodies.prepare(flexible)
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        return rod, flexible, self.assembly.getSimulationResults()

    def test_cantilever_deformation(self):
        """The cantilever sags: its end, made of its mode shapes and the
        run's coordinates, where the beam's is, q L^4 / 8 E I; at its hold,
        nothing"""
        rod, flexible, results = self.cantilever(6)
        self.assertEqual(flexible.ModeShapeColumns, 12)
        self.assertEqual(len(flexible.ModeShapes), 3 * len(flexible.Nodes) * 12)
        shape = FlexibleBodies.Deformation(flexible, results)
        self.assertTrue(shape.valid)
        # In the rod's coordinates, which move with its floating frame: in
        # the assembly's at the last frame, its pose there
        last = len(results["Times"]) - 1
        self.assembly.updateForFrame(last)
        points = shape.pointsAt(results["Times"][-1], 1.0)
        pose = rod.Placement

        def at(x):
            near = min(
                range(len(shape.rest)),
                key=lambda i: (shape.rest[i][0] - x) ** 2
                + (shape.rest[i][1] - 5) ** 2
                + (shape.rest[i][2] - 5) ** 2,
            )
            # Where it is, against where it was: the rod at (10, 0, 0), unturned
            start = App.Vector(*shape.rest[near]) + App.Vector(10, 0, 0)
            return pose.multVec(App.Vector(*points[near])) - start

        # Aluminium, 70 GPa and 2700 kg/m^3, SI
        e, rho, side, length = 70e9, 2700.0, 0.01, 0.1
        q = rho * side * side * G * 1e-3
        sag = q * length**4 / (8 * e * side**4 / 12) * 1e3  # mm
        self.assertAlmostEqual(at(100).z, -sag, delta=0.03 * sag)
        self.assertLess(abs(at(0).z), 0.01 * sag)

    def test_cantilever_stress(self):
        """The cantilever's stress, each mode's weighed by the run's
        coordinates: settled, along its top the beam's, M c / I with M = q
        (L - x)^2 / 2, in tension, below it the same in compression; the
        largest the root's; the Results box's channel of it, the largest at
        each frame"""
        import numpy as np

        import CommandCreateSimulation

        rod, flexible, results = self.cantilever(12)
        shape = FlexibleBodies.Deformation(flexible, results)
        self.assertTrue(shape.valid)
        youngs, poisson = shape.elasticity
        self.assertAlmostEqual(youngs, 70e3, delta=1.0)  # MPa
        time = results["Times"][-1]
        stress = shape.stressAt(time)
        self.assertEqual(stress.shape, (len(shape.rest), 6))
        # N, mm and MPa: aluminium's 2700 kg/m^3
        q = 2700e-9 * 100 * G * 1e-3
        inertia = 10.0**4 / 12

        def beam(x, z):
            return q * (100 - x) ** 2 / 2 * (z - 5) / inertia

        for x in (30.0, 50.0, 70.0):
            for z in (10.0, 0.0):
                near = int(np.argmin(np.linalg.norm(shape.rest - (x, 5.0, z), axis=1)))
                p = shape.rest[near]
                want = beam(p[0], p[2])
                # 1.3% at most here; near the free end, where it is a
                # hundredth of the root's, the modes left out take a fifth
                self.assertAlmostEqual(stress[near][0], want, delta=0.02 * abs(want), msg=(x, z))
        # The largest at the root, as large as the beam's there or more: the
        # hold's corners
        root = q * 100**2 / 2 * 5 / inertia
        vonMises = shape.vonMisesAt(time)
        worst = shape.rest[int(np.argmax(vonMises))]
        self.assertLess(worst[0], 10.0)
        largest = shape.largestStresses()
        self.assertEqual(len(largest), len(results["Times"]))
        self.assertAlmostEqual(largest[-1], float(np.max(vonMises)), delta=1e-9 * root)
        # 1.06 times
        self.assertGreater(largest[-1], root)
        self.assertLess(largest[-1], 1.2 * root)
        self.assertEqual(shape.peakStress(), max(largest))
        # The Results box's
        channels = [
            c
            for c in CommandCreateSimulation.resultChannels(self.assembly)["Channels"]
            if c["Quantity"] == "FlexibleVonMises"
        ]
        self.assertEqual(len(channels), 1)
        self.assertEqual(channels[0]["Object"].FullName, flexible.FullName)
        self.assertEqual(channels[0]["Values"], largest)
        self.assertNotIn(
            "ModalCoordinates",
            [c["Quantity"] for c in CommandCreateSimulation.resultChannels(self.assembly)["Channels"]],
        )

    def test_pendulum_stress(self):
        """The hinged rod let go level, its ringing at release died away:
        along its top the stress of a bar pinned at one end and turning
        about it under gravity, M = rho A g x (L - x)^2 / 4 L -- the load
        gravity's less the turn's, g (1 - 3 x / 2 L), upward past 2 L / 3:
        what is beyond a section lifts it, so the rod bends concave up -- in
        compression, below it in tension, the largest at L / 3. Its bending
        alone, half the difference of top and bottom: the swing's pull along
        the rod adds the same to both. 24 modes: at 12, L / 3 is 4% over"""
        import numpy as np

        rod, hinge = self.pendulum()
        sim = self.study(0.03, 1e-4)
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        flexible.Modes = 24
        FlexibleBodies.prepare(flexible)
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        results = self.assembly.getSimulationResults()
        shape = FlexibleBodies.Deformation(flexible, results)
        self.assertTrue(shape.valid)
        time = 0.02
        stress = shape.stressAt(time)
        # Turned so far: cos of it, the load's share across the rod
        turned = math.cos(0.5 * 3 * G / (2 * 100) * time**2)
        q = 2700e-9 * 100 * G * 1e-3 * turned
        inertia = 10.0**4 / 12
        def want(p):
            return -q * p[0] * (100 - p[0]) ** 2 / 400 * (p[2] - 5) / inertia

        for x in (20.0, 100.0 / 3, 50.0, 70.0):
            top, below = (
                int(np.argmin(np.linalg.norm(shape.rest - (x, 5.0, z), axis=1))) for z in (10.0, 0.0)
            )
            self.assertLess(stress[top][0], 0.0)
            self.assertGreater(stress[below][0], 0.0)
            # The difference, which the pull along the rod is not in; 1.7%
            # at most
            ratio = (stress[top][0] - stress[below][0]) / (
                want(shape.rest[top]) - want(shape.rest[below])
            )
            self.assertAlmostEqual(ratio, 1.0, delta=0.03, msg=x)

    def test_propped_cantilever(self):
        """A contact on a flexible part (D4b step 5), through the faces it
        is touched on: the rod clamped at one end to the grounded block, a
        pad under its free end resting on a grounded post, the pad's face
        its contact face. Under its weight the post takes what a propped
        cantilever's prop does -- the rod's bending decides it, which a
        rigid rod leaves to chance -- q (6 L^2 - 4 L a + a^2) / 8 a, the
        prop at a, and the pad's own weight; the clamp the rest"""
        import Part

        import CommandCreateContact

        base = self.box("Base", (10, 10, 10), (0, 0, 0))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
        JointObject.GroundedJoint(ground, base)
        rod = self.assembly.newObject("Part::Feature", "Rod")
        pad = Part.makeBox(5, 10, 1, App.Vector(95, 0, -1))
        rod.Shape = Part.makeBox(100, 10, 10).fuse(pad).removeSplitter()
        rod.Placement.Base = App.Vector(10, 0, 0)
        post = self.box("Post", (8, 14, 10), (10 + 93.5, -2, -11))
        held = self.joints.newObject("Assembly::GroundedJointPython", "GroundedPost")
        JointObject.GroundedJoint(held, post)
        self.doc.recompute()
        faces = rod.Shape.Faces
        bottom = min(range(len(faces)), key=lambda k: faces[k].CenterOfMass.z)
        self.assertAlmostEqual(faces[bottom].CenterOfMass.z, -1.0, places=6)
        # Placed: its end at x = 10
        ends = [k for k, f in enumerate(faces) if abs(f.CenterOfMass.x - 10) < 1e-6]
        self.assertEqual(len(ends), 1)
        clamp = self.joints.newObject("App::FeaturePython", "Fixed")
        JointObject.Joint(clamp, JointObject.JointTypes.index("Fixed"))
        clamp.Detach1 = True
        clamp.Detach2 = True
        clamp.Reference1 = (base, ["Face2"])
        clamp.Reference2 = (rod, ["Face%d" % (ends[0] + 1)])
        clamp.Placement1 = plc(10, 5, 5)
        clamp.Placement2 = plc(0, 5, 5)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        contact = CommandCreateContact.newContactSet(self.assembly, [rod], [post])
        contact.StiffnessFrom = "Penetration"
        contact.AllowedPenetration = 1e-5
        sim = self.study(0.02, 1e-4)
        sim.Elements = list(sim.Elements) + [contact]
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        flexible.Modes = 12
        flexible.Damping = 0.5
        flexible.ContactFaces = [(rod, ["Face%d" % (bottom + 1)])]
        FlexibleBodies.prepare(flexible)
        self.assertEqual(flexible.InterfaceFrames[-1], FlexibleBodies.CONTACT)
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        results = self.assembly.getSimulationResults()
        normal = [
            c["Values"]
            for c in results["Channels"]
            if c["Quantity"] == "ContactNormalForce" and c["Name"] == contact.Name
        ]
        self.assertEqual(len(normal), 1)
        # mN: aluminium's 2700 kg/m^3, the prop at the pad's middle
        q = 2700e-9 * 100 * G
        a = 97.5
        weight = 2700e-9 * 50 * G
        want = q * (6 * 100**2 - 4 * 100 * a + a**2) / (8 * a) + weight
        # 1.4% over; a rigid rod's prop takes nothing, its clamp all
        self.assertAlmostEqual(normal[0][-1].z, want, delta=0.03 * want)
        # The clamp the rest of its weight
        force = [
            c["Values"]
            for c in results["Channels"]
            if c["Quantity"] == "Force" and c["Name"] == clamp.Name
        ]
        self.assertEqual(len(force), 1)
        total = q * 100 + weight
        self.assertAlmostEqual(abs(force[0][-1].z) + normal[0][-1].z, total, delta=1e-3 * total)

    def strike(self, flexible):
        """An aluminium strip 200 x 20 x 4 hinged at one end, a pad 5 x 20 x
        1 under its free end, let go level onto a grounded post 10 mm under
        the pad, stiff (an allowance of 1e-4 mm): the run's status and the
        contact's largest normal force, mN"""
        import Part

        import CommandCreateContact

        # The hinge 1 mm under the strip's middle: the solve lowers it so
        mount = self.box("Mount", (10, 20, 10), (0, 0, -4))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedMount")
        JointObject.GroundedJoint(ground, mount)
        strip = self.assembly.newObject("Part::Feature", "Strip")
        pad = Part.makeBox(5, 20, 1, App.Vector(195, 0, -1))
        strip.Shape = Part.makeBox(200, 20, 4).fuse(pad).removeSplitter()
        strip.Placement.Base = App.Vector(10, 0, 0)
        post = self.box("Post", (15, 30, 30), (200, -5, -41))
        held = self.joints.newObject("Assembly::GroundedJointPython", "GroundedPost")
        JointObject.GroundedJoint(held, post)
        self.doc.recompute()
        faces = strip.Shape.Faces
        bottom = min(range(len(faces)), key=lambda k: faces[k].CenterOfMass.z)
        end = [k for k, f in enumerate(faces) if abs(f.CenterOfMass.x - 10) < 1e-6][0]
        hinge = self.joints.newObject("App::FeaturePython", "Revolute")
        JointObject.Joint(hinge, JointObject.JointTypes.index("Revolute"))
        hinge.Detach1 = True
        hinge.Detach2 = True
        hinge.Reference1 = (mount, ["Face2"])
        hinge.Reference2 = (strip, ["Face%d" % (end + 1)])
        hinge.Placement1 = plc(10, 10, 5, App.Vector(1, 0, 0), -90)
        hinge.Placement2 = plc(0, 10, 2, App.Vector(1, 0, 0), -90)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        contact = CommandCreateContact.newContactSet(self.assembly, [strip], [post])
        contact.StiffnessFrom = "Penetration"
        contact.AllowedPenetration = 1e-4
        sim = self.study(0.06, 2e-4)
        sim.Elements = [contact]
        if flexible:
            body = FlexibleBodies.makeFlexibleBody(self.assembly, strip)
            body.MeshSize = 8.0
            body.ContactFaces = [(strip, ["Face%d" % (bottom + 1)])]
            FlexibleBodies.prepare(body)
        self.doc.recompute()
        status = self.assembly.generateSimulation(sim)
        results = self.assembly.getSimulationResults()
        normal = [c["Values"] for c in results["Channels"] if c["Quantity"] == "ContactNormalForce"]
        return status, max(v.z for v in normal[0]) if normal and normal[0] else 0.0

    def test_strike(self):
        """The flexible strip strikes the post and bounces: the run goes
        through it -- a substep taken again after one ran off starts its
        floating frame from where it was, not where that left it; it did not,
        and as the strip left the post every substep after failed, down to
        a nanosecond -- and the blow is softer than the rigid strip's, its
        own compliance in series with the contact's"""
        status, flexible = self.strike(True)
        self.assertEqual(status, 0)
        App.closeDocument(self.doc.Name)
        self.doc = App.newDocument("TestFlexibleBody")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.getGroup("Joints", True)
        status, rigid = self.strike(False)
        self.assertEqual(status, 0)
        self.assertGreater(flexible, 0.0)
        self.assertLess(flexible, rigid)

    def test_contact_needs_its_faces(self):
        """A contact on a flexible part without the faces it is touched on
        is refused (the console says to name them, its ContactFaces)"""
        rod, hinge = self.pendulum()
        floor = self.box("Floor", (200, 40, 5), (-50, -15, -60))
        held = self.joints.newObject("Assembly::GroundedJointPython", "GroundedFloor")
        JointObject.GroundedJoint(held, floor)
        self.doc.recompute()
        import CommandCreateContact

        contact = CommandCreateContact.newContactSet(self.assembly, [rod], [floor])
        sim = self.study(0.01, 1e-3)
        sim.Elements = list(sim.Elements) + [contact]
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        flexible.Modes = 4
        FlexibleBodies.prepare(flexible)
        self.doc.recompute()
        self.assertNotEqual(self.assembly.generateSimulation(sim), 0)
        self.assertEqual(self.assembly.numberOfFrames(), 0)

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
