"""An undo step an input's handling left open is closed at the end of its
group, unless something holds it (docs/MultiViewEdit.md sec 12.2 rules 1
and 5).

Claims:
  - a click whose handler sets a step and never closes it: the step is
    committed once the click's work has run, so a later click's change to
    another document does not join it;
  - a step set with persist (an editor that keeps its step over several
    inputs) stays open past its click;
  - a step set while a task dialog is open stays open: the dialog's;
  - a step set on a mouse press stays open while the button is down and
    is committed after the release;
  - a step set from a line typed into the Python console stays open, as
    a script's always did;
  - with Document/UndoGroupByInput off, the click's step stays open as
    before.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets
from PySide6.QtTest import QTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))

PARAM = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")
PARAM.SetBool("AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=200):
    end = QtCore.QDeadlineTimer(ms)
    while not end.hasExpired():
        QtWidgets.QApplication.processEvents(QtCore.QEventLoop.AllEvents, 20)


def setup():
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    a = FreeCAD.newDocument("GroupA")
    a.UndoMode = 1
    a.addObject("Part::Box", "Box")
    a.recompute()
    b = FreeCAD.newDocument("GroupB")
    b.UndoMode = 1
    b.addObject("Part::Box", "Box")
    b.recompute()
    a.clearUndos()
    b.clearUndos()
    FreeCAD.setActiveDocument(a.Name)
    settle()
    return a, b


def active():
    return FreeCAD.getActiveTransaction()


def lists(a, b):
    return "A %s B %s active %s" % (list(a.UndoNames), list(b.UndoNames), active())


def button(handler, signal="clicked"):
    btn = QtWidgets.QPushButton("go", FreeCADGui.getMainWindow())
    btn.setGeometry(0, 0, 40, 20)
    btn.show()
    getattr(btn, signal).connect(handler)
    return btn


def click(handler):
    btn = button(handler)
    QTest.mouseClick(btn, QtCore.Qt.LeftButton)
    btn.deleteLater()
    settle()


class Panel:
    def __init__(self):
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle("hold")

    def accept(self):
        return True

    def reject(self):
        return True


def run():
    try:
        a, b = setup()

        def loose():
            FreeCAD.setActiveTransaction("loose step")
            a.Box.Height = 20

        click(loose)
        check("a click's step left open is closed after its work",
              active() is None and not a.HasPendingTransaction
              and list(a.UndoNames) == ["loose step"], lists(a, b))

        def other():
            b.Box.Height = 30

        click(other)
        check("a later click's change does not join it",
              list(b.UndoNames) == [] and list(a.UndoNames) == ["loose step"], lists(a, b))
        FreeCAD.setActiveDocument(a.Name)
        FreeCADGui.runCommand("Std_Undo")
        check("one undo takes back the click's step only",
              a.Box.Height.Value == 10 and b.Box.Height.Value == 30,
              "A %s B %s" % (a.Box.Height.Value, b.Box.Height.Value))

        a, b = setup()

        def kept():
            FreeCAD.setActiveTransaction("kept step", True)
            a.Box.Height = 21

        click(kept)
        check("a step set with persist stays open past its click",
              active() is not None and active()[0] == "kept step", lists(a, b))
        FreeCAD.closeActiveTransaction()

        a, b = setup()
        FreeCADGui.Control.showDialog(Panel())
        settle()

        def inDialog():
            FreeCAD.setActiveTransaction("dialog step")
            a.Box.Height = 22

        click(inDialog)
        check("a step set while a task dialog is open stays open",
              active() is not None and active()[0] == "dialog step", lists(a, b))
        FreeCADGui.Control.closeDialog()
        FreeCAD.closeActiveTransaction()
        settle()

        a, b = setup()

        def pressed():
            FreeCAD.setActiveTransaction("drag step")
            a.Box.Height = 23

        btn = button(pressed, "pressed")
        QTest.mousePress(btn, QtCore.Qt.LeftButton)
        settle()
        down = QtWidgets.QApplication.mouseButtons() != QtCore.Qt.NoButton
        check("a step set on a press stays open while the button is down",
              down and active() is not None and active()[0] == "drag step",
              "buttons down %s %s" % (down, lists(a, b)))
        QTest.mouseRelease(btn, QtCore.Qt.LeftButton)
        btn.deleteLater()
        settle()
        check("and is committed after the release",
              active() is None and list(a.UndoNames) == ["drag step"], lists(a, b))

        a, b = setup()
        console = None
        for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget):
            if w.metaObject().className() == "Gui::PythonConsole":
                console = w
                break
        if check("the Python console found", console is not None):
            console.setFocus()
            QTest.keyClicks(console, "FreeCAD.setActiveTransaction('console step'); "
                            "App.getDocument('GroupA').Box.Height = 24")
            QTest.keyClick(console, QtCore.Qt.Key_Return)
            settle()
            check("a step set from the console stays open, a script's",
                  a.Box.Height.Value == 24 and active() is not None
                  and active()[0] == "console step", lists(a, b))
            FreeCAD.closeActiveTransaction()

        a, b = setup()
        PARAM.SetBool("UndoGroupByInput", False)
        click(loose)
        check("off: the click's step stays open as before",
              active() is not None and active()[0] == "loose step", lists(a, b))
        FreeCAD.closeActiveTransaction()
        PARAM.SetBool("UndoGroupByInput", True)
    except Exception:
        note("ABORT " + traceback.format_exc())
    # Closed here: a modified one would ask to be saved at the quit
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
