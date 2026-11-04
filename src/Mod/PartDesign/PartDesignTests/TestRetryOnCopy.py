# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileNotice: Part of the FreeCAD project.

################################################################################
#                                                                              #
#   FreeCAD is free software: you can redistribute it and/or modify            #
#   it under the terms of the GNU Lesser General Public License as             #
#   published by the Free Software Foundation, either version 2.1              #
#   of the License, or (at your option) any later version.                     #
#                                                                              #
#   FreeCAD is distributed in the hope that it will be useful,                 #
#   but WITHOUT ANY WARRANTY; without even the implied warranty                #
#   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                    #
#   See the GNU Lesser General Public License for more details.                #
#                                                                              #
#   You should have received a copy of the GNU Lesser General Public           #
#   License along with FreeCAD. If not, see https://www.gnu.org/licenses       #
#                                                                              #
################################################################################

"""PartDesign::Feature's RetryOnCopy: with shape values frozen (Part's
ImmutableShapeValues), a feature whose recompute fails is recomputed once more
on copies of its inputs -- for a failure the freeze causes that no refusal
reports. Off by default, and no retry where nothing is frozen."""

import unittest

import FreeCAD

PART = "User parameter:BaseApp/Preferences/Mod/Part"
FREEZE = "ImmutableShapeValues"


class FailsOnce:
    """Raises on its first run, as a Python feature fails; the shape of the
    base feature each run read is kept in runs."""

    def __init__(self, obj, runs):
        obj.Proxy = self
        self.runs = runs

    def execute(self, obj):
        base = obj.BaseFeature.Shape
        self.runs.append(base)
        if len(self.runs) == 1:
            raise RuntimeError("the first run fails")
        obj.Shape = base.copy()

    def dumps(self):
        return None

    def loads(self, state):
        return None


class TestRetryOnCopy(unittest.TestCase):
    def setUp(self):
        grp = FreeCAD.ParamGet(PART)
        self.had = FREEZE in grp.GetBools()
        self.old = grp.GetBool(FREEZE)
        self.doc = FreeCAD.newDocument("RetryOnCopy")

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)
        grp = FreeCAD.ParamGet(PART)
        if self.had:
            grp.SetBool(FREEZE, self.old)
        else:
            grp.RemBool(FREEZE)

    def recompute(self, frozen, retry):
        FreeCAD.ParamGet(PART).SetBool(FREEZE, frozen)
        body = self.doc.addObject("PartDesign::Body", "Body")
        box = self.doc.addObject("PartDesign::AdditiveBox", "Box")
        body.addObject(box)
        self.doc.recompute()
        feature = self.doc.addObject("PartDesign::FeaturePython", "FailsOnce")
        runs = []
        FailsOnce(feature, runs)
        body.addObject(feature)
        feature.RetryOnCopy = retry
        self.doc.recompute()
        return box, feature, runs

    def testOffByDefault(self):
        feature = self.doc.addObject("PartDesign::FeaturePython", "Feature")
        self.assertFalse(feature.RetryOnCopy)

    def testFrozenWithoutRetryFails(self):
        _, feature, runs = self.recompute(frozen=True, retry=False)
        self.assertIn("Invalid", feature.State)
        self.assertEqual(len(runs), 1)

    def testFrozenWithRetryRunsOnCopies(self):
        box, feature, runs = self.recompute(frozen=True, retry=True)
        self.assertNotIn("Invalid", feature.State)
        self.assertEqual(len(runs), 2)
        # the second run read a copy of the base, not the frozen value
        self.assertTrue(runs[0].isPartner(box.Shape))
        self.assertFalse(runs[1].isPartner(box.Shape))
        self.assertAlmostEqual(feature.Shape.Volume, box.Shape.Volume)

    def testUnfrozenNoRetry(self):
        _, feature, runs = self.recompute(frozen=False, retry=True)
        self.assertIn("Invalid", feature.State)
        self.assertEqual(len(runs), 1)
