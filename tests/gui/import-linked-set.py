"""File > Import a Linked Set (docs/TransactionLogJournal.md sec 35 L6).

Our `Asm` links the box of our `Part`; a copy of both elsewhere, its box
made longer and its `Asm` linking a new `Bolt` too. With our `Asm` active,
Std_ImportLinkedSet opens the dialog; their top is read.

Claims:
  - the dialog lists their three files, `Part` and `Asm` paired with ours by
    the history both hold and merged by default, their `Bolt` alone and
    written by default;
  - Import brings `Part` and `Asm` as branches and holds `Bolt`;
  - the dialog closed, the command run again offers the set imported and
    not merged (sec 35 Q9), and opens at its merge;
  - Merge takes them in: our `Asm` links our `Part` with their longer box,
    and their `Bolt` written beside it;
  - as one undo step: undone from the Gui, our `Part`'s box is as it was
    and our `Asm` has no link to `Bolt`.
"""
import os
import shutil
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
ROOT = os.path.join(OUT, "setroot")
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
        if w.isVisible() and w.objectName() == "DlgImportLinkedSet":
            return w
    return None


def rows(dlg):
    tree = dlg.findChild(QtWidgets.QTreeWidget, "files")
    out = {}
    for i in range(tree.topLevelItemCount()):
        item = tree.topLevelItem(i)
        action = dlg.findChild(QtWidgets.QComboBox, "action%d" % i)
        out[os.path.basename(item.text(0))] = {
            "paired": item.text(2),
            "action": action.currentText() if action else "",
            "state": item.text(5),
        }
    return out


def drive(left=[60]):
    dlg = dialog()
    if dlg is None:
        left[0] -= 1
        if left[0] <= 0:
            note("FAIL the dialog did not show")
            QtCore.QTimer.singleShot(300, merged)
            return
        QtCore.QTimer.singleShot(200, drive)
        return
    try:
        dlg.findChild(QtWidgets.QLineEdit, "theirs").setText(os.path.join(STATE["theirs"], "Asm.FCStd"))
        dlg.findChild(QtWidgets.QPushButton, "read").click()
        STATE["read"] = rows(dlg)
        dlg.findChild(QtWidgets.QPushButton, "import").click()
        STATE["imported"] = rows(dlg)
        # Closed before the merge: the set waits.
        dlg.reject()
        QtCore.QTimer.singleShot(300, again)
    except Exception:
        note("ABORT drive:\n" + traceback.format_exc())
        dlg.reject()
        QtCore.QTimer.singleShot(300, merged)


def answer():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if w.isVisible() and isinstance(w, QtWidgets.QMessageBox):
            STATE.setdefault("asked", []).append(w.text())
            (w.button(QtWidgets.QMessageBox.Yes) or w.defaultButton()).click()


ANSWER = QtCore.QTimer()
ANSWER.timeout.connect(answer)


def again():
    QtCore.QTimer.singleShot(200, resumed)
    ANSWER.start(200)
    FreeCADGui.runCommand("Std_ImportLinkedSet")


def resumed(left=[60]):
    dlg = dialog()
    if dlg is None:
        left[0] -= 1
        if left[0] <= 0:
            ANSWER.stop()
            note("FAIL the dialog did not show again")
            QtCore.QTimer.singleShot(300, merged)
            return
        QtCore.QTimer.singleShot(200, resumed)
        return
    try:
        STATE["resumed"] = rows(dlg)
        STATE["mergeable"] = dlg.findChild(QtWidgets.QPushButton, "merge").isEnabled()
        dlg.findChild(QtWidgets.QPushButton, "merge").click()
    except Exception:
        note("ABORT resumed:\n" + traceback.format_exc())
        dlg.reject()
    QtCore.QTimer.singleShot(300, merged)


