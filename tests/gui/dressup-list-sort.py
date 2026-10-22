"""A dress-up's edge list sorts by the column whose title is clicked.

The reporter, 2026-10-23: "The edge list in the edit panel is a list view or
tree view. Can we add column sorting function"

A PartDesign box, a fillet of four edges listed out of order (Edge10, Edge2,
Edge12, Edge1): Edge2 with two points of its own (radius 0.5, then 0.8),
Edge12 one (0.3), the others on the default radius 1. In edit. Claims:
  - opened: the feature's order, no sort shown;
  - a click on "Element": Edge1, Edge2, Edge10, Edge12 (the number taken as
    one), a second the other way, a third the feature's order again;
  - a click on "Radius": by each edge's first radius, an edge on the default
    by the default -- Edge12, Edge2, then Edge10, Edge1 as the feature has them;
  - the rows under Edge2 keep their order, and the selected row its selection;
  - a sort writes nothing: Base and the undo count as they were;
  - a row removed while sorted: Base the remaining edges in the feature's
    order, the list still sorted by radius;
  - a chamfer sorts by "Element" too, Base unchanged, each row's flip box
    still open.

Clicks the list's header only; runs offscreen as well.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "DressUpListSort"
EDGES = ["Edge10", "Edge2", "Edge12", "Edge1"]

state = {"done": False}


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
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        time.sleep(0.01)


def mw():
    return FreeCADGui.getMainWindow()


def tree():
    for w in mw().findChildren(QtWidgets.QTreeWidget):
        if w.objectName() == "treeWidgetReferences" and w.isVisible():
            return w
    return None


def rows(widget):
    return [widget.topLevelItem(i).text(0) for i in range(widget.topLevelItemCount())]


def row(widget, text):
    for i in range(widget.topLevelItemCount()):
        if widget.topLevelItem(i).text(0) == text:
            return widget.topLevelItem(i)
    return None


def column(widget, title):
    header = widget.headerItem()
    for i in range(widget.columnCount()):
        if header.text(i) == title:
            return i
    return -1


def click_title(widget, logical):
    """A click on a column's title, where the header shows it"""
    header = widget.header()
    x = header.sectionViewportPosition(logical) + header.sectionSize(logical) // 2
    pos = QtCore.QPoint(x, header.height() // 2)
    QtTest.QTest.mouseClick(header.viewport(), Qt.LeftButton, Qt.NoModifier, pos)
    pump(20)


def indicator(widget):
    header = widget.header()
    return header.sortIndicatorSection(), header.sortIndicatorOrder()


def base_subs(feature):
    return list(feature.Base[1]) if feature.Base else []


def child_values(item, col):
    return [round(float(item.child(i).data(col, Qt.UserRole) or 0.0), 6)
            for i in range(item.childCount())]


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        doc = FreeCAD.newDocument(DOC)
        doc.UndoMode = 1
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, EDGES)
        fillet.Radius = 1.0
        fillet.Segments = [("Edge2", [(0.0, 0.5), (1.0, 0.8)]), ("Edge12", [(0.0, 0.3)])]
        doc.recompute()
        check("the fillet made", not list(fillet.FailedEdges) and "Invalid" not in fillet.State,
              "FailedEdges %s, state %s" % (list(fillet.FailedEdges), fillet.State))

        gdoc = FreeCADGui.getDocument(DOC)
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump()
        gdoc.setEdit(fillet, 0)
        pump(80)
        widget = tree()
        if not check("the fillet is in edit, its edge list shown",
                     gdoc.getInEdit() is not None and widget is not None):
            return
        element = column(widget, "Element")
        radius = column(widget, "Radius")
        note("INFO columns: Element %d, Radius %d, count %d" % (element, radius,
                                                               widget.columnCount()))
        check("opened: the feature's order, no sort shown",
              rows(widget) == EDGES and indicator(widget)[0] == -1,
              (rows(widget), indicator(widget)))
        edge2 = row(widget, "Edge2")
        children = child_values(edge2, radius) if edge2 else None
        note("INFO Edge2's points' radii: %s" % children)

        subs0 = base_subs(fillet)
        undo0 = doc.UndoCount
        widget.clearSelection()
        row(widget, "Edge10").setSelected(True)
        pump(10)

        click_title(widget, element)
        check("a click on Element: by the element, its number taken as one",
              rows(widget) == ["Edge1", "Edge2", "Edge10", "Edge12"]
              and indicator(widget) == (element, Qt.AscendingOrder),
              (rows(widget), indicator(widget)))
        click_title(widget, element)
        check("... a second, the other way",
              rows(widget) == ["Edge12", "Edge10", "Edge2", "Edge1"]
              and indicator(widget) == (element, Qt.DescendingOrder),
              (rows(widget), indicator(widget)))
        click_title(widget, element)
        check("... a third, the feature's order again",
              rows(widget) == EDGES and indicator(widget)[0] == -1,
              (rows(widget), indicator(widget)))

        click_title(widget, radius)
        check("a click on Radius: by the first radius, the default for an edge without",
              rows(widget) == ["Edge12", "Edge2", "Edge10", "Edge1"],
              rows(widget))
        edge2 = row(widget, "Edge2")
        check("... the rows under Edge2 in their order",
              edge2 is not None and child_values(edge2, radius) == children,
              child_values(edge2, radius) if edge2 else None)
        check("... the selected row still selected",
              [i.text(0) for i in widget.selectedItems()] == ["Edge10"],
              [i.text(0) for i in widget.selectedItems()])
        check("a sort writes nothing: Base as it was", base_subs(fillet) == subs0,
              base_subs(fillet))
        check("... no undo step", doc.UndoCount == undo0, "%d -> %d" % (undo0, doc.UndoCount))

        # a row removed while sorted
        widget.clearSelection()
        row(widget, "Edge12").setSelected(True)
        pump(10)
        remove = [a for a in widget.actions() if a.text() == "Remove"]
        if check("the list's Remove action", len(remove) == 1, [a.text() for a in widget.actions()]):
            remove[0].trigger()
            pump(80)
            widget = tree()
            check("a row removed while sorted: Base the rest in the feature's order",
                  base_subs(fillet) == ["Edge10", "Edge2", "Edge1"], base_subs(fillet))
            check("... the list still sorted by radius",
                  widget is not None and rows(widget) == ["Edge2", "Edge10", "Edge1"]
                  and indicator(widget) == (radius, Qt.AscendingOrder),
                  (rows(widget), indicator(widget)) if widget else None)

        gdoc.resetEdit()
        pump(60)
        chamfer_doc()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def chamfer_doc():
    name = DOC + "C"
    doc = FreeCAD.newDocument(name)
    body = doc.addObject("PartDesign::Body", "Body")
    box = body.newObject("PartDesign::AdditiveBox", "Box")
    box.Length = box.Width = box.Height = 10
    doc.recompute()
    chamfer = body.newObject("PartDesign::Chamfer", "Chamfer")
    chamfer.Base = (box, ["Edge10", "Edge2", "Edge1"])
    chamfer.Size = 1.0
    doc.recompute()
    gdoc = FreeCADGui.getDocument(name)
    FreeCADGui.SendMsgToActiveView("ViewFit")
    pump()
    gdoc.setEdit(chamfer, 0)
    pump(80)
    widget = tree()
    if not check("the chamfer is in edit, its edge list shown",
                 gdoc.getInEdit() is not None and widget is not None):
        return
    subs0 = base_subs(chamfer)
    flip = 4
    open0 = [widget.isPersistentEditorOpen(widget.topLevelItem(i), flip)
             for i in range(widget.topLevelItemCount())]
    note("INFO chamfer rows %s, flip boxes open %s" % (rows(widget), open0))
    click_title(widget, column(widget, "Element"))
    check("a chamfer sorts by Element", rows(widget) == ["Edge1", "Edge2", "Edge10"],
          rows(widget))
    check("... Base as it was", base_subs(chamfer) == subs0, base_subs(chamfer))
    check("... each row's flip box as open as before",
          [widget.isPersistentEditorOpen(widget.topLevelItem(i), flip)
           for i in range(widget.topLevelItemCount())] == open0)
    gdoc.resetEdit()
    pump(40)


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        try:
            FreeCADGui.getDocument(name).resetEdit()
            pump()
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
