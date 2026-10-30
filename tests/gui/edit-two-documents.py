"""Two documents in edit at once, each with its own panel and its own step
(docs/HandsOnQueue.md entry 232).

Until now an edit started in one document ended any other's: the
application has one active transaction, and a second panel opening its
own would have committed the first one's work in progress. An edit's
transaction is now its document's own while the edit runs
(App.bindActiveTransaction), and Control lets the panel of another
document's edit stand beside a new one.

Two documents, each a body with a pad on a sketch. A's pad is put in edit
in A's window, then B's pad in B's window. Each edit changes its pad's
Label2 -- a property the pad's panel does not write back on OK, so what is
kept or taken back is the step's doing alone.

Claims:
  - with A in edit, B's window has no dialog in its way (the PD commands
    there are not greyed out by A's panel);
  - both documents are in edit, and each window's document answers its
    own pad;
  - each window has a task dialog of its own, both open at once;
  - an object in edit in one document is not put in edit through
    another (a link to it): refused, with a box saying where it is in
    edit and a button that goes there;
  - B's change goes in B's step and leaves A's open;
  - Cancel in B takes back B's change and ends B's edit, and A stays in
    edit with its change;
  - OK in A keeps A's change, and A's undo takes back A's change only.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = FreeCAD.Vector
state = {"done": False}

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
    settle(100)


def view3d(gdoc):
    return gdoc.mdiViewsOfType("Gui::View3DInventor")[0]


def make(name):
    doc = FreeCAD.newDocument(name)
    body = doc.addObject("PartDesign::Body", "Body")
    sketch = body.newObject("Sketcher::SketchObject", "Sketch")
    sketch.Support = (doc.getObject("XY_Plane"), [""])
    sketch.MapMode = "FlatFace"
    corners = [V(0, 0, 0), V(10, 0, 0), V(10, 10, 0), V(0, 10, 0)]
    for i in range(4):
        sketch.addGeometry(Part.LineSegment(corners[i], corners[(i + 1) % 4]))
    pad = body.newObject("PartDesign::Pad", "Pad")
    pad.Profile = sketch
    pad.Length = 10
    doc.recompute()
    doc.UndoMode = 1
    doc.clearUndos()
    settle()
    return doc, pad


def run():
    # The edit that holds one transaction to OK, which this is about --
    # an edit's one step, bound to its document: View/EditStepEachChange off; each change a step of
    # its own, the default, is tests/gui/edit-step-each-change.py
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetBool(
        "EditStepEachChange", False)
    try:
        a, padA = make("EditTwoA")
        b, padB = make("EditTwoB")
        ga, gb = FreeCADGui.getDocument(a.Name), FreeCADGui.getDocument(b.Name)
        va, vb = view3d(ga), view3d(gb)

        # As a command starting an edit does (a double click in the tree): the
        # step it opens is the edit's, which the edit's binding takes over
        activate(va)
        FreeCAD.setActiveTransaction("Edit A")
        ga.setEdit(padA, 0)
        settle(1000)
        if not check("A's pad is in edit", ga.getInEdit() is not None):
            return
        activate(va)
        padA.Label2 = "changed in A"

        activate(vb)
        check("with A in edit, nothing stands in B's window's way",
              not FreeCADGui.Control.activeDialog())

        # A's pad through a link of B's: the same object, in edit already
        link = b.addObject("App::Link", "LinkToA")
        link.LinkedObject = a.getObject("Body")
        b.recompute()
        settle()
        b.clearUndos()
        refused = not gb.setEdit(link, 0, "Pad.")
        settle(300)
        check("A's pad, in edit, is not put in edit again through B's link",
              refused and gb.getInEdit() is None)
        boxes = [w for w in QtWidgets.QApplication.topLevelWidgets()
                 if w.objectName() == "EditInUse" and w.isVisible()]
        check("a box says where it is in edit", len(boxes) == 1
              and "EditTwoA" in boxes[0].text(), [w.text() for w in boxes])
        if boxes:
            go = [btn for btn in boxes[0].buttons() if btn.text() == "Go there"]
            if check("with a button that goes there", len(go) == 1):
                # The click closes the box, which deletes itself
                go[0].click()
                settle(300)
                active = FreeCADGui.activeDocument().Document.Name
                check("which activates A's window", active == a.Name, active)
            else:
                boxes[0].close()
                settle()
        # In B's window: a change to B while A's is the active one would
        # go in A's step, as a part edited in place in an assembly does
        activate(vb)
        b.removeObject(link.Name)
        b.recompute()
        b.clearUndos()

        FreeCAD.setActiveTransaction("Edit B")
        gb.setEdit(padB, 0)
        settle(1000)
        editing = [d.Document.Name for d in FreeCADGui.editDocuments()]
        check("both documents are in edit", sorted(editing) == sorted([a.Name, b.Name]),
              editing)
        activate(va)
        inA = ga.getInEdit()
        activate(vb)
        inB = gb.getInEdit()
        check("each document answers its own pad",
              inA is not None and inA.Object == padA
              and inB is not None and inB.Object == padB, (inA, inB))

        dlgA = FreeCADGui.Control.activeTaskDialog(view=va)
        dlgB = FreeCADGui.Control.activeTaskDialog(view=vb)
        check("each window has a dialog of its own, both open",
              dlgA is not None and dlgB is not None, (dlgA, dlgB))

        activate(vb)
        padB.Label2 = "changed in B"
        check("B's change leaves A's step open", a.HasPendingTransaction)
        check("and is B's own", b.HasPendingTransaction)

        activate(vb)
        FreeCADGui.Control.activeTaskDialog(view=vb).reject()
        settle(1000)
        activate(vb)
        check("Cancel in B ends B's edit", gb.getInEdit() is None)
        check("and takes back B's change", padB.Label2 == "", padB.Label2)
        activate(va)
        check("A stays in edit", ga.getInEdit() is not None)
        check("with its change", padA.Label2 == "changed in A", padA.Label2)
        check("and its dialog", FreeCADGui.Control.activeDialog(view=va))

        FreeCADGui.Control.activeTaskDialog(view=va).accept()
        settle(1000)
        activate(va)
        check("OK in A ends A's edit", ga.getInEdit() is None)
        check("and keeps A's change", padA.Label2 == "changed in A", padA.Label2)
        a.undo()
        settle()
        check("A's undo takes back A's change", padA.Label2 == "", padA.Label2)
        check("and touches nothing of B's", padB.Label2 == "", padB.Label2)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCADGui.getDocument(name).resetEdit()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
