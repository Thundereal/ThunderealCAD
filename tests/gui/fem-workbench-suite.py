"""The FEM workbench's GUI tests (src/Mod/Fem/TestFemGui.py), in the GUI.

They need view providers and their scene graphs, which the headless run
does not have. GT_FEM_TESTS names one module or case instead (a dotted
name, e.g. femtest.gui.test_meshcolor).

Each unittest case is written as a PASS or FAIL line, a skipped one as SKIP
with its reason. Deferred deletes are flushed after each case, as in
assembly-workbench-suite.py.
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
        FreeCADGui.activateWorkbench("FemWorkbench")
        import TestFemGui

        name = os.environ.get("GT_FEM_TESTS")
        if name:
            suite = unittest.defaultTestLoader.loadTestsFromName(name)
        else:
            suite = unittest.defaultTestLoader.loadTestsFromModule(TestFemGui)
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
