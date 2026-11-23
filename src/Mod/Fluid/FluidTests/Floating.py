# SPDX-License-Identifier: LGPL-2.1-or-later

# A part moved by the fluid, for every backend that moves one
# (docs/FluidSimulation.md sec 12, S4): a block in a tank of still water.
# Where it comes to rest is known exactly -- Archimedes: as deep as the
# water it pushes aside weighs what it does.

import math

import FreeCAD as App
import Part

import FluidPlayback

from FluidTests.DamBreak import backendNamed

WATER = 998.2  # kg/m^3, the fluid's own


class Floating:
    """A tank 120 x 120 inside, its walls 10 thick and 130 high, open above;
    water 60 deep, less where the block is; a block 60 x 60 x 40 in the
    middle, level; a resolution of 5 mm.

    A test class is this and unittest.TestCase, and sets BACKEND."""

    BACKEND = ""

    def setUp(self):
        found = backendNamed(self.BACKEND)
        if found is None or not found["Available"]:
            self.skipTest(f"no {self.BACKEND} on this machine")
        self.assertTrue(found["FreeBodies"])
        self.doc = App.newDocument("FluidFloating")
        tank = self.doc.addObject("Part::Feature", "Tank")
        outer = Part.makeBox(140, 140, 140, App.Vector(-10, -10, -10))
        tank.Shape = outer.cut(Part.makeBox(120, 120, 131))
        water = self.doc.addObject("Part::Box", "Water")
        water.Length, water.Width, water.Height = 120, 120, 60
        self.block = self.doc.addObject("Part::Box", "Block")
        self.block.Length, self.block.Width, self.block.Height = 60, 60, 40
        self.doc.recompute()
        self.study = FluidPlayback.makeStudy(self.doc, [tank])
        FluidPlayback.addFluid(self.study, water)
        self.study.Backend = self.BACKEND
        self.study.Resolution = 5
        self.study.OutputInterval = 0.05

    def tearDown(self):
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def run_block(self, density, bottom, end):
        """The block of that density (kg/m^3) let go with its underside
        that high above the tank's floor; where its underside is at each
        frame, how far it leans (deg) and how far aside it went (mm)."""
        self.block.Placement.Base = App.Vector(30, 30, bottom)
        self.body = FluidPlayback.addBody(self.study, self.block, density)
        self.study.TimeEnd = end
        self.doc.recompute()
        self.assertEqual(self.study.run(), 0, self.study.Status)
        results = self.study.getResults()
        self.assertEqual([body["Object"] for body in results["Bodies"]], [self.block])
        rows = []
        for index in range(results["Frames"]):
            frame = self.study.getFrame(index)
            (motion,) = frame["Bodies"]
            # the middle of its underside, as the motion takes it
            under = motion.multVec(App.Vector(60, 60, bottom))
            rows.append(
                (
                    frame["Time"],
                    under.z,
                    math.degrees(motion.Rotation.Angle),
                    math.hypot(under.x - 60, under.y - 60),
                    frame["Count"],
                )
            )
        self.assertEqual(rows[0][1:4], (bottom, 0.0, 0.0))
        return rows

    def test_floats_at_its_draught(self):
        # half the water's density: half of it under, and it is put there
        draught = 40.0 * 500.0 / WATER
        rest = 60.0 - draught
        rows = self.run_block(500.0, rest, 1.0)
        self.assertEqual(len(rows), 21)
        for time, under, lean, aside, count in rows:
            # It stays: within 3 mm, and that in the first tenths, while
            # the water settles against it (measured: 2.1 down at 0.15 s).
            self.assertAlmostEqual(under, rest, delta=3.0, msg=f"at {time}")
            self.assertLess(lean, 3.0, f"at {time}")
            self.assertLess(aside, 2.0, f"at {time}")
            self.assertEqual(count, rows[0][4], f"at {time}")
        late = [under for time, under, _, _, _ in rows if time >= 0.5]
        # measured: 0.7 mm deep, a seventh of the spacing, 3.6% of the
        # draught
        self.assertAlmostEqual(sum(late) / len(late), rest, delta=1.5)

    def test_dropped_it_comes_to_float(self):
        # Let go 10 above the water, which is all there now: the block
        # in it raises it by what the block pushes aside, 20 x 60 x 60
        # over the tank's 120 x 120, and floats that much higher.
        draught = 40.0 * 500.0 / WATER
        rest = 60.0 + draught * 60.0 * 60.0 / (120.0 * 120.0) - draught
        rows = self.run_block(500.0, 70.0, 1.5)
        # it goes under its draught and comes back up over it (measured:
        # 30.5 at the lowest, 54 at the next height)
        self.assertLess(min(under for _, under, _, _, _ in rows), rest - 8.0)
        self.assertGreater(max(under for time, under, _, _, _ in rows if time > 0.25), rest + 4.0)
        for time, under, lean, aside, count in rows:
            self.assertLess(lean, 20.0, f"at {time}")
            self.assertEqual(count, rows[0][4], f"at {time}")
        late = [under for time, under, _, _, _ in rows if time >= 1.0]
        # still going up and down 2 mm each way; measured in the mean 0.2
        # under the rest
        self.assertAlmostEqual(sum(late) / len(late), rest, delta=2.5)

    def test_heavier_than_water_it_rests_on_the_floor(self):
        rows = self.run_block(2000.0, 40.0, 1.0)
        spacing = self.study.Resolution.Value
        for time, under, lean, aside, count in rows:
            # never through the floor, nor thrown back up from it, and no
            # water lost from under it
            self.assertGreater(under, 0.5 * spacing, f"at {time}")
            self.assertLess(lean, 10.0, f"at {time}")
            self.assertEqual(count, rows[0][4], f"at {time}")
        for time, under, lean, aside, count in rows:
            if time >= 0.5:
                # A particle's width off the floor, the one layer of water
                # under it: where a backend of particles holds a part off
                # a wall (measured: 4.8 to 4.95 of 5, leaning up to 2.6
                # degrees on the layer, in fifteen runs).
                self.assertAlmostEqual(under, spacing, delta=0.2 * spacing, msg=f"at {time}")
                self.assertLess(lean, 5.0, f"at {time}")
