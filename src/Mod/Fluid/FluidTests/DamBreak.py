# SPDX-License-Identifier: LGPL-2.1-or-later

# One dam break for every backend (docs/FluidSimulation.md sec 12, S2 and
# S3): the same parts, the same study, the same things asked of the result.
# What differs between a backend of cells and one of particles is said by
# the few names a test class sets, and nowhere else.

import math
import time

import FreeCAD as App
import Part

import FluidApp
import FluidPlayback

G = 9.80665


def backendNamed(name):
    for backend in FluidApp.getBackends():
        if backend["Name"] == name:
            return backend
    return None


def energyHolds(test, study, fed=False):
    """A run's own account of its energy, from a solver that keeps one: the
    fluid's energy and its work on what it moves did not rise past the
    start, and a mechanism's lag made no great share of what went over. A
    run that inlets fed has no such budget, and is not judged by it."""
    energy = study.getResults().get("Energy")
    if energy is None:
        return None
    test.assertTrue(energy["Judged"])
    test.assertEqual(energy["Fed"], fed)
    test.assertEqual(energy["BudgetJudged"], not fed)
    test.assertFalse(energy["Rose"], energy["Rise"])
    test.assertFalse(energy["Lagged"], energy["Made"])
    return energy


class DamBreak:
    """A tank 400 x 100 inside, its walls 10 thick and 200 high, open above;
    water 100 x 100 x 100 against its -x end; a resolution of 10 mm.

    A test class is this and unittest.TestCase, and sets:
      BACKEND      the backend's name
      KIND         what its frames are: "Surface" or "Particles"
      EDGE         how far the water reaches past a frame's points, mm: a
                   surface's points are the water's edge, a particle's is
                   its middle
      EXACT_TIMES  frames are at the output interval's multiples exactly;
                   otherwise at the first of the solver's steps past each
      STEP         the longest step the solver takes, s
      PRESSURE     how far the pressure at the end may be from the weight of
                   the water standing there, as a share of it
    """

    BACKEND = ""
    KIND = ""
    EDGE = 0.0
    EXACT_TIMES = True
    STEP = 0.0
    PRESSURE = 0.35

    def setUp(self):
        found = backendNamed(self.BACKEND)
        if found is None or not found["Available"]:
            self.skipTest(f"no {self.BACKEND} on this machine")
        self.doc = App.newDocument("FluidDam")
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
        self.study.Backend = self.BACKEND
        self.study.Resolution = 10
        self.study.TimeEnd = 0.3
        self.study.OutputInterval = 0.02
        self.doc.recompute()

    def tearDown(self):
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def checkStart(self, points, triangles):
        """The frame at the start is the water as it was given."""
        if self.KIND == "Surface":
            # a box
            self.assertEqual(len(triangles), 12)
        else:
            # ten particles each way
            self.assertEqual(len(points), 1000)
            self.assertFalse(triangles is not None and len(triangles))
        self.assertAlmostEqual(float(points[:, 0].max()) + self.EDGE, 100.0, places=3)
        self.assertAlmostEqual(float(points[:, 2].max()) + self.EDGE, 100.0, places=3)

    def test_dam_break(self):
        self.assertEqual(self.study.run(), 0, self.study.Status)
        energyHolds(self, self.study)
        results = self.study.getResults()
        self.assertEqual(results["Backend"], self.BACKEND)
        self.assertTrue(results["Version"])
        self.assertEqual(results["Kind"], self.KIND)
        # 0 to 0.3 s, a frame each 0.02
        self.assertEqual(results["Frames"], 16)
        for got, want in zip(results["Times"], [0.02 * k for k in range(16)]):
            if self.EXACT_TIMES:
                self.assertAlmostEqual(got, want, places=9)
            else:
                self.assertGreaterEqual(got, want - 1e-9)
                self.assertLess(got, want + self.STEP + 1e-9)

        frames = FluidPlayback.Frames(self.study)
        self.checkStart(*frames.frame(0))

        # the front runs along the floor to the far wall, 300 mm away; in
        # the experiments it goes at 1.3 to 1.7 sqrt(g H) once it runs
        # (Lobovsky 2014), 1.3 to 1.7 m/s here, and it never goes back
        fronts = []
        for index in range(1, len(frames)):
            points, triangles = frames.frame(index)
            if self.KIND == "Surface":
                self.assertGreater(len(triangles), 0, f"frame {index}")
                self.assertTrue(int(triangles.max()) < len(points))
            else:
                # none is lost: the tank is higher than the water climbs
                self.assertEqual(len(points), 1000, f"frame {index}")
            fronts.append(float(points[:, 0].max()) + self.EDGE)
        self.assertGreater(fronts[0], 100.0)
        # A particle pressed on a wall rests nearer it than the half
        # spacing it starts at: measured, its middle comes to the wall's
        # face and up to 0.07 of a spacing past it.
        slack = 1.0 if self.KIND == "Surface" else 0.75 * self.study.Resolution.Value
        for before, after in zip(fronts, fronts[1:]):
            self.assertGreaterEqual(after, before - slack)
        self.assertAlmostEqual(fronts[-1], 400.0, delta=slack)
        speed = math.sqrt(G * 0.1)
        # frames 6 and 10: 0.12 and 0.2 s
        took = results["Times"][10] - results["Times"][6]
        run = (fronts[9] - fronts[5]) / 1000.0 / took
        self.assertGreater(run, 1.2 * speed)
        self.assertLess(run, 2.0 * speed)

        # the monitors at the solver's steps, far more than the frames
        times = results["MonitorTimes"]
        self.assertGreater(len(times), 3 * results["Frames"])
        self.assertEqual(times, sorted(times))
        if self.EXACT_TIMES:
            self.assertAlmostEqual(times[-1], 0.3, places=6)
        else:
            self.assertGreaterEqual(times[-1], 0.3 - 1e-9)
            self.assertLess(times[-1], 0.3 + self.STEP)
        height, pressure = results["Monitors"]
        self.assertEqual(height["Object"], self.height)
        self.assertEqual(height["Unit"], "mm")
        self.assertEqual(len(height["Values"]), len(times))
        # the column starts 100 mm high over the point and sinks
        self.assertAlmostEqual(height["Values"][0], 100.0, delta=6.0)
        self.assertLess(height["Values"][-1], 60.0)
        self.assertGreater(height["Values"][-1], 5.0)
        self.checkPressure(times, height["Values"], pressure["Values"])

    def checkPressure(self, times, heights, pressures):
        """The pressure 2 mm above the floor ends as the weight of what
        stands there: both taken over the last 0.03 s, the water still
        moving."""
        last = [i for i, t in enumerate(times) if t >= 0.27]
        self.assertGreater(len(last), 3)
        stands = sum(heights[i] for i in last) / len(last)
        still = 998.2 * G * (stands - 2.0) * 1e-3
        ends = sum(pressures[i] for i in last) / len(last)
        self.assertAlmostEqual(ends, still, delta=self.PRESSURE * still)

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
