"""Undo of a linked pair, and where a step is listed (docs/MultiViewEdit.md
sec 12.2 rules 2 and 4).

A holds a copy of B's box (a Part::FeaturePython over an XLink); one step
changes B's box and the recompute writes A's copy. One undo with A active
must take back both, however the step was opened. A step is listed, as an
empty `-> <name>`, in the document of the view the input that opened it
went to -- not in the active document: a script's step lands only where
it changed (sec 11).

Claims:
  - a step set app-wide, one B.openTransaction() opens, and one bound to
    B (an edit of B's) each give A a step of its own under the same id:
    no `-> ` pointer, and one undo from A takes back B's box and A's copy;
  - a step from a timer that changes B only leaves A's list empty, A
    active;
  - a click in A's view whose handler changes B only lists the step in A
    as `-> click step`, and one undo from A takes back B's box;
  - a click in B's view, A active, lists the step in B only.
"""
import os
import tempfile
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets
from PySide6.QtTest import QTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))

FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


class Copy:
    def __init__(self, obj):
        obj.addProperty("App::PropertyXLink", "Src")
        obj.Proxy = self

    def execute(self, obj):
        if obj.Src:
            obj.Shape = obj.Src.Shape.copy()


def setup():
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    d = tempfile.mkdtemp()
    b = FreeCAD.newDocument("PairB")
    b.UndoMode = 1
    b.addObject("Part::Box", "Box")
    b.recompute()
    b.saveAs(os.path.join(d, "pairb.FCStd"))
    a = FreeCAD.newDocument("PairA")
    a.UndoMode = 1
    c = a.addObject("Part::FeaturePython", "Copy")
    Copy(c)
    c.Src = b.Box
    a.recompute()
    a.saveAs(os.path.join(d, "paira.FCStd"))
    a.clearUndos()
    b.clearUndos()
    FreeCAD.setActiveDocument(a.Name)
    return a, b


def lists(a, b):
    return "A %s B %s" % (list(a.UndoNames), list(b.UndoNames))


def change(a, b):
    b.Box.Height = 25
    b.recompute()
    a.recompute()


def undoneFromA(name, a, b):
    FreeCAD.setActiveDocument(a.Name)
    FreeCADGui.runCommand("Std_Undo")
    check(name, b.Box.Height.Value == 10 and round(a.Copy.Shape.BoundBox.ZMax, 3) == 10,
          "Box.Height %s Copy.ZMax %s %s" % (
              b.Box.Height.Value, round(a.Copy.Shape.BoundBox.ZMax, 3), lists(a, b)))


def viewOf(doc):
    title = os.path.splitext(os.path.basename(doc.FileName))[0] + " : "
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget):
        if w.metaObject().className() == "Gui::View3DInventor" \
                and w.windowTitle().startswith(title):
            return w
    return None


def click(view, handler):
    btn = QtWidgets.QPushButton("go", view)
    btn.setGeometry(0, 0, 40, 20)
    btn.show()
    btn.clicked.connect(handler)
    QTest.mouseClick(btn, QtCore.Qt.LeftButton)
    btn.deleteLater()


def run():
    try:
        a, b = setup()
        FreeCAD.setActiveTransaction("cmd step")
        change(a, b)
        FreeCAD.closeActiveTransaction()
        check("an app-wide step is A's own, no pointer",
              list(a.UndoNames) == ["cmd step"] and list(b.UndoNames) == ["cmd step"],
              lists(a, b))
        undoneFromA("one undo takes back the app-wide step in both", a, b)

        a, b = setup()
        b.openTransaction("script step")
        change(a, b)
        b.commitTransaction()
        check("B.openTransaction()'s step is A's own, no pointer",
              list(a.UndoNames) == ["script step"] and list(b.UndoNames) == ["script step"],
              lists(a, b))
        undoneFromA("one undo takes back B's script step in both", a, b)

        a, b = setup()
        FreeCAD.setActiveDocument(b.Name)
        FreeCAD.bindActiveTransaction(b)
        FreeCAD.setActiveTransaction("edit step")
        FreeCAD.setActiveDocument(a.Name)
        change(a, b)
        FreeCAD.unbindActiveTransaction(b)
        check("A joins B's bound step, no pointer",
              list(a.UndoNames) == ["edit step"] and list(b.UndoNames) == ["edit step"],
              lists(a, b))
        undoneFromA("one undo takes back B's bound step in both", a, b)

        a, b = setup()
        FreeCAD.setActiveTransaction("timer step")
        b.Box.Height = 30
        b.recompute()
        FreeCAD.closeActiveTransaction()
        check("a timer's step changing B only is not listed in A",
              list(a.UndoNames) == [] and list(b.UndoNames) == ["timer step"], lists(a, b))

        a, b = setup()

        def inB(name, height):
            def handler():
                FreeCAD.setActiveTransaction(name)
                b.Box.Height = height
                b.recompute()
                FreeCAD.closeActiveTransaction()
            return handler

        view = viewOf(a)
        if check("A's view found", view is not None):
            click(view, inB("click step", 30))
            check("a click in A's view lists B's step in A",
                  list(a.UndoNames) == ["-> click step"]
                  and list(b.UndoNames) == ["click step"], lists(a, b))
            FreeCADGui.runCommand("Std_Undo")
            check("one undo from A takes back the click's step",
                  b.Box.Height.Value == 10, "Box.Height %s %s" % (b.Box.Height.Value, lists(a, b)))

        a, b = setup()
        view = viewOf(b)
        FreeCAD.setActiveDocument(a.Name)
        if check("B's view found", view is not None):
            click(view, inB("b click", 35))
            check("a click in B's view, A active, lists the step in B only",
                  list(a.UndoNames) == [] and list(b.UndoNames) == ["b click"], lists(a, b))
    except Exception:
        note("ABORT " + traceback.format_exc())
    # Closed here: a modified one would ask to be saved at the quit
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
