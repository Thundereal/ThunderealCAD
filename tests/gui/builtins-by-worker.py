"""Every built-in object of the install's documents is recomputed by a
worker, in the GUI (docs/WorkerProcess.md sec 73).

The reporter, 2026-11-26: "fix the code to make any built-in objects to
recompute in worker, the fallback is only for external addon code". The
sweep of Mod/Test/TestWorkerBuiltIns.py, run where the documents have view
providers, so that a worker's copy has the views as the GUI saves them:
every FCStd under the install's home (data/examples, the modules' test
fixtures) opened, every object touched, recomputed by a worker with
Workers/NoteGuiUp on. Claim: no built-in object made here, no job taken
back for one, none failed -- each fault named with its document, what it
used and where. WORKER_SWEEP_GLOB narrows it, WORKER_SWEEP_OUT keeps the
report (GT_OUT/sweep.json by default).
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def wait(pool, seconds):
    end = time.time() + seconds
    while pool.recomputing() and time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)
    for _ in range(20):
        QtWidgets.QApplication.processEvents()
    return not pool.recomputing()


def dismiss():
    """A box a document's open raises (a missing file, an old format): no
    one is there to answer it"""
    box = QtWidgets.QApplication.activeModalWidget()
    if box is not None:
        note("INFO dismissed: %s" % (box.windowTitle(),))
        box.close()


def run():
    try:
        import TestWorkerBuiltIns as T

        os.environ.setdefault("WORKER_SWEEP_OUT", os.path.join(OUT, "sweep.json"))
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
            "WarnRecomputeOnRestore", False)
        sweeper = QtCore.QTimer()
        sweeper.timeout.connect(dismiss)
        sweeper.start(500)
        report = T.sweep(T.documents(), wait,
                         lambda p: note("INFO sweeping %s" % os.path.relpath(p, FreeCAD.getHomePath())))
        sweeper.stop()
        swept = [e for e in report if "skipped" not in e]
        for e in report:
            if "skipped" in e:
                note("INFO skipped %s: %s" % (os.path.basename(e["file"]), e["skipped"]))
        bad = T.faults(report)
        for line in bad:
            note("INFO fault: " + line)
        check("documents swept", len(swept) > 0, "%d of %d" % (len(swept), len(report)))
        check("every built-in object made by a worker", bad == [], "%d faults" % len(bad))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
