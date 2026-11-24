# SPDX-License-Identifier: LGPL-2.1-or-later

# An inlet, for every backend that has one (docs/FluidSimulation.md sec 12,
# S5): a jet out of a nozzle's face onto a plate square to it, with no
# gravity. What the plate is pressed with is known exactly -- the momentum
# the jet brings a second, density x area x speed^2 -- for a jet that
# leaves along the plate.

import FreeCAD as App
import Part

import FluidPlayback

from FluidTests.DamBreak import backendNamed

WATER = 998.2  # kg/m^3, the fluid's own


class Jet:
    """A nozzle 20 x 20 across, its face at x 0 looking along +x; a plate
    120 x 120 and 10 thick, its face 100 on; no gravity, the space open all
    round; 2 m/s; a resolution of RESOLUTION mm -- 5, while the Windows
    host's hard hangs under the fluid runner are looked into (the user's
    ruling of 2026-11-24); it was 2.5.

    A test class is this and unittest.TestCase, and sets BACKEND and OVER:
    how much more than the closed form the backend may read, as a part of
    it."""

    BACKEND = ""
    OVER = 0.0
    SPEED = 2.0  # m/s
    RESOLUTION = 5.0  # mm

    def layer(self):
        """Particles a layer of the jet has, and how long one takes to
        go its width, s."""
        across = round(20.0 / self.RESOLUTION)
        return across * across, self.RESOLUTION * 1e-3 / self.SPEED

    def setUp(self):
        found = backendNamed(self.BACKEND)
        if found is None or not found["Available"]:
            self.skipTest(f"no {self.BACKEND} on this machine")
        self.assertTrue(found["Inlets"])
        self.assertTrue(found["Forces"])
        self.doc = App.newDocument("FluidJet")
        self.nozzle = self.doc.addObject("Part::Feature", "Nozzle")
        self.nozzle.Shape = Part.makeBox(20, 20, 20, App.Vector(-20, -10, -10))
        self.plate = self.doc.addObject("Part::Feature", "Plate")
        self.plate.Shape = Part.makeBox(10, 120, 120, App.Vector(100, -60, -60))
        self.doc.recompute()
        self.study = FluidPlayback.makeStudy(self.doc, [self.nozzle, self.plate])
        self.study.Gravity = App.Vector(0, 0, 0)
        self.study.OpenSides = True
        self.study.Backend = self.BACKEND
        self.study.Resolution = self.RESOLUTION
        self.study.OutputInterval = 0.01
        self.study.TimeEnd = 0.3
        # the box's face that looks along +x
        self.inlet = FluidPlayback.addInlet(self.study, self.nozzle, "Face2", 1000.0 * self.SPEED)
        self.force = FluidPlayback.addForceMonitor(
            self.study, self.plate, FluidPlayback.forceDirection(self.study)
        )
        # where the jet's axis meets the plate
        self.pressure = FluidPlayback.addMonitor(self.study, "Pressure", App.Vector(100, 0, 0))
        self.doc.recompute()

    def tearDown(self):
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def test_the_plate_is_pressed_with_the_jets_momentum(self):
        self.assertEqual(self.force.Direction, App.Vector(1, 0, 0))
        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        monitor, pressure = results["Monitors"]
        self.assertEqual((monitor["Kind"], monitor["Unit"]), ("Force", "N"))
        self.assertEqual(monitor["Object"], self.force)
        times = results["MonitorTimes"]
        values = monitor["Values"]
        # nothing until the jet is there: 100 mm at 2 m/s
        early = [abs(v) for t, v in zip(times, values) if t < 0.04]
        self.assertTrue(early)
        self.assertLess(max(early), 0.01)
        # then the momentum a second, a mean over time: the steps are not
        # of one length
        expect = WATER * 0.02 * 0.02 * self.SPEED * self.SPEED
        area = 0.0
        span = 0.0
        pressed = 0.0
        low = high = None
        for k in range(1, len(times)):
            if times[k] < 0.15:
                continue
            step = times[k] - times[k - 1]
            area += values[k] * step
            pressed += pressure["Values"][k] * step
            span += step
            low = values[k] if low is None else min(low, values[k])
            high = values[k] if high is None else max(high, values[k])
        mean = area / span
        self.assertGreater(mean, 0.97 * expect, (mean, expect))
        self.assertLess(mean, (1.0 + self.OVER) * expect, (mean, expect))
        # and steadily: no step's twice as much, or half. At 5 mm a layer
        # is 4 x 4 and comes in whole (measured: 0.60 to 1.84 times; at
        # 2.5 mm under one and a half)
        self.assertGreater(low, 0.5 * expect)
        self.assertLess(high, 2.2 * expect)
        # The pressure there, on a wall a force is asked of as well: of
        # the size of the jet's own, density x speed^2 / 2, and under it
        # -- a monitor's patch is three spacings across, of a jet four
        # across at 5 mm (measured: 0.39 to 0.40; two thirds at 2.5 mm,
        # the jet eight across).
        stagnation = 0.5 * WATER * self.SPEED * self.SPEED
        self.assertGreater(pressed / span, 0.3 * stagnation)
        self.assertLess(pressed / span, 1.1 * stagnation)
        # the fluid is all the inlet's: none at the start, more each frame
        # until the first of it leaves the space
        counts = [self.study.getFrame(k)["Count"] for k in range(results["Frames"])]
        self.assertEqual(counts[0], 0)
        for k in range(1, 5):
            self.assertGreater(counts[k], counts[k - 1])
        # a layer each time one has gone its width, less what has left
        each, every = self.layer()
        self.assertLessEqual(counts[-1], each * (round(0.3 / every) + 1))
        self.assertGreater(counts[-1], each * round(0.05 / every))

    def test_an_inlet_that_stops(self):
        self.inlet.EndTime = 0.05
        self.study.TimeEnd = 0.2
        self.doc.recompute()
        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        counts = [self.study.getFrame(k)["Count"] for k in range(results["Frames"])]
        # 0.05 s of layers were sent, and no more after
        each, every = self.layer()
        sent = round(0.05 / every) + 1
        self.assertLessEqual(max(counts), each * (sent + 1))
        self.assertGreaterEqual(max(counts), each * (sent - 3))
        times = results["MonitorTimes"]
        values = results["Monitors"][0]["Values"]
        # the last of it reaches the plate at 0.1 s; well after, nothing
        # presses
        late = [abs(v) for t, v in zip(times, values) if t > 0.17]
        self.assertTrue(late)
        self.assertLess(max(late), 0.2)
