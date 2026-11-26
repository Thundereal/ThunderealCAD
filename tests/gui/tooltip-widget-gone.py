"""A tool tip asked for by a widget that is gone before the tip shows.

FreeCAD's own tool tip (Gui::ToolTip, used whenever View/ToolTipIconSize is
above 0, 64 by default) waits 80 ms before it shows. It keeps the widget that
asked in a QPointer; when that widget was destroyed in those 80 ms the pointer
was null, and the show timer took the branch for "no widget" -- which asked
the widget for its screen. A crash in QWidget::screen() from the timer, seen
under a clone of the reporter's profile in ServeDatumInPlace and
ServeDatumMeasure (docs/SmokeTesting.md, step 1).

Measured here: a widget with a tool tip is sent the tool tip event, as a hover
does, and deleted at once; 300 ms on the program still runs and shows no tip
for it. And a widget that stays shows its tip, so the first check is not a
tip that never shows at all. Before the fix the second case ends the program.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

try:
    import shiboken6 as shiboken
except ImportError:
    import shiboken2 as shiboken

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def tips():
    """How many of FreeCAD's tip labels are on screen (their text is drawn, not
    the label's text())"""
    return len([w for w in QtWidgets.QApplication.topLevelWidgets()
                if w.metaObject().className() == "Gui::TipLabel" and w.isVisible()])


def hide():
    """Take the tip down as a pre-selection taken away does"""
    FreeCADGui.Selection.setPreselection(state["box"], "Face1")
    FreeCADGui.Selection.clearPreselection()
    pump(0.3)


def hover(widget):
    """The tool tip event a hover sends, at the widget's middle"""
    local = widget.rect().center()
    event = QtGui.QHelpEvent(QtCore.QEvent.ToolTip, local, widget.mapToGlobal(local))
    QtWidgets.QApplication.sendEvent(widget, event)


def make(text):
    w = QtWidgets.QPushButton("tip")
    w.setToolTip(text)
    w.resize(120, 40)
    w.move(200, 200)
    w.show()
    pump(0.3)
    return w


def run():
    try:
        size = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").GetInt(
            "ToolTipIconSize", 64)
        check("FreeCAD's own tool tip is the one in use", size > 0, size)

        # Gui::ToolTip watches the application's events from its first use
        # on; a pre-selection taken away is one (it hides the tip), as a
        # hover over the 3D view does in a session
        doc = FreeCAD.newDocument("TooltipWidgetGone")
        box = doc.addObject("Part::Box", "Box")
        doc.recompute()
        state["box"] = box
        hide()

        w = make("still here")
        hover(w)
        pump(0.4)
        shown = tips()
        check("a widget that stays shows its tip", shown == 1, shown)
        hide()
        check("and the tip goes", tips() == 0, tips())
        w.close()
        shiboken.delete(w)
        pump(0.3)

        w = make("gone at once")
        hover(w)
        shiboken.delete(w)  # within the 80 ms the tip waits
        pump(0.4)
        shown = tips()
        check("a widget gone before its tip shows: still running, no tip for it",
              shown == 0, shown)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
