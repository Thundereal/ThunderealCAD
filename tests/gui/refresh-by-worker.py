"""Std_Refresh hands the recompute to a worker, by default, and not when
the setting says no.

docs/WorkerProcess.md sec 19 and 20, the user 2026-11-18: "make it default
gated by a setting". RecomputeCommand of Preferences/Workers ("Recompute in
a worker process" in the Document preferences) is on unless set off.
Claims:
  - with the setting as it comes, Std_Refresh returns with the recompute
    still with a worker, the document still touched;
  - its result is put in: nothing touched after, the cut's volume that of
    the edited box, nothing executed here;
  - set off, Std_Refresh recomputes here at once.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part  # noqa: F401
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
WORKERS = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Workers")


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


def touched(doc):
    return sorted(o.Name for o in doc.Objects if "Touched" in o.State)


def run():
    try:
        WORKERS.RemBool("RecomputeCommand")
        doc = FreeCAD.newDocument("RefreshByWorker")
        box = doc.addObject("Part::Box", "Box")
        tool = doc.addObject("Part::Cylinder", "Tool")
        tool.Radius = 2
        tool.Height = 30
        tool.Placement.Base = FreeCAD.Vector(5, 5, -5)
        cut = doc.addObject("Part::Cut", "Cut")
        cut.Base = box
        cut.Tool = tool
        doc.recompute()
        pump(1.0)
        pool = FreeCAD.workerPool(doc)

        box.Length = 14
        FreeCADGui.runCommand("Std_Refresh")
        check("by default, Std_Refresh hands it to a worker", pool.recomputing(),
              pool.recomputeInfo())
        check("... and returns with the document still touched", touched(doc) == ["Box"],
              touched(doc))
        end = time.time() + 120
        while pool.recomputing() and time.time() < end:
            pump(0.05)
        info = pool.recomputeInfo()
        check("its result is put in", info["Applied"] == 1 and touched(doc) == [],
              (info, touched(doc)))
        check("... nothing executed here", info["ExecutedHere"] == 0, info)
        want = 14 * 10 * 10 - 3.141592653589793 * 4 * 10
        check("... the cut's volume that of the edited box", abs(cut.Shape.Volume - want) < 1e-6,
              cut.Shape.Volume)

        WORKERS.SetBool("RecomputeCommand", False)
        box.Length = 15
        FreeCADGui.runCommand("Std_Refresh")
        check("set off, Std_Refresh recomputes here at once",
              not pool.recomputing() and touched(doc) == [] and pool.recomputeInfo()["Started"] == 1,
              (pool.recomputeInfo(), touched(doc)))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        WORKERS.RemBool("RecomputeCommand")
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
