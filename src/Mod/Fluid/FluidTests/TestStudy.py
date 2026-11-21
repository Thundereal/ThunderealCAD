# SPDX-License-Identifier: LGPL-2.1-or-later

# The fluid workbench's frame (docs/FluidSimulation.md sec 12, S1): the study,
# the model a backend is given, the run and its kept results, on the backend
# of test data -- every particle in free fall, so each frame has a closed form.

import array
import os
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
        else:
            # it says where it looked
            self.assertIn("OpenFoamPath", foam["Reason"])

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
        path = os.path.join(tempfile.mkdtemp(), "FluidKept.FCStd")
        self.doc.saveAs(path)
        App.closeDocument(self.doc.Name)
        self.doc = App.openDocument(path)
        study = self.doc.getObject("Study")
        results = study.getResults()
        self.assertEqual(results["Frames"], 9)
        self.assertEqual(results["Monitors"][0]["Object"], self.doc.getObject("Height"))
        self.assertEqual(pointsOf(study.getFrame(4)), before)
        self.assertFalse(study.isStale())

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
