# SPDX-License-Identifier: LGPL-2.1-or-later

# The fluid workbench's frame (docs/FluidSimulation.md sec 12, S1): the study,
# the model a backend is given, the run and its kept results, on the backend
# of test data -- every particle in free fall, so each frame has a closed form.

import array
import os
import platform
import shutil
import tempfile
import time
import unittest

import FreeCAD as App
import Part

import FluidApp

G = 9.80665  # m/s^2


def box(doc, name, size, at):
    obj = doc.addObject("Part::Box", name)
    obj.Length, obj.Width, obj.Height = size
    obj.Placement.Base = App.Vector(*at)
    return obj


def pointsOf(frame):
    values = array.array("f")
    values.frombytes(frame["Points"])
    return [tuple(values[i : i + 3]) for i in range(0, len(values), 3)]


class FluidCase(unittest.TestCase):
    """A tank floor 400 x 200 at z 0 and a block of water 100 x 100 x 200
    standing 300 above it, at a resolution of 20 mm."""

    def setUp(self):
        self.doc = App.newDocument("FluidTest")
        self.floor = box(self.doc, "Floor", (400, 200, 10), (0, 0, -10))
        self.water = box(self.doc, "Water", (100, 100, 200), (0, 50, 300))
        self.study = self.doc.addObject("Fluid::Study", "Study")
        self.study.Walls = [self.floor]
        self.fluid = self.doc.addObject("Fluid::InitialFluid", "Fluid")
        self.fluid.Volume = self.water
        self.height = self.doc.addObject("Fluid::Monitor", "Height")
        self.height.Point = App.Vector(50, 100, 0)
        self.pressure = self.doc.addObject("Fluid::Monitor", "Pressure")
        self.pressure.Kind = "Pressure"
        self.pressure.Point = App.Vector(50, 100, 0)
        self.study.Group = [self.fluid, self.height, self.pressure]
        self.study.Resolution = 20
        self.study.TimeEnd = 0.4
        self.study.OutputInterval = 0.05
        self.study.Backend = "FreeFall"
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)


class TestBackends(unittest.TestCase):
    def test_openfoam_is_listed_first(self):
        backends = FluidApp.getBackends()
        self.assertEqual(backends[0]["Name"], "OpenFOAM")
        foam = backends[0]
        self.assertFalse(foam["TestData"])
        self.assertFalse(foam["InProcess"])
        self.assertEqual(foam["Results"], "Surface")
        self.assertTrue(foam["FreeSurface"])
        if foam["Available"]:
            self.assertTrue(foam["Version"])
        elif platform.system() == "Windows":
            # no search there: it says why (OpenFoamBackend.cpp, search())
            self.assertIn("WSL", foam["Reason"])
        else:
            # it says where it looked
            self.assertIn("OpenFoamPath", foam["Reason"])

    @staticmethod
    def pretended(folder, along, across):
        """A folder that passes for an OpenFOAM: its simpleFoam writes what a
        duct's flow lost, one figure for a duct along x and one for the rest."""
        os.makedirs(os.path.join(folder, "bin"))
        os.makedirs(os.path.join(folder, "etc"))
        with open(os.path.join(folder, "etc", "bashrc"), "w") as out:
            out.write('export PATH="%s/bin:$PATH"\n' % folder)
        solver = (
            "#!/bin/bash\n"
            "mkdir -p postProcessing/pressure/0\n"
            "if grep -q 'probeLocations ((0.04 0 0)' system/controlDict; "
            "then loss=%r; else loss=%r; fi\n"
            "printf '# Time 0 1\\n1 %%s 0\\n' \"$loss\" > postProcessing/pressure/0/p\n"
        ) % (along, across)
        for name, text in (("blockMesh", "#!/bin/bash\nexit 0\n"), ("simpleFoam", solver)):
            path = os.path.join(folder, "bin", name)
            with open(path, "w") as out:
                out.write(text)
            os.chmod(path, 0o755)

    @unittest.skipIf(os.name == "nt", "the backend drives no OpenFOAM on Windows yet")
    def test_an_openfoam_is_put_to_the_duct(self):
        folder = tempfile.mkdtemp()
        try:
            wrong = os.path.join(folder, "wrong")
            self.pretended(wrong, 0.1695, 0.1371)
            fault = FluidApp.checkOpenFoam(wrong)
            self.assertIn("computes viscous flow wrongly", fault)
            self.assertIn("23.6% more pressure laid along x", fault)
            right = os.path.join(folder, "right")
            self.pretended(right, 0.13707, 0.13712)
            self.assertEqual(FluidApp.checkOpenFoam(right), "")
            # what could not be run is not refused, and nothing is nothing
            broken = os.path.join(folder, "broken")
            self.pretended(broken, 0.0, 0.0)
            self.assertEqual(FluidApp.checkOpenFoam(broken), "")
            self.assertEqual(FluidApp.checkOpenFoam(os.path.join(folder, "none")), "")
        finally:
            shutil.rmtree(folder, ignore_errors=True)

    def test_a_faulty_openfoam_is_not_used(self):
        named = os.environ.get("FLUID_OPENFOAM_PATH")
        if not named or FluidApp.checkOpenFoam(named) == "":
            self.skipTest("no OpenFOAM with the fault is named on this machine")
        foam = {b["Name"]: b for b in FluidApp.getBackends()}["OpenFOAM"]
        if foam["Available"]:
            # another one was found, and that one is right
            return
        self.assertIn("is not used", foam["Reason"])
        self.assertIn("computes viscous flow wrongly", foam["Reason"])
        self.assertIn(named, foam["Reason"])

    def test_free_fall_is_listed_as_test_data(self):
        backends = {b["Name"]: b for b in FluidApp.getBackends()}
        self.assertIn("FreeFall", backends)
        free = backends["FreeFall"]
        self.assertTrue(free["TestData"])
        self.assertTrue(free["Available"])
        self.assertEqual(free["Results"], "Particles")
        self.assertTrue(free["Repeatable"])


