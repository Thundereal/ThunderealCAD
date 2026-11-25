"""An old file spells digests once its user accepts the migration recompute.

docs/StringTableContentIds.md sec 12, the user 2026-11-18: "when open older
file, if prompt for migration and user accepts, only then auto enable the
content-id".

A document saved counting its string ids: a Part::Box with a shape with no
element names (what a file written before primitives named their elements
holds, so that it comes back wanting the migration), and a cut of two more
boxes, whose names spell ids of the table. Opened twice, the prompt
answered by a timer that finds it. Claims:
  - the prompt comes;
  - answered No: the table counts ids, the cut keeps its names, the box
    waits for the migration;
  - answered Yes: the table spells digests, the cut and the box are
    recomputed, the cut names the same elements as before by spellings the
    table holds.
"""
import os
import re
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


class Answer:
    """Clicks `button` on the migration prompt when it shows."""

    def __init__(self, button):
        self.button = button
        self.seen = False
        self.timer = QtCore.QTimer()
        self.timer.timeout.connect(self.look)
        self.timer.start(100)

    def look(self):
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QMessageBox) and w.isVisible() \
                    and "migration" in w.text():
                self.seen = True
                self.timer.stop()
                w.button(self.button).click()
                return

    def stop(self):
        self.timer.stop()


def spellings(hasher, text):
    out = []
    for x in re.findall(r"#([0-9a-f]+)", text):
        out.append(bool(hasher.getIDBySpelling(int(x, 16))))
    return out


def make(path):
    PARAM.SetBool("ContentStringIds", False)
    try:
        doc = FreeCAD.newDocument("Counted")
    finally:
        PARAM.RemBool("ContentStringIds")
    box = doc.addObject("Part::Box", "Box")
    a = doc.addObject("Part::Box", "A")
    b = doc.addObject("Part::Box", "B")
    b.Placement.Base = FreeCAD.Vector(5, 5, 5)
    cut = doc.addObject("Part::Cut", "Cut")
    cut.Base = a
    cut.Tool = b
    doc.recompute()
    box.Shape = Part.makeBox(10, 10, 10)
    for obj in doc.Objects:
        obj.purgeTouched()
    note("INFO saved: content ids %s, cut names %d" % (
        doc.Hasher.ContentIds, len(cut.Shape.ElementMap)))
    names = dict(cut.Shape.ElementMap)
    doc.saveAs(path)
    FreeCAD.closeDocument(doc.Name)
    pump(20)
    return names


def settle(doc, seconds=60):
    end = time.time() + seconds
    while time.time() < end:
        pump(10)
        if not any("Touched" in o.State for o in doc.Objects):
            return True
    return False


def run():
    warn = PARAM.GetBool("WarnRecomputeOnRestore", True)
    try:
        PARAM.SetBool("WarnRecomputeOnRestore", True)
        path = os.path.join(OUT, "counted.FCStd")
        old = make(path)
        check("the saved cut names elements with ids", any("#" in k for k in old))

        answer = Answer(QtWidgets.QMessageBox.No)
        doc = FreeCAD.openDocument(path)
        pump(60)
        answer.stop()
        check("No: the prompt came", answer.seen)
        note("INFO No: cut %s, table %d strings, transactions %d" % (
            doc.getObject("Cut").State, doc.Hasher.Size, len(doc.getTransactionVersions())))
        check("No: the table counts ids", not doc.Hasher.ContentIds)
        check("No: the cut keeps its names", dict(doc.getObject("Cut").Shape.ElementMap) == old)
        check("No: the box waits for the migration",
              "MigrationPending" in doc.getObject("Box").State, doc.getObject("Box").State)
        FreeCAD.closeDocument(doc.Name)
        pump(20)

        answer = Answer(QtWidgets.QMessageBox.Yes)
        doc = FreeCAD.openDocument(path)
        pump(60)
        answer.stop()
        check("Yes: the prompt came", answer.seen)
        check("Yes: the table spells digests", doc.Hasher.ContentIds)
        check("Yes: the recompute is done", settle(doc),
              [(o.Name, o.State) for o in doc.Objects])
        cut = doc.getObject("Cut")
        new = dict(cut.Shape.ElementMap)
        check("Yes: the cut's names are made again", new != old)
        check("Yes: ... naming the same elements",
              sorted(new.values()) == sorted(old.values()), (len(new), len(old)))
        missing = [k for k in new if not all(spellings(doc.Hasher, k))]
        check("Yes: ... by spellings the table holds", not missing, missing[:3])
        check("Yes: the box is named", doc.getObject("Box").Shape.ElementMapSize > 0)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        PARAM.SetBool("WarnRecomputeOnRestore", warn)
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
