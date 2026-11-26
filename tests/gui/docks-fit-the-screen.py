"""The Animation timeline and the Transaction log, docked side by side, leave
the main window narrow enough for a 1920 screen.

Under a clone of the reporter's profile the main window asked Windows for a
minimum width of 2979 to 3355 px on its 1920 px screen (docs/SmokeTesting.md,
step 1): with the docks in place, the timeline asked for 1446 px -- its row
of controls laid out flat, some twenty widgets -- and the transaction log for
1334, its status line a label that did not wrap.

Measured here, both docks shown and docked in the bottom area side by side,
the log's status line given a long text (a comparison's summary is that
long): the main window's minimum width is under 1920, and neither dock asks
for more than 600. The timeline's row keeps its look: with the dock wide it
does not scroll and is as high as its controls; narrowed below the row it
scrolls sideways, every control still there. Before the change the first
claim fails (the two docks alone ask for some 2800 px).
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
state = {"done": False}
LONG = ("Compared with the branch: 14 transactions only here, 9 only there, 3 in both "
        "with different results -- the property lists of Pad, Pocket001, Fillet, "
        "Sketch003 and LinearPattern differ; the report view says which.")


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


def dock(mw, inner_class):
    for d in mw.findChildren(QtWidgets.QDockWidget):
        w = d.widget()
        if w is not None and w.metaObject().className() == inner_class:
            return d
    return None


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showNormal()
        settle(0.3)
        timeline = dock(mw, "Gui::DockWnd::AnimationTimeline")
        log = dock(mw, "Gui::DockWnd::TransactionLogView")
        if not check("both docks are there", timeline is not None and log is not None,
                     (timeline, log)):
            return
        for d in (timeline, log):
            d.setFloating(False)
            d.show()
        mw.addDockWidget(Qt.BottomDockWidgetArea, timeline)
        mw.splitDockWidget(timeline, log, Qt.Horizontal)
        settle(0.5)
        labels = [l for l in log.widget().findChildren(QtWidgets.QLabel)
                  if l.parentWidget() is log.widget()]
        for l in labels:
            if not l.text():
                l.setText(LONG)  # the status line, empty until something is said
        settle(0.5)
        check("both shown, docked in the bottom area",
              all(d.isVisible() and not d.isFloating()
                  and mw.dockWidgetArea(d) == Qt.BottomDockWidgetArea for d in (timeline, log)))
        widths = (timeline.minimumSizeHint().width(), log.minimumSizeHint().width())
        check("neither dock asks for more than 600 px", max(widths) <= 600, widths)
        need = mw.minimumSizeHint().width()
        check("the main window fits a 1920 screen", need < 1920, need)

        # The row keeps its look when there is room, and scrolls when not
        row = next((a for a in timeline.widget().findChildren(QtWidgets.QScrollArea)
                    if a.widget() is not None
                    and a.widget().findChildren(QtWidgets.QDoubleSpinBox)), None)
        if check("the timeline's controls are in one row", row is not None):
            # floated, so the dock takes the width asked of it whatever the
            # screen and the other docks leave
            wide = row.widget().sizeHint().width() + 200
            timeline.setFloating(True)
            timeline.resize(wide, 200)
            settle(0.8)
            bar = row.horizontalScrollBar()
            check("wide: the row does not scroll", not bar.isVisible(), (row.width(), wide))
            check("and is as high as its controls",
                  row.height() == row.widget().sizeHint().height(),
                  (row.height(), row.widget().sizeHint().height()))
            timeline.resize(300, 200)
            settle(0.8)
            check("narrow: the row scrolls sideways", bar.isVisible(), row.width())
            boxes = row.widget().findChildren(QtWidgets.QDoubleSpinBox)
            check("every control still there", len(boxes) == 5, len(boxes))
            timeline.setFloating(False)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
