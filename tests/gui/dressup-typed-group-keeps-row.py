"""A group typed into a dress-up's Group cell leaves its row alive while Qt commits it.

docs/HandsOnQueue.md entry 214: on the staged copy the reporter typed a
fillet edge's Group from 3 to 1 and pressed Enter, and the heap was corrupt
(c0000374), the stack in FilletSegmentDelegate::setModelData. The commit's
setData told the panel at once, which joined the group and filled the list
anew -- the row deleted while Qt was still in its setData. A heap may well
survive that, so the claims are on the list. The reporter: "why there is
any need to refresh list? the grouping only takes effect fillet setting
change" -- only the edge's own entry changes, and its row is changed in
place.

A PartDesign box, a fillet of three edges on the default radius. In edit,
typed into the Group cells with Enter: A 1, B 2, then B 1. Claims, for each:
  - no row of the list is deleted (the list is not filled anew);
  - the group is the edge's, and one undo step each;
and the same for a chamfer: A 1, B 1 (B's join shares A's undo step, as it
did before: noted, not claimed).

Runs offscreen as well.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

try:
    from PySide6 import QtTest
    import shiboken6 as shiboken
except ImportError:
    from PySide2 import QtTest
    import shiboken2 as shiboken

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "DressUpTypedGroup"
GROUP_COLUMN = 5
EDGES = ["Edge1", "Edge2", "Edge3"]

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


def group_of(widget, edge):
    r = row(widget, edge)
    return int(r.data(GROUP_COLUMN, Qt.UserRole) or 0) if r else None


def open_editor(widget):
    focus = QtWidgets.QApplication.focusWidget()
    if isinstance(focus, QtWidgets.QSpinBox):
        return focus
    for w in widget.viewport().findChildren(QtWidgets.QSpinBox):
        if w.isVisible():
            return w
    return None


def type_group(doc, edge, group, what, own_step=True):
    widget = tree()
    item = row(widget, edge)
    if not check("%s: %s's row" % (what, edge), item is not None):
        return
    # (the editor of the edit before may still be on its way out)
    editor = None
    for _ in range(3):
        widget.setCurrentItem(item, GROUP_COLUMN)
        widget.editItem(item, GROUP_COLUMN)
        pump(20)
        editor = open_editor(widget)
        if isinstance(editor, QtWidgets.QSpinBox):
            break
        pump(50)
    if not check("%s: a number editor on %s's Group cell" % (what, edge),
                 isinstance(editor, QtWidgets.QSpinBox), editor):
        return
    undo = doc.UndoCount
    editor.setValue(group)
    # Qt commits the edit from the event loop, its editor shown until it is
    # closed after the commit; the list filled anew while the editor is up
    # is the row deleted under the commit. (A row C++ deletes still looks
    # valid from Python, hence the model's signals.)
    model = widget.model()
    seen = []

    def refill(*args):
        seen.append("editor up" if shiboken.isValid(editor) and editor.isVisible()
                    else "editor gone")

    model.modelAboutToBeReset.connect(refill)
    model.rowsAboutToBeRemoved.connect(refill)
    try:
        QtTest.QTest.keyClick(editor, Qt.Key_Return)
        # long enough for the edit's step to close before the next one
        pump(250)
    finally:
        model.modelAboutToBeReset.disconnect(refill)
        model.rowsAboutToBeRemoved.disconnect(refill)
    note("INFO %s: at each list refill: %s" % (what, seen))
    check("%s: the row changed in place, no row of the list deleted" % what, not seen, seen)
    widget = tree()
    check("%s: then %s is in group %d" % (what, edge, group),
          widget is not None and group_of(widget, edge) == group,
          group_of(widget, edge) if widget else None)
    if own_step:
        check("%s: ... one undo step" % what, doc.UndoCount - undo == 1,
              "%d -> %d" % (undo, doc.UndoCount))
    else:
        note("INFO %s: undo steps %d -> %d" % (what, undo, doc.UndoCount))


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
        gdoc = FreeCADGui.getDocument(DOC)

        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, EDGES)
        fillet.Radius = 1.0
        doc.recompute()
        gdoc.setEdit(fillet, 0)
        pump(80)
        if check("the fillet is in edit, its list shown",
                 gdoc.getInEdit() is not None and tree() is not None):
            type_group(doc, "Edge1", 1, "fillet")
            type_group(doc, "Edge2", 2, "fillet")
            type_group(doc, "Edge2", 1, "fillet, a group to another")
            check("the fillet still in edit", gdoc.getInEdit() is not None)
        gdoc.resetEdit()
        pump(40)
        doc.removeObject(fillet.Name)
        doc.recompute()

        chamfer = body.newObject("PartDesign::Chamfer", "Chamfer")
        chamfer.Base = (box, EDGES)
        chamfer.Size = 1.0
        doc.recompute()
        gdoc.setEdit(chamfer, 0)
        pump(80)
        if check("the chamfer is in edit, its list shown",
                 gdoc.getInEdit() is not None and tree() is not None):
            type_group(doc, "Edge1", 1, "chamfer")
            # (a chamfer's join goes into the step before it, as it did before
            # 214: one undo takes both back -- not this test's matter)
            type_group(doc, "Edge2", 1, "chamfer, joining", own_step=False)
            check("the chamfer still in edit", gdoc.getInEdit() is not None)
        gdoc.resetEdit()
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
