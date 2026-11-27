"""An edit in the property editor whose recompute is long goes to a worker;
a short one is made here at once, as before.

docs/WorkerProcess.md sec 28, the user 2026-11-19: "Go" (the callers of
the recompute by workers next). Claims:
  - an Input typed into the editor for an object whose execute() took 1 s
    when last made: the editor is back at once, the recompute with a
    worker, and the result comes -- Result twice the Input, made by the
    worker's process;
  - one undo takes back the edit and its result together;
  - an object that took no time: made here at once, by this process;
  - RecomputeEdits off: the slow one is made here.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

import TestWorkerPool

OUT = os.path.realpath(os.environ["GT_OUT"])
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


def find(model, parent, text):
    for row in range(model.rowCount(parent)):
        index = model.index(row, 0, parent)
        if str(model.data(index, QtCore.Qt.DisplayRole)) == text:
            return index
        if model.rowCount(index):
            inner = find(model, index, text)
            if inner is not None:
                return inner
    return None


def editor():
    for view in FreeCADGui.getMainWindow().findChildren(QtWidgets.QTreeView):
        if view.metaObject().className() == "Gui::PropertyEditor::PropertyEditor" \
                and view.model() is not None and find(view.model(), QtCore.QModelIndex(),
                                                      "Input") is not None:
            return view
    return None


def typeInto(obj, value):
    """Input typed into the property editor and Return pressed; the seconds
    it took to come back"""
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(obj)
    pump(0.5)
    view = editor()
    model = view.model()
    name = find(model, QtCore.QModelIndex(), "Input")
    index = model.index(name.row(), 1, name.parent())
    view.setCurrentIndex(index)
    # A click opens the editor and the edit's transaction
    view.clicked.emit(index)
    pump(0.3)
    box = view.indexWidget(index)
    spin = box if isinstance(box, QtWidgets.QAbstractSpinBox) else \
        (box.findChild(QtWidgets.QAbstractSpinBox) if box else None)
    if spin is None:
        spin = QtWidgets.QApplication.focusWidget()
    spin.setFocus()
    spin.selectAll()
    QtWidgets.QApplication.sendEvent(spin, QtGui.QKeyEvent(QtCore.QEvent.KeyPress,
                                                            QtCore.Qt.Key_A,
                                                            QtCore.Qt.ControlModifier))
    for ch in str(value):
        QtWidgets.QApplication.sendEvent(
            spin, QtGui.QKeyEvent(QtCore.QEvent.KeyPress, getattr(QtCore.Qt, "Key_" + ch),
                                  QtCore.Qt.NoModifier, ch))
    began = time.time()
    QtWidgets.QApplication.sendEvent(spin, QtGui.QKeyEvent(QtCore.QEvent.KeyPress,
                                                            QtCore.Qt.Key_Return,
                                                            QtCore.Qt.NoModifier))
    # Done once the editor closed its transaction: the recompute it calls
    # for made, or handed over
    end = time.time() + 30
    while FreeCAD.getActiveTransaction() and time.time() < end:
        QtWidgets.QApplication.processEvents()
    return time.time() - began


def run():
    try:
        WORKERS.RemBool("RecomputeEdits")
        doc = FreeCAD.newDocument("EditByWorker")
        doc.UndoMode = 1
        slow = doc.addObject("App::FeaturePython", "Slow")
        TestWorkerPool._Counted(slow)
        slow.Sleep = 1.0
        slow.Input = 1
        quick = doc.addObject("App::FeaturePython", "Quick")
        TestWorkerPool._Counted(quick)
        doc.recompute()  # each made once: what they take is known
        pool = FreeCAD.workerPool(doc)
        check("both made here first", slow.Result == 2 and slow.MadeBy == os.getpid())
        undos = list(doc.UndoNames)

        took = typeInto(slow, 7)
        check("the editor took the slow one's Input", slow.Input == 7, slow.Input)
        check("... came back at once", took < 0.5, took)
        check("... its recompute with a worker", pool.recomputing(), pool.recomputeInfo())
        check("... not made yet", slow.Result == 2, slow.Result)
        end = time.time() + 60
        while pool.recomputing() and time.time() < end:
            pump(0.05)
        check("the result comes, the worker's",
              slow.Result == 14 and slow.MadeBy not in (0, os.getpid())
              and "Touched" not in slow.State, (slow.Result, slow.MadeBy, slow.State))
        steps = [n for n in doc.UndoNames if n not in undos]
        check("one step for the edit", len(steps) == 1, doc.UndoNames)
        doc.undo()
        check("one undo takes back the edit and its result",
              slow.Input == 1 and slow.Result == 2 and "Touched" not in slow.State,
              (slow.Input, slow.Result, slow.State))

        took = typeInto(quick, 4)
        check("the quick one made here at once",
              quick.Result == 8 and quick.MadeBy == os.getpid() and not pool.recomputing(),
              (quick.Result, quick.MadeBy, took))

        WORKERS.SetBool("RecomputeEdits", False)
        took = typeInto(slow, 9)
        check("set off, the slow one made here",
              slow.Result == 18 and slow.MadeBy == os.getpid() and took >= 1.0
              and not pool.recomputing(), (slow.Result, slow.MadeBy, took))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        WORKERS.RemBool("RecomputeEdits")
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