class TestStudy(FluidCase):
    def test_model_in_si(self):
        model = self.study.buildModel()
        self.assertEqual(model["Walls"], 1)
        self.assertEqual(model["Fluids"], 1)
        self.assertEqual(model["Monitors"], 2)
        # two boxes, twelve triangles each
        self.assertEqual(model["Triangles"], 24)
        self.assertAlmostEqual(model["Resolution"], 0.02, places=12)
        for got, want in zip(model["SpaceMin"], (0.0, 0.0, -0.01)):
            self.assertAlmostEqual(got, want, places=12)
        for got, want in zip(model["SpaceMax"], (0.4, 0.2, 0.5)):
            self.assertAlmostEqual(got, want, places=12)
        self.assertAlmostEqual(model["Gravity"][2], -G, places=9)
        self.assertAlmostEqual(model["TimeEnd"], 0.4, places=12)

    def test_resolution_left_to_the_study(self):
        self.study.Resolution = 0
        # a twentieth of the water's smallest side, 100 mm
        self.assertAlmostEqual(self.study.buildModel()["Resolution"], 0.005, places=12)

    def test_wall_follows_its_placement(self):
        before = self.study.buildModel()
        self.floor.Placement.Base = App.Vector(0, 0, -60)
        self.doc.recompute()
        after = self.study.buildModel()
        self.assertAlmostEqual(after["SpaceMin"][2], -0.06, places=12)
        self.assertNotEqual(before["Signature"], after["Signature"])

    def test_link_as_a_wall(self):
        link = self.doc.addObject("App::Link", "Lid")
        link.LinkedObject = self.floor
        link.Placement.Base = App.Vector(0, 0, 700)
        self.study.Walls = [self.floor, link]
        self.doc.recompute()
        model = self.study.buildModel()
        self.assertEqual(model["Walls"], 2)
        # the copy stands where the Link puts it, 700 to 710
        self.assertAlmostEqual(model["SpaceMax"][2], 0.71, places=12)

    def test_a_volume_that_is_no_solid_is_named(self):
        face = self.doc.addObject("Part::Plane", "Sheet")
        self.fluid.Volume = face
        self.doc.recompute()
        with self.assertRaises(ValueError) as caught:
            self.study.buildModel()
        self.assertIn("Sheet", str(caught.exception))

    def test_no_backend_named_and_none_to_run(self):
        # the test data is never chosen by itself: without a solver on this
        # machine there is none to run
        if not any(b["Available"] and not b["TestData"] for b in FluidApp.getBackends()):
            self.study.Backend = ""
            self.assertEqual(self.study.run(), -3)
            self.assertIn("no solver", self.study.Status)
        self.study.Backend = "Nobody"
        self.assertEqual(self.study.run(), -3)
        self.assertIn("Nobody", self.study.Status)
        self.assertIsNone(self.study.getResults())

    def test_no_fluid(self):
        self.study.Group = [self.height]
        self.assertEqual(self.study.run(), -4)
        self.assertIsNone(self.study.getResults())


