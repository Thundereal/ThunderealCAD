"""The lists in overlaid docks take the thin overlaid scroll bars, and give them back.

docs/HandsOnQueue.md entry 225 (the reporter, 2026-10-23): "would it be
possible to have a general way to replace the scroll bar of all other
widgets in overlay transparent mode?", then "225, go ahead. two hide
scroll bar options stay the same, but default false". The tree's option is
TreeView/HideScrollBarInOverlay now; the old key HideScrollBar, true
wherever a preference page saved its old default, is no longer read.

The old key set true; a PartDesign box with a fillet. Claims:
  - docks not overlaid: the tree has no laid-over bars;
  - every dock overlaid and see-through: the tree, the property view and
    the report view each have them, their own bars off;
  - the fillet put in edit after that: its edge list in the task panel has
    them too (made later, reached as it is shown);
  - the tree's option on, the docks overlaid again: the tree has none, the
    property view still has;
  - docks back in place: no laid-over bars anywhere, the tree's own bars
    as they were.

Runs offscreen as well.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
TREE = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/TreeView")

state = {"done": False, "overlay": False, "transparent": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


def mw():
    return FreeCADGui.getMainWindow()


def find(class_name, object_name=None):
    for w in mw().findChildren(QtWidgets.QAbstractScrollArea):
        if not w.isVisible():
            continue
        if class_name and w.metaObject().className() != class_name:
            continue
        if object_name and w.objectName() != object_name:
            continue
        return w
    return None


def has_bars(area):
    if area is None:
        return None
    names = {b.objectName() for b in area.findChildren(QtWidgets.QScrollBar)
             if b.parent() is area}
    return "OverlayScrollBarV" in names and "OverlayScrollBarH" in names


def report_view():
    for w in mw().findChildren(QtWidgets.QTextEdit):
        if w.isVisible() and w.metaObject().className() == "Gui::DockWnd::ReportOutput":
            return w
    return None


def toggle_overlay():
    FreeCADGui.runCommand("Std_DockOverlayAll")
    state["overlay"] = not state["overlay"]
    settle(2)


def run():
    try:
        TREE.RemBool("HideScrollBarInOverlay")
        TREE.SetBool("HideScrollBar", True)
        report = mw().findChild(QtWidgets.QDockWidget, "Report view")
        if report is not None:
            report.show()
        doc = FreeCAD.newDocument("OverlayScrollBars")
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1", "Edge2", "Edge3"])
        doc.recompute()
        settle(1.5)

        tree = find("Gui::TreeWidget")
        if not check("the tree is shown", tree is not None):
            return
        own_policy = tree.verticalScrollBarPolicy()
        check("docks in place: the tree has no laid-over bars", not has_bars(tree))

        toggle_overlay()
        FreeCADGui.runCommand("Std_DockOverlayTransparentAll")
        state["transparent"] = True
        settle(2)
        tree = find("Gui::TreeWidget")
        props = find("Gui::PropertyEditor::PropertyEditor")
        rep = report_view()
        note("INFO found tree %s, property view %s, report view %s"
             % (tree is not None, props is not None, rep is not None))
        check("overlaid: the tree has them", has_bars(tree))
        check("... its own bars off", tree is not None
              and tree.verticalScrollBarPolicy() == Qt.ScrollBarAlwaysOff)
        check("overlaid: the property view has them", has_bars(props))
        check("overlaid: the report view has them", has_bars(rep))

        g = FreeCADGui.getDocument(doc.Name)
        g.setEdit(fillet, 0)
        settle(2)
        edges = find(None, "treeWidgetReferences")
        check("the fillet's edge list, made after, has them", has_bars(edges),
              edges is not None)
        g.resetEdit()
        settle(1)

        TREE.SetBool("HideScrollBarInOverlay", True)
        toggle_overlay()
        toggle_overlay()
        tree = find("Gui::TreeWidget")
        props = find("Gui::PropertyEditor::PropertyEditor")
        check("the tree's option on: the tree has none", has_bars(tree) is False)
        check("... the property view still has them", has_bars(props))
        TREE.RemBool("HideScrollBarInOverlay")

        FreeCADGui.runCommand("Std_DockOverlayTransparentAll")
        state["transparent"] = False
        toggle_overlay()
        tree = find("Gui::TreeWidget")
        props = find("Gui::PropertyEditor::PropertyEditor")
        rep = report_view()
        check("docks in place: none on the tree, the property view, the report view",
              not has_bars(tree) and not has_bars(props) and not has_bars(rep),
              (has_bars(tree), has_bars(props), has_bars(rep)))
        check("... the tree's own bars as they were", tree is not None
              and tree.verticalScrollBarPolicy() == own_policy,
              (tree.verticalScrollBarPolicy() if tree else None, own_policy))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        if state["transparent"]:
            FreeCADGui.runCommand("Std_DockOverlayTransparentAll")
        if state["overlay"]:
            FreeCADGui.runCommand("Std_DockOverlayAll")
    except Exception:
        pass
    TREE.RemBool("HideScrollBarInOverlay")
    TREE.RemBool("HideScrollBar")
    for name in list(FreeCAD.listDocuments().keys()):
        try:
            FreeCADGui.getDocument(name).resetEdit()
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
