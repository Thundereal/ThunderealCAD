"""End edit, Std_ResetEdit, and what Escape does now (docs/HandsOnQueue.md
entry 203).

Escape no longer ends an edit by default: Std_ResetEdit ("End edit",
Shift+Esc unless the user rebinds it) does, and is active only while the
current view is in edit. View/EscapeEndsEdit brings the old Escape back,
and a sketch's Mod/Sketcher/LeaveSketchWithEscape can only narrow it. A
task dialog that is no edit keeps its Cancel on Escape.

  - outside an edit the command is inactive, and Shift+Esc in the view is
    still the view's selection clear;
  - in a sketch edit it is active; Escape, in the view and in the task
    panel, leaves the edit on;
  - Shift+Esc there ends the edit, and clears the selection with it;
  - with EscapeEndsEdit on, Escape in the view ends it again; with
    LeaveSketchWithEscape off as well, it does not;
  - a dialog that is no edit is still rejected by Escape.

Shift+Esc goes through QTest, which passes Qt's shortcut map as a real key
does; the plain Escapes are sent to the focus widget as sketch tests send
them. Needs a real screen: the shortcut map asks for an active window.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets
try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "EndEditCommand"
VIEW = "User parameter:BaseApp/Preferences/View"
SKETCHER = "User parameter:BaseApp/Preferences/Mod/Sketcher"
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds=0.3):
    import time
    end = time.monotonic() + seconds
    while True:
        QtCore.QCoreApplication.processEvents()
        if time.monotonic() >= end:
            break
        time.sleep(0.01)


def in_edit():
    return FreeCADGui.getDocument(DOC).getInEdit() is not None


def command_active():
    return FreeCADGui.Command.get("Std_ResetEdit").isActive()


def focus():
    return QtWidgets.QApplication.focusWidget() or FreeCADGui.getMainWindow()


def escape(target=None):
    w = target or focus()
    for kind in (QtCore.QEvent.ShortcutOverride, QtCore.QEvent.KeyPress, QtCore.QEvent.KeyRelease):
        QtWidgets.QApplication.sendEvent(
            w, QtGui.QKeyEvent(kind, QtCore.Qt.Key_Escape, QtCore.Qt.NoModifier))
    settle(0.8)


def shift_escape():
    QtTest.QTest.keyClick(focus(), QtCore.Qt.Key_Escape, QtCore.Qt.ShiftModifier)
    settle(0.8)


def os_shift_escape():
    """Shift+Esc as the OS delivers it, or None where that cannot be done.

    The view's own selection clear reads the OS key state
    (QApplication::queryKeyboardModifiers), which a synthetic key never
    shifts; only a real key shows that the key reaches the view at all.
    """
    import sys
    if sys.platform != "win32":
        return None
    import ctypes
    user32 = ctypes.windll.user32
    mw = FreeCADGui.getMainWindow()
    mw.activateWindow()
    settle(0.5)
    if user32.GetForegroundWindow() != int(mw.winId()):
        return None
    # Pumped between the four: posted all at once, the shift is up again
    # by the time the view asks the OS about it
    for vk, up in ((0x10, 0), (0x1B, 0), (0x1B, 2), (0x10, 2)):
        user32.keybd_event(vk, 0, up, 0)
        settle(0.15)
    settle(0.8)
    return True


def task_view():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget):
        if w.metaObject().className() == "Gui::TaskView::TaskView" and w.isVisible():
            return w
    return None


def view_widget():
    view = FreeCADGui.getDocument(DOC).mdiViewsOfType("Gui::View3DInventor")[0]
    return view.graphicsView().viewport()


def enter(sk):
    FreeCADGui.getDocument(DOC).setEdit(sk)
    settle(1.0)
    view_widget().setFocus()
    settle(0.3)


def leave():
    if in_edit():
        FreeCADGui.getDocument(DOC).resetEdit()
        settle(0.5)


class Plain:
    """A task dialog that is no edit: Ok and Cancel, and a record of which."""

    def __init__(self):
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle("Plain task")
        self.calls = []

    def accept(self):
        self.calls.append("accept")
        FreeCADGui.Control.closeDialog()
        return True

    def reject(self):
        self.calls.append("reject")
        FreeCADGui.Control.closeDialog()
        return True


def run():
    try:
        import Part

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        mw.activateWindow()
        doc = FreeCAD.newDocument(DOC)
        box = doc.addObject("Part::Box", "Box")
        box.Placement.Base = FreeCAD.Vector(30, 0, 0)
        sk = doc.addObject("Sketcher::SketchObject", "Sketch")
        sk.addGeometry(Part.LineSegment(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(10, 5, 0)), False)
        doc.recompute()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        settle(1.0)

        # -- outside an edit ---------------------------------------------
        check("outside an edit End edit is inactive", not command_active())
        FreeCADGui.Selection.addSelection(DOC, "Box")
        view_widget().setFocus()
        settle(0.3)
        if os_shift_escape() is None:
            note("NOTE the main window is not in front: the OS key was not sent")
        else:
            check("and Shift+Esc in the view still clears the selection",
                  not FreeCADGui.Selection.getSelectionEx("*"),
                  [s.ObjectName for s in FreeCADGui.Selection.getSelectionEx("*")])
        FreeCADGui.Selection.clearSelection()

        # -- in an edit, the defaults ------------------------------------
        enter(sk)
        check("in a sketch edit", in_edit())
        check("End edit is active", command_active())
        escape(view_widget())
        check("Escape in the view leaves the edit on", in_edit())
        tv = task_view()
        if tv is not None:
            escape(tv)
        check("and Escape in the task panel too", tv is not None and in_edit(),
              tv is not None)
        FreeCADGui.Selection.addSelection(DOC, "Sketch", "Edge1")
        settle(0.3)
        had = bool(FreeCADGui.Selection.getSelectionEx("*"))
        view_widget().setFocus()
        settle(0.3)
        shift_escape()
        check("Shift+Esc ends the edit", not in_edit())
        check("and clears the selection with it",
              had and not FreeCADGui.Selection.getSelectionEx("*"),
              (had, [s.ObjectName for s in FreeCADGui.Selection.getSelectionEx("*")]))
        check("End edit is inactive again", not command_active())

        # -- the old Escape, by preference -------------------------------
        FreeCAD.ParamGet(VIEW).SetBool("EscapeEndsEdit", True)
        enter(sk)
        escape(view_widget())
        check("with EscapeEndsEdit, Escape in the view ends the edit", not in_edit())
        leave()
        FreeCAD.ParamGet(SKETCHER).SetBool("LeaveSketchWithEscape", False)
        enter(sk)
        escape(view_widget())
        check("but a sketch with LeaveSketchWithEscape off stays in edit", in_edit())
        leave()
        FreeCAD.ParamGet(SKETCHER).RemBool("LeaveSketchWithEscape")
        FreeCAD.ParamGet(VIEW).RemBool("EscapeEndsEdit")

        # -- a dialog that is no edit ------------------------------------
        panel = Plain()
        FreeCADGui.Control.showDialog(panel)
        settle(0.8)
        tv = task_view()
        if tv is not None:
            escape(tv)
        check("a task dialog that is no edit is still rejected by Escape",
              panel.calls == ["reject"], panel.calls)
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    FreeCAD.ParamGet(SKETCHER).RemBool("LeaveSketchWithEscape")
    FreeCAD.ParamGet(VIEW).RemBool("EscapeEndsEdit")
    if FreeCAD.listDocuments().get(DOC) and in_edit():
        FreeCADGui.getDocument(DOC).resetEdit()
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
