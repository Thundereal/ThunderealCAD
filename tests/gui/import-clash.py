"""A paste onto objects already there (docs/TransactionLog.md sec 36.4).

`Obj` of document A is copied into document B, where it is changed and
linked by `L`. B's `Obj` and `L` are copied and pasted into A, where `Obj`
is live under the same UID.

Claims:
  - the paste asks, listing B's `Obj` against A's;
  - Assign all: A's `Obj` takes B's state, and the `L` pasted links A's
    `Obj`, with no second `Obj`;
  - one Std_Undo takes it back;
  - pasted again, Duplicate all: a second object beside A's, of a UID of
    its own;
  - pasted again and cancelled: nothing is added;
  - pasted into B, where it was copied from: nothing is asked.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
STATE = {}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def dialog():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if w.isVisible() and w.objectName() == "DlgImportClash":
            return w
    return None


def rows(dlg):
    tree = dlg.findChild(QtWidgets.QTreeWidget, "clashes")
    return [(tree.topLevelItem(i).text(0), tree.topLevelItem(i).text(1))
            for i in range(tree.topLevelItemCount())]


def docs():
    return FreeCAD.getDocument(STATE["a"]), FreeCAD.getDocument(STATE["b"])


def paste(into, how, then):
    """Paste into `into`, answering the dialog with `how` (a button's object
    name, or "reject"); `then` runs after."""
    STATE["asked"] = None

    def drive(left=[25]):
        dlg = dialog()
        if dlg is None:
            left[0] -= 1
            if left[0] > 0:
                QtCore.QTimer.singleShot(100, drive)
            else:
                left[0] = 25
            return
        left[0] = 25
        STATE["asked"] = rows(dlg)
        if how == "reject":
            dlg.reject()
        else:
            dlg.findChild(QtWidgets.QPushButton, how).click()

    FreeCAD.setActiveDocument(into.Name)
    FreeCADGui.ActiveDocument = FreeCADGui.getDocument(into.Name)
    FreeCADGui.Selection.clearSelection()
    QtCore.QTimer.singleShot(100, drive)
    FreeCADGui.runCommand("Std_Paste")
    QtCore.QTimer.singleShot(3000, then)


def build():
    try:
        a = FreeCAD.newDocument("ClashA")
        a.UndoMode = 1
        a.openTransaction("make")
        obj = a.addObject("App::FeatureTest", "Obj")
        a.commitTransaction()
        b = FreeCAD.newDocument("ClashB")
        away = b.copyObject(obj)
        away.Integer = 5
        b.addObject("App::Link", "L").LinkedObject = away
        STATE["a"], STATE["b"] = a.Name, b.Name
        STATE["uid"] = obj.ObjectUid
        STATE["before"] = obj.Integer
        STATE["count"] = len(a.Objects)
        FreeCAD.setActiveDocument(b.Name)
        FreeCADGui.ActiveDocument = FreeCADGui.getDocument(b.Name)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(away)
        FreeCADGui.Selection.addSelection(b.L)
        FreeCADGui.runCommand("Std_Copy")
        paste(a, "assignAll", assigned)
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()


def assigned():
    try:
        a, b = docs()
        asked = STATE["asked"]
        check("the paste asks, listing their Obj against ours",
              asked is not None and len(asked) == 1 and "Obj" in asked[0][1], asked)
        links = [o for o in a.Objects if o.isDerivedFrom("App::Link")]
        check("Assign all: ours takes their state, the link pasted links ours",
              a.Obj.Integer == 5 and len(links) == 1 and links[0].LinkedObject == a.Obj
              and len(a.Objects) == STATE["count"] + 1,
              (a.Obj.Integer, [o.Name for o in a.Objects]))
        FreeCADGui.runCommand("Std_Undo")
        QtCore.QTimer.singleShot(500, undone)
    except Exception:
        note("ABORT assigned:\n" + traceback.format_exc())
        finish()


def undone():
    try:
        a, b = docs()
        check("one undo takes it back",
              a.Obj.Integer == STATE["before"] and len(a.Objects) == STATE["count"],
              (a.Obj.Integer, [o.Name for o in a.Objects]))
        paste(a, "duplicateAll", duplicated)
    except Exception:
        note("ABORT undone:\n" + traceback.format_exc())
        finish()


def duplicated():
    try:
        a, b = docs()
        made = [o for o in a.Objects if o.isDerivedFrom("App::FeatureTest") and o.Name != "Obj"]
        check("Duplicate all: a second object, a UID of its own",
              STATE["asked"] is not None and len(made) == 1
              and made[0].ObjectUid != STATE["uid"] and a.Obj.Integer == STATE["before"],
              [(o.Name, o.ObjectUid) for o in made])
        STATE["count"] = len(a.Objects)
        paste(a, "reject", cancelled)
    except Exception:
        note("ABORT duplicated:\n" + traceback.format_exc())
        finish()


def cancelled():
    try:
        a, b = docs()
        check("cancelled, nothing is added",
              STATE["asked"] is not None and len(a.Objects) == STATE["count"],
              [o.Name for o in a.Objects])
        STATE["countB"] = len(b.Objects)
        paste(b, "reject", beside)
    except Exception:
        note("ABORT cancelled:\n" + traceback.format_exc())
        finish()


def beside():
    try:
        a, b = docs()
        check("pasted where it was copied from, nothing is asked",
              STATE["asked"] is None and len(b.Objects) == STATE["countB"] + 2,
              (STATE["asked"], [o.Name for o in b.Objects]))
    except Exception:
        note("ABORT beside:\n" + traceback.format_exc())
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
