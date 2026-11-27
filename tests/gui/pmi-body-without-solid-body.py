"""A PMI annotation on a Body that has no solid body makes the solid body, not a message.

The reporter, 2026-11-26: "when try to create a PMI object with exiting body
without a SolidBody it prints a message, which the user (even me) has no
clue what to do next. how about just auto create the SolidBody, add the
group for annotation and then create the PMI object". The message: "the Body
has no solid body yet: recompute it once it holds an annotation set". The
command made the set only for a Body with none and relied on the recompute
after it; a Body that had its set made no set and no recompute.

Three Bodies with a box: one with its annotation set and its solid body
removed, nothing recomputed since; one with AddSolidBody off and no set, as
a Body restored from a file older than that property (the reporter: "I get
the message by opening a legacy file where body does not have SolidBody");
one with it off and a set. In each a face of the box picked and PMI_Datum
run. Claims: no message box; the Body has a solid body and AddSolidBody on;
a datum is made; one undo step takes it all back.

Runs on the hidden desktop (scripts/gui-test.cmd); GT_EXPECT_OLD=1 for a
build without the fix, which says the message.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
state = {"done": False}
boxes = []


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


def sweep():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(w, QtWidgets.QMessageBox) and w.isVisible():
            boxes.append(w.text())
            w.close()


SWEEPER = QtCore.QTimer()
SWEEPER.timeout.connect(sweep)
SWEEPER.start(200)


def solids(body):
    return [o.Name for o in body.Group if o.TypeId == "PartDesign::SolidBody"]


def pmi_objects(doc):
    return sorted(o.TypeId for o in doc.Objects if o.TypeId.startswith("PMI::"))


def make(case):
    """A Body with a box in the state the case names"""
    import PMI

    doc = FreeCAD.newDocument("PMINoSolid" + case)
    doc.UndoMode = 1
    body = doc.addObject("PartDesign::Body", "Body")
    box = doc.addObject("PartDesign::AdditiveBox", "Box")
    body.addObject(box)
    doc.recompute()
    if case == "removed":
        # its set there, its solid body removed, nothing recomputed since
        PMI.getSet(body)
        doc.recompute()
        doc.removeObject(solids(body)[0])
    elif case == "legacy":
        # as restored from a file older than AddSolidBody: off, no set
        body.AddSolidBody = False
        doc.recompute()
    elif case == "legacy-set":
        # off, with a set made before it was switched off
        PMI.getSet(body)
        doc.recompute()
        body.AddSolidBody = False
        doc.recompute()
    settle(1)
    return doc, body


CASES = (("removed", "a set, no solid body"),
         ("legacy", "AddSolidBody off, no set"),
         ("legacy-set", "AddSolidBody off, a set"))


def one(case, what):
    import PMI

    del boxes[:]
    doc, body = make(case)
    has_set = PMI.getSet(body, False) is not None
    check("%s: the state: %s" % (case, what), not solids(body)
          and has_set == (case != "legacy")
          and bool(body.AddSolidBody) == (case == "removed"),
          [o.Name for o in body.Group] + [body.AddSolidBody])
    before = pmi_objects(doc)
    undo = doc.UndoCount
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(doc.Name, body.Name, "Box.Face6")
    FreeCADGui.runCommand("PMI_Datum")
    settle(2)
    sweep()
    after = pmi_objects(doc)
    note("INFO %s: boxes %s" % (case, boxes))
    note("INFO %s: PMI objects %s -> %s" % (case, before, after))
    if os.environ.get("GT_EXPECT_OLD"):
        check("%s: before the fix: the message" % case,
              any("no solid body" in b for b in boxes), boxes)
        return
    check("%s: no message box" % case, not boxes, boxes)
    check("%s: the Body has a solid body" % case, bool(solids(body)),
          [o.Name for o in body.Group])
    check("%s: AddSolidBody on" % case, bool(body.AddSolidBody))
    check("%s: a datum made" % case,
          len(after) > len(before) and any("Datum" in t for t in after), after)
    check("%s: one undo step" % case, doc.UndoCount == undo + 1, (undo, doc.UndoCount))
    doc.undo()
    doc.recompute()
    settle(1)
    check("%s: undone: no datum" % case, pmi_objects(doc) == before, pmi_objects(doc))
    check("%s: undone: AddSolidBody as it was" % case,
          bool(body.AddSolidBody) == (case == "removed"), body.AddSolidBody)


def run():
    try:
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        settle(2)
        for case, what in CASES:
            try:
                one(case, what)
            except Exception:
                note("ABORT %s: " % case + traceback.format_exc().replace("\n", " | "))
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


QtCore.QTimer.singleShot(3000, run)
