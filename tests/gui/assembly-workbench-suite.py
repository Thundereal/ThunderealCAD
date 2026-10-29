"""The Assembly workbench's tests, in the GUI.

src/Mod/Assembly/TestAssemblyWorkbench.py lists every Assembly test module;
run headless, the GUI ones skip. Here the whole list runs with the
workbench active, so the panels, the tree and the 3D view are there.
GT_ASSEMBLY_TESTS names one module or case instead (a dotted name, e.g.
AssemblyTests.TestGuiCommands).

Each unittest case is written as a PASS or FAIL line, a skipped one as SKIP
with its reason: some need a sample file that is not given. Deferred deletes
are flushed after each case, as a long run of task panels otherwise piles
up widgets to Windows' cap of GDI objects.
"""
import os
import traceback
import unittest

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


class Result(unittest.TextTestResult):
    def stopTest(self, test):
        super().stopTest(test)
        app = QtCore.QCoreApplication.instance()
        app.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        app.processEvents()

    def addSuccess(self, test):
        super().addSuccess(test)
        note("PASS " + test.id())

    def addFailure(self, test, err):
        super().addFailure(test, err)
        note("FAIL " + test.id() + " | " + str(err[1])[:2000])

    def addError(self, test, err):
        super().addError(test, err)
        note("FAIL " + test.id() + " | " + "".join(traceback.format_exception(*err))[-2000:])

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        note("SKIP " + test.id() + " | " + reason)


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
            "AutoSaveEnabled", False
        )
        FreeCADGui.activateWorkbench("AssemblyWorkbench")
        name = os.environ.get("GT_ASSEMBLY_TESTS")
        if name:
            import TestAssemblyWorkbench  # noqa: F401, the modules it imports

            suite = unittest.defaultTestLoader.loadTestsFromName(name)
        else:
            import TestAssemblyWorkbench

            suite = unittest.defaultTestLoader.loadTestsFromModule(TestAssemblyWorkbench)
        with open(os.path.join(OUT, "unittest.txt"), "w") as stream:
            result = unittest.TextTestRunner(
                stream=stream, resultclass=Result, verbosity=2
            ).run(suite)
        note(
            "RAN %d, %d failures, %d errors, %d skipped"
            % (result.testsRun, len(result.failures), len(result.errors), len(result.skipped))
        )
        if result.testsRun == 0:
            note("FAIL no test ran")
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
