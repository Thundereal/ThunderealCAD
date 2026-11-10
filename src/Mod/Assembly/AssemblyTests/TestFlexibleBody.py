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

    def test_mesh_in_no_document(self):
        """prepare() meshes in no document (Fem.meshNetgen()): the mesh the
        Netgen object makes, no document made, and a transaction open before
        it open after it -- a task panel's, which Cancel then aborts"""
        import Fem
        import ObjectsFem
        import Part

        rod, hinge = self.pendulum()
        self.study(0.01, 1e-3)
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        flexible.MeshSize = 4.0
        self.doc.UndoMode = 1
        names = set(App.listDocuments())
        self.doc.openTransaction("Mesh")
        self.assertTrue(FlexibleBodies.prepare(flexible))
        self.assertTrue(self.doc.HasPendingTransaction)
        self.assertEqual(set(App.listDocuments()), names)
        count = len(flexible.Nodes)
        self.doc.abortTransaction()
        self.assertEqual(len(flexible.Nodes), 0)

        # The Netgen object's mesh of the same shape and settings
        local = Part.getShape(rod, "", transform=False)
        direct = Fem.meshNetgen(local, MaxSize=4.0, SecondOrder=True, Fineness=2)
        self.assertEqual(direct.NodeCount, count)
        shape = self.doc.addObject("Part::Feature", "Shape")
        shape.Shape = local
        mesh = ObjectsFem.makeMeshNetgenLegacy(self.doc, "Mesh")
        mesh.Shape = shape
        mesh.MaxSize = 4.0
        mesh.SecondOrder = True
        mesh.Fineness = 2
        self.doc.recompute()
        self.assertEqual(mesh.FemMesh.NodeCount, direct.NodeCount)
        self.assertEqual(mesh.FemMesh.VolumeCount, direct.VolumeCount)
        # Its faces' nodes, by the faces of the shape meshed
        self.assertGreater(len(direct.getNodesByFace(local.Faces[0])), 0)
        with self.assertRaises(ValueError):
            Fem.meshNetgen(local, Fineness=6)

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

    def test_reduction_kept_in_the_file(self):
        """A flexible body keeps its last run's reduction (Reduction), saved
        with the document and opened again as it was; the run there goes as
        the first, the same mode shapes and coordinates. Its mesh made again,
        it has none. Opened from a file of another name, the document named
        again, the joint's frame is the one the mesh was made for (its full
        name kept in InterfaceFrames, of the document saved), its mesh not
        stale. (That the run takes the file's when this process has none:
        FlexibleBody.aRunTakesTheReductionItsModelHands)"""
        import os
        import tempfile

        # Opened as saved: the clamp, a Python joint, would be converted to
        # the C++ joint on opening, a joint of another name
        convert = self.params.GetBool("ConvertJointsOnOpen", True)
        self.params.SetBool("ConvertJointsOnOpen", False)
        self.addCleanup(self.params.SetBool, "ConvertJointsOnOpen", convert)
        rod, flexible, results = self.cantilever(6)
        kept = list(flexible.Reduction)
        self.assertGreater(len(kept), 0)
        shapes = list(flexible.ModeShapes)

        def modal(results):
            return [
                list(c["Values"])
                for c in results["Channels"]
                if c["Quantity"] == "ModalCoordinates"
            ]

        def apart(a, b):
            """The largest difference of two lists of numbers, inf if their
            lengths differ"""
            a, b = list(a), list(b)
            if len(a) != len(b):
                return float("inf")
            return max((abs(x - y) for x, y in zip(a, b)), default=0.0)

        first = modal(results)
        self.assertEqual(len(first), 1)
        name = flexible.Name
        sim = [o for o in self.doc.Objects if o.isDerivedFrom("Assembly::Simulation")][0].Name
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "kept.FCStd")
            self.doc.saveAs(path)
            App.closeDocument(self.doc.Name)
            self.doc = App.openDocument(path)
            self.assembly = self.doc.getObject("Assembly")
            self.joints = self.assembly.getGroup("Joints", True)
            flexible = self.doc.getObject(name)
            self.assertNotEqual(self.doc.Name, "TestFlexibleBody")
            self.assertEqual(FlexibleBodies.meshState(flexible), "current")
            # Compared by their largest difference: assertEqual of two long
            # lists that differ spends hours in difflib
            self.assertEqual(apart(flexible.Reduction, kept), 0.0)
            self.assertEqual(self.assembly.generateSimulation(self.doc.getObject(sim)), 0)
            again = modal(self.assembly.getSimulationResults())
            self.assertEqual(apart(flexible.Reduction, kept), 0.0)
            self.assertEqual(apart(flexible.ModeShapes, shapes), 0.0)
            self.assertEqual(len(again), 1)
            self.assertEqual(len(again[0]), len(first[0]))
            for a, b in zip(again[0], first[0]):
                for x, y in zip(a, b):
                    self.assertAlmostEqual(x, y, delta=1e-9 * (1 + abs(y)))
            FlexibleBodies.prepare(flexible, force=True)
            self.assertEqual(len(flexible.Reduction), 0)
            self.renew()

    def test_cantilever_deformation(self):
        """The cantilever sags: its end, made of its mode shapes and the
        run's coordinates, where the beam's is, q L^4 / 8 E I; at its hold,
        nothing"""
        rod, flexible, results = self.cantilever(6)
        # The hold's six, the six modes, the twelve residual modes of the
        # inertia loads
        self.assertEqual(flexible.ModeShapeColumns, 24)
        self.assertEqual(len(flexible.ModeShapes), 3 * len(flexible.Nodes) * 24)
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
        each frame. Six modes: the inertia loads' residual modes take what
        the modes left out would of the gravity -- without them, a fifth of
        the stress 80 mm out"""
        import numpy as np

        import CommandCreateSimulation

        rod, flexible, results = self.cantilever(6)
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

        # Without the residual modes, their twelve columns left out
        modal = shape.stresses()
        coordinates = shape.coordinatesAt(time)
        without = np.einsum("nkc,c->nk", modal[:, :, :-12], coordinates[:-12])
        for x in (30.0, 50.0, 70.0, 80.0):
            for z in (10.0, 0.0):
                near = int(np.argmin(np.linalg.norm(shape.rest - (x, 5.0, z), axis=1)))
                p = shape.rest[near]
                want = beam(p[0], p[2])
                # 2.4% at most here, 80 mm out, where it is a sixteenth of
                # the root's; nearer the free end than its depth the beam's
                # is not the part's (9% at 90 mm, at 6 modes as at 12)
                self.assertAlmostEqual(stress[near][0], want, delta=0.03 * abs(want), msg=(x, z))
                if x >= 70.0:
                    # 13% and 21% under
                    self.assertLess(abs(without[near][0]), 0.9 * abs(want), msg=(x, z))
        # Their coefficients: the gravity, the rod at rest in the frame's axes
        loads = [
            c
            for c in results["Channels"]
            if c["Quantity"] == "InertialLoad"
        ]
        self.assertEqual(len(loads), 1)
        for at in (0, -1):
            load = loads[0]["Values"][at]
            self.assertEqual(len(load), 12)
            self.assertAlmostEqual(load[2], -G * 1e-3, delta=1e-3, msg=at)
            for k in (0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11):
                self.assertLess(abs(load[k]), 1e-3, msg=(at, k))
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
        the rod adds the same to both. 12 modes, with the inertia loads'
        residual modes as good as 24 without: without, 4% over at L / 3 and
        20% at 90 mm"""
        import numpy as np

        rod, hinge = self.pendulum()
        sim = self.study(0.03, 1e-4)
        flexible = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
        flexible.Modes = 12
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

        for x in (20.0, 100.0 / 3, 50.0, 70.0, 80.0, 90.0):
            top, below = (
                int(np.argmin(np.linalg.norm(shape.rest - (x, 5.0, z), axis=1))) for z in (10.0, 0.0)
            )
            self.assertLess(stress[top][0], 0.0)
            self.assertGreater(stress[below][0], 0.0)
            # The difference, which the pull along the rod is not in; 2.2%
            # at most
            ratio = (stress[top][0] - stress[below][0]) / (
                want(shape.rest[top]) - want(shape.rest[below])
            )
            self.assertAlmostEqual(ratio, 1.0, delta=0.03, msg=x)

    def propped(self, touchedOn):
        """The rod clamped at one end to the grounded block, a pad under its
        free end resting on a grounded post, the pad's face its contact face
        (touchedOn) or none: the contact's normal force and the clamp's at the
        end, what a propped cantilever's prop takes and the whole weight
        (mN), and the run's time (s)"""
        import time

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
        if touchedOn:
            flexible.ContactFaces = [(rod, ["Face%d" % (bottom + 1)])]
        FlexibleBodies.prepare(flexible)
        self.assertEqual(FlexibleBodies.CONTACT in flexible.InterfaceFrames, touchedOn)
        self.doc.recompute()
        start = time.perf_counter()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        took = time.perf_counter() - start
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
        force = [
            c["Values"]
            for c in results["Channels"]
            if c["Quantity"] == "Force" and c["Name"] == clamp.Name
        ]
        self.assertEqual(len(force), 1)
        total = q * 100 + weight
        return normal[0][-1].z, abs(force[0][-1].z), want, total, took

    def test_propped_cantilever(self):
        """A contact on a flexible part (D4b step 5), through the faces it
        is touched on: the rod clamped at one end to the grounded block, a
        pad under its free end resting on a grounded post, the pad's face
        its contact face. Under its weight the post takes what a propped
        cantilever's prop does -- the rod's bending decides it, which a
        rigid rod leaves to chance -- q (6 L^2 - 4 L a + a^2) / 8 a, the
        prop at a, and the pad's own weight; the clamp the rest"""
        normal, clamp, want, total, _ = self.propped(True)
        # 1.4% over; a rigid rod's prop takes nothing, its clamp all
        self.assertAlmostEqual(normal, want, delta=0.03 * want)
        self.assertAlmostEqual(clamp + normal, total, delta=1e-3 * total)

    def test_propped_cantilever_anywhere(self):
        """A contact anywhere on a flexible part, without the faces it is
        touched on: the pad's points press the rod's mesh where they are,
        each one's force on the part's reduced coordinates and its stiffness
        with it. The post takes the propped cantilever's prop, as through the
        faces"""
        normal, clamp, want, total, took = self.propped(False)
        print("anywhere: prop %.4g mN for %.4g, clamp %.4g, run %.1f s" % (normal, want, clamp, took))
        self.assertAlmostEqual(normal, want, delta=0.03 * want)
        self.assertAlmostEqual(clamp + normal, total, delta=1e-3 * total)

    def strike(self, flexible, touchedOn=True):
        """An aluminium strip 200 x 20 x 4 hinged at one end, a pad 5 x 20 x
        1 under its free end, let go level onto a grounded post 10 mm under
        the pad, stiff (an allowance of 1e-4 mm), the pad's face its contact
        face (touchedOn) or none: the run's status and the contact's largest
        normal force, mN"""
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
            if touchedOn:
                body.ContactFaces = [(strip, ["Face%d" % (bottom + 1)])]
            FlexibleBodies.prepare(body)
            self.assertEqual(FlexibleBodies.CONTACT in body.InterfaceFrames, touchedOn)
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
        print("strike: %.4g mN, rigid %.4g" % (flexible, rigid))
        self.assertGreater(flexible, 0.0)
        self.assertLess(flexible, rigid)

    def test_strike_anywhere(self):
        """The flexible strip strikes the post anywhere on it, without the
        faces it is touched on: the run goes through, the blow softer than
        the rigid strip's"""
        status, flexible = self.strike(True, False)
        self.assertEqual(status, 0)
        self.renew()
        status, rigid = self.strike(False)
        self.assertEqual(status, 0)
        print("strike anywhere: %.4g mN, rigid %.4g" % (flexible, rigid))
        self.assertGreater(flexible, 0.0)
        self.assertLess(flexible, rigid)

    def renew(self):
        """A new document for a second run, as setUp's"""
        App.closeDocument(self.doc.Name)
        self.doc = App.newDocument("TestFlexibleBody")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.getGroup("Joints", True)

    def hingeLoaded(self, flexible, load, end, step):
        """The pendulum let go level with load(hinge) on its hinge (None:
        nothing), flexible
        (6 modes) or rigid: the run's status, the rod's angle below the
        horizontal at each frame (deg) and the run's time (s)"""
        import time

        rod, hinge = self.pendulum()
        item = load(hinge)
        sim = self.study(end, step)
        if flexible:
            body = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
            body.Modes = 6
            FlexibleBodies.prepare(body)
        if item is not None:
            sim.Elements = list(sim.Elements) + [item]
        self.doc.recompute()
        start = time.perf_counter()
        status = self.assembly.generateSimulation(sim)
        took = time.perf_counter() - start
        angles = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            centre = rod.Placement.multVec(App.Vector(50, 5, 5))
            angles.append(math.degrees(math.atan2(5 - centre.z, centre.x - 10)))
        return status, angles, took

    def test_stop_on_flexible_part(self):
        """A penalty stop on the flexible rod's hinge, 30 deg below the
        horizontal, 10 N m per radian, damped near critically: the rod comes
        to rest on it where the rigid one does, m g L cos a = k (a - 30 deg).
        The stop's substeps from the rod's inertia about the hinge, not the
        hinge's interface body's, a millionth of the rod's mass at the
        hinge's axis"""

        def stop(hinge):
            hinge.EnableAngleMin = True
            hinge.EnableAngleMax = True
            hinge.AngleMin = -30.0
            hinge.AngleMax = 30.0
            physics = self.assembly.getGroup("Physics", True)
            s = physics.newObject("Assembly::JointStop", "JointStop")
            s.Joint = hinge
            s.Mode = "Penalty"
            s.TorsionalStiffness = "10 N*m"
            s.TorsionalDamping = "0.05 N*m*s"
            return s

        status, flexible, took = self.hingeLoaded(True, stop, 0.3, 1e-3)
        self.assertEqual(status, 0)
        self.renew()
        status, rigid, _ = self.hingeLoaded(False, stop, 0.3, 1e-3)
        self.assertEqual(status, 0)
        # SI, as TestDynamics.test_stop_penalty_rotation
        torque = 100 * 10 * 10 * 2.7e-6 * G * 1e-3 * 0.05
        a = math.pi / 6
        for _ in range(20):
            a -= (10.0 * (a - math.pi / 6) - torque * math.cos(a)) / (10.0 + torque * math.sin(a))
        self.assertAlmostEqual(rigid[-1], math.degrees(a), delta=1e-3)
        self.assertAlmostEqual(flexible[-1], rigid[-1], delta=1e-2)
        self.assertGreater(max(flexible), 30.0)
        self.assertLess(took, 60.0)

    def test_stop_bounce_on_flexible_part(self):
        """Undamped, the stop hands the flexible rod its energy back as it
        does the rigid one's: let go level onto it, it comes back to where it
        fell from. The stop sets the substeps by the rod's inertia about the
        hinge: by the interface body's, which reads no mass in the run, it
        set none, and the rod came back 0.28 deg above level, the rigid one
        0.02"""

        def stop(hinge):
            hinge.EnableAngleMin = True
            hinge.EnableAngleMax = True
            hinge.AngleMin = -30.0
            hinge.AngleMax = 30.0
            physics = self.assembly.getGroup("Physics", True)
            s = physics.newObject("Assembly::JointStop", "JointStop")
            s.Joint = hinge
            s.Mode = "Penalty"
            s.TorsionalStiffness = "10 N*m"
            s.TorsionalDamping = "0 N*m*s"
            return s

        def bounce(flexible):
            status, angles, _ = self.hingeLoaded(flexible, stop, 0.25, 1e-3)
            self.assertEqual(status, 0)
            deepest = max(range(len(angles)), key=lambda k: angles[k])
            return angles[deepest], min(angles[deepest:])

        deepest, back = bounce(True)
        self.renew()
        rigidDeepest, rigidBack = bounce(False)
        self.assertGreater(rigidDeepest, 30.0)
        self.assertAlmostEqual(rigidBack, 0.0, delta=0.05)
        self.assertAlmostEqual(deepest, rigidDeepest, delta=0.05)
        self.assertAlmostEqual(back, rigidBack, delta=0.1)

    def test_friction_on_flexible_part(self):
        """Joint friction on the flexible rod's hinge, preloaded 3 N mm --
        the pin's own, on the hinge's reaction, takes less off the swing in
        50 ms than the substeps' own spread, 0.02 deg: let go level, it swings down slowed by the
        friction as the rigid one is. Its substeps from the rod's inertia
        about the hinge, as a stop's"""
        import CommandCreateSpring

        def friction(hinge):
            f = CommandCreateSpring.makeJointFriction(self.assembly, hinge)
            f.PinRadius = 5.0
            f.PreloadTorque = 3000.0  # mN mm
            return f

        status, flexible, took = self.hingeLoaded(True, friction, 0.05, 1e-3)
        self.assertEqual(status, 0)
        self.renew()
        status, rigid, _ = self.hingeLoaded(False, friction, 0.05, 1e-3)
        self.assertEqual(status, 0)
        self.renew()
        status, free, _ = self.hingeLoaded(False, lambda hinge: None, 0.05, 1e-3)
        self.assertEqual(status, 0)
        self.assertEqual(len(flexible), len(rigid))
        # The friction slows it
        self.assertLess(rigid[-1], free[-1] - 0.1)
        for k, (f, r) in enumerate(zip(flexible, rigid)):
            self.assertAlmostEqual(f, r, delta=0.05, msg=k)
        self.assertLess(took, 60.0)

    def test_contact_anywhere(self):
        """A contact on a flexible part without the faces it is touched on
        runs, anywhere on the part (it was refused)"""
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
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assertEqual(self.assembly.numberOfFrames(), 11)

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
