"""What will not resolve after a set's merge, said before it (docs/TransactionLogJournal.md
sec 35.6 Q4 (b), 35.16 G5).

Our `Asm` links the box of our `Part`; a copy of both elsewhere, where
their `Part` gains a cylinder and their `Asm` links it. In File > Import a
Linked Set their set is read and imported, and `Part` set to Keep ours.

Claims:
  - Merge asks first: one reference will not resolve, `L3.LinkedObject ->
    Part.FCStd#Cyl`, and it offers to merge `Part.FCStd` too; the top's
    line says so;
  - taking the offer, `Part` is merged with the top: our `Asm` links our
    `Part`'s cylinder, and nothing is reported.
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
KEEP_OURS = 2   # App::SetMergeAction::KeepOurs


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


def lines(dlg):
    tree = dlg.findChild(QtWidgets.QTreeWidget, "files")
    out = {}
    for i in range(tree.topLevelItemCount()):
        item = tree.topLevelItem(i)
        out[os.path.basename(item.text(0))] = (i, item.text(5), item.toolTip(5))
    return out


def answer():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if w.isVisible() and isinstance(w, QtWidgets.QMessageBox):
            STATE.setdefault("asked", []).append(w.text())
            too = [b for b in w.buttons() if b.text().startswith("Merge Part")]
            STATE.setdefault("offered", []).append([b.text() for b in w.buttons()])
            dlg = dialog()
            if dlg is not None and "lines" not in STATE:
                STATE["lines"] = lines(dlg)
            (too[0] if too else w.defaultButton()).click()


ANSWER = QtCore.QTimer()
ANSWER.timeout.connect(answer)


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
        dlg.findChild(QtWidgets.QLineEdit, "theirs").setText(
            os.path.join(STATE["theirs"], "Asm.FCStd"))
        dlg.findChild(QtWidgets.QPushButton, "read").click()
        dlg.findChild(QtWidgets.QPushButton, "import").click()
        row = lines(dlg)["Part.FCStd"][0]
        action = dlg.findChild(QtWidgets.QComboBox, "action%d" % row)
        action.setCurrentIndex(action.findData(KEEP_OURS))
        ANSWER.start(200)
        dlg.findChild(QtWidgets.QPushButton, "merge").click()
    except Exception:
        note("ABORT drive:\n" + traceback.format_exc())
        dlg.reject()
    QtCore.QTimer.singleShot(300, merged)


def build():
    try:
        shutil.rmtree(ROOT, True)
        ours = os.path.join(ROOT, "ours")
        theirs = os.path.join(ROOT, "theirs")
        os.makedirs(ours)
        part = FreeCAD.newDocument("RefPart")
        part.UndoMode = 1
        part.openTransaction("box")
        box = part.addObject("Part::Box", "Box")
        part.recompute()
        part.commitTransaction()
        part.saveAs(os.path.join(ours, "Part.FCStd"))
        asm = FreeCAD.newDocument("RefAsm")
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
        cyl = part.addObject("Part::Cylinder", "Cyl")
        part.recompute()
        part.save()
        asm = FreeCAD.openDocument(os.path.join(theirs, "Asm.FCStd"))
        asm.addObject("App::Link", "L3").LinkedObject = cyl
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
        asked = STATE.get("asked", [])
        offered = STATE.get("offered", [])
        check(
            "Merge asks first, naming the reference, and offers to merge Part too",
            len(asked) == 1
            and "1 reference" in asked[0]
            and "L3.LinkedObject -> Part.FCStd#Cyl" in asked[0]
            and any(t.startswith("Merge Part.FCStd too") for t in offered[0]),
            (asked, offered),
        )
        top = STATE.get("lines", {}).get("Asm.FCStd", (0, "", ""))
        check(
            "the top's line says so",
            "will not resolve" in top[1] and "Part.FCStd#Cyl" in top[2],
            top,
        )
        doc = FreeCAD.getDocument(STATE["doc"])
        l3 = doc.getObject("L3")
        cyl = l3.LinkedObject if l3 else None
        check(
            "taken, Part is merged with the top: our Asm links our Part's cylinder",
            cyl is not None
            and os.path.realpath(cyl.Document.FileName)
            == os.path.realpath(os.path.join(STATE["ours"], "Part.FCStd"))
            and dialog() is None,
            (l3, cyl and cyl.Document.FileName),
        )
    except Exception:
        note("ABORT merged:\n" + traceback.format_exc())
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
