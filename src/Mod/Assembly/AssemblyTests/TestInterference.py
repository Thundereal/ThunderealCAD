# SPDX-License-Identifier: LGPL-2.1-or-later
"""The interference check (docs/AssemblyInterference.md): AssemblyObject.
findInterferences in both modes, on a hand-made assembly whose answers are
known -- an overlap of 200 mm^3, a box inside another, two bars crossing,
faces in contact, a pin snug in its hole, a gap of 0.05 mm.
"""

import unittest

import FreeCAD as App
import JointObject
import Part

from AssemblyTests.TestCamJoint import CamMechanism


class TestInterference(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestInterference")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")

        def box(name, l, w, h, x, y, z):
            b = self.assembly.newObject("Part::Box", name)
            b.Length, b.Width, b.Height = l, w, h
            b.Placement.Base = App.Vector(x, y, z)
            return b

        self.A = box("A", 10, 10, 10, 0, 0, 0)
        self.B = box("B", 10, 10, 10, 8, 0, 0)  # into A by 2: 200 mm^3
        self.C = box("C", 10, 10, 10, 0, 10, 0)  # on A's face, on B's
        self.D = box("D", 10, 10, 10, 100, 0, 0)  # far from all
        self.Big = box("Big", 30, 30, 30, 200, 0, 0)
        self.Small = box("Small", 2, 2, 2, 210, 10, 10)  # inside Big: 8 mm^3
        self.Plate = self.assembly.newObject("Part::Feature", "Plate")
        self.Plate.Shape = Part.makeBox(20, 20, 4, App.Vector(300, 0, 0)).cut(
            Part.makeCylinder(3, 4, App.Vector(310, 10, 0))
        )
        self.Pin = self.assembly.newObject("Part::Cylinder", "Pin")  # snug in the hole
        self.Pin.Radius, self.Pin.Height = 3, 10
        self.Pin.Placement.Base = App.Vector(310, 10, -3)
        # Two bars crossing: 8 mm^3 in common, no vertex of either in the other
        self.Bar1 = box("Bar1", 40, 2, 2, 400, 19, 0)
        self.Bar2 = box("Bar2", 2, 40, 2, 419, 0, 0)
        self.Gap1 = box("Gap1", 10, 10, 10, 500, 0, 0)
        self.Gap2 = box("Gap2", 10, 10, 10, 510.05, 0, 0)  # 0.05 mm away
        # No surfaces crossing: two copies in one place, and a box in another
        # on five of its faces
        self.Twin1 = box("Twin1", 10, 10, 10, 600, 0, 0)
        self.Twin2 = box("Twin2", 10, 10, 10, 600, 0, 0)
        self.Outer = box("Outer", 20, 10, 10, 700, 0, 0)
        self.Inner = box("Inner", 10, 10, 10, 700, 0, 0)
        # Two filleted plates of one thickness, into each other by 2 mm edge
        # to edge: coplanar faces and tangent blends, no surfaces crossing
        plate = Part.makeBox(20, 20, 6)
        plate = plate.makeFillet(1.0, plate.Edges)
        for name, x in (("Strip1", 800), ("Strip2", 818)):
            strip = self.assembly.newObject("Part::Feature", name)
            strip.Shape = plate
            strip.Placement.Base = App.Vector(x, 0, 0)
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    INTERFERING = [
        ("A", "B"),
        ("Bar1", "Bar2"),
        ("Big", "Small"),
        ("Inner", "Outer"),
        ("Strip1", "Strip2"),
        ("Twin1", "Twin2"),
    ]

    def pairs(self, results, kind):
        return sorted(
            tuple(sorted((r["Object1"].Name, r["Object2"].Name)))
            for r in results
            if r["Kind"] == kind
        )

    def test_exact(self):
        """The volumes in common, and the solid of it; contacts when asked."""
        results = self.assembly.findInterferences(mode="Exact", tolerance=0.01)
        self.assertEqual(self.pairs(results, "Interference"), self.INTERFERING)
        self.assertEqual(self.pairs(results, "Contact"), [])
        volumes = {tuple(sorted((r["Object1"].Name, r["Object2"].Name))): r for r in results}
        self.assertAlmostEqual(volumes[("A", "B")]["Volume"], 200.0, places=6)
        self.assertAlmostEqual(volumes[("Big", "Small")]["Volume"], 8.0, places=6)
        self.assertAlmostEqual(volumes[("Bar1", "Bar2")]["Volume"], 8.0, places=6)
        self.assertAlmostEqual(volumes[("Twin1", "Twin2")]["Volume"], 1000.0, places=6)
        self.assertAlmostEqual(volumes[("Inner", "Outer")]["Volume"], 1000.0, places=6)
        overlap = volumes[("A", "B")]["Overlap"]
        self.assertIsNotNone(overlap)
        self.assertAlmostEqual(overlap.Volume, 200.0, places=6)
        self.assertAlmostEqual(overlap.BoundBox.XMin, 8.0, places=6)

        results = self.assembly.findInterferences(mode="Exact", tolerance=0.01, contacts=True)
        self.assertEqual(
            self.pairs(results, "Contact"), [("A", "C"), ("B", "C"), ("Pin", "Plate")]
        )
        for r in results:
            if r["Kind"] == "Contact":
                self.assertLess(r["Distance"], 1e-6)

    def test_fast(self):
        """The same pairs as Exact, on meshes; Fast says that they overlap."""
        results = self.assembly.findInterferences(mode="Fast", tolerance=0.01, contacts=True)
        self.assertEqual(self.pairs(results, "Interference"), self.INTERFERING)
        self.assertEqual(
            self.pairs(results, "Contact"), [("A", "C"), ("B", "C"), ("Pin", "Plate")]
        )
        for r in results:
            self.assertEqual(r["Volume"], 0.0)
            self.assertIsNone(r["Overlap"])
            self.assertGreaterEqual(r["Tolerance"], 0.01)

    def test_tolerance(self):
        """A gap within the tolerance is a contact, beyond it nothing."""
        for mode in ("Exact", "Fast"):
            gap = [self.Gap1, self.Gap2]
            near = self.assembly.findInterferences(
                mode=mode, tolerance=0.1, contacts=True, objects=gap, deflection=0.001
            )
            self.assertEqual(self.pairs(near, "Contact"), [("Gap1", "Gap2")], mode)
            far = self.assembly.findInterferences(
                mode=mode, tolerance=0.01, contacts=True, objects=gap, deflection=0.001
            )
            self.assertEqual(far, [], mode)
        exact = self.assembly.findInterferences(
            mode="Exact", tolerance=0.1, contacts=True, objects=[self.Gap1, self.Gap2]
        )
        self.assertAlmostEqual(exact[0]["Distance"], 0.05, places=6)

    def test_objects(self):
        """Only the components given."""
        results = self.assembly.findInterferences(objects=[self.A, self.B, self.D])
        self.assertEqual(self.pairs(results, "Interference"), [("A", "B")])

    def test_link(self):
        """A link is a component, its shape where the link puts it."""
        link = self.assembly.newObject("App::Link", "Copy")
        link.LinkedObject = self.D
        # The link's placement in place of D's: from x = 105, on D by 5
        link.Placement.Base = App.Vector(105, 0, 0)
        self.doc.recompute()
        results = self.assembly.findInterferences(objects=[self.D, link])
        self.assertEqual(len(results), 1)
        self.assertAlmostEqual(results[0]["Volume"], 500.0, places=6)
        results = self.assembly.findInterferences(mode="Fast", objects=[self.D, link])
        self.assertEqual(self.pairs(results, "Interference"), [("Copy", "D")])

    def test_bad_mode(self):
        with self.assertRaises(ValueError):
            self.assembly.findInterferences(mode="Approximate")