class TestResults(FluidCase):
    def test_frames_in_free_fall(self):
        self.assertEqual(self.study.run(), 0)
        self.assertEqual(self.study.Status, "Done")
        results = self.study.getResults()
        self.assertEqual(results["Backend"], "FreeFall")
        self.assertEqual(results["Kind"], "Particles")
        # 0 to 0.4 s, a frame each 0.05
        self.assertEqual(results["Frames"], 9)
        for got, want in zip(results["Times"], [0.05 * k for k in range(9)]):
            self.assertAlmostEqual(got, want, places=12)

        first = self.study.getFrame(0)
        # 100 x 100 x 200 at 20: 5 x 5 x 10
        self.assertEqual(first["Count"], 250)
        start = pointsOf(first)
        self.assertAlmostEqual(min(p[2] for p in start), 310.0, places=3)
        self.assertAlmostEqual(max(p[2] for p in start), 490.0, places=3)
        self.assertAlmostEqual(min(p[0] for p in start), 10.0, places=3)
        self.assertAlmostEqual(max(p[1] for p in start), 140.0, places=3)

        # at 0.2 s every particle is 196.133 mm lower, none at the floor yet
        frame = self.study.getFrame(4)
        self.assertAlmostEqual(frame["Time"], 0.2, places=12)
        drop = 0.5 * G * 0.2**2 * 1000.0
        for was, now in zip(start, pointsOf(frame)):
            self.assertAlmostEqual(now[0], was[0], places=3)
            self.assertAlmostEqual(now[1], was[1], places=3)
            self.assertAlmostEqual(now[2], was[2] - drop, delta=2e-3)

        # at 0.4 s, 784.5 mm of fall: all on the space's floor, the bottom
        # of the wall, half a spacing above it
        last = pointsOf(self.study.getFrame(8))
        self.assertEqual(len(last), 250)
        for p in last:
            self.assertAlmostEqual(p[2], 0.0, places=3)

        with self.assertRaises(IndexError):
            self.study.getFrame(9)

    def test_monitors(self):
        self.assertEqual(self.study.run(), 0)
        results = self.study.getResults()
        monitors = results["Monitors"]
        # the test data's monitors are at the frames' times
        self.assertEqual(results["MonitorTimes"], results["Times"])
        self.assertEqual([m["Kind"] for m in monitors], ["Height", "Pressure"])
        self.assertEqual([m["Unit"] for m in monitors], ["mm", "Pa"])
        self.assertEqual(monitors[0]["Object"], self.height)
        self.assertEqual(monitors[1]["Object"], self.pressure)
        # the water's top above the point, z 0: 500 mm less the fall
        for k, got in enumerate(monitors[0]["Values"]):
            fall = 0.5 * G * (0.05 * k) ** 2 * 1000.0
            want = max(500.0 - fall, 10.0)
            self.assertAlmostEqual(got, want, delta=1e-6, msg=f"frame {k}")
        # test data: the weight of that height, 998.2 kg/m^3
        for height, got in zip(monitors[0]["Values"], monitors[1]["Values"]):
            self.assertAlmostEqual(got, 998.2 * G * height * 1e-3, delta=1e-6)

    def test_run_in_its_thread(self):
        self.assertEqual(self.study.start(), 0)
        progress = self.study.getProgress()
        self.assertTrue(progress["Running"])
        # a second run is refused while the first is on
        self.assertEqual(self.study.start(), -1)
        deadline = time.time() + 30.0
        while not self.study.getProgress()["Done"] and time.time() < deadline:
            time.sleep(0.01)
        progress = self.study.getProgress()
        self.assertTrue(progress["Done"])
        self.assertEqual(progress["Frames"], 9)
        self.assertAlmostEqual(progress["Time"], 0.4, places=12)
        self.assertEqual(self.study.finish(), 0)
        self.assertFalse(self.study.getProgress()["Running"])
        self.assertEqual(self.study.getResults()["Frames"], 9)

    def test_cancelled_keeps_the_results_before(self):
        self.assertEqual(self.study.run(), 0)
        self.study.Resolution = 2
        self.study.OutputInterval = 0.0005
        self.assertEqual(self.study.start(), 0)
        self.study.cancel()
        self.assertEqual(self.study.finish(), -7)
        self.assertIn("Cancelled", self.study.Status)
        self.assertEqual(self.study.getResults()["Frames"], 9)

    def test_stale_when_the_model_changes(self):
        self.assertEqual(self.study.run(), 0)
        self.assertFalse(self.study.isStale())
        self.water.Height = 150
        self.doc.recompute()
        self.assertTrue(self.study.isStale())
        self.water.Height = 200
        self.doc.recompute()
        self.assertFalse(self.study.isStale())
        self.study.TimeEnd = 0.5
        self.assertTrue(self.study.isStale())

    def test_kept_in_the_document(self):
        self.assertEqual(self.study.run(), 0)
        before = pointsOf(self.study.getFrame(4))
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "FluidKept.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        study = self.doc.getObject("Study")
        results = study.getResults()
        self.assertEqual(results["Frames"], 9)
        self.assertEqual(results["Monitors"][0]["Object"], self.doc.getObject("Height"))
        self.assertEqual(pointsOf(study.getFrame(4)), before)
        self.assertFalse(study.isStale())

    def test_opened_in_a_fresh_session(self):
        # Opening a file loads a type's module by the type's prefix: Fluid,
        # not FluidApp. With no module of that name, a session that had not
        # imported FluidApp itself lost every Fluid object in the file
        import subprocess

        from Document import consoleExe

        exe = consoleExe()
        if not exe:
            self.skipTest("no FreeCADCmd beside this build")
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "FluidFresh.FCStd")
        self.doc.saveAs(path)
        found = os.path.join(folder, "types.txt")
        script = os.path.join(folder, "open.py")
        with open(script, "w") as f:
            f.write(
                "import FreeCAD\n"
                f"doc = FreeCAD.openDocument({path!r})\n"
                f"with open({found!r}, 'w') as out:\n"
                "    out.write(' '.join(sorted({o.TypeId for o in doc.Objects})))\n"
            )
        subprocess.run([exe, script], capture_output=True, timeout=300)
        with open(found) as f:
            types = f.read().split()
        self.assertIn("Fluid::Study", types)
        self.assertIn("Fluid::InitialFluid", types)

    def test_archive_reads_as_numpy(self):
        try:
            import numpy
        except ImportError:
            self.skipTest("numpy is not installed")
        import io
        import json
        import zipfile

        self.assertEqual(self.study.run(), 0)
        with zipfile.ZipFile(self.study.Results) as archive:
            index = json.loads(archive.read("index.json"))
            times = numpy.load(io.BytesIO(archive.read("times.npy")))
            samples = numpy.load(io.BytesIO(archive.read("samples.npy")))
            monitors = numpy.load(io.BytesIO(archive.read("monitors.npy")))
            points = numpy.load(io.BytesIO(archive.read("frames/000004.points.npy")))
        self.assertEqual(index["format"], "ThunderealCAD fluid results")
        self.assertEqual(index["frames"], 9)
        self.assertEqual(index["units"]["length"], "mm")
        self.assertEqual(times.shape, (9,))
        self.assertEqual(index["samples"], 9)
        self.assertEqual(samples.shape, (9,))
        self.assertEqual(monitors.shape, (9, 2))
        self.assertEqual(points.shape, (250, 3))
        self.assertEqual(points.dtype, numpy.float32)


