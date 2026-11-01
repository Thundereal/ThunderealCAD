"""The steps of an edit ended with OK are one undo (docs/MultiViewEdit.md
sec 12.11).

A pad edited in its task panel: two values typed, each a step of its own
(View/EditStepEachChange), then OK.

Claims:
  - the edit's steps are one group;
  - Std_Undo takes the edit whole, back to before it; Std_Redo brings it back;
  - the undo button's list shows the edit as one row, a menu of its steps;
  - picking a step in it undoes the edit down to that step only, and what is
    left of the edit is still one undo;
  - clicking the edit's row itself takes the whole edit;
  - a Cancel's steps go back as one redo.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets
from PySide6 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = FreeCAD.Vector
Qt = QtCore.Qt
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()
    for _ in range(5):
        QtCore.QCoreApplication.processEvents()


def activate(view):
    FreeCADGui.getMainWindow().setActiveWindow(view)
    settle(150)


def dialog(view):
    return FreeCADGui.Control.activeTaskDialog(view=view)


def type_into(view, name, text):
    activate(view)
    dlg = dialog(view)
    box = None
    for w in dlg.getDialogContent():
        for b in w.findChildren(QtWidgets.QAbstractSpinBox):
            if b.objectName() == name and (box is None or not box.isVisible()):
                box = b
    if box is None:
        return False
    box.window().activateWindow()
    box.setFocus()
    settle(20)
    line = box.findChild(QtWidgets.QLineEdit)
    line.selectAll()
    QtTest.QTest.keyClicks(line, text)
    QtTest.QTest.keyClick(box, Qt.Key_Return)
    settle(300)
    FreeCADGui.getMainWindow().setFocus()
    settle(500)
    return True


def pace():
    """Past the coalescing window (TransactionLogJournal.md sec 37.4 P4)"""
    settle(3500)


def command(view, name):
    activate(view)
    FreeCADGui.runCommand(name)
    settle(800)


def undo_menu():
    """The undo button's list, filled as when it drops down"""
    for menu in QtWidgets.QApplication.topLevelWidgets():
        if menu.metaObject().className() == "Gui::Dialog::UndoDialog":
            menu.aboutToShow.emit()
            return menu
    return None


def rows(menu):
    out = []
    for a in menu.actions():
        if a.menu():
            out.append((a.text(), [s.text() for s in a.menu().actions()]))
        else:
            out.append((a.text(), None))
    return out


def run():
    try:
        VIEW.SetBool("PerViewEdit", False)
        VIEW.SetBool("EditStepEachChange", True)
        doc = FreeCAD.newDocument("EditGroup")
        body = doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        sketch.Support = (doc.getObject("XY_Plane"), [""])
        sketch.MapMode = "FlatFace"
        corners = [V(0, 0, 0), V(10, 0, 0), V(10, 10, 0), V(0, 10, 0)]
        for i in range(4):
            sketch.addGeometry(Part.LineSegment(corners[i], corners[(i + 1) % 4]))
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 15
        doc.recompute()
        doc.UndoMode = 1
        doc.clearUndos()
        gdoc = FreeCADGui.getDocument(doc.Name)
        v3d = gdoc.mdiViewsOfType("Gui::View3DInventor")[0]

        def edit():
            activate(v3d)
            FreeCAD.setActiveTransaction("Edit Pad")
            gdoc.setEdit(pad, 0)
            settle(1500)

        doc.openTransaction("Before")
        pad.Reversed = False
        doc.Body.Label = "Body"
        doc.commitTransaction()
        before = doc.UndoNames

        edit()
        type_into(v3d, "lengthEdit", "7")
        pace()
        type_into(v3d, "lengthEdit", "8")
        dialog(v3d).accept()
        settle(1500)
        steps = doc.getUndoSteps()
        note("  steps after OK %s" % (steps,))
        groups = {g for _, _, g in steps if g}
        edit_steps = [s for s in steps if s[2]]
        check("the edit's steps are one group",
              len(groups) == 1 and len(edit_steps) >= 2
              and round(pad.Length.Value, 3) == 8.0, steps)

        menu = undo_menu()
        listed = rows(menu) if menu else []
        note("  undo list %s" % (listed,))
        check("the undo list shows the edit as one row, a menu of its steps",
              listed and listed[0][1] is not None and len(listed[0][1]) == len(edit_steps),
              listed)

        command(v3d, "Std_Undo")
        check("Std_Undo takes the edit whole",
              round(pad.Length.Value, 3) == 15.0 and doc.UndoNames == before,
              (pad.Length, doc.UndoNames))
        command(v3d, "Std_Redo")
        check("Std_Redo brings it back", round(pad.Length.Value, 3) == 8.0
              and len(doc.getUndoSteps()) == len(steps), (pad.Length, doc.getUndoSteps()))

        # the row itself clicked, not hovered: the whole edit
        menu = undo_menu()
        row = [a for a in menu.actions() if a.menu()]
        if row:
            menu.popup(QtCore.QPoint(200, 200))
            settle(300)
            rect = menu.actionGeometry(row[0])
            QtTest.QTest.mouseClick(menu, Qt.LeftButton, Qt.NoModifier, rect.center())
            settle(800)
        check("clicking the edit's row takes the whole edit",
              row and round(pad.Length.Value, 3) == 15.0 and doc.UndoNames == before,
              (pad.Length, doc.UndoNames))
        command(v3d, "Std_Redo")
        check("one redo brings the whole edit back", round(pad.Length.Value, 3) == 8.0,
              pad.Length)

        # a step of it picked: the edit down to that step only
        menu = undo_menu()
        group = [a for a in menu.actions() if a.menu()][0].menu()
        picked = [a for a in group.actions()]
        note("  group menu %s" % ([a.text() for a in picked],))
        # newest first: OK's step, the step that typed 8, the one that typed 7
        picked[1].trigger()
        settle(800)
        left = doc.getUndoSteps()
        note("  after the fine undo: length %s steps %s" % (pad.Length, left))
        check("picking a step undoes the edit down to it only",
              round(pad.Length.Value, 3) == 7.0 and len(left) == len(before) + 1,
              (pad.Length, left))
        check("what is left of the edit is still of the group",
              len({g for _, _, g in left if g}) == 1, left)
        command(v3d, "Std_Undo")
        check("Std_Undo takes the rest of it", round(pad.Length.Value, 3) == 15.0
              and doc.UndoNames == before, (pad.Length, doc.UndoNames))
        command(v3d, "Std_Redo")
        check("one redo brings all of it back", round(pad.Length.Value, 3) == 8.0, pad.Length)

        # Cancel: its steps go back as one redo
        edit()
        type_into(v3d, "lengthEdit", "3")
        pace()
        type_into(v3d, "lengthEdit", "4")
        dialog(v3d).reject()
        settle(1500)
        check("Cancel takes the edit back", round(pad.Length.Value, 3) == 8.0, pad.Length)
        redo = doc.getRedoSteps()
        note("  redo after Cancel %s" % (redo,))
        command(v3d, "Std_Redo")
        check("and one redo brings all of it back", round(pad.Length.Value, 3) == 4.0,
              (pad.Length, redo))
    except Exception:
        note("FAIL exception\n" + traceback.format_exc())


def finish():
    try:
        FreeCADGui.resetEdit()
    except Exception:
        pass
    # closed, or quitting asks to save them
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


def main():
    run()
    finish()


QtCore.QTimer.singleShot(1500, main)
