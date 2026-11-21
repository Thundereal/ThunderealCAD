# SPDX-License-Identifier: LGPL-2.1-or-later

from FluidTests.TestStudy import TestBackends, TestStudy, TestResults
from FluidTests.TestPlayback import TestFrames, TestSetUp, TestFrameView

# Use the modules so that code checkers don't complain (flake8)
True if TestBackends else False
True if TestStudy else False
True if TestResults else False
True if TestFrames else False
True if TestSetUp else False
True if TestFrameView else False
