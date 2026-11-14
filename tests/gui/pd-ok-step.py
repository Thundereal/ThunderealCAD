"""OK of a PartDesign feature's panel is an undo step of its own, holding
the recompute OK makes (docs/HandsOnQueue.md entry 228: "Ok would be
separate as it normally triggers the recompute").

The step OK opened was committed by the edit's end, before the recompute
that follows it: the step was left empty, and the recompute's writes went
into a step of their own, an implicit one, under no name.

An OK with nothing changed is a step still: it remakes the fillet and what
follows it, to the same shapes, and the log records those writes, each
op's value the one it had. It has its row, unlike the empty step before.

A body: a box, a fillet of one of its edges, and a pad on a sketch after
the fillet. The fillet in edit, a radius typed into its panel, OK: the pad
is made again on the new fillet.

Claims:
  - OK of the fillet is the newest undo step, named for it;
  - the pad made again is in it: no implicit row after the typed step;
  - it is one of the edit's group, with the typed step.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets
from PySide6 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = FreeCAD.Vector
Qt = QtCore.Qt
state = {"done": False}

FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()
    for _ in range(5):
        QtCore.QCoreApplication.processEvents()


def type_into(name, text):
    """Type into the spin box `name` of the open dialog."""
    dlg = FreeCADGui.Control.activeTaskDialog()
    if dlg is None:
        return False
    box = None
    for w in dlg.getDialogContent():
        for b in w.findChildren(QtWidgets.QAbstractSpinBox):
            if b.objectName() == name and (box is None or not box.isVisible()):
                box = b
    if box is None:
        return False
    box.window().activateWindow()
    box.setFocus()
    settle(20)
    line = box.findChild(QtWidgets.QLineEdit)
    line.selectAll()
    QtTest.QTest.keyClicks(line, text)
    QtTest.QTest.keyClick(box, Qt.Key_Return)
    settle(200)
    FreeCADGui.getMainWindow().setFocus()
    settle(1500)
    return True


def edit(gdoc, obj):
    FreeCAD.setActiveTransaction("Edit " + obj.Name)
    gdoc.setEdit(obj, 0)
    settle(1000)


def last_seq(doc):
    return max([r["seq"] for r in doc.getTransactionLog()] or [0])


def run():
    try:
        doc = FreeCAD.newDocument("PdOkStep")
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1"])
        fillet.Radius = 1.0
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        sketch.Support = (doc.getObject("XY_Plane"), [""])
        sketch.MapMode = "FlatFace"
        corners = [V(2, 2, 0), V(8, 2, 0), V(8, 8, 0), V(2, 8, 0)]
        for i in range(4):
            sketch.addGeometry(Part.LineSegment(corners[i], corners[(i + 1) % 4]))
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 15
        doc.recompute()
        doc.UndoMode = 1
        doc.clearUndos()
        gdoc = FreeCADGui.getDocument(doc.Name)
        settle()

        edit(gdoc, fillet)
        typed = type_into("filletRadius", "1.5")
        steps = doc.getUndoSteps()
        check("the radius typed is a step", typed and steps and steps[0][1] != "Fillet: OK, recompute",
              steps[:2])
        seq = last_seq(doc)
        FreeCADGui.Control.activeTaskDialog().accept()
        settle(1500)
        after = doc.getUndoSteps()
        check("OK of the fillet is the newest step, named for it",
              after and after[0][1] == "Fillet: OK, recompute", after[:3])
        rows = [(r["kind"], r["name"]) for r in doc.getTransactionLog() if r["seq"] > seq]
        check("the pad made again is in it: no implicit row",
              all(kind != "implicit" for kind, _ in rows) and ("user", "Fillet: OK, recompute") in rows,
              rows)
        group = {g for _, _, g in after[:2]}
        check("it is one of the edit's group, with the typed step",
              len(after) >= 2 and len(group) == 1 and 0 not in group, after[:3])

    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.resetEdit()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
