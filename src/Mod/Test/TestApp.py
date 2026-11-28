# ***************************************************************************
# *   Copyright (c) 2002 Juergen Riegel <juergen.riegel@web.de>             *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful,            *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with FreeCAD; if not, write to the Free Software        *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************/

import FreeCAD
import os
import sys
import tempfile
import unittest


# ---------------------------------------------------------------------------
# define the functions to test the FreeCAD base code
# ---------------------------------------------------------------------------


def tryLoadingTest(testName):
    "Loads and returns testName, or a failing TestCase if unsuccessful."

    try:
        return unittest.defaultTestLoader.loadTestsFromName(testName)

    except ImportError:

        class LoadFailed(unittest.TestCase):
            def __init__(self, testName):
                # setattr() first, because TestCase ctor checks for methodName.
                setattr(self, "failed_to_load_" + testName, self._runTest)
                super(LoadFailed, self).__init__("failed_to_load_" + testName)
                self.testName = testName

            def __name__(self):
                return "Loading " + self.testName

            def _runTest(self):
                self.fail("Couldn't load " + self.testName)

        return LoadFailed(testName)


def All():
    # Registered tests
    tests = FreeCAD.__unit_test__

    suite = unittest.TestSuite()

    for test in tests:
        suite.addTest(tryLoadingTest(test))

    return suite


def PrintAll():
    # Registered tests
    tests = FreeCAD.__unit_test__

    suite = unittest.TestSuite()

    FreeCAD.Console.PrintMessage("\nRegistered test units:\n\n")
    for test in tests:
        FreeCAD.Console.PrintMessage(("%s\n" % test))
    FreeCAD.Console.PrintMessage("\nPlease choose one or use 0 for all\n")

    return suite


class _TempRootUnchanged(unittest.TestCase):
    """What the temp guard's failure is booked against"""

    def runTest(self):
        pass


class TempGuard:
    """What a test run leaves in the temporary folders: FC_TEST_TEMP_GUARD
    set to "report" lists every entry that is new in them after the run;
    "fail" also fails the run naming them. Unset, nothing is looked at.

    Meant for a run whose temporary folders are its own -- TMPDIR (TEMP on
    Windows) and FREECAD_USER_TEMP pointed at an empty folder -- since what
    the system or another process writes in a shared one would be counted."""

    def __init__(self):
        self.mode = os.environ.get("FC_TEST_TEMP_GUARD", "").strip().lower()
        self.before = {}
        if not self.mode:
            return
        for root in (FreeCAD.getTempPath(), tempfile.gettempdir()):
            root = os.path.realpath(root)
            if root not in self.before and os.path.isdir(root):
                self.before[root] = set(os.listdir(root))

    def check(self, result):
        if not self.mode:
            return
        left = []
        for root, names in self.before.items():
            for name in sorted(set(os.listdir(root)) - names):
                left.append(os.path.join(root, name))
        print("TEMP GUARD (%s): %d new entries in %s" % (
            self.mode, len(left), ", ".join(self.before)))
        for path in left:
            print("TEMP GUARD left: %s" % path)
        if left and self.mode == "fail":
            result.errors.append((
                _TempRootUnchanged(),
                "the run left %d entries in the temporary folders: %s"
                % (len(left), ", ".join(os.path.basename(p) for p in left[:20]))))


def TestText(s):
    guard = TempGuard()
    s = unittest.defaultTestLoader.loadTestsFromName(s)
    r = unittest.TextTestRunner(stream=sys.stdout, verbosity=2)
    retval = r.run(s)
    guard.check(retval)
    # Flushing to make sure the stream is written to the console
    # before the wrapping process stops executing. Without this line
    # executing the tests from command line did not show stats
    # and proper traceback in some cases.
    sys.stdout.flush()
    return retval


def Test(s):
    TestText(s)


def testAll():
    r = unittest.TextTestRunner(stream=sys.stdout, verbosity=2)
    return r.run(All())


def testUnit():
    TestText(unittest.TestLoader().loadTestsFromName("UnitTests"))


def testDocument():
    suite = unittest.TestSuite()
    suite.addTest(unittest.defaultTestLoader.loadTestsFromName("Document"))
    TestText(suite)
