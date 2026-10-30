"""Two PartDesign edits in one document, one per view, with PerViewEdit on
(docs/HandsOnQueue.md entry 232).

The reporter's decisions: each change of an edit is a step of its own,
OK keeps the steps, Cancel takes back the edit's own steps and no other
view's; where the other view changed one of them since, the user picks
"Undo anyway", "Skip" or "Keep". An object in edit in one view is not
put in edit in another. A view may edit what another view's edit depends
on.

A body: a box, a fillet of one of its edges, a pad on a sketch after it,
and a helix after that. Two 3D windows of the document: the fillet in edit in the first, the
pad in the second -- the pad depends on the fillet. The values are typed
into each panel, as the user does.

Claims:
  - both are in edit at once, each window answering its own, each with
    its dialog;
  - the fillet is not put in edit in the second window as well, and the
    pad's edit there is left as it was;
  - Cancel of the pad with a step of the fillet's after its own takes
    back the pad's and keeps the fillet's, and the fillet stays in edit;
  - a change to the pad in edit that is not its panel's -- a script's --
    is the edit's: its Cancel takes it back too;
  - Cancel of a new feature, its body changed since by a change that is
    not the edit's, asks: "Skip" leaves the body's tip as changed, "Undo
    anyway" puts it back; the new feature goes either way;
  - a helix's edit in the second window, beside the fillet's, takes back
    its own on Cancel and keeps the fillet's;
  - OK of the fillet keeps its value and ends its edit;
  - a linear pattern's edit in the second window, beside the fillet's,
    takes back its own on Cancel and keeps the fillet's; its OK keeps its
    value and ends its edit only;
  - a MultiTransform's edit beside the fillet's: a transformation added
    in its panel goes with its Cancel, its feature too, and the fillet's
    change typed after it stays.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtGui, QtWidgets
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


def type_into_class(view, cls, text):
    """Type into the first visible spin box of class `cls` of the dialog of
    `view` -- the pattern editor's boxes have no names."""
    activate(view)
    dlg = FreeCADGui.Control.activeTaskDialog(view=view)
    if dlg is None:
        return False
    box = None
    for w in dlg.getDialogContent():
        for b in w.findChildren(QtWidgets.QAbstractSpinBox):
            if b.metaObject().className() == cls and b.isVisible() and box is None:
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


