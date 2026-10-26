"""Every change in a chamfer's panel is an undo step of its own; Cancel takes the edit back.

docs/HandsOnQueue.md entry 215: a chamfer edge joining a group shared the
undo step of the change before it -- the chamfer's whole edit was one step,
every change of the panel in it. The fillet's has been a step a change
since entry 136.

A PartDesign box, a chamfer of three edges, size 1. In edit. Claims:
  - a size typed into Edge1's row: one undo step;
  - Edge2's group typed (a new group), then Edge3 joining it: one step each;
  - the default size changed in its box: one step, once the box lets go
    of the focus;
  - one undo takes back the last change only (the default size);
  - Cancel: the chamfer as it was before the edit -- no edge values of
    its own, its size 1.

Runs offscreen as well.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ChamferUndoSteps"
GROUP_COLUMN = 5

state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        time.sleep(0.01)


def mw():
    return FreeCADGui.getMainWindow()


def tree():
    for w in mw().findChildren(QtWidgets.QTreeWidget):
        if w.objectName() == "treeWidgetReferences" and w.isVisible():
            return w
    return None


def row(widget, text):
    for i in range(widget.topLevelItemCount()):
        if widget.topLevelItem(i).text(0) == text:
            return widget.topLevelItem(i)
    return None


def editor_of(widget, kind):
    focus = QtWidgets.QApplication.focusWidget()
    if isinstance(focus, kind):
        return focus
    for w in widget.viewport().findChildren(kind):
        if w.isVisible():
            return w
    return None


def type_cell(edge, column, text):
    widget = tree()
    item = row(widget, edge)
    editor = None
    for _ in range(3):
        widget.setCurrentItem(item, column)
        widget.editItem(item, column)
        pump(20)
        editor = editor_of(widget, QtWidgets.QSpinBox if column == GROUP_COLUMN
                           else QtWidgets.QAbstractSpinBox)
        if editor is not None:
            break
        pump(50)
    if editor is None:
        return False
    line = editor.findChild(QtWidgets.QLineEdit)
    line.selectAll()
    QtTest.QTest.keyClicks(line, text)
    QtTest.QTest.keyClick(editor, Qt.Key_Return)
    pump(250)
    return True


def one_step(doc, what, act):
    before = doc.UndoCount
    ok = act()
    pump(100)
    return check("%s: one undo step" % what, ok and doc.UndoCount - before == 1,
                 "%s, %d -> %d %s" % (ok, before, doc.UndoCount, list(doc.UndoNames)[:3]))


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        doc = FreeCAD.newDocument(DOC)
        doc.UndoMode = 1
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        chamfer = body.newObject("PartDesign::Chamfer", "Chamfer")
        chamfer.Base = (box, ["Edge1", "Edge2", "Edge3"])
        chamfer.Size = 1.0
        doc.recompute()
        start_info = list(chamfer.ChamferInfo)
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(chamfer, 0)
        pump(80)
        if not check("the chamfer is in edit, its list shown", tree() is not None):
            return
        note("INFO undo at the edit's start %d %s" % (doc.UndoCount, list(doc.UndoNames)[:3]))

        one_step(doc, "a size typed into Edge1's row", lambda: type_cell("Edge1", 1, "2"))
        one_step(doc, "Edge2's group typed", lambda: type_cell("Edge2", GROUP_COLUMN, "1"))
        one_step(doc, "Edge3 joining it", lambda: type_cell("Edge3", GROUP_COLUMN, "1"))
        note("INFO ChamferInfo %s" % list(chamfer.ChamferInfo))

        size = None
        for w in mw().findChildren(QtWidgets.QAbstractSpinBox):
            if w.objectName() == "chamferSize" and w.isVisible():
                size = w

        def set_size():
            if size is None:
                return False
            size.setFocus()
            pump(10)
            line = size.findChild(QtWidgets.QLineEdit)
            line.selectAll()
            QtTest.QTest.keyClicks(line, "1.5")
            QtTest.QTest.keyClick(size, Qt.Key_Return)
            pump(100)
            # the box holds its step while it has the focus
            tree().setFocus()
            pump(200)
            return True

        one_step(doc, "the default size set in its box", set_size)
        check("... the chamfer's size 1.5", abs(chamfer.Size.Value - 1.5) < 1e-6, chamfer.Size)
        info = list(chamfer.ChamferInfo)
        doc.undo()
        pump(100)
        check("one undo takes back the default size only",
              abs(chamfer.Size.Value - 1.0) < 1e-6 and list(chamfer.ChamferInfo) == info,
              (chamfer.Size, list(chamfer.ChamferInfo)))

        cancel = None
        for b in mw().findChildren(QtWidgets.QPushButton):
            if b.isVisible() and b.text().replace("&", "") == "Cancel":
                cancel = b
        if not check("the panel's Cancel button", cancel is not None):
            return
        cancel.click()
        pump(150)
        check("Cancel: the chamfer as before the edit",
              list(chamfer.ChamferInfo) == start_info and abs(chamfer.Size.Value - 1.0) < 1e-6,
              (list(chamfer.ChamferInfo), chamfer.Size))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        try:
            FreeCADGui.getDocument(name).resetEdit()
            pump()
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
