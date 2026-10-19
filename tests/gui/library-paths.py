"""Library paths in the Gui (docs/LibraryPaths.md 3.9, Q2, Q8).

A document links into a library, a folder named in the settings; a copy of
the library is at a second folder.

Claims:
  - the settings naming the second folder while the document was closed,
    the document opens through its own folder (Q2), and the report of its
    open says so with "Use the settings' folder"; clicked, the link is to
    the second folder's file, and the document's map names it;
  - the document's map then naming a folder that does not exist (its
    links left as they are), a change of the setting from Python is asked
    about (Q8): the question lists the document, and "Apply now" links it
    to the folder the settings now name.
"""
import os
import shutil
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
ROOT = os.path.join(OUT, "libroot")
LIB = os.path.join(ROOT, "lib")
LIB2 = os.path.join(ROOT, "lib2")
EXT = ".TStd"
PART = os.path.join(LIB, "f", "bolt" + EXT)
ASM = os.path.join(ROOT, "asm", "asm" + EXT)
NAME = "GuiLib"


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def same(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def linked_file():
    doc = FreeCAD.getDocument("asm")
    target = doc.getObject("H").L
    return target.Document.FileName if target else ""


def top(name):
    for w in QtWidgets.QApplication.topLevelWidgets():
        if w.objectName() == name and w.isVisible():
            return w
    return None


def build():
    try:
        shutil.rmtree(ROOT, True)
        os.makedirs(os.path.join(LIB, "f"))
        os.makedirs(os.path.join(ROOT, "asm"))
        part = FreeCAD.newDocument("bolt")
        part.addObject("App::FeaturePython", "Bolt")
        part.saveAs(PART)
        os.makedirs(os.path.join(LIB2, "f"))
        shutil.copy(PART, os.path.join(LIB2, "f", "bolt" + EXT))
        FreeCAD.setLibraryPath(NAME, LIB)
        asm = FreeCAD.newDocument("asm")
        h = asm.addObject("App::FeaturePython", "H")
        h.addProperty("App::PropertyXLink", "L")
        h.L = part.Bolt
        asm.saveAs(ASM)
        check("the link is saved under the library", asm.LibraryPaths.get(NAME) == LIB,
              asm.LibraryPaths)
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        # Elsewhere now, for the settings, and both hold the file.
        FreeCAD.setLibraryPath(NAME, LIB2)
        FreeCAD.openDocument(ASM)
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()
        return
    QtCore.QTimer.singleShot(1500, reported)


def reported():
    try:
        check("the document opened through its own folder (Q2)",
              same(linked_file(), PART), linked_file())
        dialog = top("libraryPathsReport")
        if check("the open is reported", dialog is not None):
            button = dialog.findChild(QtWidgets.QPushButton, "libraryPathsUseSettings")
            if check("with \"Use the settings' folder\"", button is not None):
                button.click()
                QtCore.QCoreApplication.processEvents()
                check("clicked, the link is to the settings' folder",
                      same(linked_file(), os.path.join(LIB2, "f", "bolt" + EXT)),
                      linked_file())
                check("and the document's map names it",
                      FreeCAD.getDocument("asm").LibraryPaths.get(NAME) == LIB2,
                      FreeCAD.getDocument("asm").LibraryPaths)
            dialog.close()
    except Exception:
        note("ABORT reported:\n" + traceback.format_exc())
    QtCore.QTimer.singleShot(500, changed)


ASKED = {}


def answer():
    box = top("libraryPathsApply")
    ASKED["box"] = box is not None
    if box is None:
        return
    ASKED["text"] = box.informativeText()
    box.button(QtWidgets.QMessageBox.Yes).click()


def changed():
    try:
        doc = FreeCAD.getDocument("asm")
        doc.LibraryPaths = {NAME: os.path.join(ROOT, "gone")}
        check("a map naming no folder leaves the link", same(
            linked_file(), os.path.join(LIB2, "f", "bolt" + EXT)), linked_file())
        QtCore.QTimer.singleShot(500, answer)
        ret = FreeCAD.setLibraryPath(NAME, LIB)
        check("the change was asked about (Q8)", ASKED.get("box"), ASKED)
        check("listing the document", "asm" in ASKED.get("text", ""), ASKED.get("text"))
        check("the function returns what would change", ret == {"asm": 1}, ret)
        check("applied, the link is to the folder the settings name",
              same(linked_file(), PART), linked_file())
    except Exception:
        note("ABORT changed:\n" + traceback.format_exc())
    QtCore.QTimer.singleShot(300, preferences)


def look():
    try:
        dialog = QtWidgets.QApplication.activeModalWidget()
        table = None
        if dialog is not None:
            table = dialog.findChild(QtWidgets.QTableWidget, "libraryPathsTable")
        rows = {}
        if table is not None:
            for i in range(table.rowCount()):
                rows[table.item(i, 0).text()] = table.item(i, 1).text()
        check("the preference page lists the library paths",
              rows.get(NAME) == LIB, rows)
        if dialog is not None:
            dialog.reject()
    except Exception:
        note("ABORT look:\n" + traceback.format_exc())


def preferences():
    try:
        QtCore.QTimer.singleShot(1500, look)
        FreeCADGui.showPreferences("General", 1)
    except Exception:
        note("ABORT preferences:\n" + traceback.format_exc())
    finish()


def finish():
    try:
        FreeCAD.removeLibraryPath(NAME)
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
    except Exception:
        note("ABORT finish:\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, build)