def trigger(view, text):
    """Trigger the action `text` of the dialog of `view`."""
    activate(view)
    dlg = FreeCADGui.Control.activeTaskDialog(view=view)
    if dlg is None:
        return False
    for w in dlg.getDialogContent():
        for a in w.findChildren(QtGui.QAction):
            if a.text() == text:
                a.trigger()
                settle(1000)
                return True
    return False


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
        # A helix of a small square about the sketch's vertical axis
        profile = body.newObject("Sketcher::SketchObject", "HelixProfile")
        profile.Support = (doc.getObject("XZ_Plane"), [""])
        profile.MapMode = "FlatFace"
        corners = [V(20, 0, 0), V(22, 0, 0), V(22, 2, 0), V(20, 2, 0)]
        for i in range(4):
            profile.addGeometry(Part.LineSegment(corners[i], corners[(i + 1) % 4]))
        helix = body.newObject("PartDesign::AdditiveHelix", "Helix")
        helix.Profile = profile
        helix.ReferenceAxis = (profile, ["V_Axis"])
        helix.Pitch = 5
        helix.Height = 10
        pattern = body.newObject("PartDesign::LinearPattern", "Pattern")
        pattern.Originals = [pad]
        pattern.Direction = (doc.getObject("X_Axis"), [""])
        pattern.Length = 30
        pattern.Occurrences = 2
        multi = body.newObject("PartDesign::MultiTransform", "Multi")
        multi.Originals = [pad]
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

        # A change to the feature in edit is the edit's, whoever makes it:
        # one from a timer, as a script's or the property editor's, goes
        # with its Cancel
        edit(gdoc, v2, pad, "Edit Pad")
        type_into(v2, "lengthEdit", "8")
        doc.openTransaction("a script's")
        pad.Length = 9
        doc.commitTransaction()
        activate(v2)
        cancel(v2)
        check("a change to the pad in edit, not the panel's, goes with its Cancel",
              round(pad.Length.Value, 3) == 15.0, pad.Length)

        # A new feature: the step that made it changed its body too, and
        # the body was changed since by a change that is not the edit's
        tip = body.Tip
        for n, (choice, expected) in enumerate((("Skip", box), ("Undo anyway", tip))):
            activate(v2)
            FreeCAD.setActiveTransaction("Create Pad")
            pad2 = body.newObject("PartDesign::Pad", "New%d" % n)
            pad2.Profile = sketch
            pad2.Length = 5
            name = pad2.Name
            # in the transaction that made it, as a command's edit is
            gdoc.setEdit(pad2, 0)
            settle(1000)
            type_into(v2, "lengthEdit", "6")
            doc.openTransaction("theirs")
            body.Tip = box
            doc.commitTransaction()
            activate(v2)
            answer_later(choice)
            cancel(v2)
            check("Cancel asks what to do with the body changed since (%s)" % choice,
                  state["asked"])
            check("%s: the new feature is gone" % choice, doc.getObject(name) is None,
                  name)
            check("%s: the body's tip is %s" % (choice, expected.Name),
                  body.Tip == expected, body.Tip and body.Tip.Name)
            if body.Tip != tip:
                doc.openTransaction("back")
                body.Tip = tip
                doc.commitTransaction()

        # The helix's panel too: beside the fillet's, its Cancel its own
        edit(gdoc, v2, helix, "Edit Helix")
        check("the helix goes in edit beside the fillet",
              (in_edit(gdoc, v1), in_edit(gdoc, v2)) == ("Fillet", "Helix"),
              (in_edit(gdoc, v1), in_edit(gdoc, v2)))
        type_into(v2, "height", "12")
        type_into(v1, "filletRadius", "1.3")
        check("the values typed are the features' (helix)",
              (round(fillet.Radius.Value, 3), round(helix.Height.Value, 3)) == (1.3, 12.0),
              (fillet.Radius, helix.Height))
        activate(v2)
        cancel(v2)
        check("Cancel of the helix takes back its height", round(helix.Height.Value, 3) == 10.0,
              helix.Height)
        check("and keeps the fillet's", round(fillet.Radius.Value, 3) == 1.3, fillet.Radius)

        activate(v1)
        FreeCADGui.Control.activeTaskDialog(view=v1).accept()
        settle(1000)
        check("OK of the fillet keeps its value", round(fillet.Radius.Value, 3) == 1.3,
              fillet.Radius)
        check("and ends its edit", in_edit(gdoc, v1) is None)

        # A pattern's panel too: beside the fillet's, its Cancel its own
        edit(gdoc, v1, fillet, "Edit Fillet")
        edit(gdoc, v2, pattern, "Edit Pattern")
        check("the pattern goes in edit beside the fillet",
              (in_edit(gdoc, v1), in_edit(gdoc, v2)) == ("Fillet", "Pattern"),
              (in_edit(gdoc, v1), in_edit(gdoc, v2)))
        typed = type_into_class(v2, "Gui::UIntSpinBox", "3")
        type_into(v1, "filletRadius", "1.4")
        check("the values typed are the features' (pattern)",
              typed and (round(fillet.Radius.Value, 3), pattern.Occurrences) == (1.4, 3),
              (typed, fillet.Radius, pattern.Occurrences))
        activate(v2)
        cancel(v2)
        check("Cancel of the pattern takes back its occurrences", pattern.Occurrences == 2,
              pattern.Occurrences)
        check("and keeps the fillet's (pattern)", round(fillet.Radius.Value, 3) == 1.4,
              fillet.Radius)
        check("the fillet stays in edit (pattern)", in_edit(gdoc, v1) == "Fillet",
              in_edit(gdoc, v1))
        edit(gdoc, v2, pattern, "Edit Pattern")
        type_into_class(v2, "Gui::UIntSpinBox", "4")
        activate(v2)
        FreeCADGui.Control.activeTaskDialog(view=v2).accept()
        settle(1000)
        check("OK of the pattern keeps its occurrences and ends its edit",
              pattern.Occurrences == 4 and in_edit(gdoc, v2) is None
              and in_edit(gdoc, v1) == "Fillet",
              (pattern.Occurrences, in_edit(gdoc, v2), in_edit(gdoc, v1)))

        # A MultiTransform's panel: what it adds is its own step
        edit(gdoc, v2, multi, "Edit Multi")
        check("the MultiTransform goes in edit beside the fillet",
              (in_edit(gdoc, v1), in_edit(gdoc, v2)) == ("Fillet", "Multi"),
              (in_edit(gdoc, v1), in_edit(gdoc, v2)))
        added = trigger(v2, "Add linear pattern")
        made = [o.Name for o in multi.Transformations]
        type_into(v1, "filletRadius", "1.5")
        check("a transformation added in its panel is the MultiTransform's",
              added and len(made) == 1, (added, made))
        activate(v2)
        cancel(v2)
        check("Cancel of the MultiTransform takes back the transformation added",
              multi.Transformations == [] and all(doc.getObject(n) is None for n in made),
              ([o.Name for o in multi.Transformations],
               [n for n in made if doc.getObject(n) is not None]))
        check("and keeps the fillet's (MultiTransform)", round(fillet.Radius.Value, 3) == 1.5,
              fillet.Radius)
        check("the fillet stays in edit (MultiTransform)", in_edit(gdoc, v1) == "Fillet",
              in_edit(gdoc, v1))

        activate(v1)
        FreeCADGui.Control.activeTaskDialog(view=v1).accept()
        settle(1000)
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
