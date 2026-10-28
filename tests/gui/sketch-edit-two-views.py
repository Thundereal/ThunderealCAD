"""Two sketches in edit in one document, one per view, with PerViewEdit on
(docs/HandsOnQueue.md entry 232, the Sketcher round).

A sketch's edit holds no transaction: each tool's change is a step of its
own. Which steps are its own is said by the code that opens them
(Gui::EditStepScope): a Sketcher command run in the edit's window, an event
to its view or its panel; and any step that changes the sketch, whoever
makes it, as no other edit may. Its Cancel takes back those and no other.
Its OK ends its own edit only.

Two sketches, A with two lines and B with one, two 3D windows: A in edit in
the first, B in the second. An edit's step is Sketcher_DeleteAllGeometry run in
its window. A change made from a timer -- this script runs in one -- is
nobody's, whichever window is active -- it stands for another view's panel
writing late, after the user has clicked into the sketch's window -- unless
it changes a sketch in edit.

Claims:
  - both are in edit at once, each window answering its own, each with
    its dialog, the tree marking both;
  - A is not put in edit in the second window as well;
  - Cancel of B, with a step of A's after its own and a timer's change to
    A made with B's window active, takes back B's step only: A's step and
    the change to A stay; A stays in edit, the tree marks A only;
  - a timer's change to B while B is in edit, made with A's window
    active, is B's: Cancel of B takes it back with B's step;
  - OK of A keeps what it did and ends its edit, its tree mark with it.
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


def click_later(name, text):
    """Click `text` in the modal box `name` (objectName, or its text) once
    it is up."""
    def click():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QMessageBox) and w.isVisible() \
                    and (w.objectName() == name or w.text() == name):
                for b in w.buttons():
                    if b.text().replace("&", "") == text:
                        state["asked"] = True
                        b.click()
                        return
        QtCore.QTimer.singleShot(100, click)
    state["asked"] = False
    QtCore.QTimer.singleShot(200, click)


def delete_all(view):
    """The edit's step: Sketcher_DeleteAllGeometry run in `view`'s window."""
    activate(view)
    click_later("Delete all geometry and constraints?", "Yes")
    FreeCADGui.runCommand("Sketcher_DeleteAllGeometry")
    settle(500)
    return state["asked"]


def timer_change(doc, view, name, sketch, y):
    """A change from a timer -- nobody's -- with `view` active."""
    activate(view)
    doc.openTransaction(name)
    sketch.addGeometry(Part.LineSegment(V(0, y, 0), V(10, y, 0)))
    doc.commitTransaction()
    doc.recompute()
    settle(200)


def ys(sketch):
    return sorted(round(g.StartPoint.y, 3) for g in sketch.Geometry)


def cancel(view):
    activate(view)
    FreeCADGui.Control.activeTaskDialog(view=view).reject()
    settle(1000)


def run():
    try:
        VIEW.SetBool("PerViewEdit", True)
        doc = FreeCAD.newDocument("SketchTwoViews")
        a = doc.addObject("Sketcher::SketchObject", "A")
        for y in (0, 5):
            a.addGeometry(Part.LineSegment(V(0, y, 0), V(10, y, 0)))
        b = doc.addObject("Sketcher::SketchObject", "B")
        b.Placement = FreeCAD.Placement(V(0, 0, 20), FreeCAD.Rotation())
        b.addGeometry(Part.LineSegment(V(0, 1, 0), V(10, 1, 0)))
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

        # B's step, then A's, then a timer's change to A with B's window
        # active: the timer's is not B's
        check("B's command runs in its window", delete_all(v2))
        check("A's command runs in its window", delete_all(v1))
        timer_change(doc, v2, "late write", a, 20)
        check("each did what it does", (ys(a), ys(b)) == ([20.0], []), (ys(a), ys(b)))
        cancel(v2)
        check("Cancel of B takes back its step", ys(b) == [1.0], ys(b))
        check("and keeps A's step and the timer's change, made in B's window",
              ys(a) == [20.0], ys(a))
        check("A stays in edit, B does not",
              (in_edit(gdoc, v1), in_edit(gdoc, v2)) == ("A", None),
              (in_edit(gdoc, v1), in_edit(gdoc, v2)))
        check("and the tree marks A only", (marked("A"), marked("B")) == (True, False),
              (marked("A"), marked("B")))

        # A change to the sketch in edit is the edit's, whoever makes it --
        # a pick in the tree, the property editor, a script -- here one
        # from a timer, with the other window active
        edit(gdoc, v2, b)
        delete_all(v2)
        timer_change(doc, v1, "a script's", b, 15)
        check("B's step and the change to B are made", ys(b) == [15.0], ys(b))
        cancel(v2)
        check("Cancel of B takes back both", ys(b) == [1.0], ys(b))
        check("and A's lines are as they were", ys(a) == [20.0], ys(a))

        activate(v1)
        FreeCADGui.Control.activeTaskDialog(view=v1).accept()
        settle(1000)
        check("OK of A keeps what it did", ys(a) == [20.0], ys(a))
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
