"""Entering and leaving an edit does not restyle the whole task view.

At each edit's start and end the task view sets its panels' scheme again,
and every ActionPanel, TaskGroup and TaskHeader set its style sheet again --
the same sheet -- which Qt answers by repolishing the whole subtree: some
3300 style changes on leaving a sketch. Without an application style sheet
each is cheap; with one (the reporter's Dark theme, FreeCAD.qss) entering a
sketch took 1.7 s and leaving it 1.6 s, against 0.8 and 0.3, and under a
clone of the reporter's profile GuiSketchSymmetricElement ran out of its
150 s (docs/SmokeTesting.md, step 1).

Counted here, not timed, so it holds whatever the machine and the sheet:
the StyleChange events Qt delivers while a sketch goes into edit and while
it leaves it, three sketches, the main loop running between them. Each
count stays under 1500. Before the change: about 2900 to enter and 3300 to
leave.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
LIMIT = 1500
state = {"done": False, "n": 0, "doc": None}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


class Counter(QtCore.QObject):
    def __init__(self):
        super().__init__()
        self.on = False
        self.n = 0

    def eventFilter(self, obj, event):
        if self.on and event.type() == QtCore.QEvent.StyleChange:
            self.n += 1
        return False


counter = Counter()


def counted(fn):
    counter.n = 0
    counter.on = True
    start = time.perf_counter()
    fn()
    for _ in range(3):
        QtWidgets.QApplication.processEvents()
    counter.on = False
    return counter.n, time.perf_counter() - start


def step():
    try:
        import Part
        import SketcherGui  # noqa: F401  -- registers the sketch's edit

        if state["doc"] is None:
            QtWidgets.QApplication.instance().installEventFilter(counter)
            state["doc"] = FreeCAD.newDocument("TaskViewNoRestyle")
        doc = state["doc"]
        n = state["n"]
        sk = doc.addObject("Sketcher::SketchObject", "Sketch%d" % n)
        sk.addGeometry(Part.LineSegment(FreeCAD.Vector(2, 3, 0), FreeCAD.Vector(8, 7, 0)), False)
        doc.recompute()
        enter, t_enter = counted(lambda: FreeCADGui.ActiveDocument.setEdit(sk, 0))
        check("sketch %d: entering the edit: fewer than %d style changes" % (n, LIMIT),
              enter < LIMIT, "%d style changes, %.2f s" % (enter, t_enter))
        leave, t_leave = counted(lambda: FreeCADGui.ActiveDocument.resetEdit())
        check("sketch %d: and leaving it" % n, leave < LIMIT,
              "%d style changes, %.2f s" % (leave, t_leave))
        state["n"] += 1
        if state["n"] < 3:
            # back to the main loop, where the closed panels are deleted
            QtCore.QTimer.singleShot(500, step)
            return
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    QtWidgets.QApplication.instance().removeEventFilter(counter)
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, step)
