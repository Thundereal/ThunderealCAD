"""A copy linking into a document never saved, pasted after it is gone
(docs/LibraryPaths.md sec 12).

A saved document holds a link into a document never saved; the link's
holder is copied with Std_Copy, which writes a snapshot of the unsaved
document into its transient directory.

Claims:
  - the copy wrote the snapshot;
  - the unsaved document closed, and its snapshot kept where it was (as a
    FreeCAD that did not close it would find it), Std_Paste into a saved
    document opens the snapshot as a document never saved, and links to it;
  - saving the document pasted into asks to save the dependent files, and
    the snapshot, never saved, is given its file by a Save As first;
  - opened again, the link is to that file.
"""
import glob
import os
import shutil
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
ROOT = os.path.join(OUT, "snaproot")
STATE = {}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def same(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def answer(times=60):
    """Answer what comes up: the dependencies of the copy (the selection
    only), the question of saving the dependent files (Yes), and a Save As
    (the name kept in STATE["saveas"]); any other box by its default button."""

    def poll(left=[times]):
        for w in QtWidgets.QApplication.topLevelWidgets():
            if not w.isVisible():
                continue
            if w.metaObject().className() == "Gui::DlgObjectSelection":
                for b in w.findChildren(QtWidgets.QPushButton):
                    if "Original" in b.text():
                        b.click()
                        STATE.setdefault("asked", []).append("selection")
                        break
            elif isinstance(w, QtWidgets.QFileDialog):
                edit = w.findChild(QtWidgets.QLineEdit, "fileNameEdit")
                if edit is not None:
                    edit.setText(STATE["saveas"])
                STATE.setdefault("asked", []).append("saveas")
                w.done(QtWidgets.QDialog.Accepted)
            elif isinstance(w, QtWidgets.QMessageBox):
                STATE.setdefault("asked", []).append(w.text())
                # Yes, else what it offers first (the compact format's warning)
                yes = w.button(QtWidgets.QMessageBox.Yes) or w.defaultButton()
                if yes is not None:
                    yes.click()
                else:
                    w.accept()
        left[0] -= 1
        if left[0] > 0:
            QtCore.QTimer.singleShot(200, poll)

    QtCore.QTimer.singleShot(200, poll)


def build():
    try:
        shutil.rmtree(ROOT, True)
        os.makedirs(ROOT)
        unsaved = FreeCAD.newDocument("SnapU")
        unsaved.addObject("App::FeaturePython", "Bolt")
        owner = FreeCAD.newDocument("SnapA")
        owner.saveAs(os.path.join(ROOT, "a.FCStd"))
        holder = owner.addObject("App::FeaturePython", "H")
        holder.addProperty("App::PropertyXLink", "L")
        holder.L = unsaved.Bolt
        target = FreeCAD.newDocument("SnapB")
        target.saveAs(os.path.join(ROOT, "b.FCStd"))
        STATE["transient"] = unsaved.TransientDir

        FreeCAD.setActiveDocument("SnapA")
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(holder)
        answer(20)
        FreeCADGui.runCommand("Std_Copy")
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()
        return
    QtCore.QTimer.singleShot(2000, copied)


def copied():
    try:
        snaps = glob.glob(os.path.join(STATE["transient"], "Snapshot", "SnapU.*"))
        if not check("the copy wrote the snapshot", len(snaps) == 1, snaps):
            finish()
            return
        STATE["snapshot"] = snaps[0]
        kept = os.path.join(ROOT, "kept")
        shutil.copy(snaps[0], kept)
        # Gone from here; its snapshot where another FreeCAD would find it.
        FreeCAD.closeDocument("SnapU")
        os.makedirs(os.path.dirname(snaps[0]), exist_ok=True)
        shutil.copy(kept, snaps[0])

        FreeCAD.setActiveDocument("SnapB")
        FreeCADGui.ActiveDocument = FreeCADGui.getDocument("SnapB")
        FreeCADGui.runCommand("Std_Paste")
        target = FreeCAD.getDocument("SnapB")
        pasted = [o for o in target.Objects if hasattr(o, "L")]
        linked = pasted[0].L if pasted else None
        check("the paste links", linked is not None and linked.Name == "Bolt", linked)
        if linked is not None:
            STATE["snapdoc"] = linked.Document.Name
            check("to the snapshot, opened as a document never saved",
                  linked.Document.FileName == "", linked.Document.FileName)
    except Exception:
        note("ABORT copied:\n" + traceback.format_exc())
        finish()
        return
    QtCore.QTimer.singleShot(500, save)


def save():
    try:
        STATE["asked"] = []
        # in the snapshot's own format, which a name of the other one is asked about
        STATE["saveas"] = os.path.join(ROOT, "bolt" + os.path.splitext(STATE["snapshot"])[1])
        answer(60)
        ok = FreeCADGui.getDocument("SnapB").save()
        asked = STATE.get("asked", [])
        check("the save went through", ok, asked)
        check("asked to save the dependent files",
              any("dependent" in str(a) for a in asked), asked)
        check("and the snapshot was given its file by a Save As", "saveas" in asked, asked)
        snapdoc = FreeCAD.getDocument(STATE["snapdoc"])
        STATE["boltfile"] = snapdoc.FileName
        check("the snapshot has its file now",
              snapdoc.FileName and os.path.dirname(snapdoc.FileName) == ROOT, snapdoc.FileName)
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        target = FreeCAD.openDocument(os.path.join(ROOT, "b.FCStd"))
        pasted = [o for o in target.Objects if hasattr(o, "L")]
        linked = pasted[0].L if pasted else None
        check("opened again, the link is to that file",
              linked is not None and same(linked.Document.FileName, STATE["boltfile"]),
              linked.Document.FileName if linked else None)
    except Exception:
        note("ABORT save:\n" + traceback.format_exc())
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
