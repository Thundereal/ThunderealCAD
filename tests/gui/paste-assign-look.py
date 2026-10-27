"""A paste assigned onto an object here takes the look brought too (docs/TransactionLogJournal.md
sec 35.18), unless the paste dialog's check says not.

`Box` of document A is copied into B, where it is made longer, drawn with
thicker lines and coloured. B's box is copied and pasted into A, and
assigned onto A's box in the paste dialog.

Claims:
  - the dialog offers the look, checked;
  - checked: A's box takes their length and their look;
  - one Std_Undo takes both back;
  - unchecked: A's box takes their length and keeps its own look.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
STATE = {}
Qt = QtCore.Qt


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def dialog():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if w.isVisible() and w.objectName() == "DlgPasteSelection":
            return w
    return None


def look(obj):
    vp = obj.ViewObject
    return (vp.LineWidth, tuple(round(c, 3) for c in vp.ShapeAppearance[0].DiffuseColor[:3]))


def paste(into, keepLook, then):
    STATE["offered"] = None

    def drive(left=[25]):
        dlg = dialog()
        if dlg is None:
            left[0] -= 1
            if left[0] > 0:
                QtCore.QTimer.singleShot(100, drive)
            return
        left[0] = 25
        box = dlg.findChild(QtWidgets.QCheckBox, "copyLook")
        STATE["offered"] = box is not None and box.isChecked()
        tree = dlg.findChild(QtWidgets.QTreeWidget, "treeWidget")
        for i in range(tree.topLevelItemCount()):
            it = tree.topLevelItem(i)
            if it.text(0).startswith("Box"):
                it.setCheckState(1, Qt.Checked)
        if box is not None and keepLook:
            box.setChecked(False)

        def accept():
            dlg.findChild(QtWidgets.QDialogButtonBox, "buttonBox").button(
                QtWidgets.QDialogButtonBox.Ok).click()

        QtCore.QTimer.singleShot(300, accept)

    FreeCAD.setActiveDocument(into.Name)
    FreeCADGui.ActiveDocument = FreeCADGui.getDocument(into.Name)
    FreeCADGui.Selection.clearSelection()
    QtCore.QTimer.singleShot(100, drive)
    FreeCADGui.runCommand("Std_Paste")
    QtCore.QTimer.singleShot(2500, then)


def build():
    try:
        a = FreeCAD.newDocument("LookA")
        a.UndoMode = 1
        a.openTransaction("make")
        box = a.addObject("Part::Box", "Box")
        a.commitTransaction()
        a.recompute()
        box.ViewObject.LineWidth = 2
        STATE["a"] = a.Name
        STATE["before"] = look(box)
        b = FreeCAD.newDocument("LookB")
        theirs = b.copyObject([box])[0]
        b.recompute()
        theirs.Length = 25
        theirs.ViewObject.LineWidth = 5
        theirs.ViewObject.ShapeAppearance = (FreeCAD.Material(DiffuseColor=(1.0, 0.0, 0.0)),)
        b.recompute()
        STATE["b"] = b.Name
        STATE["theirs"] = look(theirs)
        FreeCAD.setActiveDocument(b.Name)
        FreeCADGui.ActiveDocument = FreeCADGui.getDocument(b.Name)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(theirs)
        FreeCADGui.runCommand("Std_Copy")
        paste(a, False, taken)
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()


def taken():
    try:
        a = FreeCAD.getDocument(STATE["a"])
        check("the dialog offers the look, checked", STATE["offered"] is True, STATE["offered"])
        check("checked: our box takes their length and their look",
              a.Box.Length.Value == 25 and look(a.Box) == STATE["theirs"]
              and len(a.Objects) == 1,
              (a.Box.Length.Value, look(a.Box), STATE["theirs"], [o.Name for o in a.Objects]))
        FreeCADGui.runCommand("Std_Undo")
        QtCore.QTimer.singleShot(500, undone)
    except Exception:
        note("ABORT taken:\n" + traceback.format_exc())
        finish()


def undone():
    try:
        a = FreeCAD.getDocument(STATE["a"])
        check("one undo takes both back",
              a.Box.Length.Value == 10 and look(a.Box) == STATE["before"],
              (a.Box.Length.Value, look(a.Box), STATE["before"]))
        paste(a, True, kept)
    except Exception:
        note("ABORT undone:\n" + traceback.format_exc())
        finish()


def kept():
    try:
        a = FreeCAD.getDocument(STATE["a"])
        check("unchecked: our box takes their length and keeps its look",
              a.Box.Length.Value == 25 and look(a.Box) == STATE["before"],
              (a.Box.Length.Value, look(a.Box), STATE["before"]))
    except Exception:
        note("ABORT kept:\n" + traceback.format_exc())
    finish()


def finish():
    try:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
    except Exception:
        note("ABORT finish:\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, build)
