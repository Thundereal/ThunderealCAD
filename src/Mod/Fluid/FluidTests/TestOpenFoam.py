# SPDX-License-Identifier: LGPL-2.1-or-later

# The dam break run on OpenFOAM (docs/FluidSimulation.md sec 12, S2), where
# there is one on this machine: found by the preference Mod/Fluid/
# OpenFoamPath, the environment's FLUID_OPENFOAM_PATH or PATH, or in the
# usual places. Skipped where there is none.

import unittest

from FluidTests.DamBreak import DamBreak, G
from FluidTests.Pipe import Pipe
from FluidTests.StillWater import StillWater


class TestOpenFoamDamBreak(DamBreak, unittest.TestCase):
    BACKEND = "OpenFOAM"
    KIND = "Surface"

    def checkPressure(self, times, heights, pressures):
        super().checkPressure(times, heights, pressures)
        # at the start it is below the column's weight -- the water is
        # already falling
        weight = 998.2 * G * 0.098
        self.assertLess(pressures[0], 1.05 * weight)
        self.assertGreater(pressures[0], 0.3 * weight)


class TestOpenFoamStillWater(StillWater, unittest.TestCase):
    BACKEND = "OpenFOAM"
    KIND = "Surface"


class TestOpenFoamPipe(Pipe, unittest.TestCase):
    BACKEND = "OpenFOAM"
