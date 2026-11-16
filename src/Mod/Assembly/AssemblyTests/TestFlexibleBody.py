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
        # Drawn scaled up to be seen, its largest a tenth of the rod, or as
        # the flexible body's BendingScale has it; the run stays the study's
        import numpy as np

        automatic = shape.scale
        largest = max(
            float(np.max(np.linalg.norm(shape.pointsAt(t, 1.0) - shape.rest, axis=1)))
            for t in results["Times"]
        )
        self.assertAlmostEqual(automatic * largest, 10.0, delta=0.01)
        flexible.BendingScale = 1.0
        self.doc.recompute()
        self.assertEqual(shape.scale, 1.0)
        self.assertFalse(results["Simulation"].ResultsStale)
        self.assertEqual(len(self.assembly.getSimulationResults()["Times"]), len(results["Times"]))
        flexible.BendingScale = 0.0
        self.assertEqual(shape.scale, automatic)
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

    def test_bent_onto_bar_anywhere(self):
        """A strip 200 x 20 x 2 hinged at an end and let go level lands on a
        grounded bar 120 mm out, 10 mm under it, anywhere on it, and stays on
        the bar bent, 0.4 mm off where its hinge's end has it rigid: the
        contact is kept through that, its force never more than the blow's.
        The strip is touched on its mesh's surface, which goes with its
        bending. Its shapes riding its first interface rigid, the contact was
        lost as the strip bent and found again 0.18 mm in: 62 kN on 21 g,
        where the blow is 340 N"""
        import CommandCreateContact

        mount = self.box("Mount", (10, 20, 10), (0, 0, -4))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedMount")
        JointObject.GroundedJoint(ground, mount)
        strip = self.box("Strip", (200, 20, 2), (10, 0, 0))
        bar = self.assembly.newObject("Part::Cylinder", "Bar")
        bar.Radius = 5
        bar.Height = 30
        bar.Placement = plc(130, -5, -15, App.Vector(1, 0, 0), -90)
        held = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBar")
        JointObject.GroundedJoint(held, bar)
        self.doc.recompute()
        hinge = self.joints.newObject("App::FeaturePython", "Revolute")
        JointObject.Joint(hinge, JointObject.JointTypes.index("Revolute"))
        hinge.Detach1 = True
        hinge.Detach2 = True
        hinge.Reference1 = (mount, ["Face2"])
        hinge.Reference2 = (strip, ["Face1"])
        hinge.Placement1 = plc(10, 10, 5, App.Vector(1, 0, 0), -90)
        hinge.Placement2 = plc(0, 10, 1, App.Vector(1, 0, 0), -90)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        contact = CommandCreateContact.newContactSet(self.assembly, [strip], [bar])
        sim = self.study(0.051, 2e-4)
        sim.Elements = [contact]
        body = FlexibleBodies.makeFlexibleBody(self.assembly, strip)
        body.MeshSize = 8.0
        FlexibleBodies.prepare(body)
        self.assertNotIn(FlexibleBodies.CONTACT, body.InterfaceFrames)
        self.doc.recompute()

        class Warnings:
            def __init__(self):
                self.messages = []

            def __call__(self, notifier, msg, level):
                if level == "Warning":
                    self.messages.append(msg)

        warnings = Warnings()
        App.Console.AttachObserver(warnings)
        try:
            self.assertEqual(self.assembly.generateSimulation(sim), 0)
        finally:
            App.Console.DetachObserver(warnings)
        # Touched on its mesh (the run says when a part has none)
        self.assertFalse(any("no mesh of its surface" in m for m in warnings.messages), warnings.messages)
        results = self.assembly.getSimulationResults()
        times = results["Times"]
        normal = [c["Values"] for c in results["Channels"] if c["Quantity"] == "ContactNormalForce"]
        forces = [v.z for v in normal[0]]
        hit = next(k for k, f in enumerate(forces) if f > 0)
        blow = max(f for t, f in zip(times, forces) if t - times[hit] < 5e-5)
        after = [f for t, f in zip(times, forces) if t - times[hit] > 1e-3]
        print("bent onto the bar: first touch %.6f s, the blow %.4g mN, after it at most %.4g, "
              "pressing in %d of %d frames" % (
                  times[hit], blow, max(after), sum(1 for f in after if f > 0), len(after)))
        self.assertGreater(blow, 0.0)
        self.assertLessEqual(max(forces), blow)
        # On the bar still a millisecond and more after, pressing lightly
        self.assertGreater(sum(1 for f in after if f > 0), len(after) // 2)
        self.assertLess(max(after), 0.1 * blow)

    def stripOnBars(self, formula, end, step, bars=True, gap=0.05, rise=1.0, lower=False):
        """A strip 200 x 20 x 2 hinged at an end to a grounded mount, its
        hinge turned by a Position motion (formula, rad), without gravity.
        lower: in place of the bars a second strip 20 x 100 x 2 across under
        it 110 to 130 mm from the hinge, gap under it, clamped at its far end
        by a fixed joint to a grounded block, flexible and touched anywhere
        too, their contact without friction.
        With bars, a grounded bar across it 120 mm from the hinge, gap under
        it, and another 195 mm out, rise over it, each in a contact set of
        its own with the strip, flexible and touched anywhere; without, the
        strip rigid and alone. The contacts are springs of 1000 N/mm, not the
        materials': under the first bar's 18 N the strip sinks 0.02 mm where
        it is bent 7 mm, and the run steps a fiftieth of that spring's 0.9 ms
        under the strip while the bars are loaded, not of the materials'
        0.07 ms. The strip, its flexible body, the two contact sets and the
        run's status"""
        import CommandCreateContact

        mount = self.box("Mount", (10, 20, 10), (0, 0, -4))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedMount")
        JointObject.GroundedJoint(ground, mount)
        strip = self.box("Strip", (200, 20, 2), (10, 0, 0))
        placed = []
        if lower:
            across = self.box("Lower", (20, 100, 2), (120, -80, -2 - gap))
            block = self.box("Block", (20, 10, 10), (120, -90, -6 - gap))
            held = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBlock")
            JointObject.GroundedJoint(held, block)
            self.doc.recompute()
            at = plc(130, -80, -1 - gap)
            clamp = self.joints.newObject("App::FeaturePython", "Fixed")
            JointObject.Joint(clamp, JointObject.JointTypes.index("Fixed"))
            clamp.Detach1 = True
            clamp.Detach2 = True
            clamp.Reference1 = (block, ["Face4"])
            clamp.Reference2 = (across, ["Face3"])
            clamp.Placement1 = block.Placement.inverse().multiply(at)
            clamp.Placement2 = across.Placement.inverse().multiply(at)
            placed.append(across)
        elif bars:
            for name, x, z in (("Under", 130, -5 - gap), ("Over", 205, 7 + rise)):
                bar = self.assembly.newObject("Part::Cylinder", name)
                bar.Radius = 5
                bar.Height = 30
                bar.Placement = plc(x, -5, z, App.Vector(1, 0, 0), -90)
                held = self.joints.newObject("Assembly::GroundedJointPython", "Grounded" + name)
                JointObject.GroundedJoint(held, bar)
                placed.append(bar)
        self.doc.recompute()
        hinge = self.joints.newObject("App::FeaturePython", "Revolute")
        JointObject.Joint(hinge, JointObject.JointTypes.index("Revolute"))
        hinge.Detach1 = True
        hinge.Detach2 = True
        hinge.Reference1 = (mount, ["Face2"])
        hinge.Reference2 = (strip, ["Face1"])
        hinge.Placement1 = plc(10, 10, 5, App.Vector(1, 0, 0), -90)
        hinge.Placement2 = plc(0, 10, 1, App.Vector(1, 0, 0), -90)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        contacts = [
            CommandCreateContact.newContactSet(self.assembly, [strip], [bar]) for bar in placed
        ]
        for contact in contacts:
            contact.StiffnessFrom = "Value"
            contact.Stiffness = 1000.0
            contact.Exponent = 1.0
            if lower:
                contact.FrictionFrom = "Value"
                contact.StaticFriction = 0.0
                contact.DynamicFriction = 0.0
        sim = self.study(end, step)
        sim.GravityEnabled = False
        sim.Elements = contacts
        motion = sim.newObject("Assembly::Motion", "Motion")
        motion.Joint = hinge
        motion.MotionType = "Angular"
        motion.Control = "Position"
        motion.Formula = formula
        body = None
        if bars:
            body = FlexibleBodies.makeFlexibleBody(self.assembly, strip)
            body.MeshSize = 8.0
            FlexibleBodies.prepare(body)
            self.assertNotIn(FlexibleBodies.CONTACT, body.InterfaceFrames)
            if lower:
                other = FlexibleBodies.makeFlexibleBody(self.assembly, placed[0])
                other.MeshSize = 8.0
                FlexibleBodies.prepare(other)
        self.doc.recompute()
        return strip, body, contacts, self.assembly.generateSimulation(sim)

    def test_strip_on_flexible_strip(self):
        """Two flexible parts' flat faces meeting at their edges (D4b step
        16): test_bent_between_bars_anywhere's strip turned down onto the
        free end of a second strip across under it, clamped at its far end,
        both flexible and touched anywhere. A point under a flat face is
        under the one, of that face and its neighbours at a convex edge, it
        is least deep under that faces the other part's surface
        (flatFaces()): the strips press each other straight, the force's
        lean the lower strip's own slope. Before, an edge of one in a side
        face of the other was a patch of its own, and the force leaned 25 to
        35 deg along the lower strip"""
        strip, _, _, status = self.stripOnBars("0.1*time", 0.01, 0.005, bars=False)
        self.assertEqual(status, 0)
        self.assembly.updateForFrame(self.assembly.numberOfFrames() - 1)
        down = strip.Placement.multVec(App.Vector(200, 10, 1)).z < 1.0
        self.renew()
        turn, end = 0.06, 0.1
        formula = "%g*(time/%g)*(time/%g)*(3 - 2*time/%g)" % (
            turn if down else -turn,
            end,
            end,
            end,
        )
        # A quarter of the turn: the strips met at 8 ms, 2.8 N between them
        # at 35
        _, _, contacts, status = self.stripOnBars(formula, 0.035, 5e-4, True, lower=True)
        self.assertEqual(status, 0)
        results = self.assembly.getSimulationResults()
        values = [
            c["Values"]
            for c in results["Channels"]
            if c["Quantity"] == "ContactNormalForce" and c["Name"] == contacts[0].Name
        ]
        self.assertEqual(len(values), 1)
        late = [v * 1e-3 for v in values[0][-20:]]
        pressing = [abs(v.z) for v in late]
        leaning = [math.hypot(v.x, v.y) / abs(v.z) for v in late]
        print("strip on a flexible strip: at the end %.4g N, leaning at most %.3g of it"
              % (pressing[-1], max(leaning)))
        self.assertGreater(pressing[-1], 2.3)
        self.assertLess(pressing[-1], 3.3)
        # Rising with the turn, no frame under the one before by a fiftieth
        # of the last
        for before, after in zip(pressing, pressing[1:]):
            self.assertGreater(after, before - 0.02 * pressing[-1])
        self.assertGreater(pressing[-1], pressing[0])
        self.assertLess(max(leaning), 0.05)

    def test_bent_between_bars_anywhere(self):
        """A flexible part is touched where it is, however far it has bent
        from where its first interface has it rigid (D4b step 12). A strip
        200 x 20 x 2 without gravity, its hinge turned 0.06 rad by a motion
        onto a bar across it 120 mm out: bent over the bar its far end
        rises, and 195 mm out meets a second bar 1 mm over it. The strip is
        14 mm there from where its hinge's end has it rigid, and 7 mm at the
        first bar: its shapes riding that end, the second bar was never met
        and the first was 7 mm in. Both bars press as a beam's supports do,
        the beam clamped at the hinge's turn"""
        # Which way the motion turns the hinge: a rigid strip alone
        strip, _, _, status = self.stripOnBars("0.1*time", 0.01, 0.005, bars=False)
        self.assertEqual(status, 0)
        self.assembly.updateForFrame(self.assembly.numberOfFrames() - 1)
        down = strip.Placement.multVec(App.Vector(200, 10, 1)).z < 1.0
        self.renew()
        turn, end, gap, rise = 0.06, 0.1, 0.05, 1.0
        formula = "%g*(time/%g)*(time/%g)*(3 - 2*time/%g)" % (
            turn if down else -turn,
            end,
            end,
            end,
        )
        strip, body, contacts, status = self.stripOnBars(formula, end, 5e-4, True, gap, rise)
        self.assertEqual(status, 0)
        youngs, _ = FlexibleBodies.elasticityOf(body)
        stiff = youngs * 20 * 2**3 / 12.0  # N mm^2
        a, b = 120.0, 195.0

        def supports(angle):
            """The bars' forces (N) at the hinge's turn: a cantilever off the
            turned end, held at a, and at b once it has come there"""
            if angle * a <= gap:
                return 0.0, 0.0
            under = 3.0 * stiff * (angle * a - gap) / a**3
            at = -angle * b + under * a * a * (3 * b - a) / (6.0 * stiff)
            if at <= rise:
                return under, 0.0
            cross = a * a * (3 * b - a) / 6.0
            m = [[a**3 / 3.0, -cross], [cross, -(b**3) / 3.0]]
            r = [stiff * (angle * a - gap), stiff * (rise + angle * b)]
            det = m[0][0] * m[1][1] - m[0][1] * m[1][0]
            return (
                (r[0] * m[1][1] - m[0][1] * r[1]) / det,
                (m[0][0] * r[1] - m[1][0] * r[0]) / det,
            )

        results = self.assembly.getSimulationResults()
        times = results["Times"]
        forces = []
        for contact in contacts:
            values = [
                c["Values"]
                for c in results["Channels"]
                if c["Quantity"] == "ContactNormalForce" and c["Name"] == contact.Name
            ]
            self.assertEqual(len(values), 1)
            forces.append([abs(v.z) * 1e-3 for v in values[0]])
        under, over = forces
        angles = [turn * (t / end) ** 2 * (3 - 2 * t / end) for t in times]
        want = [supports(angle) for angle in angles]
        due = next(k for k, w in enumerate(want) if w[1] > 0)
        met = next((k for k, f in enumerate(over) if f > 0), None)
        self.assertIsNotNone(met, "the second bar is never met")
        late = range(len(times) - len(times) // 5, len(times))

        def mean(values):
            return sum(values[k] for k in late) / len(late)

        got = mean(under), mean(over)
        beam = mean([w[0] for w in want]), mean([w[1] for w in want])
        print("bent between bars: the second met at %.4g rad for %.4g; in the last fifth the "
              "first presses %.4g N for %.4g, the second %.4g N for %.4g; at most %.4g and "
              "%.4g N" % (angles[met], angles[due], got[0], beam[0], got[1], beam[1],
                          max(under), max(over)))
        self.assertAlmostEqual(angles[met], angles[due], delta=0.1 * angles[due])
        self.assertAlmostEqual(got[0], beam[0], delta=0.1 * beam[0])
        self.assertAlmostEqual(got[1], beam[1], delta=0.15 * beam[1])
        # No blow as either is met
        self.assertLess(max(under), 1.5 * want[-1][0])
        self.assertLess(max(over), 1.5 * want[-1][1])

    def test_round_anywhere(self):
        """A round face of a flexible part touched anywhere is its elements'
        quadratic faces, not their flat triangles (D4b step 13). A rod R 5 x
        40 held by a hinge on its axis at an end face, turned one revolution
        in 0.1 s by a motion, a grounded plate under it 0.05 mm into its
        round, the contact 1000 N/mm, no friction or gravity. Its faces at
        a 4 mm mesh stand 0.2 mm out of their flat triangles: on those the
        force ran from 0 to 104 N as they rolled by, the plate lost each
        time. On the faces it held within a tenth over the turn, under the
        50 N of a rigid rod pressed 0.05 mm.

        And the round lying along the plate touches it at two points, at the
        ends of their line on the round itself (D4b step 27,
        FlexibleContact::toFlat()), as it does a round it lies along (step
        25): each face's own farthest towards the plate is off the line
        where the face stands in from its round, and the force went 42.5 to
        46.8 N as the faces came round, a tenth; now within a hundredth,
        44.6 to 44.9"""
        self.roundTurned("a round anywhere", 55, -10, within=0.01)

    def test_round_past_edge(self):
        """A flexible part running on past the edge of a box it presses into
        (D4b step 14): the rod of test_round_anywhere on a plate from x = 5
        to 35, its ends under the rod. Bullet took the plate's end faces for
        points of the rod 0.036 mm under its top, 0.124 mm in along the rod,
        and the force ran from 35 to 126 N; each such point is under the face
        it is least deep under that faces the rod's surface (leastDeepFace()),
        and it holds as on the whole plate: within a hundredth (D4b step 27),
        the line's ends where the plate's face ends under the rod
        (reachOver()), not where the rod's faces had points found -- 45.5 to
        45.7 N for 42.0 to 47.3"""
        self.roundTurned("a round past an edge", 30, 5, within=0.01)

    def test_round_on_roller(self):
        """A flexible part's round on a rigid round (D4b step 15): the rod of
        test_round_anywhere on a grounded roller R 5 along it, 0.05 mm into
        its round. Each point goes to its face's point nearest the roller's
        axis, across into the next face where that is beyond its own
        (deepestOnFace()); on the quarter triangles' normals the force ran
        from 32 to 46 N as the rod turned, its run 157 s for 14"""
        self.roundTurned("a round on a roller", 55, -10, roller=True)

    def test_round_on_flexible_plate(self):
        """Two flexible parts touching anywhere (D4b step 16): the rod of
        test_round_anywhere on the plate made flexible too, held all along
        its underside. Each point between the rod's curved face and the
        plate's flat one takes the plate's normal and goes to the rod's
        face's deepest (flatFaces(), FlexibleContact::place()), and a
        triangle gone through the other's surface is not apart from it
        (satApart()): before, the rod's triangles along its line gave no
        points, the force ran from 20 to 44 N and the run took 826 s.

        And the rod touches the flexible plate at the two ends of their
        line (D4b step 28), as it does a rigid one (step 27), the plate's
        point under each on its own surface there
        (FlexibleContact::under()): the force within a hundredth, 44.6 to
        44.9 N as on the rigid plate, for 43.8 to 47.2"""
        import time

        start = time.perf_counter()
        self.roundTurned("a round on a flexible plate", 55, -10, flexiblePlate=True, within=0.01)
        self.assertLess(time.perf_counter() - start, 120.0)

    def test_round_past_flexible_edge(self):
        """A flexible part past the edge of another's flat face (D4b step
        16): test_round_past_edge's plate, its ends under the rod, flexible
        too. A node of the rod just past the plate's end, under its top, is
        under the top, not through the end (flatFaces()): taken through the
        end it was a patch of its own, 40 N along the rod, and the force
        over the turn ran from 35 to 130 N.

        The line's ends are where the plate's flat triangles in the
        contact's plane end under the rod (D4b step 28,
        FlexibleContact::reachOnFlat()): the force within a hundredth, 45.5
        to 45.7 N as on the rigid plate, for 39.5 to 47.3. By the plane of
        the triangle a point was found on, which past the plate's end is
        the end's, it went from 28.3 to 49.4 N"""
        self.roundTurned(
            "a round past a flexible edge", 30, 5, flexiblePlate=True, within=0.01
        )

    def test_round_on_flexible_roller(self):
        """Two flexible parts' rounds on each other (D4b step 16):
        test_round_on_roller's roller flexible too, held at both its ends.
        The rod's face is of its round, the round where the rod's mesh has
        it (FlexibleContact::roundAt()), and the roller's point goes to its
        face's nearest that round's axis as against a rigid one; on their
        quarter triangles the run did not end in 18 minutes. Over 0.03 to
        0.045 s of the turn, a quarter of it.

        The two rounds along each other touch at the two ends of their line
        (D4b way 1 step 2, thinOut()): the roller's points slid into the
        rod's round are one line by which of the rod's rounds it is, not by
        each point's fit of it, and the rod's points slid the other way go.
        The force within a two-hundredth, 44.52 to 44.65 N; as found, 44.73
        to 45.58 over 45.18"""
        self.roundTurned(
            "a round on a flexible roller",
            55,
            -10,
            roller=True,
            flexiblePlate=True,
            until=0.045,
            within=0.005,
        )

    def test_friction_on_flexible_round(self):
        """The friction on a flexible part's round touched anywhere, against
        its closed form (D4b step 17): test_round_anywhere's rod on its
        plate with a friction of 0.3, sliding all the turn -- the motion
        turning it gives 0.3 N R, the rod's radius the friction's arm. Each
        point is slid again to its face's deepest at every update of a
        substep (FlexibleContact::follow()): placed at the substep's start
        alone it was carried round with the rod, w h R past where the rod is
        deepest, and the normal's force turned the rod on -- 0.284 N R at
        steps of 0.2 ms, and -0.0097 N R with no friction, where
        test_round_anywhere now holds it to none. On the line's two ends
        (D4b step 27) the force within a hundredth, the torque 0.298 N R"""
        self.roundTurned("friction on a round", 55, -10, friction=0.3, within=0.01)

    def test_friction_on_roller(self):
        """The same on a rigid roller (D4b step 17), the points found again
        on their faces by Newton's steps with the faces' curve
        (deepestOnFace()): Gauss and Newton's alone left them a substep's
        turn either side of the line, the torque 0.310 N R"""
        self.roundTurned("friction on a roller", 55, -10, roller=True, friction=0.3)

    def test_friction_between_flexible_parts(self):
        """The friction between two flexible parts, against its closed form
        (D4b step 17): the rod on the plate flexible too, a friction of 0.3
        -- 0.282 N R before. On the line's two ends (D4b step 28) the force
        within a hundredth"""
        self.roundTurned(
            "friction between flexible parts",
            55,
            -10,
            flexiblePlate=True,
            friction=0.3,
            within=0.01,
        )

    def test_cone_on_plate(self):
        """A flexible part's round of no cylinder lying along a plate (D4b way
        1 step 3): test_round_anywhere's rod a cone, R 5 at its held end and
        4 at the other, its hinge tilted so its side lies along the plate,
        0.05 mm into it, a friction of 0.3. A face of no round had its
        points where collision detection found them, each at its own face's
        farthest towards the plate, which stands in from the cone between its
        nodes: the force ran from 41.9 to 46.5 N over the turn. Now the line
        the two touch along by their curvature (LocalSurface::contactOf()),
        its two ends on the cone's true surface (FlexibleContact::toBent()),
        the solid's own nine more points of each face
        (Flexible::trueFaces): within a hundredth, 43.88 to 44.16, and the
        motion's torque the friction's on the cone's middle radius, 4.5"""
        self.roundTurned(
            "a cone on a plate", 55, -10, cone=4.0, friction=0.3, arm=4.5, within=0.01
        )

    def test_cone_on_flexible_plate(self):
        """The same on the plate made flexible too (D4b way 1 step 3), the
        plate's point under each of the line's ends on its surface there
        (FlexibleContact::under()): within a hundredth, 43.89 to 44.17 N,
        for 42.59 to 46.80"""
        self.roundTurned(
            "a cone on a flexible plate",
            55,
            -10,
            cone=4.0,
            arm=4.5,
            flexiblePlate=True,
            within=0.01,
        )

    def test_rigid_cone_on_plate(self):
        """A rigid part's round of no cylinder lying along a plate (D4b way 1
        step 5): test_cone_on_plate's cone rigid, a friction of 0.3. Its
        contact shape is a hull, its facets a deflection in from the cone,
        and Bullet's points on them: the force ran from 44.3 to 50.0 N over
        the turn, for the 50 of 0.05 mm on the springs. Each point now on
        the cone's surface as its solid has it, the hull's triangle curved by
        its corners' normals, at the surface's farthest towards the plate in
        the plane across its side (hullFarthest()): 49.99 to 50.00 N, the
        motion's torque the friction's on the cone's middle radius"""
        self.roundTurned(
            "a rigid cone on a plate",
            55,
            -10,
            cone=4.0,
            rigidRod=True,
            friction=0.3,
            arm=4.5,
            within=0.001,
            most=50.5,
        )

    def test_pressure_on_a_rigid_round(self):
        """A pressure over the area (D4b way 2): the contact's stiffness 25
        N/mm^3 a pressure per depth, summed over where the surfaces overlap.
        A rigid cylinder R 5 x 40 lying along a plate 0.05 mm into it: across
        the line the overlap is a parabola's, 4/3 sqrt(2 R) d^1.5 a length,
        and the force k L 4/3 sqrt(2 R) d^1.5, 47.14 N, over the turn; a
        rigid ball R 5 pressed 0.05 mm with 1000 N/mm^3, a disc's, k pi R d^2,
        39.27 N"""
        rod = self.roundTurned(
            "a pressure on a rigid round", 55, -10, rigidRod=True, pressure=25.0, within=0.001
        )
        line = 25.0 * 40.0 * 4.0 / 3.0 * math.sqrt(2 * 5.0) * 0.05**1.5
        self.assertLess(abs(rod - line), 0.005 * line)
        self.renew()
        ball = self.roundTurned(
            "a pressure on a rigid ball",
            55,
            -10,
            rigidRod=True,
            ball=True,
            pressure=1000.0,
            within=0.001,
        )
        disc = 1000.0 * math.pi * 5.0 * 0.05**2
        self.assertLess(abs(ball - disc), 0.005 * disc)

    def test_pressure_on_a_cone_and_a_flexible_round(self):
        """A pressure over the area on a round whose radius changes along
        its line, and on a flexible part (D4b way 2). The rigid cone of
        test_rigid_cone_on_plate, 25 N/mm^3: across its side sqrt(2 r) runs
        from R 5 to 4 along it, k 4/3 d^1.5 sqrt(2) int sqrt(r) ds, 44.71 N --
        each patch's line its hull's (hullLineReach()), its radius at each end
        and linear between, not the mean of where its points were found,
        which ran it from 42.5 to 45.0. The flexible rod: its curvature its
        faces' true surface's (FlexibleContact::curvatureOf()), the mesh's own
        reading it as a ball at times -- from 1.2 to 60 N; now within a
        hundredth, under the rigid rod's 47.14 by its give"""
        half = math.atan2(1.0, 40.0)
        root = (2.0 / 3.0) * (5.0**1.5 - 4.0**1.5) / (5.0 - 4.0)
        cone = 25.0 * 4.0 / 3.0 * 0.05**1.5 * math.sqrt(2.0) * root * 40.0 / math.cos(half) ** 1.5
        rigid = self.roundTurned(
            "a pressure on a rigid cone",
            55,
            -10,
            cone=4.0,
            rigidRod=True,
            arm=4.5,
            pressure=25.0,
            within=0.01,
        )
        self.assertLess(abs(rigid - cone), 0.01 * cone)
        self.renew()
        flexible = self.roundTurned(
            "a pressure on a flexible round", 55, -10, pressure=25.0, within=0.01
        )
        self.assertLess(flexible, 47.14)

    def test_round_in_bore(self):
        """A flexible part's round in a rigid bore (D4b step 18):
        test_round_anywhere's rod in a grounded block's bore R 5.5, the
        bore's wall 0.05 mm into its underside. A hollow round's deepest is
        the face's point farthest from its axis (deepestOnFace()), and the
        points that all go to the one line are thinned to one a place
        (thinOut()): on its triangles against the bore's, 3500 points, the
        run did not end in 10 minutes. Over 0.03 to 0.045 s of the turn"""
        self.roundTurned("a round in a bore", 55, -10, bore=5.5, until=0.045)

    def test_rigid_round_in_flexible_bore(self):
        """A rigid round in a flexible part's bore (D4b step 18): the rod
        rigid, the block flexible, held all along its underside, a friction
        of 0.3. The block gives a tenth of a micron under it, and the rod
        presses it the two rigid parts' 50 N, the motion's torque 0.3 N R:
        with the wall's points at the rod's ends left on their flat
        triangles, which stand in from a hollow face, and its strays off the
        line among the 900 on it, 47.6 N in 430 s"""
        self.roundTurned(
            "a rigid round in a flexible bore",
            55,
            -10,
            bore=5.5,
            flexiblePlate=True,
            rigidRod=True,
            friction=0.3,
            most=50.5,
        )

    def roundTurned(
        self,
        name,
        length,
        start,
        roller=False,
        flexiblePlate=False,
        within=0.1,
        until=0.1,
        friction=0.0,
        bore=None,
        rigidRod=False,
        most=50.0,
        cone=None,
        arm=5.0,
        ball=False,
        pressure=None,
    ):
        """test_round_anywhere's rod turned on a plate 'length' long from x =
        'start', or on a roller R 5 along it that long: the force within a
        tenth ('within') of its mean over the turn, the mean under a rigid
        rod's 50 N; and the motion's torque the friction's, 'friction' N R,
        to 0.006 N R -- none without friction. flexiblePlate: the plate
        flexible too, held all along its underside by a fixed joint to a
        grounded base -- the roller, at both its ends by fixed joints to
        grounded blocks. bore: a block 'length' x 20 x 20 about the rod
        instead, a bore of that radius through it along the rod, its wall
        0.05 mm into the rod's underside. rigidRod: the rod rigid. most: what
        the mean stays under. until: the run's end, of the turn's 0.1 s.
        cone: the rod a cone instead, R 5 at its held end and 'cone' at the
        other, its hinge's axis tilted so the cone lies along the plate, the
        plate 0.05 mm into it there; a round of no cylinder. arm: the
        friction's on the rod's axis, its radius where the force is. ball:
        the rod a ball R 5 instead, turned about its centre. pressure: the
        contact's law a pressure over the area (D4b way 2), N/mm^3. Returns
        the mean force over the turn"""
        import CommandCreateContact

        mount = self.box("Mount", (10, 20, 20), (-10, -10, -10))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedMount")
        JointObject.GroundedJoint(ground, mount)
        tilt = 0.0
        top = -5.0
        if ball:
            rod = self.assembly.newObject("Part::Sphere", "Rod")
            rod.Radius = 5
        elif cone:
            rod = self.assembly.newObject("Part::Cone", "Rod")
            rod.Radius1 = 5
            rod.Radius2 = cone
            rod.Height = 40
            # The far end down by the cone's angle: the line under it flat
            tilt = math.degrees(math.atan2(5.0 - cone, 40.0))
            top = -5.0 * math.cos(math.radians(tilt))
        else:
            rod = self.assembly.newObject("Part::Cylinder", "Rod")
            rod.Radius = 5
            rod.Height = 40
        rod.Placement = plc(0, 0, 0, App.Vector(0, 1, 0), 90 + tilt)
        low = top - 5.0 + 0.05
        if bore:
            import Part

            low = bore - 5.0 + 0.05 - 10.0
            plate = self.assembly.newObject("Part::Feature", "Block")
            plate.Shape = Part.makeBox(length, 20, 20).cut(
                Part.makeCylinder(bore, length, App.Vector(0, 10, 10), App.Vector(1, 0, 0))
            )
            plate.Placement.Base = App.Vector(start, -10, low)
        elif roller:
            plate = self.assembly.newObject("Part::Cylinder", "Roller")
            plate.Radius = 5
            plate.Height = length
            plate.Placement = plc(start, 0, -10 + 0.05, App.Vector(0, 1, 0), 90)
        else:
            plate = self.box("Plate", (length, 20, 5), (start, -10, low))
        if flexiblePlate and roller:
            self.doc.recompute()
            for k, x in enumerate((start, start + length)):
                block = self.box(
                    "Block%d" % k, (10, 20, 20), (x - 10 if k == 0 else x, -10, -20 + 0.05)
                )
                held = self.joints.newObject(
                    "Assembly::GroundedJointPython", "GroundedBlock%d" % k
                )
                JointObject.GroundedJoint(held, block)
                self.doc.recompute()
                face = next(
                    "Face%d" % (i + 1)
                    for i, f in enumerate(plate.Shape.Faces)
                    if f.Surface.__class__.__name__ == "Plane" and abs(f.CenterOfMass.x - x) < 1e-6
                )
                at = plc(x, 0, -10 + 0.05)
                clamp = self.joints.newObject("App::FeaturePython", "Fixed%d" % k)
                JointObject.Joint(clamp, JointObject.JointTypes.index("Fixed"))
                clamp.Detach1 = True
                clamp.Detach2 = True
                clamp.Reference1 = (block, ["Face2" if k == 0 else "Face1"])
                clamp.Reference2 = (plate, [face])
                clamp.Placement1 = block.Placement.inverse().multiply(at)
                clamp.Placement2 = plate.Placement.inverse().multiply(at)
        elif flexiblePlate:
            base = self.box("Base", (length, 20, 5), (start, -10, low - 5))
            held = self.joints.newObject("Assembly::GroundedJointPython", "GroundedBase")
            JointObject.GroundedJoint(held, base)
            self.doc.recompute()
            # The plate's underside, a box's Face5; a block's by where it is
            face = "Face5"
            if bore:
                face = next(
                    "Face%d" % (i + 1)
                    for i, f in enumerate(plate.Shape.Faces)
                    if f.Surface.__class__.__name__ == "Plane"
                    and abs(f.CenterOfMass.z - low) < 1e-6
                )
            under = self.joints.newObject("App::FeaturePython", "Fixed")
            JointObject.Joint(under, JointObject.JointTypes.index("Fixed"))
            under.Detach1 = True
            under.Detach2 = True
            under.Reference1 = (base, ["Face6"])
            under.Reference2 = (plate, [face])
            under.Placement1 = plc(length / 2.0, 10, 5)
            under.Placement2 = plc(length / 2.0, 10, 0)
        else:
            held = self.joints.newObject("Assembly::GroundedJointPython", "GroundedPlate")
            JointObject.GroundedJoint(held, plate)
        self.doc.recompute()
        # Its end face at x = 0; a ball's own, held at its centre
        end = "Face1" if ball else next(
            "Face%d" % (i + 1)
            for i, face in enumerate(rod.Shape.Faces)
            if face.Surface.__class__.__name__ == "Plane" and abs(face.CenterOfMass.x) < 1e-6
        )
        hinge = self.joints.newObject("App::FeaturePython", "Revolute")
        JointObject.Joint(hinge, JointObject.JointTypes.index("Revolute"))
        hinge.Detach1 = True
        hinge.Detach2 = True
        hinge.Reference1 = (mount, ["Face2"])
        hinge.Reference2 = (rod, [end])
        hinge.Placement1 = plc(10, 10, 10, App.Vector(0, 1, 0), 90 + tilt)
        hinge.Placement2 = plc(0, 0, 0)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        contact = CommandCreateContact.newContactSet(self.assembly, [rod], [plate])
        contact.StiffnessFrom = "Pressure" if pressure else "Value"
        contact.Stiffness = pressure if pressure else 1000.0
        contact.Exponent = 1.0
        contact.FrictionFrom = "Value"
        contact.StaticFriction = friction
        contact.DynamicFriction = friction
        sim = self.study(until, 2e-4)
        sim.GravityEnabled = False
        sim.Elements = [contact]
        motion = sim.newObject("Assembly::Motion", "Motion")
        motion.Joint = hinge
        motion.MotionType = "Angular"
        motion.Control = "Position"
        motion.Formula = "%r*(time/0.1)*(time/0.1)*(3 - 2*time/0.1)" % (2 * math.pi)
        if not rigidRod:
            body = FlexibleBodies.makeFlexibleBody(self.assembly, rod)
            body.MeshSize = 4.0
            FlexibleBodies.prepare(body)
        if flexiblePlate:
            other = FlexibleBodies.makeFlexibleBody(self.assembly, plate)
            other.MeshSize = 4.0
            FlexibleBodies.prepare(other)
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        results = self.assembly.getSimulationResults()
        values = [
            c["Values"]
            for c in results["Channels"]
            if c["Quantity"] == "ContactNormalForce" and c["Name"] == contact.Name
        ]
        self.assertEqual(len(values), 1)
        turning = [
            v.Length * 1e-3 for t, v in zip(results["Times"], values[0]) if 0.03 <= t <= until
        ]
        mean = sum(turning) / len(turning)
        print("%s: over the turn %.4g N, from %.4g to %.4g"
              % (name, mean, min(turning), max(turning)))
        self.assertGreater(mean, 35.0)
        self.assertLess(mean, most)
        self.assertGreater(min(turning), (1.0 - within) * mean)
        self.assertLess(max(turning), (1.0 + within) * mean)
        # The motion's torque, N mm: the friction's on the rod's radius
        torques = [c["Values"] for c in results["Channels"] if c["Quantity"] == "MotorTorque"]
        self.assertEqual(len(torques), 1)
        turned = [v * 1e-3 for t, v in zip(results["Times"], torques[0]) if 0.03 <= t <= until]
        torque = sum(turned) / len(turned)
        print("%s: the motion's torque %.4g N mm, %.4f N R for a friction of %g"
              % (name, torque, torque / (mean * arm), friction))
        self.assertLess(abs(torque - friction * mean * arm), 0.006 * mean * arm)
        return mean

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

    def test_held_by_a_free_part(self):
        """A flexible part held by a part nothing joins to the ground (D4b
        step 19): a hanger 10 x 20 x 80 hung on a grounded pin R 5 by a bore
        R 5.2, a block fixed under it, let down 0.01 mm onto the pin under
        gravity. The joint to the block is in the model only once the
        contact has made the hanger a body, after the flexible body's record
        was: the record had no nodes for it, the hanger was tied to it at
        the one nearest, its first three modes turns about that node at
        0.002 Hz, and it bounced off the pin at 600 N. Its interfaces are
        taken once those joints are in (AssemblyObject::
        flexibleInterfaces()): its first mode is over 1 kHz, and it comes to
        rest on the pin, pressing it its weight and the block's"""
        hanger, block, held, contact = self.hangerOnPin(0.0, 0.0)
        sim = self.study(0.03, 1e-3)
        sim.Elements = [contact]
        body = FlexibleBodies.makeFlexibleBody(self.assembly, hanger)
        body.MeshSize = 4.0
        FlexibleBodies.prepare(body)
        self.assertEqual(list(body.InterfaceFrames), [held.FullName])
        self.doc.recompute()
        hz = self.assembly.getFlexibleFrequencies(sim, body)
        self.assertGreater(hz[0], 1000.0)
        weight = 0.0
        for entry in self.assembly.getMassProperties()["Components"]:
            if entry["Object"].Name in (hanger.Name, block.Name):
                weight += entry["Mass"] * 9.80665
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        results = self.assembly.getSimulationResults()
        values = [
            c["Values"]
            for c in results["Channels"]
            if c["Quantity"] == "ContactNormalForce" and c["Name"] == contact.Name
        ]
        self.assertEqual(len(values), 1)
        forces = [v.Length * 1e-3 for v in values[0]]
        resting = [f for t, f in zip(results["Times"], forces) if t >= 0.02]
        mean = sum(resting) / len(resting)
        print("held by a free part: first mode %.4g Hz; at most %.4g N, resting %.4g N of %.4g"
              % (hz[0], max(forces), mean, weight))
        self.assertLess(max(forces), 10.0)
        self.assertLess(abs(mean - weight), 0.05 * weight)

    def hangerOnPin(self, angle, mu):
        """A hanger 10 x 20 x 80 hung on a grounded pin R 5 along x by a bore
        R 5.2, its wall 0.01 mm over the pin, a block 30 x 30 x 30 fixed
        under it; the two turned 'angle' deg about the pin, and a contact
        between the pin and the hanger, 1000 N/mm, of friction 'mu'. The
        hanger, the block, their joint and the contact"""
        import CommandCreateContact
        import Part

        pin = self.assembly.newObject("Part::Cylinder", "Pin")
        pin.Radius = 5
        pin.Height = 30
        pin.Placement = plc(-15, 0, 0, App.Vector(0, 1, 0), 90)
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedPin")
        JointObject.GroundedJoint(ground, pin)
        hanger = self.assembly.newObject("Part::Feature", "Hanger")
        hanger.Shape = Part.makeBox(10, 20, 80).cut(
            Part.makeCylinder(5.2, 10, App.Vector(0, 10, 68), App.Vector(1, 0, 0))
        )
        # The bore's wall 0.01 mm over the pin's top
        hanger.Placement.Base = App.Vector(-5, -10, -68.19)
        block = self.box("Block", (30, 30, 30), (-15, -15, -98.19))
        self.doc.recompute()
        under = next(
            "Face%d" % (i + 1)
            for i, f in enumerate(hanger.Shape.Faces)
            if f.Surface.__class__.__name__ == "Plane" and abs(f.CenterOfMass.z + 68.19) < 1e-6
        )
        at = plc(0, 0, -68.19)
        held = self.joints.newObject("App::FeaturePython", "Fixed")
        JointObject.Joint(held, JointObject.JointTypes.index("Fixed"))
        held.Detach1 = True
        held.Detach2 = True
        held.Reference1 = (block, ["Face6"])
        held.Reference2 = (hanger, [under])
        held.Placement1 = block.Placement.inverse().multiply(at)
        held.Placement2 = hanger.Placement.inverse().multiply(at)
        if angle:
            turn = App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(1, 0, 0), angle))
            hanger.Placement = turn.multiply(hanger.Placement)
            block.Placement = turn.multiply(block.Placement)
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        contact = CommandCreateContact.newContactSet(self.assembly, [pin], [hanger])
        contact.StiffnessFrom = "Value"
        contact.Stiffness = 1000.0
        contact.Exponent = 1.0
        contact.FrictionFrom = "Value"
        contact.StaticFriction = mu
        contact.DynamicFriction = mu
        return hanger, block, held, contact

    def test_swings_on_a_pin(self):
        """A flexible part swinging on a pin lies along it as a rigid one
        does (D4b step 25): the hanger of test_held_by_a_free_part let go 30
        deg off the vertical, a friction of 0.1. The places along the bore's
        line on the pin were where collision detection found them, two at
        one end and five at the other, each put where its faces are deepest
        in the pin -- which a bore 0.2 mm over its pin is within a micron of
        for 6 deg either side of the line. The bore rocked on the pin by
        2.5 mrad and the pin's force stepped 0.11 N rms from frame to frame.
        Two points now, at the ends of the line the two rounds touch along
        (FlexibleContact::toLine()): the bore within 0.1 mrad of the pin,
        the force's steps under 0.03 N rms -- a rigid hanger's are 0.015 --
        and the force between 0.8 and 1.6 N.

        And the pin holds it what its swing asks (D4b step 26): passing the
        vertical, its weight and m d w^2, d its centre of mass's 63.8 mm
        under the pin. With Chrono's linear inertial term the flexible
        hanger, whose interface the block carries round 68 mm under the
        pin, pressed the pin its own mass by that and w^2 more, 7% over"""
        hanger, block, held, contact = self.hangerOnPin(30.0, 0.1)
        sim = self.study(0.2, 1e-3)
        sim.Elements = [contact]
        body = FlexibleBodies.makeFlexibleBody(self.assembly, hanger)
        body.MeshSize = 4.0
        FlexibleBodies.prepare(body)
        self.doc.recompute()
        mass = 0.0
        for entry in self.assembly.getMassProperties()["Components"]:
            if entry["Object"].Name in (hanger.Name, block.Name):
                mass += entry["Mass"]
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        results = self.assembly.getSimulationResults()
        times = results["Times"]
        values = [
            c["Values"]
            for c in results["Channels"]
            if c["Quantity"] == "ContactNormalForce" and c["Name"] == contact.Name
        ]
        self.assertEqual(len(values), 1)
        forces = [v.Length * 1e-3 for v in values[0]]
        late = [k for k, t in enumerate(times) if t >= 0.02]
        steps = [forces[k] - forces[k - 1] for k in late[1:]]
        rough = math.sqrt(sum(d * d for d in steps) / len(steps))
        # The bore's axis, the hanger's x, off the pin's
        off = 0.0
        swung = []
        for k in late:
            self.assembly.updateForFrame(k)
            along = hanger.Placement.Rotation.multVec(App.Vector(1, 0, 0))
            off = max(off, math.hypot(along.y, along.z))
            centre = block.Placement.multVec(App.Vector(15, 15, 15))
            swung.append(math.degrees(math.atan2(centre.y, -centre.z)))
        print("swings on a pin: %d frames, from %.3g to %.3g deg; the pin's force %.4g to %.4g N, "
              "%.4g N rms from frame to frame; the bore off the pin's axis by %.3g urad at most"
              % (len(times), swung[0], swung[-1], min(forces[k] for k in late),
                 max(forces[k] for k in late), rough, off * 1e6))
        # Passing the vertical: the frames either side of it 4 ms apart
        low = min(range(len(late)), key=lambda j: abs(swung[j]))
        self.assertTrue(4 <= low < len(late) - 4)
        before = low
        while times[late[low]] - times[late[before]] < 0.002:
            before -= 1
        after = low
        while times[late[after]] - times[late[low]] < 0.002:
            after += 1
        turning = math.radians(swung[after] - swung[before]) / (
            times[late[after]] - times[late[before]]
        )
        asked = mass * (
            9.80665 * math.cos(math.radians(swung[low])) + 63.79e-3 * turning * turning
        )
        print("swings on a pin: passing the vertical at %.4g s, %.4g rad/s: %.4g N for the %.4g "
              "its motion asks" % (times[late[low]], turning, forces[late[low]], asked))
        self.assertLess(abs(forces[late[low]] - asked), 0.03 * asked)
        self.assertLess(swung[-1], -15.0)
        self.assertLess(rough, 0.03)
        self.assertLess(off, 1e-4)
        self.assertGreater(min(forces[k] for k in late), 0.8)
        self.assertLess(max(forces[k] for k in late), 1.6)

    def test_held_by_nothing(self):
        """A flexible part no joint holds (D4b step 21): a bar 80 x 10 x 4
        across two grounded rollers R 5, 60 mm apart, 0.01 mm over them,
        let fall under gravity and touched anywhere. It was refused, having
        no interface to be reduced about; it has one of its own now, the
        element at its mesh's middle (ChronoFlexible::ownInterface()), in
        the run and in Find Modes. It comes to rest on the rollers pressing
        them its weight, sagging between them as a beam on two supports with
        its ends hanging over does. Find Modes shows what it rings at free
        (step 24), a free beam's first bending, not its modes held at that
        element"""
        import CommandCreateContact
        import numpy as np

        rollers = []
        for i, x in enumerate((-30.0, 30.0)):
            roller = self.assembly.newObject("Part::Cylinder", "Roller%d" % i)
            roller.Radius = 5
            roller.Height = 30
            roller.Placement = plc(x, 15, 0, App.Vector(1, 0, 0), 90)
            ground = self.joints.newObject("Assembly::GroundedJointPython", "Grounded%d" % i)
            JointObject.GroundedJoint(ground, roller)
            rollers.append(roller)
        bar = self.box("Bar", (80, 10, 4), (-40, -5, 5.01))
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        contact = CommandCreateContact.newContactSet(self.assembly, rollers, [bar])
        contact.StiffnessFrom = "Value"
        contact.Stiffness = 1000.0
        contact.Exponent = 1.0
        contact.FrictionFrom = "Value"
        contact.StaticFriction = 0.1
        contact.DynamicFriction = 0.1
        sim = self.study(0.05, 1e-3)
        sim.Elements = [contact]
        body = FlexibleBodies.makeFlexibleBody(self.assembly, bar)
        body.MeshSize = 4.0
        FlexibleBodies.prepare(body)
        self.assertEqual(list(body.InterfaceFrames), [])
        self.doc.recompute()
        hz = self.assembly.getFlexibleFrequencies(sim, body)
        weight = 0.0
        for entry in self.assembly.getMassProperties()["Components"]:
            if entry["Object"].Name == bar.Name:
                weight += entry["Mass"] * 9.80665
        # A free beam's first bending, 22.3733 / (2 pi) sqrt(E I / (m L^3))
        youngs = FlexibleBodies.elasticityOf(body)[0]
        ringing = (22.3733 / (2 * math.pi)) * math.sqrt(
            1e3 * youngs * (640.0 / 12.0) / ((weight / 9.80665) * 80.0**3)
        )
        self.assertEqual(len(hz), body.Modes)
        self.assertLess(abs(hz[0] - ringing), 0.01 * ringing)
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        results = self.assembly.getSimulationResults()
        times = results["Times"]
        values = [
            c["Values"]
            for c in results["Channels"]
            if c["Quantity"] == "ContactNormalForce" and c["Name"] == contact.Name
        ]
        self.assertEqual(len(values), 1)
        forces = [v.Length * 1e-3 for v in values[0]]
        resting = [f for t, f in zip(times, forces) if t >= 0.03]
        mean = sum(resting) / len(resting)
        # Its sag: its underside at midspan against its underside over the
        # rollers, by its deformation; the beam's w L^2 (5 L^2 - 24 a^2) /
        # (384 E I), L 60 between the supports and a 10 over each
        deformation = FlexibleBodies.Deformation(body, results)
        self.assertTrue(deformation.valid)
        moved = deformation.psi @ deformation.coordinatesAt(times[-1])
        rest = deformation.rest
        low = np.abs(rest[:, 2]) < 1e-6
        mid = low & (np.abs(rest[:, 0] - 40.0) < 2.5)
        over = low & ((np.abs(rest[:, 0] - 10.0) < 2.5) | (np.abs(rest[:, 0] - 70.0) < 2.5))
        sag = moved[over, 2].mean() - moved[mid, 2].mean()
        beam = (weight / 80.0) * 3600.0 * (5 * 3600.0 - 2400.0) / (384.0 * youngs * 640.0 / 12.0)
        print("held by nothing: first mode %.5g Hz of a free beam's %.5g; at most %.4g N, "
              "resting %.5g N of %.5g; sag %.4g um of a beam's %.4g"
              % (hz[0], ringing, max(forces), mean, weight, sag * 1e3, beam * 1e3))
        self.assertLess(max(forces), 3.0)
        self.assertLess(abs(mean - weight), 0.01 * weight)
        self.assertLess(abs(sag - beam), 0.03 * beam)

    def test_sticks_on_a_slope(self):
        """A flexible part sticks, and slides, as a rigid one does (D4b step
        23): a bar 40 x 10 x 4 no joint holds, lying on a grounded plate
        tilted 5 deg, touched anywhere. tan 5 deg is 0.0875: with a friction
        of 0.2 the plate holds it, the friction its weight's part down the
        slope, and it creeps by the friction's slip alone; with 0.05 it
        slides, the friction mu N, gaining g (sin a - mu cos a)"""
        import CommandCreateContact

        angle = math.radians(5.0)
        tilt = App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(0, 1, 0), 5.0))
        plate = self.box("Plate", (160, 40, 5), (0, 0, 0))
        plate.Placement = tilt.multiply(App.Placement(App.Vector(-80, -20, -5), App.Rotation()))
        ground = self.joints.newObject("Assembly::GroundedJointPython", "GroundedPlate")
        JointObject.GroundedJoint(ground, plate)
        bar = self.box("Bar", (40, 10, 4), (0, 0, 0))
        bar.Placement = tilt.multiply(App.Placement(App.Vector(-60, -5, 0.01), App.Rotation()))
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)
        contact = CommandCreateContact.newContactSet(self.assembly, [plate], [bar])
        contact.StiffnessFrom = "Value"
        contact.Stiffness = 1000.0
        contact.Exponent = 1.0
        contact.FrictionFrom = "Value"
        sim = self.study(0.1, 1e-3)
        sim.Elements = [contact]
        body = FlexibleBodies.makeFlexibleBody(self.assembly, bar)
        body.MeshSize = 4.0
        FlexibleBodies.prepare(body)
        self.doc.recompute()
        weight = 0.0
        for entry in self.assembly.getMassProperties()["Components"]:
            if entry["Object"].Name == bar.Name:
                weight += entry["Mass"] * 9.80665
        down = tilt.Rotation.multVec(App.Vector(1, 0, 0))

        def run(mu):
            # The forces' means over the last half, and the bar's speed down
            # the slope at 0.05 s and 0.099 s
            contact.StaticFriction = mu
            contact.DynamicFriction = mu
            self.doc.recompute()
            self.assertEqual(self.assembly.generateSimulation(sim), 0)
            results = self.assembly.getSimulationResults()
            times = results["Times"]

            def channel(quantity):
                values = [
                    c["Values"]
                    for c in results["Channels"]
                    if c["Quantity"] == quantity and c["Name"] == contact.Name
                ]
                self.assertEqual(len(values), 1)
                return [v.Length * 1e-3 for v in values[0]]

            normal = channel("ContactNormalForce")
            friction = channel("ContactFrictionForce")
            late = [k for k, t in enumerate(times) if t >= 0.05]
            along = []
            for k in range(len(times)):
                self.assembly.updateForFrame(k)
                along.append(bar.Placement.multVec(App.Vector(20, 5, 2)).dot(down))
            k0, k1 = late[0], late[-1]
            v0 = (along[k0 + 1] - along[k0 - 1]) / (times[k0 + 1] - times[k0 - 1])
            v1 = (along[k1] - along[k1 - 2]) / (times[k1] - times[k1 - 2])
            gain = (v1 - v0) / (times[k1 - 1] - times[k0])
            return (
                sum(normal[k] for k in late) / len(late),
                sum(friction[k] for k in late) / len(late),
                v1,
                gain,
            )

        held = run(0.2)
        slid = run(0.05)
        free = 9806.65 * (math.sin(angle) - 0.05 * math.cos(angle))
        print("sticks on a slope: mu 0.2, N %.5g of %.5g, F %.5g of %.5g, creeping %.4g mm/s; "
              "mu 0.05, F %.5g of %.5g, gaining %.5g mm/s^2 of %.5g"
              % (held[0], weight * math.cos(angle), held[1], weight * math.sin(angle), held[2],
                 slid[1], 0.05 * weight * math.cos(angle), slid[3], free))
        self.assertLess(abs(held[0] - weight * math.cos(angle)), 0.01 * weight)
        self.assertLess(abs(held[1] - weight * math.sin(angle)), 0.01 * weight * math.sin(angle))
        self.assertLess(abs(held[2]), 0.1)
        self.assertLess(abs(held[3]), 1.0)
        self.assertLess(abs(slid[1] - 0.05 * slid[0]), 0.01 * 0.05 * slid[0])
        self.assertLess(abs(slid[3] - free), 0.01 * free)

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