class TestInterferenceOverFrames(unittest.TestCase):
    """Over a simulation's frames: a box slides up through a plate, and two
    boxes that do not move overlap all along."""

    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self.doc = App.newDocument("TestInterferenceOverFrames")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")

        def box(name, l, w, h, x, y, z):
            b = self.assembly.newObject("Part::Box", name)
            b.Length, b.Width, b.Height = l, w, h
            b.Placement.Base = App.Vector(x, y, z)
            return b

        self.G = box("G", 10, 10, 10, 0, 0, 0)
        self.S = box("S", 10, 10, 10, 0, 0, 10)  # on G, sliding up
        self.Plate = box("Plate", 20, 20, 5, -5, -5, 25)  # across S's way
        self.P = box("P", 10, 10, 10, 100, 0, 0)
        self.Q = box("Q", 10, 10, 10, 105, 0, 0)  # into P by 5, never moving
        joints = self.assembly.getGroup("Joints", True)
        for obj in (self.G, self.Plate):
            ground = joints.newObject("Assembly::GroundedJointPython", "Grounded" + obj.Name)
            JointObject.GroundedJoint(ground, obj)
        self.doc.recompute()
        slider = self.assembly.makeJoint("Slider", (self.G, "Face6"), (self.S, "Face5"))
        self.doc.recompute()
        sim = CamMechanism.make_simulation(
            self, [(slider, "Linear", "10*time")], t_end=2.0, t_step=0.1
        )
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.frames = self.assembly.numberOfFrames()
        self.assertGreater(self.frames, 20)

    def tearDown(self):
        App.closeDocument(self.doc.Name)
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)

    def expected(self):
        """Each frame's overlap of S and the plate, by playing it back."""
        volumes = []
        for frame in range(self.frames):
            self.assembly.updateForFrame(frame)
            common = self.S.Shape.common(self.Plate.Shape)
            volumes.append(common.Volume if common.Solids else 0.0)
        self.assembly.updateForFrame(0)
        return volumes

    def by_frame(self, results, pair):
        return {
            r["Frame"]: r
            for r in results
            if tuple(sorted((r["Object1"].Name, r["Object2"].Name))) == pair
            and r["Kind"] == "Interference"
        }

    def test_exact(self):
        """Every frame's volume, the document left where it was."""
        volumes = self.expected()
        self.assertGreater(max(volumes), 400.0)  # the slide went through the plate
        before = App.Placement(self.S.Placement)
        results = self.assembly.findInterferences(frames=range(self.frames))
        self.assertTrue(self.S.Placement.isSame(before, 1e-12))
        slid = self.by_frame(results, ("Plate", "S"))
        for frame, volume in enumerate(volumes):
            if volume > 1e-6:
                self.assertIn(frame, slid)
                self.assertAlmostEqual(slid[frame]["Volume"], volume, places=6)
                self.assertAlmostEqual(
                    slid[frame]["Overlap"].Volume, volume, places=6
                )
            else:
                self.assertNotIn(frame, slid)
        # P and Q never move: found at every frame, with the same volume
        still = self.by_frame(results, ("P", "Q"))
        self.assertEqual(sorted(still), list(range(self.frames)))
        for r in still.values():
            self.assertAlmostEqual(r["Volume"], 500.0, places=6)
        # The frames' times, in order (OndselSolver gives the start twice)
        times = [still[f]["Time"] for f in range(self.frames)]
        self.assertEqual(times, sorted(times))
        self.assertAlmostEqual(times[-1] - times[0], 2.0, delta=1e-9)

    def test_fast(self):
        """The same frames on meshes, but for overlaps thinner than the tolerance."""
        volumes = self.expected()
        results = self.assembly.findInterferences(mode="Fast", frames=range(self.frames))
        slid = self.by_frame(results, ("Plate", "S"))
        for frame, volume in enumerate(volumes):
            # The cross section is 100 mm^2: 1 mm^3 is 0.01 mm deep
            if volume > 1.0:
                self.assertIn(frame, slid)
            elif volume < 1e-6:
                self.assertNotIn(frame, slid)
        self.assertEqual(sorted(self.by_frame(results, ("P", "Q"))), list(range(self.frames)))

    def test_some_frames(self):
        """Only the frames asked for, with their times."""
        volumes = self.expected()
        frames = [f for f in range(self.frames) if volumes[f] > 1.0][:2] + [0]
        results = self.assembly.findInterferences(frames=frames, objects=[self.S, self.Plate])
        self.assertEqual(sorted(r["Frame"] for r in results), sorted(frames[:2]))
        for r in results:
            self.assertGreaterEqual(r["Time"], 0.0)

    def test_bad_frames(self):
        with self.assertRaises(ValueError):
            self.assembly.findInterferences(frames=[self.frames])
        with self.assertRaises(TypeError):
            self.assembly.findInterferences(frames=["0"])


