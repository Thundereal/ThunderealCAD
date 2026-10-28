"""Two sketches in edit in one document, one per view, with PerViewEdit on
(docs/HandsOnQueue.md entry 232, the Sketcher round).

A sketch's edit holds no transaction: each tool's change is a step of its
own, and the steps opened while the edit is the one worked in are its own.
Its Cancel takes back those and no other view's; where something of them
was changed since by another view's step, the user picks "Undo anyway",
"Skip" or "Keep" (the reporter's ruling). Its OK ends its own edit only.

Two sketches, A and B, two 3D windows: A in edit in the first, B in the
second. A step stands for a tool's change -- a transaction opened, a line
added, committed -- made with the window of the edit active, as a tool's
is.

Claims:
  - both are in edit at once, each window answering its own, each with
    its dialog;
  - A is not put in edit in the second window as well;
  - Cancel of B, with a step of A's after its own, takes back B's lines
    and keeps A's; A stays in edit;
  - with B's lines changed since by A's edit, Cancel asks: "Skip" leaves
    them, "Undo anyway" takes B back to none;
  - OK of A keeps its lines and ends its edit;
  - the tree marks each sketch in edit, and drops only the mark of the
    edit that ends.
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


def marked(name):
    """Whether the tree marks the item `name` as in edit (its background)."""
    for tree in FreeCADGui.getMainWindow().findChildren(QtWidgets.QTreeWidget):
        it = QtWidgets.QTreeWidgetItemIterator(tree)
        while it.value():
            item = it.value()
            if item.text(0) == name and item.background(0).style() != QtCore.Qt.NoBrush:
                return True
            it += 1
    return False


def edit(gdoc, view, obj):
    activate(view)
    ok = gdoc.setEdit(obj, 0)
    settle(1000)
    return ok


def step(doc, view, name, sketch, y):
    """A tool's change in `view`: a line added to `sketch`, one step."""
    activate(view)
    doc.openTransaction(name)
    sketch.addGeometry(Part.LineSegment(V(0, y, 0), V(10, y, 0)))
    doc.commitTransaction()
    doc.recompute()
    settle(200)


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


def cancel(view):
    activate(view)
    FreeCADGui.Control.activeTaskDialog(view=view).reject()
    settle(1000)


def run():
    try:
        VIEW.SetBool("PerViewEdit", True)
        doc = FreeCAD.newDocument("SketchTwoViews")
        a = doc.addObject("Sketcher::SketchObject", "A")
        b = doc.addObject("Sketcher::SketchObject", "B")
        b.Placement = FreeCAD.Placement(V(0, 0, 20), FreeCAD.Rotation())
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

        edit(gdoc, v1, a)
        edit(gdoc, v2, b)
        check("each window answers its own sketch",
              (in_edit(gdoc, v1), in_edit(gdoc, v2)) == ("A", "B"),
              (in_edit(gdoc, v1), in_edit(gdoc, v2)))
        check("each window has its dialog",
              FreeCADGui.Control.activeTaskDialog(view=v1) is not None
              and FreeCADGui.Control.activeTaskDialog(view=v2) is not None)
        check("the tree marks both as in edit", (marked("A"), marked("B")) == (True, True),
              (marked("A"), marked("B")))

        activate(v2)
        refused = not gdoc.setEdit(a, 0)
        settle(300)
        boxes = [w for w in QtWidgets.QApplication.topLevelWidgets()
                 if w.objectName() == "EditInUse" and w.isVisible()]
        check("A is not put in edit in the second window as well",
              refused and len(boxes) == 1, [w.text() for w in boxes])
        for w in boxes:
            w.close()
        settle()
        check("and B's edit there is left as it was", in_edit(gdoc, v2) == "B",
              in_edit(gdoc, v2))

        # B's step between two of A's
        step(doc, v1, "A line 1", a, 0)
        step(doc, v2, "B line 1", b, 0)
        step(doc, v1, "A line 2", a, 5)
        check("the lines are the sketches'", (a.GeometryCount, b.GeometryCount) == (2, 1),
              (a.GeometryCount, b.GeometryCount))
        cancel(v2)
        check("Cancel of B takes back its line", b.GeometryCount == 0, b.GeometryCount)
        check("and keeps A's, though one came after", a.GeometryCount == 2, a.GeometryCount)
        check("A stays in edit, B does not",
              (in_edit(gdoc, v1), in_edit(gdoc, v2)) == ("A", None),
              (in_edit(gdoc, v1), in_edit(gdoc, v2)))
        check("and the tree marks A only", (marked("A"), marked("B")) == (True, False),
              (marked("A"), marked("B")))

        for choice, expected in (("Skip", 2), ("Undo anyway", 0)):
            edit(gdoc, v2, b)
            step(doc, v2, "B line", b, 10)
            # A's edit changes B's lines over it
            step(doc, v1, "A on B", b, 15)
            answer_later(choice)
            cancel(v2)
            check("Cancel asks what to do with the change made since (%s)" % choice,
                  state["asked"])
            check("%s: B has %d lines" % (choice, expected), b.GeometryCount == expected,
                  b.GeometryCount)
            if b.GeometryCount:
                doc.openTransaction("clear B")
                b.deleteAllGeometry()
                doc.commitTransaction()

        activate(v1)
        FreeCADGui.Control.activeTaskDialog(view=v1).accept()
        settle(1000)
        check("OK of A keeps its lines", a.GeometryCount == 2, a.GeometryCount)
        check("and ends its edit", in_edit(gdoc, v1) is None)
        check("its tree mark with it", not marked("A"))
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
