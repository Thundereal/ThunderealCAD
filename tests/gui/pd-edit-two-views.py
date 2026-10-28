"""Two PartDesign edits in one document, one per view, with PerViewEdit on
(docs/HandsOnQueue.md entry 232).

The reporter's decisions: each change of an edit is a step of its own,
OK keeps the steps, Cancel takes back the edit's own steps and no other
view's; where the other view changed one of them since, the user picks
"Undo anyway", "Skip" or "Keep". An object in edit in one view is not
put in edit in another. A view may edit what another view's edit depends
on.

A body: a box, a fillet of one of its edges, and a pad on a sketch after
it. Two 3D windows of the document: the fillet in edit in the first, the
pad in the second -- the pad depends on the fillet. The values are typed
into each panel, as the user does.

Claims:
  - both are in edit at once, each window answering its own, each with
    its dialog;
  - the fillet is not put in edit in the second window as well, and the
    pad's edit there is left as it was;
  - Cancel of the pad with a step of the fillet's after its own takes
    back the pad's and keeps the fillet's, and the fillet stays in edit;
  - with the pad's value changed again since by another step, Cancel asks:
    "Skip" leaves the other change, "Undo anyway" puts the pad's back;
  - OK of the fillet keeps its value and ends its edit.
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

VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name
         + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()
    for _ in range(5):
        QtCore.QCoreApplication.processEvents()


def activate(view):
    FreeCADGui.getMainWindow().setActiveWindow(view)
    settle(150)


def in_edit(gdoc, view):
    activate(view)
    vp = gdoc.getInEdit()
    return vp.Object.Name if vp else None


def type_into(view, name, text):
    """Type into the spin box `name` of the dialog of `view`."""
    activate(view)
    dlg = FreeCADGui.Control.activeTaskDialog(view=view)
    if dlg is None:
        return False
    box = None
    for w in dlg.getDialogContent():
        for b in w.findChildren(QtWidgets.QAbstractSpinBox):
            if b.objectName() == name:
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
    settle(300)
    return True


def answer_later(text):
    """Click `text` in the Cancel question once it is up (it is modal)."""
    def click():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if w.objectName() == "CancelOwnSteps" and w.isVisible():
                for b in w.buttons():
                    if b.text() == text:
                        state["asked"] = True
                        b.click()
                        return
        QtCore.QTimer.singleShot(100, click)
    state["asked"] = False
    QtCore.QTimer.singleShot(200, click)


def edit(gdoc, view, obj, name):
    activate(view)
    FreeCAD.setActiveTransaction(name)
    ok = gdoc.setEdit(obj, 0)
    settle(1000)
    return ok


def cancel(view):
    FreeCADGui.Control.activeTaskDialog(view=view).reject()
    settle(1000)


def run():
    try:
        VIEW.SetBool("PerViewEdit", True)
        doc = FreeCAD.newDocument("PdTwoViews")
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

        v1 = gdoc.mdiViewsOfType("Gui::View3DInventor")[0]
        activate(v1)
        FreeCADGui.runCommand("Std_ViewCreate")
        settle()
        views = gdoc.mdiViewsOfType("Gui::View3DInventor")
        if len(views) != 2:
            note("ABORT no second 3D view")
            return
        v2 = views[1]

        edit(gdoc, v1, fillet, "Edit Fillet")
        edit(gdoc, v2, pad, "Edit Pad")
        check("each window answers its own edit",
              (in_edit(gdoc, v1), in_edit(gdoc, v2)) == ("Fillet", "Pad"),
              (in_edit(gdoc, v1), in_edit(gdoc, v2)))
        check("each window has its dialog",
              FreeCADGui.Control.activeTaskDialog(view=v1) is not None
              and FreeCADGui.Control.activeTaskDialog(view=v2) is not None)

        activate(v2)
        refused = not gdoc.setEdit(fillet, 0)
        settle(300)
        boxes = [w for w in QtWidgets.QApplication.topLevelWidgets()
                 if w.objectName() == "EditInUse" and w.isVisible()]
        check("the fillet is not put in edit in the second window as well",
              refused and len(boxes) == 1, [w.text() for w in boxes])
        for w in boxes:
            w.close()
        settle()
        check("and the pad's edit there is left as it was", in_edit(gdoc, v2) == "Pad",
              in_edit(gdoc, v2))

        # The pad's step between two of the fillet's
        type_into(v1, "filletRadius", "1.5")
        check("the fillet's step is named for what it did",
              doc.UndoNames[:1] == ["Fillet: default radius"], doc.UndoNames)
        type_into(v2, "lengthEdit", "7")
        check("the pad's change is a step of its own",
              doc.UndoNames[:2] == ["Edit Pad", "Fillet: default radius"], doc.UndoNames)
        type_into(v1, "filletRadius", "1.2")
        check("the values typed are the features'",
              (round(fillet.Radius.Value, 3), round(pad.Length.Value, 3)) == (1.2, 7.0),
              (fillet.Radius, pad.Length))
        activate(v2)
        cancel(v2)
        check("Cancel of the pad takes back its value", round(pad.Length.Value, 3) == 15.0,
              pad.Length)
        check("and keeps the fillet's, though it came after",
              round(fillet.Radius.Value, 3) == 1.2, fillet.Radius)
        check("the fillet stays in edit, the pad does not",
              (in_edit(gdoc, v1), in_edit(gdoc, v2)) == ("Fillet", None),
              (in_edit(gdoc, v1), in_edit(gdoc, v2)))

        for choice, expected in (("Skip", 9.0), ("Undo anyway", 15.0)):
            edit(gdoc, v2, pad, "Edit Pad")
            type_into(v2, "lengthEdit", "8")
            # Another step over it, as another view's would be
            doc.openTransaction("theirs")
            pad.Length = 9
            doc.commitTransaction()
            activate(v2)
            answer_later(choice)
            cancel(v2)
            check("Cancel asks what to do with the change made since (%s)" % choice,
                  state["asked"])
            check("%s: the pad's value is %s" % (choice, expected),
                  round(pad.Length.Value, 3) == expected, pad.Length)
            if pad.Length.Value != 15:
                doc.openTransaction("back")
                pad.Length = 15
                doc.commitTransaction()

        activate(v1)
        FreeCADGui.Control.activeTaskDialog(view=v1).accept()
        settle(1000)
        check("OK of the fillet keeps its value", round(fillet.Radius.Value, 3) == 1.2,
              fillet.Radius)
        check("and ends its edit", in_edit(gdoc, v1) is None)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    VIEW.SetBool("PerViewEdit", False)
    try:
        FreeCADGui.resetEdit()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
