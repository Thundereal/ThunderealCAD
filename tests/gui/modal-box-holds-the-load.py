"""A modal box up during a progressive load holds the load (entry 242).

The reporter 2026-11-02: "it seems the migration recompute dialog is not
load modal, neither is recompute error dialog." Both boxes were modal to
the input -- the main window disabled, a click on it and a wheel in the 3D
view went nowhere -- but they come up from the end of the open, while a
progressive load is still building the scene in slices off timers, and the
box's own event loop kept running those slices: the model went on filling
in behind the question about recomputing it. Now a timed slice of the view
provider drain or the visual drain waits while a modal widget is up
(Gui::Application::isLoadHeld).

The migration prompt needs a file that needs migrating; this raises a modal
box of its own at the same moment instead. A document of SPHERES spheres
and BOXES boxes is saved and opened again with ProgressiveLoad on and
PreMeshOnLoad off (every visual built on the GUI thread, in slices). As
soon as the open returns with the scene still building, a modal box is
shown for two seconds. Claims:
  - the scene is still being built when the box comes up;
  - no visual is built while the box is up, and the scene is still building
    when it goes;
  - once it has gone the load ends, every object with its visual.
Then (entry 247: the recompute the migration prompt starts shows a modal
progress dialog, which held the load -- the view black until its end, the
restore under the recompute not done) the document is opened again under a
modal progress dialog: only a question box holds the load, so visuals are
built under it, and the load ends.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
PATH = os.path.join(OUT, "modal-holds-load.FCStd")
SPHERES = int(os.environ.get("GT_SPHERES", "300"))
BOXES = int(os.environ.get("GT_BOXES", "300"))
BOX_SECONDS = 2.0

state = {"done": False}


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


def builds():
    return FreeCADGui.visualBuildStats()["count"]


def build():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        render = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render")
        render.SetBool("ProgressiveLoad", True)
        render.SetBool("PreMeshOnLoad", False)
        doc = FreeCAD.newDocument("ModalHoldsLoad")
        for i in range(SPHERES):
            obj = doc.addObject("Part::Sphere", "S%d" % i)
            obj.Radius = 4 + 0.001 * i
            obj.Placement.Base = FreeCAD.Vector(12 * (i % 20), 12 * (i // 20), 0)
        for i in range(BOXES):
            obj = doc.addObject("Part::Box", "B%d" % i)
            obj.Length = 5 + 0.001 * i
            obj.Placement.Base = FreeCAD.Vector(12 * (i % 20), 12 * (i // 20), 30)
        doc.recompute()
        doc.saveAs(PATH)
        FreeCAD.closeDocument(doc.Name)
        pump(1.0)
        note("INFO %d spheres and %d boxes saved" % (SPHERES, BOXES))
        QtCore.QTimer.singleShot(500, load)
    except Exception:
        note("ABORT build | " + traceback.format_exc().replace("\n", " | "))
        finish()


def load():
    try:
        doc = FreeCAD.openDocument(PATH)
        state["name"] = doc.Name
        building = FreeCADGui.isBuildingVisuals()
        note("INFO the open returned: building %s, %d built" % (building, builds()))
        if not check("the scene is still being built when the open returns", building):
            finish()
            return
        box = QtWidgets.QMessageBox(QtWidgets.QMessageBox.Question, "A question",
                                    "The load waits for the answer.",
                                    QtWidgets.QMessageBox.Ok, FreeCADGui.getMainWindow())
        seen = {}

        def up():
            seen["start"] = builds()
            seen["modal"] = QtWidgets.QApplication.activeModalWidget() is box
            QtCore.QTimer.singleShot(int(BOX_SECONDS * 1000), down)

        def down():
            seen["end"] = builds()
            seen["building"] = FreeCADGui.isBuildingVisuals()
            box.accept()

        QtCore.QTimer.singleShot(100, up)
        box.exec()
        note("INFO the box: %s" % seen)
        check("the box was the modal widget", seen.get("modal"), seen)
        check("no visual is built while the box is up",
              "start" in seen and seen["end"] == seen["start"], seen)
        check("the scene is still building when the box goes", seen.get("building"), seen)
        # the load goes on once the answer is given
        end = time.time() + 120
        while FreeCADGui.isBuildingVisuals() and time.time() < end:
            pump(0.2)
        building = FreeCADGui.isBuildingVisuals()
        made = 0
        for obj in doc.Objects:
            bound = obj.ViewObject.getBoundingBox()
            if bound.isValid() and bound.DiagonalLength > 0:
                made += 1
        note("INFO after the box: building %s, %d built, %d of %d with a visual"
             % (building, builds(), made, len(doc.Objects)))
        check("once the box has gone the load ends", not building)
        check("every object has its visual", made == len(doc.Objects),
              "%d of %d" % (made, len(doc.Objects)))
        FreeCAD.closeDocument(doc.Name)
        pump(1.0)
        QtCore.QTimer.singleShot(500, load_under_progress)
        return
    except Exception:
        note("ABORT load | " + traceback.format_exc().replace("\n", " | "))
    finish()


def load_under_progress():
    """Entry 247: the recompute the migration prompt starts shows a modal
    progress dialog, which held the load (the view black until the end, the
    restore under the recompute not done). Only a question box holds it."""
    try:
        doc = FreeCAD.openDocument(PATH)
        building = FreeCADGui.isBuildingVisuals()
        note("INFO opened again: building %s, %d built" % (building, builds()))
        if not building:
            note("INFO the load ended before a dialog could come up")
            return
        progress = QtWidgets.QProgressDialog("Recomputing", "", 0, 0, FreeCADGui.getMainWindow())
        progress.setWindowModality(QtCore.Qt.ApplicationModal)
        seen = {}

        def up():
            seen["start"] = builds()
            seen["modal"] = QtWidgets.QApplication.activeModalWidget() is progress
            QtCore.QTimer.singleShot(int(BOX_SECONDS * 1000), down)

        def down():
            seen["end"] = builds()
            progress.reset()
            progress.close()

        QtCore.QTimer.singleShot(100, up)
        progress.exec()
        note("INFO the progress dialog: %s" % seen)
        check("the progress dialog was the modal widget", seen.get("modal"), seen)
        check("a modal progress dialog does not hold the load: visuals built under it",
              "start" in seen and seen["end"] > seen["start"], seen)
        end = time.time() + 120
        while FreeCADGui.isBuildingVisuals() and time.time() < end:
            pump(0.2)
        check("and the load ends", not FreeCADGui.isBuildingVisuals())
    except Exception:
        note("ABORT progress | " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(2000, build)
