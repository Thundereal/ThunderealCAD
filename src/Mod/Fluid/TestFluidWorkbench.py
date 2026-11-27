# SPDX-License-Identifier: LGPL-2.1-or-later

from FluidTests.TestStudy import (
    TestBackends,
    TestBodies,
    TestInlets,
    TestSteady,
    TestStudy,
    TestResults,
)
from FluidTests.TestDownload import TestDownload
from FluidTests.TestOpenFoam import TestOpenFoamDamBreak, TestOpenFoamStillWater
from FluidTests.TestSplish import (
    TestSplishDamBreak,
    TestSplishFloating,
    TestSplishJet,
    TestSplishMechanism,
    TestSplishPlates,
    TestSplishStillWater,
)
from FluidTests.TestPlayback import (
    TestDoubts,
    TestFrames,
    TestMovedParts,
    TestSetUp,
    TestFrameView,
)

# Use the modules so that code checkers don't complain (flake8)
True if TestBackends else False
True if TestStudy else False
True if TestResults else False
True if TestFrames else False
True if TestDoubts else False
True if TestSetUp else False
True if TestFrameView else False
True if TestOpenFoamDamBreak else False
True if TestDownload else False
True if TestSplishDamBreak else False
True if TestOpenFoamStillWater else False
True if TestSplishStillWater else False
True if TestSplishPlates else False
True if TestBodies else False
True if TestMovedParts else False
True if TestSplishFloating else False
True if TestInlets else False
True if TestSplishJet else False
True if TestSteady else False
True if TestSplishMechanism else False
