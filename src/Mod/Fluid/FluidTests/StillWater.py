# SPDX-License-Identifier: LGPL-2.1-or-later

# Still water for every backend (docs/FluidSimulation.md sec 12, S3): a
# tank with water over its whole floor. Nothing may happen, and the
# pressures are the water's weight above -- the one case whose answer is
# known exactly.

import FreeCAD as App
import Part

import FluidPlayback

from FluidTests.DamBreak import G, backendNamed, energyHolds


class StillWater:
    """A tank 200 x 100 inside, its walls 10 thick and 150 high, open above;
    water 100 deep over its floor; a resolution of 10 mm; half a second.

    A test class is this and unittest.TestCase, and sets BACKEND, KIND and
    EDGE as for the dam break (FluidTests/DamBreak.py)."""

    BACKEND = ""
    KIND = ""
    EDGE = 0.0

    def setUp(self):
        found = backendNamed(self.BACKEND)
        if found is None or not found["Available"]:
            self.skipTest(f"no {self.BACKEND} on this machine")
        self.doc = App.newDocument("FluidStill")
        tank = self.doc.addObject("Part::Feature", "Tank")
        outer = Part.makeBox(220, 120, 160, App.Vector(-10, -10, -10))
        tank.Shape = outer.cut(Part.makeBox(200, 100, 151))
        water = self.doc.addObject("Part::Box", "Water")
        water.Length, water.Width, water.Height = 200, 100, 100
        self.doc.recompute()
        self.study = FluidPlayback.makeStudy(self.doc, [tank])
        FluidPlayback.addFluid(self.study, water)
        # the depth; the pressure on the floor, on a wall 75 mm down, and in
        # the water 55 mm down
        FluidPlayback.addMonitor(self.study, "Height", App.Vector(105, 55, 0))
        FluidPlayback.addMonitor(self.study, "Pressure", App.Vector(105, 55, 0))
        FluidPlayback.addMonitor(self.study, "Pressure", App.Vector(200, 55, 25))
        FluidPlayback.addMonitor(self.study, "Pressure", App.Vector(105, 55, 45))
        self.study.Backend = self.BACKEND
        self.study.Resolution = 10
        self.study.TimeEnd = 0.5
        self.study.OutputInterval = 0.05
        self.doc.recompute()

    def tearDown(self):
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def test_still_water(self):
        self.assertEqual(self.study.run(), 0, self.study.Status)
        energyHolds(self, self.study)
        results = self.study.getResults()
        self.assertEqual(results["Frames"], 11)
        frames = FluidPlayback.Frames(self.study)
        first, _ = frames.frame(0)
        for index in range(len(frames)):
            points, _ = frames.frame(index)
            # all of it is there, where it was: under its surface, between
            # the walls
            if self.KIND == "Particles":
                self.assertEqual(len(points), len(first), f"frame {index}")
            # Particles settle against the walls in the first quarter
            # second, and the surface moves meanwhile: measured at this
            # resolution, 4 mm for its top twenty, 8.5 for the one highest.
            over = 6.0 if self.KIND == "Surface" else 6.0 + 0.5 * self.study.Resolution.Value
            self.assertLess(float(points[:, 2].max()) + self.EDGE, 100.0 + over, f"frame {index}")
            self.assertGreater(float(points[:, 2].max()) + self.EDGE, 94.0, f"frame {index}")
            self.assertGreater(float(points[:, 0].min()), -1e-3, f"frame {index}")
            self.assertLess(float(points[:, 0].max()), 200.0 + 1e-3, f"frame {index}")

        # the second half of the run: particles settle against the walls
        # in the first tenths of a second
        times = results["MonitorTimes"]
        late = [i for i, t in enumerate(times) if t >= 0.25]
        self.assertGreater(len(late), 10)
        means = []
        for monitor in results["Monitors"]:
            means.append(sum(monitor["Values"][i] for i in late) / len(late))
        depth, floor, wall, within = means
        self.assertAlmostEqual(depth, 100.0, delta=3.0)
        weight = 998.2 * G * 1e-3  # Pa a mm of water
        # a backend of cells says the pressure of the cell the point is in,
        # which on the floor is half a cell up: 5% less here
        self.assertAlmostEqual(floor, 100.0 * weight, delta=0.08 * 100.0 * weight)
        self.assertAlmostEqual(wall, 75.0 * weight, delta=0.05 * 75.0 * weight)
        self.assertAlmostEqual(within, 55.0 * weight, delta=0.05 * 55.0 * weight)
