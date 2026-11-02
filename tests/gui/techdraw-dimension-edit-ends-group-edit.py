"""A double click on a dimension while a projection group is in edit ends it and edits the dimension.

docs/HandsOnQueue.md entry 219 (the reporter, 2026-10-23): "when editing a
techdraw projection group, double click dimension does not launch edit. I
want to dismiss projection group edit and launch dimension edit."

A box; a page with a projection group (Front, Right) at scale 1; a
distance dimension on the Front view. Claims, twice -- a double click on
the dimension's label on the page, and the tree's double click:
  - with the group's panel open and its X spacing changed in it, the
    double click closes the group's panel and opens the dimension's;
  - the group's change is kept;
  - the group's edit is an undo step of its own, before the dimension's.

Runs offscreen as well.
"""
import glob
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
DOC = "DimEditEndsGroupEdit"
REZ = 10.0

state = {"done": False}


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
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        time.sleep(0.02)


def mw():
    return FreeCADGui.getMainWindow()


def value(q):
    return q.Value if hasattr(q, "Value") else q


def panels():
    """The task panel widgets shown, by class"""
    out = []
    for w in mw().findChildren(QtWidgets.QWidget):
        name = w.metaObject().className()
        if w.isVisible() and name in ("TechDrawGui::TaskProjGroup", "TechDrawGui::TaskDimension"):
            out.append(name.split("::")[-1])
    return sorted(set(out))


def page_view():
    for w in mw().findChildren(QtWidgets.QGraphicsView):
        if w.isVisible() and w.metaObject().className() == "TechDrawGui::QGVPage":
            return w
    return None


def end_edits(gdoc):
    try:
        FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    gdoc.resetEdit()
    settle(1)


def double_click_label(grp, item, dim):
    gv = page_view()
    if gv is None:
        return False
    gv.resetTransform()
    at = (REZ * (value(grp.X) + value(item.X) + value(dim.X)),
          -REZ * (value(grp.Y) + value(item.Y) + value(dim.Y)))
    gv.centerOn(QtCore.QPointF(*at))
    settle(0.5)
    p = gv.mapFromScene(QtCore.QPointF(*at))
    note("INFO the label at scene %s, viewport %s" % (at, (p.x(), p.y())))
    QtTest.QTest.mouseDClick(gv.viewport(), Qt.LeftButton, Qt.NoModifier, p)
    settle(1.5)
    return True


def round_(doc, grp, item, dim, how, spacing):
    gdoc = FreeCADGui.getDocument(doc.Name)
    grp.ViewObject.doubleClicked()
    settle(1.5)
    if not check("%s: the group's panel open" % how, panels() == ["TaskProjGroup"], panels()):
        return
    undo = doc.UndoCount
    grp.spacingX = spacing
    doc.recompute()
    settle(3)
    if how == "on the page":
        double_click_label(grp, item, dim)
    else:
        dim.ViewObject.doubleClicked()
        settle(1.5)
    check("%s: a double click on the dimension: the group's panel closed, the dimension's open"
          % how, panels() == ["TaskDimension"], panels())
    check("%s: the group's change kept" % how, abs(value(grp.spacingX) - spacing) < 1e-6,
          grp.spacingX)
    names = list(doc.UndoNames)[:3] if hasattr(doc, "UndoNames") else []
    note("INFO %s: undo steps %d -> %d, the last %s" % (how, undo, doc.UndoCount, names))
    check("%s: the group's edit an undo step of its own" % how,
          doc.UndoCount > undo and any("Projection" in n for n in names), names)
    end_edits(gdoc)


def run():
    # The edit that holds one transaction to OK, which this is about --
    # the group's edit, one step: View/EditStepEachChange off; each change a step of
    # its own, the default, is tests/gui/edit-step-each-change.py
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetBool(
        "EditStepEachChange", False)
    try:
        doc = FreeCAD.newDocument(DOC)
        doc.UndoMode = 1
        box = doc.addObject("Part::Box", "Box")
        box.Length, box.Width, box.Height = 40, 20, 10
        doc.recompute()
        templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                           "Templates", "**", "*A4*Landscape*.svg"), recursive=True)
        page = doc.addObject("TechDraw::DrawPage", "Page")
        tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        if templates:
            tmpl.Template = templates[0]
        page.Template = tmpl
        grp = doc.addObject("TechDraw::DrawProjGroup", "ProjGroup")
        page.addView(grp)
        grp.Source = [box]
        front = grp.addProjection("Front")
        grp.addProjection("Right")
        grp.ScaleType = "Custom"
        grp.Scale = 1.0
        grp.X, grp.Y = 120, 120
        doc.recompute()
        settle(6)
        doc.recompute()
        settle(2)
        dim = doc.addObject("TechDraw::DrawViewDimension", "Dimension")
        dim.Type = "Distance"
        dim.References2D = [(front, "Edge0")]
        page.addView(dim)
        dim.X, dim.Y = 0, 30
        doc.recompute()
        settle(3)
        page.ViewObject.doubleClicked()
        settle(3)
        round_(doc, grp, front, dim, "on the page", 30)
        round_(doc, grp, front, dim, "in the tree", 45)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        try:
            FreeCADGui.getDocument(name).resetEdit()
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