def build():
    try:
        shutil.rmtree(ROOT, True)
        ours = os.path.join(ROOT, "ours")
        theirs = os.path.join(ROOT, "theirs")
        os.makedirs(ours)
        part = FreeCAD.newDocument("SetPart")
        part.UndoMode = 1
        part.openTransaction("box")
        box = part.addObject("Part::Box", "Box")
        part.recompute()
        part.commitTransaction()
        part.saveAs(os.path.join(ours, "Part.FCStd"))
        asm = FreeCAD.newDocument("SetAsm")
        asm.UndoMode = 1
        asm.openTransaction("link")
        asm.addObject("App::Link", "L").LinkedObject = box
        asm.recompute()
        asm.commitTransaction()
        asm.saveAs(os.path.join(ours, "Asm.FCStd"))
        for d in (asm, part):
            FreeCAD.closeDocument(d.Name)
        shutil.copytree(ours, theirs)
        part = FreeCAD.openDocument(os.path.join(theirs, "Part.FCStd"))
        part.Box.Length = 25
        part.recompute()
        part.save()
        bolt = FreeCAD.newDocument("SetBolt")
        cyl = bolt.addObject("Part::Cylinder", "Cyl")
        bolt.recompute()
        bolt.saveAs(os.path.join(theirs, "Bolt.FCStd"))
        asm = FreeCAD.openDocument(os.path.join(theirs, "Asm.FCStd"))
        asm.addObject("App::Link", "L2").LinkedObject = cyl
        asm.recompute()
        asm.save()
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        STATE["ours"], STATE["theirs"] = ours, theirs
        doc = FreeCAD.openDocument(os.path.join(ours, "Asm.FCStd"))
        STATE["doc"] = doc.Name
        FreeCAD.setActiveDocument(doc.Name)
        FreeCADGui.ActiveDocument = FreeCADGui.getDocument(doc.Name)
        QtCore.QTimer.singleShot(200, drive)
        FreeCADGui.runCommand("Std_ImportLinkedSet")
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()


def merged():
    ANSWER.stop()
    try:
        read = STATE.get("read", {})
        check(
            "the dialog lists their set, paired and with its actions",
            read.get("Part.FCStd", {}).get("action") == "Merge"
            and read.get("Asm.FCStd", {}).get("action") == "Merge"
            and read.get("Bolt.FCStd", {}).get("action") == "Write"
            and read.get("Part.FCStd", {}).get("paired") == "history",
            read,
        )
        imported = STATE.get("imported", {})
        check(
            "Import brings branches, and holds their file alone",
            imported.get("Part.FCStd", {}).get("state", "").startswith("branch")
            and imported.get("Asm.FCStd", {}).get("state", "").startswith("branch")
            and imported.get("Bolt.FCStd", {}).get("state") == "held",
            imported,
        )
        resumed = STATE.get("resumed", {})
        check(
            "opened again, the set waiting is offered at its merge",
            len(STATE.get("asked", [])) >= 1
            and "not merged" in STATE["asked"][0]
            and STATE.get("mergeable")
            and resumed.get("Asm.FCStd", {}).get("state", "").startswith("branch")
            and resumed.get("Bolt.FCStd", {}).get("action") == "Write",
            (STATE.get("asked"), resumed),
        )
        doc = FreeCAD.getDocument(STATE["doc"])
        bolt = os.path.join(STATE["ours"], "Bolt.FCStd")
        l2 = doc.getObject("L2")
        check(
            "Merge takes their set in",
            doc.L.LinkedObject.Length.Value == 25
            and l2 is not None
            and l2.LinkedObject is not None
            and os.path.realpath(l2.LinkedObject.Document.FileName) == os.path.realpath(bolt),
            (doc.L.LinkedObject.Length.Value, l2 and l2.LinkedObject),
        )
        check("and the set no longer waits", doc.getLinkedSetImports() == [])
        check("with nothing more asked", len(STATE.get("asked", [])) == 1, STATE.get("asked"))
        STATE["box"] = doc.L.LinkedObject
        FreeCAD.setActiveDocument(doc.Name)
        FreeCADGui.ActiveDocument = FreeCADGui.getDocument(doc.Name)
        FreeCADGui.runCommand("Std_Undo")
        QtCore.QTimer.singleShot(300, undone)
    except Exception:
        note("ABORT merged:\n" + traceback.format_exc())
        finish()


def undone():
    try:
        doc = FreeCAD.getDocument(STATE["doc"])
        check(
            "one undo step takes the set back",
            STATE["box"].Length.Value == 10 and doc.getObject("L2") is None,
            (STATE["box"].Length.Value, doc.getObject("L2")),
        )
    except Exception:
        note("ABORT undone:\n" + traceback.format_exc())
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
