"""A worker's recompute whose put-in the user stops (docs/WorkerProcess.md
sec 77, sec 78).

The put-in is a recompute here, with the worker's values supplied, and the
progress bar's Escape stops it as it stops any: the recompute throws an
abort part way. Here Escape is pressed as the first value goes in, and the
question that follows answered Yes. Claims:
  - the question is asked, and the put-in stops: counted a drop, not put
    in, the reason said;
  - the GUI does not then recompute here, as it does for a failure;
  - the workers are let go: their copies are where the whole result would
    have brought the document;
  - the next Std_Refresh is a worker's, from a new one, and comes out right:
    nothing touched, the cut's volume that of the edited box.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part  # noqa: F401
from PySide import QtCore, QtGui, QtWidgets

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


class Stopper:
    """Presses Escape as the first value of the put-in goes in"""

    def __init__(self, doc):
        self.doc = doc
        self.armed = False
        self.pressed = []
        self.asked = []
        self.timer = QtCore.QTimer()
        self.timer.setInterval(20)
        self.timer.timeout.connect(self.answer)

    def slotChangedObject(self, obj, prop):
        if not self.armed or obj.Document != self.doc or prop != "Shape":
            return
        self.armed = False
        self.pressed.append(obj.Name)
        self.timer.start()
        event = QtGui.QKeyEvent(QtCore.QEvent.KeyPress, QtCore.Qt.Key_Escape,
                                QtCore.Qt.NoModifier)
        QtWidgets.QApplication.sendEvent(FreeCADGui.getMainWindow(), event)

    def answer(self):
        box = QtWidgets.QApplication.activeModalWidget()
        if isinstance(box, QtWidgets.QMessageBox):
            self.asked.append(box.text())
            self.timer.stop()
            box.button(QtWidgets.QMessageBox.Yes).click()


def run():
    stopper = None
    try:
        WORKERS.RemBool("RecomputeCommand")
        doc = FreeCAD.newDocument("PutInStopped")
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
        stopper = Stopper(doc)
        FreeCAD.addDocumentObserver(stopper)

        box.Length = 14
        FreeCADGui.runCommand("Std_Refresh")
        check("Std_Refresh hands it to a worker", pool.recomputing(), pool.recomputeInfo())
        stopper.armed = True
        pids = []
        end = time.time() + 120
        while pool.recomputing() and time.time() < end:
            pids = pool.info()["Pids"] or pids
            pump(0.05)
        stopper.armed = False
        info = pool.recomputeInfo()
        check("Escape is pressed in the put-in, and the question asked",
              len(stopper.pressed) == 1 and len(stopper.asked) == 1,
              (stopper.pressed, stopper.asked))
        check("the put-in stops: a drop, not put in, the reason said",
              info["Cancelled"] == 1 and info["Applied"] == 0
              and info["Reason"] == "putting it in was stopped", info)
        check("... the workers are let go", pool.info()["Spoiled"] >= 1, pool.info())
        left = touched(doc)
        pump(1.0)
        info = pool.recomputeInfo()
        check("the GUI does not recompute here after it",
              not pool.recomputing() and info["Started"] == 1 and touched(doc) == left
              and left != [], (info, left, touched(doc)))

        FreeCADGui.runCommand("Std_Refresh")
        check("the next Std_Refresh is a worker's", pool.recomputing(), pool.recomputeInfo())
        end = time.time() + 120
        while pool.recomputing() and time.time() < end:
            pump(0.05)
        info = pool.recomputeInfo()
        want = 14 * 10 * 10 - 3.141592653589793 * 4 * 10
        check("... put in, nothing touched, the cut's volume that of the edited box",
              info["Applied"] == 1 and touched(doc) == [] and abs(cut.Shape.Volume - want) < 1e-6,
              (info, touched(doc), cut.Shape.Volume))
        check("... nothing executed here", info["ExecutedHere"] == 0, info)
        check("... made by a new worker", pids and not set(pool.info()["Pids"]) & set(pids),
              (pids, pool.info()["Pids"]))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        if stopper:
            FreeCAD.removeDocumentObserver(stopper)
        WORKERS.RemBool("RecomputeCommand")
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