class TestBodies(FluidCase):
    """The case with a block 50 x 50 x 50 of 600 kg/m^3 standing 100 above
    the floor, for the fluid to move (docs/FluidSimulation.md sec 12, S4).
    The space is the parts' box: its floor is the floor's underside, 110
    under the block."""

    def setUp(self):
        super().setUp()
        self.block = box(self.doc, "Block", (50, 50, 50), (200, 75, 100))
        self.body = self.doc.addObject("Fluid::FreeBody", "Body")
        self.body.Part = self.block
        self.body.Density = "600 kg/m^3"
        self.study.Group = self.study.Group + [self.body]
        self.doc.recompute()

    def test_a_part_moved_is_in_the_model(self):
        model = self.study.buildModel()
        # a wall to the fluid, as the floor is
        self.assertEqual(model["Walls"], 2)
        (body,) = model["Bodies"]
        self.assertEqual(body["Source"], "Block")
        mass = 600.0 * 0.05**3
        self.assertAlmostEqual(body["Mass"], mass, delta=1e-12)
        for got, want in zip(body["Centre"], (0.225, 0.1, 0.125)):
            self.assertAlmostEqual(got, want, delta=1e-12)
        # a cube's, about its middle: m a^2 / 6 each way and no products
        for got, want in zip(body["Inertia"], [mass * 0.05**2 / 6.0] * 3 + [0.0] * 3):
            self.assertAlmostEqual(got, want, delta=1e-12)
        self.assertAlmostEqual(model["SpaceMin"][2], -0.01, delta=1e-12)

    def test_what_it_weighs_is_of_the_model(self):
        self.assertEqual(self.study.run(), 0)
        self.assertFalse(self.study.isStale())
        self.body.Density = "700 kg/m^3"
        self.assertTrue(self.study.isStale())

    def test_what_cannot_be_moved_is_named(self):
        self.body.Density = 0
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("Body", str(raised.exception))
        self.assertIn("density", str(raised.exception))
        self.body.Density = "600 kg/m^3"
        # held and moved at once
        self.study.Walls = [self.floor, self.block]
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("Block", str(raised.exception))
        self.study.Walls = [self.floor]
        self.body.Part = None
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("no part", str(raised.exception))

    def test_where_it_is_at_each_frame(self):
        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        (body,) = results["Bodies"]
        self.assertEqual(body["Object"], self.block)
        self.assertEqual(body["Source"], "Block")
        for index, time in enumerate(results["Times"]):
            fall = min(0.5 * G * time**2 * 1000.0, 110.0)
            for motions in (
                self.study.getFrame(index)["Bodies"],
                self.study.getSurface(index)["Bodies"],
                self.study.getBodies(index),
            ):
                (motion,) = motions
                self.assertAlmostEqual(motion.Base.z, -fall, delta=1e-9)
                self.assertAlmostEqual(motion.Base.x, 0.0, delta=1e-9)
                self.assertAlmostEqual(motion.Rotation.Angle, 0.0, delta=1e-12)
        with self.assertRaises(IndexError):
            self.study.getBodies(len(results["Times"]))

    def test_kept_with_the_document(self):
        self.assertEqual(self.study.run(), 0)
        before = self.study.getBodies(2)[0].Base.z
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        path = os.path.join(folder, "FluidBodies.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        study = self.doc.getObject("Study")
        self.assertEqual(study.getResults()["Bodies"][0]["Object"], self.doc.getObject("Block"))
        self.assertEqual(study.getBodies(2)[0].Base.z, before)
        self.assertFalse(study.isStale())

    def test_a_backend_that_moves_none_says_so(self):
        foam = [b for b in FluidApp.getBackends() if b["Name"] == "OpenFOAM"][0]
        self.assertFalse(foam["FreeBodies"])
        self.assertTrue(
            [b for b in FluidApp.getBackends() if b["Name"] == "FreeFall"][0]["FreeBodies"]
        )
        if not foam["Available"]:
            self.skipTest("no OpenFOAM on this machine")
        self.study.Backend = "OpenFOAM"
        self.assertEqual(self.study.run(), -2)
        self.assertIn("cannot move a part", self.study.Status)
        self.assertIsNone(self.study.getResults())

    def test_results_without_one_have_none(self):
        self.study.Group = [o for o in self.study.Group if o != self.body]
        self.assertEqual(self.study.buildModel()["Bodies"], [])
        self.assertEqual(self.study.run(), 0)
        self.assertEqual(self.study.getResults()["Bodies"], [])
        self.assertEqual(self.study.getBodies(1), [])
        self.assertEqual(self.study.getFrame(1)["Bodies"], [])


class TestInlets(unittest.TestCase):
    """A nozzle 40 x 40 across, its face at x 0 looking along +x, and a
    plate 200 on; no gravity, the space open all round; the fluid comes in
    at 2 m/s; a resolution of 10 mm."""

    def setUp(self):
        self.doc = App.newDocument("FluidInlet")
        self.nozzle = box(self.doc, "Nozzle", (20, 40, 40), (-20, -20, -20))
        self.plate = box(self.doc, "Plate", (10, 200, 200), (200, -100, -100))
        self.study = self.doc.addObject("Fluid::Study", "Study")
        self.study.Walls = [self.nozzle, self.plate]
        self.study.Gravity = App.Vector(0, 0, 0)
        self.study.OpenSides = True
        self.inlet = self.doc.addObject("Fluid::Inlet", "Inlet")
        # a box's second face looks along +x
        self.inlet.Face = (self.nozzle, ["Face2"])
        self.inlet.Velocity = 2000
        self.force = self.doc.addObject("Fluid::Monitor", "Force")
        self.force.Kind = "Force"
        self.force.Part = self.plate
        self.force.Direction = App.Vector(2, 0, 0)
        self.study.Group = [self.inlet, self.force]
        self.study.Resolution = 10
        self.study.TimeEnd = 0.1
        self.study.OutputInterval = 0.01
        self.study.Backend = "FreeFall"
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def test_an_inlet_is_in_the_model(self):
        model = self.study.buildModel()
        self.assertEqual(model["Fluids"], 0)
        (inlet,) = model["Inlets"]
        self.assertEqual(inlet["Source"], "Inlet")
        # out of the nozzle's solid, in SI
        for got, want in zip(inlet["Direction"], (1.0, 0.0, 0.0)):
            self.assertAlmostEqual(got, want, places=12)
        self.assertAlmostEqual(inlet["Speed"], 2.0, places=12)
        self.assertAlmostEqual(inlet["Area"], 0.0016, places=12)
        for got, want in zip(inlet["Middle"], (0.0, 0.0, 0.0)):
            self.assertAlmostEqual(got, want, places=12)
        self.assertEqual((inlet["Start"], inlet["End"]), (0.0, 0.0))
        # the space is the walls' box, open all round: no gravity, no top
        self.assertEqual(model["SpaceMin"], (-0.02, -0.1, -0.1))
        self.assertEqual(model["SpaceMax"], (0.21, 0.1, 0.1))
        self.assertEqual(model["Open"], (True,) * 6)
        self.assertEqual(model["Monitors"], 1)

    def test_the_sides_of_the_space(self):
        self.study.OpenSides = False
        self.assertEqual(self.study.buildModel()["Open"], (False,) * 6)
        # with gravity one side is the top, and the others follow OpenSides
        self.study.Gravity = App.Vector(0, 0, -9806.65)
        self.assertEqual(self.study.buildModel()["Open"], (False,) * 5 + (True,))
        self.study.OpenSides = True
        self.assertEqual(self.study.buildModel()["Open"], (True,) * 6)
        self.study.OpenTop = False
        self.assertEqual(self.study.buildModel()["Open"], (True,) * 5 + (False,))

    def test_reversed_and_timed(self):
        before = self.study.buildModel()["Signature"]
        self.inlet.Reversed = True
        self.inlet.StartTime = 0.02
        self.inlet.EndTime = 0.05
        model = self.study.buildModel()
        (inlet,) = model["Inlets"]
        self.assertAlmostEqual(inlet["Direction"][0], -1.0, places=12)
        self.assertEqual((inlet["Start"], inlet["End"]), (0.02, 0.05))
        self.assertNotEqual(model["Signature"], before)

    def test_what_cannot_be_an_inlet_is_named(self):
        self.inlet.Face = None
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("no face", str(raised.exception))
        self.inlet.Face = (self.nozzle, ["Edge1"])
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("not a face", str(raised.exception))
        self.inlet.Face = (self.nozzle, ["Face2"])
        self.inlet.Velocity = 0
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("no speed", str(raised.exception))
        self.inlet.Velocity = 2000
        # a cylinder's side is no flat face
        pipe = self.doc.addObject("Part::Cylinder", "Pipe")
        self.doc.recompute()
        self.inlet.Face = (pipe, ["Face1"])
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("not flat", str(raised.exception))
        # its end is one
        self.inlet.Face = (pipe, ["Face2"])
        (inlet,) = self.study.buildModel()["Inlets"]
        self.assertAlmostEqual(abs(inlet["Direction"][2]), 1.0, places=9)

    def test_what_cannot_be_a_force_is_named(self):
        self.force.Part = None
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("no part", str(raised.exception))
        other = box(self.doc, "Other", (10, 10, 10), (50, 50, 50))
        self.doc.recompute()
        self.force.Part = other
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("not one of the study's walls", str(raised.exception))
        self.force.Part = self.plate
        self.force.Direction = App.Vector(0, 0, 0)
        with self.assertRaises(ValueError) as raised:
            self.study.buildModel()
        self.assertIn("no direction", str(raised.exception))

    def test_the_fluid_is_all_the_inlets(self):
        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        self.assertEqual(results["Frames"], 11)
        (monitor,) = results["Monitors"]
        self.assertEqual((monitor["Kind"], monitor["Unit"]), ("Force", "N"))
        self.assertEqual(monitor["Object"], self.force)
        # the test data's force: the momentum a second, 998.2 x 0.0016 x 2 x 2
        for value in monitor["Values"]:
            self.assertAlmostEqual(value, 998.2 * 0.0016 * 4.0, places=9)
        # a layer of 4 x 4 at the start and each 5 ms after
        for k in range(results["Frames"]):
            frame = self.study.getFrame(k)
            self.assertEqual(frame["Count"], 16 * (2 * k + 1))
            front = max(p[0] for p in pointsOf(frame))
            self.assertAlmostEqual(front, max(20.0 * k, 0.0), delta=1e-3)
        self.assertFalse(self.study.isStale())
        self.inlet.Velocity = 1000
        self.assertTrue(self.study.isStale())

    def test_kept_with_the_document(self):
        self.assertEqual(self.study.run(), 0, self.study.Status)
        folder = tempfile.mkdtemp()
        try:
            path = os.path.join(folder, "inlet.FCStd")
            self.doc.saveAs(path)
            App.closeDocument(self.doc.Name)
            self.doc = App.openDocument(path)
            study = self.doc.getObject("Study")
            self.assertFalse(study.isStale())
            self.assertEqual(study.getResults()["Monitors"][0]["Kind"], "Force")
            self.assertEqual(study.getFrame(2)["Count"], 80)
        finally:
            shutil.rmtree(folder, ignore_errors=True)

    def test_a_backend_that_has_none_says_so(self):
        backends = {b["Name"]: b for b in FluidApp.getBackends()}
        self.assertFalse(backends["OpenFOAM"]["Inlets"])
        self.assertFalse(backends["OpenFOAM"]["Forces"])
        self.assertTrue(backends["FreeFall"]["Inlets"])
        self.assertTrue(backends["FreeFall"]["Forces"])
        if not backends["OpenFOAM"]["Available"]:
            self.skipTest("no OpenFOAM on this machine")
        self.study.Backend = "OpenFOAM"
        self.assertEqual(self.study.run(), -2)
        self.assertIn("cannot run an inlet", self.study.Status)
        # and a force alone, with water at the start in the inlet's place
        water = box(self.doc, "Water", (40, 40, 40), (20, -20, -20))
        fluid = self.doc.addObject("Fluid::InitialFluid", "Fluid")
        fluid.Volume = water
        self.study.Group = [fluid, self.force]
        self.doc.recompute()
        self.assertEqual(self.study.run(), -2)
        self.assertIn("cannot report the force", self.study.Status)


class TestSteady(unittest.TestCase):
    """A tube along x, 10 mm inside and 14 outside, 100 long, closed at each
    end by a lid 2 thick: the fluid comes in through the face of the lid at
    x 0 at 0.1 m/s and leaves through the face of the lid at x 100."""

    def setUp(self):
        self.doc = App.newDocument("FluidSteady")
        along = App.Vector(1, 0, 0)
        tube = Part.makeCylinder(7, 100, App.Vector(0, 0, 0), along).cut(
            Part.makeCylinder(5, 100, App.Vector(0, 0, 0), along)
        )
        self.tube = self.doc.addObject("Part::Feature", "Tube")
        self.tube.Shape = tube
        self.first = self.doc.addObject("Part::Feature", "First")
        self.first.Shape = Part.makeCylinder(7, 2, App.Vector(-2, 0, 0), along)
        self.last = self.doc.addObject("Part::Feature", "Last")
        self.last.Shape = Part.makeCylinder(7, 2, App.Vector(100, 0, 0), along)
        self.study = self.doc.addObject("Fluid::Study", "Study")
        self.study.Analysis = "Steady flow"
        self.study.Walls = [self.tube, self.first, self.last]
        self.inlet = self.doc.addObject("Fluid::Inlet", "Inlet")
        self.inlet.Face = (self.first, [self.faceAt(self.first, 0)])
        self.inlet.Velocity = 100
        self.outlet = self.doc.addObject("Fluid::Outlet", "Outlet")
        self.outlet.Face = (self.last, [self.faceAt(self.last, 100)])
        self.study.Group = [self.inlet, self.outlet]
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    @staticmethod
    def faceAt(obj, x):
        """The name of the object's flat face at that x."""
        for i, face in enumerate(obj.Shape.Faces):
            box = face.BoundBox
            if abs(box.XMin - x) < 1e-9 and abs(box.XMax - x) < 1e-9:
                return "Face%d" % (i + 1)
        raise AssertionError("no face at x %g" % x)

    def test_a_study_is_of_a_free_surface_unless_told(self):
        study = self.doc.addObject("Fluid::Study", "Other")
        self.assertEqual(study.Analysis, "Free surface")
        self.assertEqual(study.buildModel()["Analysis"], "FreeSurface")
        self.assertNotIn("Domain", study.buildModel())

    def test_the_fluid_fills_the_space_inside_the_walls(self):
        model = self.study.buildModel()
        self.assertEqual(model["Analysis"], "Steady")
        self.assertEqual(model["Iterations"], 2000)
        domain = model["Domain"]
        import math

        # the tube's bore, a cylinder 10 across and 100 long, in SI; the
        # round is of flat pieces a hundredth of the resolution off it
        self.assertAlmostEqual(domain["Volume"] / (math.pi * 25e-6 * 0.1), 1.0, delta=3e-3)
        walls, inlet, outlet = domain["Areas"]
        self.assertAlmostEqual(inlet / (math.pi * 25e-6), 1.0, delta=3e-3)
        self.assertAlmostEqual(outlet / (math.pi * 25e-6), 1.0, delta=3e-3)
        self.assertAlmostEqual(walls / (math.pi * 0.01 * 0.1), 1.0, delta=3e-3)
        # the space is the fluid's box
        for got, want in zip(model["SpaceMin"], (0.0, -0.005, -0.005)):
            self.assertAlmostEqual(got, want, places=9)
        for got, want in zip(model["SpaceMax"], (0.1, 0.005, 0.005)):
            self.assertAlmostEqual(got, want, places=9)
        # a twentieth of its smallest side
        self.assertAlmostEqual(model["Resolution"], 0.0005, places=12)
        (fed,) = model["Inlets"]
        for got, want in zip(fed["Direction"], (1.0, 0.0, 0.0)):
            self.assertAlmostEqual(got, want, places=12)
        self.assertAlmostEqual(fed["Speed"], 0.1, places=12)
        (out,) = model["Outlets"]
        self.assertEqual(out["Source"], "Outlet")
        self.assertEqual(out["Pressure"], 0.0)
        self.assertAlmostEqual(out["Area"] / (math.pi * 49e-6), 1.0, delta=3e-3)

    def test_the_outlets_pressure_is_in_pascal(self):
        self.outlet.Pressure = "2 kPa"
        self.assertAlmostEqual(self.study.buildModel()["Outlets"][0]["Pressure"], 2000.0, places=9)

    def test_the_signature_follows_the_outlet_and_the_iterations(self):
        base = self.study.buildModel()["Signature"]
        self.assertEqual(base, self.study.buildModel()["Signature"])
        self.outlet.Pressure = "1 kPa"
        other = self.study.buildModel()["Signature"]
        self.assertNotEqual(base, other)
        self.study.Iterations = 500
        self.assertNotEqual(other, self.study.buildModel()["Signature"])

    def test_the_signature_is_not_of_how_finely_a_wall_was_drawn(self):
        # the 3D view keeps triangles on a part's faces, as fine as it
        # draws them: the model's own are of the shape alone, or results
        # just kept would read as of another study
        base = self.study.buildModel()
        for wall in self.study.Walls:
            wall.Shape.tessellate(0.001)
        fine = self.study.buildModel()
        self.assertEqual(fine["Triangles"], base["Triangles"])
        self.assertEqual(fine["Signature"], base["Signature"])

    def test_what_a_steady_flow_needs(self):
        self.study.Group = [self.inlet]
        with self.assertRaisesRegex(ValueError, "needs an outlet"):
            self.study.buildModel()
        self.study.Group = [self.outlet]
        with self.assertRaisesRegex(ValueError, "needs an inlet"):
            self.study.buildModel()
        self.study.Group = [self.inlet, self.outlet]
        self.outlet.Face = (self.last, ["Edge1"])
        with self.assertRaisesRegex(ValueError, "is not a face"):
            self.study.buildModel()
        self.outlet.Face = None
        with self.assertRaisesRegex(ValueError, "has no face"):
            self.study.buildModel()
        # a round face is no way out
        self.outlet.Face = (self.tube, [self.roundFace(self.tube, 5)])
        with self.assertRaisesRegex(ValueError, "is not flat"):
            self.study.buildModel()

    @staticmethod
    def roundFace(obj, radius):
        for i, face in enumerate(obj.Shape.Faces):
            if (
                face.Surface.TypeId == "Part::GeomCylinder"
                and abs(face.Surface.Radius - radius) < 1e-9
            ):
                return "Face%d" % (i + 1)
        raise AssertionError("no round face")

    def test_an_opening_without_a_lid_lets_the_space_out(self):
        self.study.Walls = [self.tube, self.first]
        with self.assertRaisesRegex(ValueError, "does not open onto a closed space"):
            self.study.buildModel()

    def test_fluid_at_the_start_is_of_another_analysis(self):
        water = box(self.doc, "Water", (10, 10, 10), (0, 0, 0))
        fluid = self.doc.addObject("Fluid::InitialFluid", "Fluid")
        fluid.Volume = water
        self.study.Group = [self.inlet, self.outlet, fluid]
        with self.assertRaisesRegex(ValueError, "fills the space"):
            self.study.buildModel()

    def test_two_spaces_are_not_one_flow(self):
        # a wall across the middle of the bore
        plug = self.doc.addObject("Part::Feature", "Plug")
        plug.Shape = Part.makeCylinder(5, 4, App.Vector(48, 0, 0), App.Vector(1, 0, 0))
        self.study.Walls = [self.tube, self.first, self.last, plug]
        self.doc.recompute()
        with self.assertRaisesRegex(ValueError, "do not open onto the same space"):
            self.study.buildModel()

    def test_who_runs_one(self):
        steady = {backend["Name"] for backend in FluidApp.getBackends() if backend["Steady"]}
        self.assertEqual(steady, {"OpenFOAM"})
        self.study.Backend = "FreeFall"
        self.assertEqual(self.study.run(), -2)
        self.assertIn("cannot run a steady flow", self.study.Status)

    def test_it_is_kept_with_the_document(self):
        folder = tempfile.mkdtemp()
        try:
            path = os.path.join(folder, "steady.FCStd")
            before = self.study.buildModel()["Signature"]
            self.doc.saveAs(path)
            App.closeDocument(self.doc.Name)
            self.doc = App.openDocument(path)
            study = self.doc.getObject("Study")
            self.assertEqual(study.Analysis, "Steady flow")
            self.assertEqual(study.buildModel()["Signature"], before)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
