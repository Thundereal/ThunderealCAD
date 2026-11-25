"""The recompute at the end of an edit -- a PartDesign dialog's OK, a
sketch's edit left -- makes the edited object here and, when what uses it
is long to make, hands that to a worker.

docs/WorkerProcess.md sec 31, the user 2026-11-20: "work on the rest of
items" (a task panel's OK). Claims:
  - a Pad's dialog, its Length changed, OK: the dialog is closed at once,
    the Pad made here, and what reads it -- a Python feature whose
    execute() took 1 s -- with a worker, its result coming from the
    worker's process;
  - a sketch's edit left after a change: the sketch made here, the rest
    by a worker the same way;
  - with nothing long after it, OK makes it all here at once.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

import Part  # noqa: F401
import Sketcher
import TestWorkerPool

OUT = os.path.realpath(os.environ["GT_OUT"])
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = FreeCAD.Vector


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


def typeInto(name, text):
    """`text` typed into the panel's field `name`, Return pressed"""
    from PySide import QtGui

    box = [w for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QAbstractSpinBox, name)
           if w.isVisible()][0]
    box.setFocus()
    box.selectAll()
    for ch in text:
        QtWidgets.QApplication.sendEvent(
            box, QtGui.QKeyEvent(QtCore.QEvent.KeyPress, getattr(QtCore.Qt, "Key_" + ch),
                                 QtCore.Qt.NoModifier, ch))
    QtWidgets.QApplication.sendEvent(box, QtGui.QKeyEvent(QtCore.QEvent.KeyPress,
                                                           QtCore.Qt.Key_Return,
                                                           QtCore.Qt.NoModifier))


def waitWorker(pool):
    end = time.time() + 60
    while pool.recomputing() and time.time() < end:
        pump(0.05)


def run():
    try:
        doc = FreeCAD.newDocument("EditEnd")
        body = doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        sketch.AttachmentSupport = (doc.getObject("XY_Plane"), [""])
        sketch.MapMode = "FlatFace"
        pts = [V(0, 0, 0), V(10, 0, 0), V(10, 10, 0), V(0, 10, 0)]
        for i in range(4):
            sketch.addGeometry(Part.LineSegment(pts[i], pts[(i + 1) % 4]), False)
        for i in range(4):
            sketch.addConstraint(Sketcher.Constraint("Coincident", i, 2, (i + 1) % 4, 1))
        sketch.addConstraint(Sketcher.Constraint("DistanceX", 0, 1, 0, 2, 10))
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 5
        slow = doc.addObject("App::FeaturePython", "Slow")
        TestWorkerPool._Counted(slow)
        slow.Sleep = 1.0
        slow.setExpression("Input", "Pad.Shape.Volume")
        doc.recompute()  # each made once here: what it takes is known
        pool = FreeCAD.workerPool(doc)
        check("made here first", slow.Result == 1000 and slow.MadeBy == os.getpid(),
              (slow.Result, slow.MadeBy))

        # A Pad's dialog
        FreeCADGui.ActiveDocument.setEdit(pad.Name)
        pump(1.0)
        typeInto("lengthEdit", "7")
        pump(1.0)  # the panel's preview made
        began = time.time()
        FreeCADGui.Control.activeTaskDialog().accept()
        took = time.time() - began
        pump(0.2)
        check("the Pad's OK closed the dialog at once",
              FreeCADGui.Control.activeDialog() is False and took < 0.6, took)
        check("... the Pad made here", abs(pad.Shape.Volume - 700) < 1e-6
              and "Touched" not in pad.State, (pad.Shape.Volume, pad.State))
        check("... what reads it with a worker", pool.recomputing() and slow.Result == 1000,
              (pool.recomputeInfo(), slow.Result))
        waitWorker(pool)
        check("... and its result the worker's",
              slow.Result == 1400 and slow.MadeBy not in (0, os.getpid())
              and not [o.Name for o in doc.Objects if "Touched" in o.State],
              (slow.Result, slow.MadeBy, [o.State for o in doc.Objects]))

        # A sketch's edit left
        FreeCADGui.ActiveDocument.setEdit(sketch.Name)
        pump(1.0)
        sketch.setDatum(4, FreeCAD.Units.Quantity("12 mm"))
        pump(0.3)
        began = time.time()
        FreeCADGui.ActiveDocument.resetEdit()
        took = time.time() - began
        pump(0.2)
        check("leaving the sketch came back at once", took < 0.6, took)
        check("... the sketch made here", "Touched" not in sketch.State
              and abs(sketch.Shape.BoundBox.XLength - 12) < 1e-6, sketch.State)
        check("... the rest with a worker", pool.recomputing(), pool.recomputeInfo())
        waitWorker(pool)
        # Coincidences only: the bottom edge made 12 long, the square goes
        # askew -- the Pad what the sketch is, and Slow what the Pad is
        check("... made by it: the Pad from the sketch, Slow from the worker",
              abs(pad.Shape.Volume - 700) > 1 and slow.Result == 2 * round(pad.Shape.Volume)
              and slow.MadeBy not in (0, os.getpid())
              and not [o.Name for o in doc.Objects if "Touched" in o.State],
              (pad.Shape.Volume, slow.Result, slow.MadeBy))

        # Nothing long after it
        slow.Sleep = 0
        doc.recompute()
        FreeCADGui.ActiveDocument.setEdit(pad.Name)
        pump(1.0)
        typeInto("lengthEdit", "9")
        pump(1.0)
        FreeCADGui.Control.activeTaskDialog().accept()
        pump(0.2)
        check("nothing long after: made here at once",
              not pool.recomputing() and slow.Result == 2 * round(pad.Shape.Volume)
              and slow.MadeBy == os.getpid() and pad.Length.Value == 9,
              (slow.Result, slow.MadeBy))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
