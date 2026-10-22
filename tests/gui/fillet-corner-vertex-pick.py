"""A click on a filleted corner's vertex in the 3D view makes it a setback corner.

docs/HandsOnQueue.md entry 202 (a), the user's hands-on of 2026-10-21:
"Looks like I cannot select a vertex in 3d view at the moment, so how to
configure corner".

A PartDesign box with a fillet on the three edges meeting at its top corner.
In edit. Claims:
  - the pointer over the corner's place: what is preselected there (noted);
  - a click there: the corner vertex is in Corners (its row in the panel);
  - with the add button down, a click there again: the corner taken away,
    and a third click: back.

Runs on the real screen; the clicks go through the window.
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
DOC = "FilletCornerVertexPick"
RADIUS = 2.0
SIZE = 10.0

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


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def viewer_widget():
    return view().graphicsView()


def to_window(x, y):
    width, height = view().getSize()
    w = viewer_widget()
    local = QtCore.QPoint(int(x), int(height - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def move(x, y):
    win = mw().windowHandle()
    QtTest.QTest.mouseMove(win, to_window(x, y))
    pump(10)
    QtTest.QTest.mouseMove(win, to_window(x + 1, y))
    pump(10)
    QtTest.QTest.mouseMove(win, to_window(x, y))
    pump(30)


def click(x, y):
    win = mw().windowHandle()
    move(x, y)
    p = to_window(x, y)
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(10)
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(80)


def preselection():
    sel = FreeCADGui.Selection.getPreselection()
    try:
        return "%s.%s" % (sel.ObjectName, sel.SubElementNames)
    except Exception:
        return str(sel)


def selection():
    return ["%s.%s" % (s.ObjectName, list(s.SubElementNames))
            for s in FreeCADGui.Selection.getSelectionEx("", 0)]


def corner_rows(tree):
    rows = []
    for i in range(tree.topLevelItemCount()):
        text = tree.topLevelItem(i).text(0)
        if "Vertex" in text:
            rows.append(text)
    return rows


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        mw().showNormal()
        mw().setGeometry(QtGui.QGuiApplication.primaryScreen().availableGeometry())
        pump(30)
        mw().activateWindow()
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = SIZE
        doc.recompute()
        corner = FreeCAD.Vector(SIZE, SIZE, SIZE)
        vertex = None
        for i, v in enumerate(box.Shape.Vertexes):
            if v.Point.distanceToPoint(corner) < 1e-7:
                vertex = "Vertex%d" % (i + 1)
        edges = ["Edge%d" % (i + 1) for i, e in enumerate(box.Shape.Edges)
                 if any(v.Point.distanceToPoint(corner) < 1e-7 for v in e.Vertexes)]
        note("INFO the corner %s, its edges %s" % (vertex, edges))
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, edges)
        fillet.Radius = RADIUS
        doc.recompute()
        check("the fillet is valid", fillet.isValid() and fillet.Shape.isValid())

        FreeCADGui.SendMsgToActiveView("ViewFit")
        FreeCADGui.activeDocument().activeView().viewIsometric()
        pump()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(fillet, 0)
        pump(80)
        if not check("the fillet is in edit", gdoc.getInEdit() is not None):
            return
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump(30)
        tree = None
        for t in mw().findChildren(QtWidgets.QTreeWidget, "treeWidgetReferences"):
            if t.isVisible():
                tree = t
        if not check("the panel's list is found", tree is not None):
            return

        at = view().getPointOnViewport(corner)
        note("INFO the corner at %s" % (tuple(at),))
        FreeCADGui.Selection.clearSelection()
        pump(10)
        move(*at)
        note("INFO the pointer on the corner: preselected %s" % preselection())
        info = view().getObjectInfo((int(at[0]), int(at[1])))
        note("INFO picked there: %s" % (
            None if not info else (info.get("Object"), info.get("Component")),))

        click(*at)
        note("INFO after the click: selection %s, Corners %s, rows %s"
             % (selection(), dict(fillet.Corners), corner_rows(tree)))
        check("a click on the corner: its vertex in Corners",
              vertex in dict(fillet.Corners), dict(fillet.Corners))
        check("... and its row in the panel", vertex in corner_rows(tree), corner_rows(tree))

        # the add button down: a click toggles the corner
        button = None
        # a check box in the fillet's panel
        for b in mw().findChildren(QtWidgets.QAbstractButton, "buttonRefAdd"):
            if b.isVisible():
                button = b
        if check("the add button is found", button is not None):
            if not button.isChecked():
                button.click()
                pump(30)
            FreeCADGui.Selection.clearSelection()
            pump(10)
            before = vertex in dict(fillet.Corners)
            click(*at)
            middle = vertex in dict(fillet.Corners)
            note("INFO add mode, a click: Corners %s -> %s, selection %s"
                 % (before, middle, selection()))
            check("add mode: a click on the corner toggles it", middle != before,
                  (before, middle))
            move(*at)
            note("INFO add mode, before the second click: preselected %s" % preselection())
            click(*at)
            last = vertex in dict(fillet.Corners)
            note("INFO add mode, the second click: Corners %s, selection %s, rows %s"
                 % (dict(fillet.Corners), selection(), corner_rows(tree)))
            check("add mode: a second click toggles it back", last == before, (before, last))
            if button.isChecked():
                button.click()
                pump(30)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.getDocument(DOC).resetEdit()
        pump()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
