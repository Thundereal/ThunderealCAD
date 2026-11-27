"""What a worker says through its Console is said here, in the Report view,
the worker named.

docs/WorkerProcess.md sec 27, the user 2026-11-19: "Go" (the worker log
relay next). Claims:
  - a job's warning and error show in the Report view as "worker N: ...",
    once its result came, and not before;
  - a job a worker cannot do (Unfit), done here instead: what the worker
    said is not shown.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.path.realpath(os.environ["GT_OUT"])
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
MODULE = "TestWorkerPool"


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


def run():
    try:
        doc = FreeCAD.newDocument("MessagesByWorker")
        doc.addObject("App::FeatureTest", "Feature")
        doc.recompute()
        pool = FreeCAD.workerPool(doc)
        check("the pool starts", pool.start(), pool.info()["Reason"])
        job = pool.submit("test.say", {"Say": [["Warning", "careful there"],
                                               ["Error", "that went wrong"]],
                                       "Seconds": 1.0}, module=MODULE)
        end = time.time() + 30
        while pool.state(job) != "Running" and time.time() < end:
            pump(0.05)
        pump(0.5)
        check("while it runs, nothing of it is shown", "careful there" not in report(),
              report()[-300:])
        end = time.time() + 60
        while (pool.info()["Busy"] or pool.info()["Queued"]) and time.time() < end:
            pump(0.05)
        pump(0.5)
        result = pool.result(job)
        text = report()
        lines = [line for line in text.splitlines() if "careful there" in line
                 or "that went wrong" in line]
        name = "worker %d: " % result["Messages"][0]["Worker"]
        check("its result came, with what it said", result["Status"] == "Done"
              and len(result["Messages"]) == 2, result)
        check("... said in the Report view, the worker named",
              [line[line.find("worker "):] for line in lines]
              == [name + "careful there", name + "that went wrong"], lines)

        job = pool.submit("test.say", {"Say": [["Warning", "not shown"]], "Unfit": True},
                          module=MODULE)
        end = time.time() + 60
        while (pool.info()["Busy"] or pool.info()["Queued"]) and time.time() < end:
            pump(0.05)
        pump(0.5)
        result = pool.result(job)
        check("an unfit job's words are not shown",
              result["Status"] == "Unfit" and "not shown" not in report(), result)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
