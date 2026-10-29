"""An edit recomputes what it needs only when that waits for the migration.

docs/HandsOnQueue.md entries 209 and 211, the user 2026-10-22: an old file
opened without its migration recompute keeps a feature's old element names,
so the edit's preview could not be told which reference each face shows --
"1, but make sure only do this for the migration case".

A Part::Box saved with a shape with no element names (what a file written
before primitives named their elements holds) comes back wanting the
migration recompute (Document::addRecomputeObject); a Mirroring of it. A
second box and its mirror, saved as they are, the second box then changed
by hand after the open. The migration prompt is turned off for the run.
Claims:
  - the stripped box comes back marked MigrationPending, the other not;
  - an edit of the second mirror leaves the user's change touched;
  - an edit of the first mirror recomputes the box: neither marked nor
    touched after, its shape named again;
  - the user's change is still touched after that too.
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
PARAM = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def edit(gdoc, obj):
    gdoc.setEdit(obj, 0)
    pump(40)
    gdoc.resetEdit()
    pump(40)


def run():
    warn = PARAM.GetBool("WarnRecomputeOnRestore", True)
    try:
        PARAM.SetBool("WarnRecomputeOnRestore", False)
        path = os.path.join(OUT, "edit-migration.FCStd")
        doc = FreeCAD.newDocument("EditMigration")
        box = doc.addObject("Part::Box", "Box")
        mirror = doc.addObject("Part::Mirroring", "Mirror")
        mirror.Source = box
        box2 = doc.addObject("Part::Box", "Box2")
        box2.Placement.Base = FreeCAD.Vector(30, 0, 0)
        mirror2 = doc.addObject("Part::Mirroring", "Mirror2")
        mirror2.Source = box2
        doc.recompute()
        # the box's shape as an old file has it: no element names
        box.Shape = Part.makeBox(10, 10, 10)
        note("INFO stripped shape's element map size %d" % box.Shape.ElementMapSize)
        for obj in doc.Objects:
            obj.purgeTouched()
        doc.saveAs(path)
        FreeCAD.closeDocument(doc.Name)
        pump(20)

        doc = FreeCAD.openDocument(path)
        pump(60)
        box, mirror = doc.getObject("Box"), doc.getObject("Mirror")
        box2, mirror2 = doc.getObject("Box2"), doc.getObject("Mirror2")
        note("INFO after the open: Box %s, Box2 %s" % (box.State, box2.State))
        check("the stripped box comes back waiting for the migration",
              "MigrationPending" in box.State, box.State)
        check("... and the other box not", "MigrationPending" not in box2.State, box2.State)

        box2.Length = 20
        gdoc = FreeCADGui.getDocument(doc.Name)
        edit(gdoc, mirror2)
        # a Part::Box rebuilds its own shape on a size change; the mirror shows
        # whether the edit recomputed
        check("an edit with no migration waiting leaves the user's change touched",
              "Touched" in box2.State and mirror2.Shape.BoundBox.XLength < 15,
              (box2.State, mirror2.Shape.BoundBox.XLength))

        edit(gdoc, mirror)
        check("an edit of what waits for the migration recomputes it",
              "MigrationPending" not in box.State and "Touched" not in box.State,
              box.State)
        check("... its shape named again", box.Shape.ElementMapSize > 0,
              box.Shape.ElementMapSize)
        check("... and the user's change is still left touched",
              "Touched" in box2.State, box2.State)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        PARAM.SetBool("WarnRecomputeOnRestore", warn)
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
