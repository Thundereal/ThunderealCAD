"""Each undo step of a fillet's or a chamfer's panel says what it did.

docs/HandsOnQueue.md entry 228 (the reporter, 2026-10-23): "add some
concise descript for each edit steps in partdesign editing task. right now
I only see the object name and edge name for any operation in fillet edit".
Every step was "Edit <name>"; now "<label>: <what>".

A PartDesign box; a fillet of three edges, in edit: the default radius
typed, Edge2 typed into group 1, Edge3 removed from the list. Then a
chamfer of three edges, in edit: the default size typed, Edge2 typed into
group 1. Claims: after each, the newest undo step's name.

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


def row(text):
    w = tree()
    for i in range(w.topLevelItemCount()):
        if w.topLevelItem(i).text(0) == text:
            return w.topLevelItem(i)
    return None


def type_group(edge, group):
    w = tree()
    item = row(edge)
    editor = None
    for _ in range(3):
        w.setCurrentItem(item, GROUP_COLUMN)
        w.editItem(item, GROUP_COLUMN)
        pump(20)
        focus = QtWidgets.QApplication.focusWidget()
        editor = focus if isinstance(focus, QtWidgets.QSpinBox) else None
        if editor is None:
            for s in w.viewport().findChildren(QtWidgets.QSpinBox):
                if s.isVisible():
                    editor = s
        if editor is not None:
            break
        pump(50)
    if editor is None:
        return False
    editor.setValue(group)
    QtTest.QTest.keyClick(editor, Qt.Key_Return)
    pump(250)
    return True


def type_box(name, text):
    box = None
    for w in mw().findChildren(QtWidgets.QAbstractSpinBox):
        if w.objectName() == name and w.isVisible():
            box = w
    if box is None:
        return False
    box.setFocus()
    pump(10)
    line = box.findChild(QtWidgets.QLineEdit)
    line.selectAll()
    QtTest.QTest.keyClicks(line, text)
    QtTest.QTest.keyClick(box, Qt.Key_Return)
    pump(100)
    tree().setFocus()
    pump(200)
    return True


def newest(doc):
    names = list(doc.UndoNames)
    return names[0] if names else None


def run():
    try:
        doc = FreeCAD.newDocument("UndoStepNames")
        doc.UndoMode = 1
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        g = FreeCADGui.getDocument(doc.Name)

        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1", "Edge2", "Edge3"])
        fillet.Radius = 1.0
        doc.recompute()
        g.setEdit(fillet, 0)
        pump(80)
        if check("the fillet is in edit", tree() is not None):
            type_box("filletRadius", "1.5")
            check("the default radius typed", newest(doc) == "Fillet: default radius", newest(doc))
            type_group("Edge2", 1)
            check("Edge2 typed into group 1", newest(doc) == "Fillet: Edge2 to group 1",
                  newest(doc))
            w = tree()
            w.clearSelection()
            row("Edge3").setSelected(True)
            pump(10)
            remove = [a for a in w.actions() if a.text() == "Remove"]
            if remove:
                remove[0].trigger()
                pump(150)
            check("Edge3 removed", newest(doc) == "Fillet: remove Edge3", newest(doc))
        g.resetEdit()
        pump(60)
        doc.removeObject(fillet.Name)
        doc.recompute()

        chamfer = body.newObject("PartDesign::Chamfer", "Chamfer")
        chamfer.Base = (box, ["Edge1", "Edge2", "Edge3"])
        chamfer.Size = 1.0
        doc.recompute()
        g.setEdit(chamfer, 0)
        pump(80)
        if check("the chamfer is in edit", tree() is not None):
            type_box("chamferSize", "1.5")
            check("the default size typed", newest(doc) == "Chamfer: default size", newest(doc))
            type_group("Edge2", 1)
            check("Edge2 typed into group 1", newest(doc) == "Chamfer: Edge2 to group 1",
                  newest(doc))
        note("INFO the undo list: %s" % list(doc.UndoNames)[:8])
        g.resetEdit()
        pump(40)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
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
