"""A save merges what another process saved into the file (docs/TransactionLog.md sec 29.8).

The file is opened here; a FreeCADCmd run opens it too, changes it and saves.

Claims:
  - while this one is idle, the other's save is said: the status bar and the
    log panel say the next save merges it;
  - a save here then merges it and writes: both changes in the document and
    in the file, and the document not modified;
  - the other saves a change this one also made: a save here stops with no
    error box, the log panel opens the merge with the conflict to pick, and
    once picked the next save writes the side picked.
"""
import os
import shutil
import subprocess
import sys
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
ROOT = os.path.join(OUT, "tworoot")


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=0):
    t = QtCore.QElapsedTimer()
    t.start()
    while True:
        QtWidgets.QApplication.processEvents(QtCore.QEventLoop.AllEvents, 50)
        if t.elapsed() >= ms:
            break


def other(path, code):
    exe = os.path.join(FreeCAD.getHomePath(), "bin",
                       "FreeCADCmd" + (".exe" if sys.platform == "win32" else ""))
    # A name of its own each run: the script is imported, and a cached one
    # of the same size written within the same second would run instead.
    other.runs = getattr(other, "runs", 0) + 1
    script = os.path.join(ROOT, "other%d.py" % other.runs)
    with open(script, "w") as f:
        f.write("import FreeCAD\n"
                "p = FreeCAD.ParamGet('User parameter:BaseApp/Preferences/Document')\n"
                "was = p.GetInt('TransactionLog', 2)\n"
                "p.SetInt('TransactionLog', 2)\n"
                "try:\n"
                "    doc = FreeCAD.openDocument(%r)\n"
                "    doc.UndoMode = 1\n"
                "    doc.openTransaction('theirs')\n"
                "    %s\n"
                "    doc.recompute()\n"
                "    doc.commitTransaction()\n"
                "    doc.save()\n"
                "    print('saved', flush=True)\n"
                "finally:\n"
                "    p.SetInt('TransactionLog', was)\n" % (path, code))
    done = subprocess.run([exe, script], capture_output=True, timeout=300)
    return b"saved" in done.stdout


def panel():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget):
        if w.metaObject().className() == "Gui::DockWnd::TransactionLogView":
            return w
    return None


def labels(dock):
    return " ".join(l.text() for l in dock.findChildren(QtWidgets.QLabel)) if dock else ""


def mergeDialog():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if w.objectName() == "TransactionMergeDialog" and w.isVisible():
            return w
    return None


def column(tree, title):
    header = tree.headerItem()
    for c in range(header.columnCount()):
        if header.text(c) == title:
            return c
    return -1


def edit(doc, name, fn):
    doc.openTransaction(name)
    fn(doc)
    doc.recompute()
    doc.commitTransaction()


def run():
    p = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")
    mode = p.GetInt("TransactionLog", 2)
    try:
        p.SetInt("TransactionLog", 2)
        shutil.rmtree(ROOT, True)
        os.makedirs(ROOT)
        path = os.path.join(ROOT, "Shared.TStd")
        doc = FreeCAD.newDocument("Shared")
        doc.UndoMode = 1
        doc.SaveSchemaVersion = 5
        edit(doc, "box", lambda d: d.addObject("Part::Box", "Box"))
        doc.saveAs(path)
        FreeCAD.closeDocument(doc.Name)
        doc = FreeCAD.openDocument(path)
        doc.UndoMode = 1
        FreeCAD.setActiveDocument(doc.Name)
        gdoc = FreeCADGui.getDocument(doc.Name)
        dock = panel()
        settle(500)

        # Idle here: the other's save is said within the panel's look.
        check("the other run saved", other(path, "doc.Box.Width = 30"))
        said = ""
        t = QtCore.QElapsedTimer()
        t.start()
        while t.elapsed() < 8000:
            settle(200)
            said = " ".join(l.text() for l in
                            FreeCADGui.getMainWindow().statusBar().findChildren(QtWidgets.QLabel))
            if "another process" in said:
                break
        check("while idle, the status bar says another process saved it",
              "another process" in said, said)
        check("and so does the log panel", "another process" in labels(dock), labels(dock))
        check("nothing of theirs is in yet", doc.Box.Width.Value == 10, doc.Box.Width.Value)

        # A save merges it and writes.
        edit(doc, "ours", lambda d: setattr(d.Box, "Length", 20))
        gdoc.save()
        settle(1500)
        pull = doc.getLastSavePull()
        check("a save merges what the other saved",
              pull["merged"] and doc.Box.Width.Value == 30 and doc.Box.Length.Value == 20,
              (pull, doc.Box.Width.Value, doc.Box.Length.Value))
        check("and the document is not modified after it", not gdoc.Modified, gdoc.Modified)
        check("nor is the file changed elsewhere any more", not doc.isFileChangedElsewhere())

        # A conflict: both make the height something else.
        check("the other run saved again", other(path, "doc.Box.Height = 7"))
        edit(doc, "ours higher", lambda d: setattr(d.Box, "Height", 9))
        seen = {}

        def drive():
            dialog = mergeDialog()
            box = QtWidgets.QApplication.activeModalWidget()
            seen["box"] = box.metaObject().className() if box else None
            if box is not None and dialog is None:
                box.close()
                return
            if dialog is None:
                QtCore.QTimer.singleShot(200, drive)
                return
            seen["found"] = True
            tree = dialog.findChild(QtWidgets.QTreeWidget)
            rows = []
            for i in range(tree.topLevelItemCount()):
                item = tree.topLevelItem(i)
                rows.append((item.text(0), item.text(1), item.text(2)))
                side = tree.itemWidget(item, column(tree, "Takes"))
                if side is not None and item.text(0).startswith("conflict"):
                    side.setCurrentIndex(side.findText("ours"))
            seen["rows"] = rows
            for b in dialog.findChildren(QtWidgets.QPushButton):
                if b.objectName() == "merge":
                    b.click()
                    return
            dialog.reject()

        QtCore.QTimer.singleShot(300, drive)
        with open(path, "rb") as f:
            before = f.read()
        gdoc.save()
        t.restart()
        while "found" not in seen and t.elapsed() < 8000:
            settle(100)
        settle(500)
        with open(path, "rb") as f:
            untouched = f.read() == before
        check("the save that conflicts writes nothing", untouched)
        check("and shows no error box", seen.get("box") != "QMessageBox", seen.get("box"))
        check("the log panel opens the merge with the conflict to pick",
              seen.get("found") and any(r[0].startswith("conflict") and r[2] == "Height"
                                        for r in seen.get("rows", [])),
              seen.get("rows"))
        gdoc.save()
        settle(500)
        check("picked, the next save writes the side picked",
              doc.getLastSavePull()["pending"] == "" and doc.Box.Height.Value == 9
              and not doc.isFileChangedElsewhere(),
              (doc.getLastSavePull(), doc.Box.Height.Value))
        FreeCAD.closeDocument(doc.Name)
        doc = FreeCAD.openDocument(path)
        check("and the file has it all",
              (doc.Box.Length.Value, doc.Box.Width.Value, doc.Box.Height.Value) == (20, 30, 9),
              (doc.Box.Length.Value, doc.Box.Width.Value, doc.Box.Height.Value))
    except Exception:
        note("ABORT run:\n" + traceback.format_exc())
    finally:
        p.SetInt("TransactionLog", mode)
    finish()


def finish():
    try:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
    except Exception:
        note("ABORT finish:\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, run)
