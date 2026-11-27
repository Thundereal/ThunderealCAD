"""Std_Export writes a STEP file in a worker, by default, and here when the
setting says no.

docs/WorkerProcess.md sec 24, the user 2026-11-19: "work on export".
ExportCommand of Preferences/Workers ("Export in a worker process" in the
Document preferences) is on unless set off. Claims:
  - with the setting as it comes, Std_Export of a STEP file (ImportGui's,
    its options asked here) returns with the export still with a worker,
    and the status bar says so;
  - the file comes, and is the design as it was when asked for: the box
    edited and recomputed right after the command is written at its old
    size;
  - set off, Std_Export writes the file here at once, and its colours,
    read back, are the worker's: the looks the worker asks of the objects
    are what this process's view providers give.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part  # noqa: F401
from PySide import QtCore, QtWidgets

OUT = os.path.realpath(os.environ["GT_OUT"])
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
WORKERS = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Workers")
DIALOG = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Dialog")
STEP = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Part/STEP")


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


MESSAGES = []
RETURNED = {}


def exportTo(path, pool):
    """Std_Export, its file dialog answered with `path`; RETURNED says how it
    was when the command came back"""

    answered = [False]
    RETURNED.clear()

    def returned():
        info = pool.info()
        RETURNED.update(busy=info["Busy"] + info["Queued"], exists=os.path.exists(path))

    def answer():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QFileDialog) and w.isVisible():
                w.selectNameFilter([f for f in w.nameFilters() if "STEP" in f][0])
                w.setDirectory(os.path.dirname(path))
                w.selectFile(os.path.basename(path))
                edit = w.findChild(QtWidgets.QLineEdit, "fileNameEdit")
                if edit:
                    edit.setText(os.path.basename(path))
                w.accept()
                answered[0] = True
                # Run once the command is back from the dialog and done
                QtCore.QTimer.singleShot(0, returned)
                return
        QtCore.QTimer.singleShot(100, answer)

    QtCore.QTimer.singleShot(300, answer)
    FreeCADGui.runCommand("Std_Export")
    # The command runs from the event loop: done once the dialog was
    # answered and the events after it taken
    end = time.time() + 30
    while not (answered[0] and RETURNED) and time.time() < end:
        pump(0.05)
    pump(0.5)
    return answered[0]


BOXES = []


def closeBoxes():
    """A message box is a failure told: noted, and closed"""
    for w in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(w, QtWidgets.QMessageBox) and w.isVisible():
            BOXES.append(w.text())
            note("message box: " + w.text())
            w.close()


def readBack(path):
    """The volume in the file, and each face's colour"""
    import Import

    doc = FreeCAD.newDocument("ReadBack")
    try:
        Import.insert(path, doc.Name)
        doc.recompute()
        parts = [o for o in doc.Objects if o.isDerivedFrom("Part::Feature") and o.Shape.Faces]
        volume = sum(o.Shape.Volume for o in parts)
        looks = sorted(tuple(tuple(round(c * 255) for c in m.DiffuseColor[:3])
                             for m in o.ShapeAppearance) for o in parts)
        return volume, looks
    finally:
        FreeCAD.closeDocument(doc.Name)


def run():
    boxes = QtCore.QTimer()
    boxes.timeout.connect(closeBoxes)
    boxes.start(200)
    try:
        WORKERS.RemBool("ExportCommand")
        DIALOG.SetBool("DontUseNativeDialog", True)
        STEP.SetBool("VisibleExportDialog", False)  # options from the settings
        doc = FreeCAD.newDocument("ExportByWorker")
        box = doc.addObject("Part::Box", "Box")
        doc.recompute()
        box.ShapeColor = (1.0, 0.0, 0.0)
        faces = box.ShapeAppearance.copy()
        faces.setSize(6)
        look = faces[2]
        look.DiffuseColor = (0.0, 0.0, 1.0, 1.0)
        faces[2] = look
        box.ShapeAppearance = faces
        doc.recompute()
        pump(1.0)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(box)
        pool = FreeCAD.workerPool(doc)
        there = os.path.join(OUT, "there.step")
        if os.path.exists(there):
            os.remove(there)

        bar = FreeCADGui.getMainWindow().statusBar()
        bar.messageChanged.connect(MESSAGES.append)
        check("the dialog was answered", exportTo(there, pool))
        check("by default, Std_Export hands it to a worker",
              RETURNED.get("busy") == 1 and not RETURNED.get("exists"), RETURNED)
        check("... and the status bar said so",
              any("in a worker process" in m for m in MESSAGES), MESSAGES)
        # An edit at once: the file is the design as it was asked for
        box.Length = 25
        doc.recompute()
        end = time.time() + 120
        while (pool.info()["Busy"] or pool.info()["Queued"]) and time.time() < end:
            pump(0.05)
        pump(0.3)
        check("the file comes", os.path.exists(there))
        check("... the status bar says it was a worker's",
              any("by a worker" in m for m in MESSAGES), MESSAGES)
        volume, looksThere = readBack(there)
        check("... the box as it was when asked for", abs(volume - 1000.0) < 1e-6, volume)
        check("... its faces' colours", looksThere == [tuple([(255, 0, 0)] * 2 + [(0, 0, 255)]
                                                             + [(255, 0, 0)] * 3)], looksThere)

        WORKERS.SetBool("ExportCommand", False)
        here = os.path.join(OUT, "here.step")
        if os.path.exists(here):
            os.remove(here)
        FreeCAD.setActiveDocument(doc.Name)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(box)
        check("the dialog was answered again", exportTo(here, pool))
        check("no failure was told", not BOXES, BOXES)
        check("set off, Std_Export writes it here at once",
              RETURNED.get("exists") and RETURNED.get("busy") == 0, RETURNED)
        volume, looksHere = readBack(here)
        check("... the box as it is now", abs(volume - 2500.0) < 1e-6, volume)
        check("... the colours the worker wrote", looksHere == looksThere, (looksHere, looksThere))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        WORKERS.RemBool("ExportCommand")
        DIALOG.RemBool("DontUseNativeDialog")
        STEP.RemBool("VisibleExportDialog")
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
