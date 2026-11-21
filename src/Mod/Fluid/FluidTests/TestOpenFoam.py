# SPDX-License-Identifier: LGPL-2.1-or-later

# A dam break run on OpenFOAM (docs/FluidSimulation.md sec 12, S2), where
# there is one on this machine: found by the preference Mod/Fluid/
# OpenFoamPath, the environment's FLUID_OPENFOAM_PATH or PATH, or in the
# usual places. Skipped where there is none.

import math
import time
import unittest

import FreeCAD as App
import Part

import FluidApp
import FluidPlayback

G = 9.80665


def openFoam():
    for backend in FluidApp.getBackends():
        if backend["Name"] == "OpenFOAM":
            return backend
    return None


class TestOpenFoamDamBreak(unittest.TestCase):
    """A tank 400 x 100 inside, its walls 10 thick and 200 high, open above;
    water 100 x 100 x 100 against its -x end; 10 mm cells."""

    def setUp(self):
        found = openFoam()
        if found is None or not found["Available"]:
            self.skipTest("no OpenFOAM on this machine")
        self.doc = App.newDocument("FluidFoam")
        tank = self.doc.addObject("Part::Feature", "Tank")
        outer = Part.makeBox(420, 120, 210, App.Vector(-10, -10, -10))
        tank.Shape = outer.cut(Part.makeBox(400, 100, 201))
        water = self.doc.addObject("Part::Box", "Water")
        water.Length, water.Width, water.Height = 100, 100, 100
        self.doc.recompute()
        self.study = FluidPlayback.makeStudy(self.doc, [tank])
        FluidPlayback.addFluid(self.study, water)
        self.height = FluidPlayback.addMonitor(self.study, "Height", App.Vector(30, 50, 0))
        self.pressure = FluidPlayback.addMonitor(self.study, "Pressure", App.Vector(30, 50, 2))
        self.study.Backend = "OpenFOAM"
        self.study.Resolution = 10
        self.study.TimeEnd = 0.3
        self.study.OutputInterval = 0.02
        self.doc.recompute()

    def tearDown(self):
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def test_dam_break(self):
        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        self.assertEqual(results["Backend"], "OpenFOAM")
        self.assertTrue(results["Version"])
        self.assertEqual(results["Kind"], "Surface")
        # 0 to 0.3 s, a frame each 0.02
        self.assertEqual(results["Frames"], 16)
        for got, want in zip(results["Times"], [0.02 * k for k in range(16)]):
            self.assertAlmostEqual(got, want, places=9)

        frames = FluidPlayback.Frames(self.study)
        # the frame at the start is the water as it was given: a box
        points, triangles = frames.frame(0)
        self.assertEqual(len(triangles), 12)
        self.assertAlmostEqual(float(points[:, 0].max()), 100.0, places=3)
        self.assertAlmostEqual(float(points[:, 2].max()), 100.0, places=3)

        # the front runs along the floor to the far wall, 300 mm away; in
        # the experiments it goes at 1.3 to 1.7 sqrt(g H) once it runs
        # (Lobovsky 2014), 1.3 to 1.7 m/s here, and it never goes back
        fronts = []
        for index in range(1, len(frames)):
            points, triangles = frames.frame(index)
            self.assertGreater(len(triangles), 0, f"frame {index}")
            self.assertTrue(int(triangles.max()) < len(points))
            fronts.append(float(points[:, 0].max()))
        self.assertGreater(fronts[0], 100.0)
        for before, after in zip(fronts, fronts[1:]):
            self.assertGreaterEqual(after, before - 1.0)
        self.assertAlmostEqual(fronts[-1], 400.0, delta=1.0)
        speed = math.sqrt(G * 0.1)
        at = {round(0.02 * (k + 1), 2): x for k, x in enumerate(fronts)}
        run = (at[0.2] - at[0.12]) / 1000.0 / 0.08
        self.assertGreater(run, 1.2 * speed)
        self.assertLess(run, 2.0 * speed)

        # the monitors at the solver's steps, far more than the frames
        times = results["MonitorTimes"]
        self.assertGreater(len(times), 3 * results["Frames"])
        self.assertEqual(times, sorted(times))
        self.assertAlmostEqual(times[-1], 0.3, places=6)
        height, pressure = results["Monitors"]
        self.assertEqual(height["Object"], self.height)
        self.assertEqual(height["Unit"], "mm")
        self.assertEqual(len(height["Values"]), len(times))
        # the column starts 100 mm high over the point and sinks
        self.assertAlmostEqual(height["Values"][0], 100.0, delta=6.0)
        self.assertLess(height["Values"][-1], 60.0)
        self.assertGreater(height["Values"][-1], 5.0)
        # the pressure 2 mm above the floor starts below the column's
        # weight -- the water is already falling -- and ends as the weight
        # of what stands there
        weight = 998.2 * G * 0.098
        self.assertLess(pressure["Values"][0], 1.05 * weight)
        self.assertGreater(pressure["Values"][0], 0.3 * weight)
        still = 998.2 * G * (height["Values"][-1] - 2.0) * 1e-3
        self.assertAlmostEqual(pressure["Values"][-1], still, delta=0.35 * still)

    def test_cancelled(self):
        self.study.TimeEnd = 30.0
        self.assertEqual(self.study.start(), 0)
        deadline = time.time() + 120.0
        while self.study.getProgress()["Frames"] < 3 and time.time() < deadline:
            time.sleep(0.05)
        self.assertGreaterEqual(self.study.getProgress()["Frames"], 3)
        self.study.cancel()
        started = time.time()
        self.assertEqual(self.study.finish(), -7)
        # the solver is stopped, not waited for
        self.assertLess(time.time() - started, 20.0)
        self.assertIsNone(self.study.getResults())