class TestDragCollision(unittest.TestCase):
    """Collision while dragging: a box on a slider dragged up into a plate, on
    every solver backend, with the DragCollision preference off, highlighting
    and stopping; two boxes overlapping from the start are left out."""

    def setUp(self):
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self._solveInCreation = self.params.GetBool("SolveInJointCreation", True)
        self.params.SetBool("SolveInJointCreation", False)
        self._mode = self.params.GetInt("DragCollision", 0)

    def tearDown(self):
        self.params.SetBool("SolveInJointCreation", self._solveInCreation)
        self.params.SetInt("DragCollision", self._mode)

    def build(self, backend):
        self.doc = App.newDocument("TestDragCollision")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.assembly.SolverBackend = backend

        def box(name, l, w, h, x, y, z):
            b = self.assembly.newObject("Part::Box", name)
            b.Length, b.Width, b.Height = l, w, h
            b.Placement.Base = App.Vector(x, y, z)
            return b

        self.G = box("G", 10, 10, 10, 0, 0, 0)
        self.S = box("S", 10, 10, 10, 0, 0, 10)  # on G, sliding up
        self.Plate = box("Plate", 20, 20, 5, -5, -5, 25)  # 5 mm above S
        self.P = box("P", 10, 10, 10, 100, 0, 0)
        self.Q = box("Q", 10, 10, 10, 105, 0, 0)  # into P from the start
        joints = self.assembly.getGroup("Joints", True)
        for obj in (self.G, self.Plate):
            ground = joints.newObject("Assembly::GroundedJointPython", "Grounded" + obj.Name)
            JointObject.GroundedJoint(ground, obj)
        self.doc.recompute()
        self.assembly.makeJoint("Slider", (self.G, "Face6"), (self.S, "Face5"))
        self.doc.recompute()
        self.assertEqual(self.assembly.solve(), 0)

    def drag(self, heights):
        """S dragged up through the heights; each step's result and S's height."""
        self.assembly.preDrag([self.S])
        steps = []
        for z in heights:
            target = App.Placement(self.S.Placement)
            target.Base.z = z
            self.S.Placement = target
            ok = self.assembly.doDragStep()
            hits = self.assembly.getDragCollisions()
            pairs = sorted(tuple(sorted((a.Name, b.Name))) for a, b in hits)
            steps.append((ok, round(self.S.Placement.Base.z, 6), pairs))
        return steps

    def backends(self):
        import AssemblyApp

        return AssemblyApp.getAvailableSolvers()

    def test_stop(self):
        """The step that would collide goes as far as it can without, and the
        drag goes on."""
        self.params.SetInt("DragCollision", 2)
        for backend in self.backends():
            with self.subTest(backend=backend):
                self.build(backend)
                try:
                    steps = self.drag([12, 14, 16, 14.5])
                    self.assertEqual(steps[0], (True, 12.0, []))
                    self.assertEqual(steps[1], (True, 14.0, []))
                    # S's top at 26: 1 mm into the plate. Halved back to the
                    # plate's underside at 25, within the tolerance and the
                    # last halving (2 mm / 256) above it
                    ok, z, pairs = steps[2]
                    self.assertFalse(ok)
                    self.assertEqual(pairs, [("Plate", "S")])
                    self.assertGreaterEqual(z, 15.0 - 1e-6)
                    self.assertLess(z, 15.05)
                    self.assertEqual(steps[3], (True, 14.5, []))
                    # The solver is where the document is: a step from there
                    self.S.Placement = App.Placement(App.Vector(0, 0, 13), App.Rotation())
                    self.assertTrue(self.assembly.doDragStep())
                    self.assertAlmostEqual(self.S.Placement.Base.z, 13.0, places=6)
                    self.assembly.postDrag()
                    self.assertEqual(self.assembly.getDragCollisions(), [])
                finally:
                    App.closeDocument(self.doc.Name)

    def test_highlight(self):
        """The step goes through; the pair it brought into collision is reported."""
        self.params.SetInt("DragCollision", 1)
        self.build("Ondsel")
        try:
            steps = self.drag([14, 16, 14])
            self.assertEqual(steps[0], (True, 14.0, []))
            self.assertEqual(steps[1], (True, 16.0, [("Plate", "S")]))
            self.assertEqual(steps[2], (True, 14.0, []))
            self.assembly.postDrag()
        finally:
            App.closeDocument(self.doc.Name)

    def test_off(self):
        self.params.SetInt("DragCollision", 0)
        self.build("Ondsel")
        try:
            steps = self.drag([16])
            self.assertEqual(steps[0], (True, 16.0, []))
            self.assembly.postDrag()
        finally:
            App.closeDocument(self.doc.Name)
