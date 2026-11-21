"""An object whose recompute crashes a worker process costs the worker, not
the application (docs/WorkerProcess.md sec 34).

A chain Before -> Crash -> After, Crash a Python feature whose execute()
takes its process down by a segmentation fault -- in a worker; run in this
process it raises instead, which the test takes for its having been run here.
Claims, through the Recompute command:
  - the recompute goes to a worker, which dies in Crash, twice; this process
    goes on;
  - Crash is left in an error that says it crashed a worker and is not
    recomputed here, shown in the Report view and on the object, and
    nothing ran it here; the line names the crash log, which holds the
    worker's stack (sec 35);
  - Recompute again: Crash is not run here nor in a worker (no worker
    dies), Before is made;
  - Crash changed (its crash fixed): Recompute makes it, in a worker, and
    what reads it.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

import TestWorkerPool

OUT = os.path.realpath(os.environ["GT_OUT"])
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def report():
    view = FreeCADGui.getMainWindow().findChild(QtWidgets.QTextEdit, "Report view")
    return view.toPlainText() if view else ""


def recompute(pool):
    FreeCADGui.runCommand("Std_Refresh")
    pump(0.3)
    end = time.time() + 120
    while pool.recomputing() and time.time() < end:
        pump(0.05)
    pump(0.5)


def run():
    try:
        doc = FreeCAD.newDocument("CrashIsolated")
        before = doc.addObject("App::FeaturePython", "Before")
        TestWorkerPool._Counted(before)
        before.Input = 2
        crash = doc.addObject("App::FeaturePython", "Crash")
        TestWorkerPool._Crashing(crash)
        crash.setExpression("Input", "Before.Result")
        after = doc.addObject("App::FeaturePython", "After")
        TestWorkerPool._Counted(after)
        after.setExpression("Input", "Crash.Result")
        pool = FreeCAD.workerPool(doc)
        TestWorkerPool._executions.pop("crashed here", None)

        recompute(pool)
        info = pool.recomputeInfo()
        check("the recompute went to a worker, which died in Crash",
              info["Failed"] == 1 and "at Crash" in info["Reason"], info)
        check("... this process goes on, Crash left in its error",
              "Invalid" in crash.State and info["Crashed"] == ["Crash"], (crash.State, info))
        text = report()
        lines = [line for line in text.splitlines() if "crashed a worker process" in line]
        check("... said in the Report view, Crash named",
              lines and "Crash" in lines[0] and "not recomputed in this process" in lines[0],
              lines[:2])
        check("... and on the object", "crashed a worker process" in crash.getStatusString(),
              crash.getStatusString())
        # What was on the worker's stack, in the crash log the line names
        # (docs/WorkerProcess.md sec 35)
        if os.name == "nt":
            # No crash handler in a Windows worker yet: no stack, no log named
            check("... a real crash, no stack (no handler on Windows)",
                  lines and "crashed (0xc0000005)" in lines[0]
                  and "stack is in" not in lines[0], lines[:1])
        else:
            log = lines[0].split("What was on the worker's stack is in ")[-1].strip() if lines else ""
            text = open(log, errors="replace").read() if log and os.path.exists(log) else ""
            check("... which names the crash log, holding the worker's stack",
                  "crashed making job" in text and "Python:" in text and "in execute" in text,
                  (log, text[-400:]))
        check("... and nothing ran it here", "crashed here" not in TestWorkerPool._executions,
              TestWorkerPool._executions)

        failed = pool.info()["Failed"]
        recompute(pool)
        check("Recompute again: no worker dies, nothing runs it here",
              pool.info()["Failed"] == failed
              and "crashed here" not in TestWorkerPool._executions
              and "Invalid" in crash.State, (pool.info()["Failed"], failed, crash.State))
        check("... and Before is made", before.Result == 4, before.Result)

        doc.openTransaction("Fix it")
        crash.Crash = False
        doc.commitTransaction()
        check("a change to it lifts its error", pool.recomputeInfo()["Crashed"] == []
              and crash.isValid(), (pool.recomputeInfo()["Crashed"], crash.State))
        recompute(pool)
        check("... Recompute makes it in a worker, and what reads it",
              crash.Result == 8 and crash.MadeBy not in (0, os.getpid())
              and after.Result == 16 and "Invalid" not in crash.State,
              (crash.Result, crash.MadeBy, after.Result, crash.State))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
