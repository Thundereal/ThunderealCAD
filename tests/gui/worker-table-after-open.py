"""A worker's copy of a file opened in the GUI holds the file's strings.

docs/StringTableContentIds.md sec 13: the 3D view the GUI makes before an
open reads the file put its render properties' documentation in the
document's string table, ids 1 and up. The file's table, which has those
ids for its own strings, then disagreed with it, and the open kept the
document's table apart from the file's -- the one the transaction log
stores and a worker reads. The worker's copy of scanner.FCStd held 151
strings of the 41,048 here, and every result went in by content.

A cut of two boxes made with no view (as a file made elsewhere: scanner's
table was), saved, closed and opened again in the GUI, undo on;
claims:
  - the view has its render properties (what put the strings there);
  - a worker's copy holds every string the cut's names spell, with the
    same text as here;
  - a recompute by the worker goes in by id.
"""
import os
import re
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
PARAM = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")


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


def run():
    warn = PARAM.GetBool("WarnRecomputeOnRestore", True)
    try:
        import TestWorkerPool  # the job kinds, registered here and in a worker

        PARAM.SetBool("WarnRecomputeOnRestore", False)
        path = os.path.join(OUT, "cut.FCStd")
        # Made with no view, as a file made elsewhere is: its ids from 1
        # are the cut's strings
        doc = FreeCAD.newDocument("Cut", hidden=True)
        a = doc.addObject("Part::Box", "A")
        b = doc.addObject("Part::Box", "B")
        b.Placement.Base = FreeCAD.Vector(5, 5, 5)
        cut = doc.addObject("Part::Cut", "Cut")
        cut.Base = a
        cut.Tool = b
        doc.recompute()
        doc.saveAs(path)
        FreeCAD.closeDocument(doc.Name)
        pump(0.5)

        doc = FreeCAD.openDocument(path)
        pump(2)
        view = FreeCADGui.getDocument(doc.Name).ActiveView
        renders = [p for p in view.PropertiesList if p.startswith("Render_")] if view else []
        check("the view has its render properties", len(renders) > 0, len(renders))
        note("INFO undo %d, table %d strings" % (doc.UndoMode, doc.Hasher.Size))

        names = list(doc.getObject("Cut").Shape.ElementMap)
        spelled = sorted({int(x, 16) for k in names for x in re.findall(r"#([0-9a-f]+)", k)})
        here = {}
        for s in spelled:
            sid = doc.Hasher.getIDBySpelling(s)
            here[str(s)] = sid.Data if sid else None
        check("the cut's names spell strings of the table here",
              spelled and all(v is not None for v in here.values()), len(spelled))

        pool = FreeCAD.workerPool(doc)
        check("the pool starts", pool.start())
        job = pool.submit("test.table", {"Spelled": spelled}, module="TestWorkerPool")
        end = time.time() + 120
        while time.time() < end and not pool.wait(0.05):
            pump(0.05)
        result = pool.result(job)
        check("the job is done", result["Status"] == "Done", result)
        there = result["Output"]["Strings"]
        missing = [s for s in here if there.get(s) != here[s]]
        check("the worker holds every string the cut's names spell, as here",
              not missing, (len(missing), result["Output"]["Size"], doc.Hasher.Size))

        for o in doc.Objects:
            o.touch()
        check("the recompute is given to the worker", pool.recompute())
        end = time.time() + 120
        while time.time() < end and pool.recomputing():
            pump(0.05)
        info = pool.recomputeInfo()
        check("its result goes in by id", info["ById"], info["ByContentWhy"])
        pool.stop()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        PARAM.SetBool("WarnRecomputeOnRestore", warn)
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
