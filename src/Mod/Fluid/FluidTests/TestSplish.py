# SPDX-License-Identifier: LGPL-2.1-or-later

# The same dam break run on SPlisHSPlasH (docs/FluidSimulation.md sec 12,
# S3), where FluidSplish was built with this FreeCAD or is named by the
# preference Mod/Fluid/SplishPath or the environment's FLUID_SPLISH_PATH.
# Skipped where it is not.

import unittest

import FreeCAD as App

import FluidPlayback

from FluidTests.DamBreak import DamBreak, backendNamed
from FluidTests.Floating import Floating
from FluidTests.StillWater import StillWater


class TestSplishDamBreak(DamBreak, unittest.TestCase):
    BACKEND = "SPlisHSPlasH"
    KIND = "Particles"
    # a particle's middle is half the resolution from the water's edge
    EDGE = 5.0
    EXACT_TIMES = False
    STEP = 0.005


class TestSplishStillWater(StillWater, unittest.TestCase):
    BACKEND = "SPlisHSPlasH"
    KIND = "Particles"
    EDGE = 5.0


class TestSplishFloating(Floating, unittest.TestCase):
    BACKEND = "SPlisHSPlasH"


class TestSplishPlates(unittest.TestCase):
    """The dam break's tank made of five plates that only touch: a floor and
    four walls. A wall's particles are laid behind each part's faces, and
    where two parts meet no part has a corner: the sheets have to meet
    there all the same, or the water runs out along every corner (it did:
    two thirds of it in 1.5 s). The walls are 400 high: the water climbs
    the far one to 270."""

    def setUp(self):
        found = backendNamed("SPlisHSPlasH")
        if found is None or not found["Available"]:
            self.skipTest("no SPlisHSPlasH on this machine")
        self.doc = App.newDocument("FluidPlates")

        def plate(name, size, at):
            box = self.doc.addObject("Part::Box", name)
            box.Length, box.Width, box.Height = size
            box.Placement.Base = App.Vector(*at)
            return box

        parts = [
            plate("Floor", (420, 120, 10), (-10, -10, -10)),
            plate("Left", (10, 120, 400), (-10, -10, 0)),
            plate("Right", (10, 120, 400), (400, -10, 0)),
            plate("Front", (400, 10, 400), (0, -10, 0)),
            plate("Back", (400, 10, 400), (0, 100, 0)),
        ]
        water = plate("Water", (100, 100, 100), (0, 0, 0))
        self.doc.recompute()
        self.study = FluidPlayback.makeStudy(self.doc, parts)
        FluidPlayback.addFluid(self.study, water)
        self.study.Backend = "SPlisHSPlasH"
        self.study.Resolution = 10
        self.study.TimeEnd = 1.0
        self.study.OutputInterval = 0.1
        self.doc.recompute()

    def tearDown(self):
        if hasattr(self, "doc"):
            App.closeDocument(self.doc.Name)

    def test_plates_hold_water(self):
        self.assertEqual(self.study.run(), 0, self.study.Status)
        frames = FluidPlayback.Frames(self.study)
        self.assertEqual(len(frames), 11)
        for index in range(len(frames)):
            points, _ = frames.frame(index)
            # all of it, and inside: a particle's middle comes to a wall's
            # face under the water's push and a little past it (measured:
            # 0.12 of the spacing), not through the wall
            self.assertEqual(len(points), 1000, f"frame {index}")
            for axis, (low, high) in enumerate(((0, 400), (0, 100), (0, 400))):
                self.assertGreater(float(points[:, axis].min()), low - 2.5, f"frame {index}")
                self.assertLess(float(points[:, axis].max()), high + 2.5, f"frame {index}")
