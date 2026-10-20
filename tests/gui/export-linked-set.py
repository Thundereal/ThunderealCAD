"""File > Export with Linked Files (docs/TransactionLog.md sec 35 L7).

An assembly links an object of a part in its folder and pins it to a
version; Std_ExportLinkedSet opens the dialog, which lists the set.

Claims:
  - the dialog lists the three files of the set: the assembly, the part,
    and the pinned version as a file of its own; with no file in a library,
    it does not offer to leave those (docs/LibraryPaths.md sec 14);
  - Export into a new folder writes them and the manifest;
  - into the same folder again, it asks first, and writes when told to.
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
        if w.isVisible() and w.objectName() == "DlgExportLinkedSet":
            return w
    return None


def answer():
    """A question that the folder is not empty answered Yes. On a timer of
    its own, which fires inside the loop the question runs."""
    for w in QtWidgets.QApplication.topLevelWidgets():
        if w.isVisible() and isinstance(w, QtWidgets.QMessageBox):
            STATE.setdefault("asked", []).append(w.text())
            yes = w.button(QtWidgets.QMessageBox.Yes) or w.defaultButton()
            yes.click()


ANSWER = QtCore.QTimer()
ANSWER.timeout.connect(answer)


def drive(folder, then):
    """Fill the dialog when it shows -- what it lists, the folder -- and
    press Export; then `then`."""

    def poll(left=[60]):
        dlg = dialog()
        if dlg is not None:
            tree = dlg.findChild(QtWidgets.QTreeWidget, "files")
            STATE["listed"] = sorted(
                tree.topLevelItem(i).text(2) for i in range(tree.topLevelItemCount())
            )
            STATE["libraries"] = dlg.findChild(QtWidgets.QCheckBox, "libraries").isVisible()
            dlg.findChild(QtWidgets.QLineEdit, "folder").setText(folder)
            box = dlg.findChild(QtWidgets.QDialogButtonBox)
            ANSWER.start(200)
            box.button(QtWidgets.QDialogButtonBox.Ok).click()
            ANSWER.stop()
            QtCore.QTimer.singleShot(300, then)
            return
        left[0] -= 1
        if left[0] <= 0:
            note("FAIL the dialog did not show")
            QtCore.QTimer.singleShot(300, then)
            return
        QtCore.QTimer.singleShot(200, poll)

    QtCore.QTimer.singleShot(200, poll)


def build():
    try:
        shutil.rmtree(ROOT, True)
        os.makedirs(ROOT)
        part = FreeCAD.newDocument("SetGuiPart")
        part.UndoMode = 1
        part.openTransaction("create")
        obj = part.addObject("App::FeatureTest", "Obj")
        obj.Integer = 1
        part.commitTransaction()
        part.saveAs(os.path.join(ROOT, "part.FCStd"))
        STATE["v"] = int(part.Version.split()[0])
        asm = FreeCAD.newDocument("SetGuiAsm")
        asm.addObject("App::Link", "Plain").LinkedObject = obj
        pin = asm.addObject("App::Link", "Pin")
        pin.LinkedObject = obj
        pin.pinLink("LinkedObject")
        asm.saveAs(os.path.join(ROOT, "asm.FCStd"))
        part.openTransaction("two")
        obj.Integer = 2
        part.commitTransaction()
        part.save()
        asm.save()
        FreeCAD.setActiveDocument(asm.Name)
        FreeCADGui.ActiveDocument = FreeCADGui.getDocument(asm.Name)
        STATE["out"] = os.path.join(ROOT, "out")
        drive(STATE["out"], exported)
        FreeCADGui.runCommand("Std_ExportLinkedSet")
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()


def exported():
    try:
        want = sorted(["asm.FCStd", "part.FCStd", "part.v%d.FCStd" % STATE["v"]])
        check("the dialog lists the set", STATE.get("listed") == want, STATE.get("listed"))
        check("with no library file, no library option", STATE.get("libraries") is False)
        out = STATE["out"]
        check(
            "Export wrote the files",
            all(os.path.isfile(os.path.join(out, p)) for p in want),
            sorted(os.listdir(out)) if os.path.isdir(out) else None,
        )
        check("and the manifest", os.path.isfile(os.path.join(out, "FreeCAD-set.json")))
        STATE["asked"] = []
        os.remove(os.path.join(out, "part.FCStd"))
        drive(out, again)
        FreeCADGui.runCommand("Std_ExportLinkedSet")
    except Exception:
        note("ABORT exported:\n" + traceback.format_exc())
        finish()


def again():
    try:
        out = STATE["out"]
        check(
            "into a folder not empty it asks first",
            len(STATE.get("asked", [])) == 1,
            STATE.get("asked"),
        )
        check("and writes when told to", os.path.isfile(os.path.join(out, "part.FCStd")))
    except Exception:
        note("ABORT again:\n" + traceback.format_exc())
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
