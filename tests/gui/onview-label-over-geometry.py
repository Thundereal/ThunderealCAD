"""An on-view value's label over the model: lit on hover, and a click on it is
the label's.

docs/HandsOnQueue.md entry 196 (the reporter, 2026-10-20): "2, on view label
no preselection highlight. 3, on view label has lower selection priority, so
click it with any base geometry behind it will select the geometry instead".

A fillet of a box in edit, the box zoomed to fill the view, so that the
top-middle label ("Radius ...") lies over the box. Claims:
  - a label pixel is found by picking, the box behind it;
  - the pointer over the label draws its text in the preselection colour,
    and off it in its own colour again;
  - nothing is preselected behind the label while the pointer is on it;
  - a click on the label selects nothing behind it, and makes its value
    active (the arrows in the selection colour).

Run through scripts/gui-test.sh (xvfb, isolated configuration, external
timeout), or by hand as `FreeCAD <this script>` with GT_OUT set.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets
from PySide.QtCore import Qt

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "OnViewLabelOverGeometry"
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


def field_text(node, name):
    value = node.getField(name).get()
    return value.getString() if hasattr(value, "getString") else str(value)


def find_all(type_name):
    from pivy import coin

    root = view().getViewer().getSoRenderManager().getSceneGraph()
    search = coin.SoSearchAction()
    search.setType(coin.SoType.fromName(type_name))
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    return [p.getTail() for p in search.getPaths()]


def label_colours():
    return [field_text(n, "textColor") for n in find_all("SoDatumLabel")]


def arrow_colours():
    colours = []
    for container in find_all("SoLinearDraggerContainer"):
        if field_text(container, "visible") not in ("TRUE", "1"):
            continue
        dragger = container.getPart("dragger", False)
        if dragger is not None:
            colours.append(field_text(dragger, "color"))
    return colours


def pick_kind(x, y):
    """What a ray at viewport pixel (x, y) hits first, on-top picks first"""
    from pivy import coin

    viewer = view().getViewer()
    region = viewer.getSoRenderManager().getViewportRegion()
    action = coin.SoRayPickAction(region)
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(1)
    action.apply(viewer.getSoRenderManager().getSceneGraph())
    picked = action.getPickedPoint()
    if picked is None:
        return None
    path = picked.getPath()
    names = [path.getNode(i).getTypeId().getName().getString() for i in range(path.getLength())]
    if any("DatumLabel" in n for n in names):
        return "label"
    return "other"


def behind(x, y):
    """Whether the model is under the pixel, the label left out"""
    from pivy import coin

    viewer = view().getViewer()
    region = viewer.getSoRenderManager().getViewportRegion()
    action = coin.SoRayPickAction(region)
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(1)
    action.setPickAll(True)
    action.apply(viewer.getSoRenderManager().getSceneGraph())
    pl = action.getPickedPointList()
    for i in range(pl.getLength()):
        path = pl[i].getPath()
        names = [path.getNode(k).getTypeId().getName().getString() for k in range(path.getLength())]
        if not any("DatumLabel" in n for n in names) and any("BrepFaceSet" in n for n in names):
            return True
    return False


def to_window(x, y):
    width, height = view().getSize()
    w = view().graphicsView()
    local = QtCore.QPoint(int(x), int(height - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def move_to(x, y):
    QtTest.QTest.mouseMove(mw().windowHandle(), to_window(x, y))
    pump(20)


def click(x, y):
    win = mw().windowHandle()
    p = to_window(x, y)
    QtTest.QTest.mouseMove(win, p)
    pump(5)
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(5)
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(40)


def selection():
    return [(s.ObjectName, list(s.SubElementNames)) for s in FreeCADGui.Selection.getSelectionEx("", 0)]


def preselected():
    try:
        sel = FreeCADGui.Selection.getPreselection()
        return (sel.Object.Name if sel and sel.Object else "")
    except RuntimeError:
        return ""


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        mw().activateWindow()
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1"])
        fillet.Radius = 1
        doc.recompute()
        gui = FreeCADGui.getDocument(DOC)
        view().viewFront()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump()
        # the box past the view's top, so the top middle label lies over it
        cam = view().getCameraNode()
        if hasattr(cam, "height"):
            cam.height.setValue(cam.height.getValue() * 0.3)
        else:
            cam.heightAngle.setValue(cam.heightAngle.getValue() * 0.3)
        pump()
        gui.setEdit(fillet, 0)
        pump(80)
        check("the fillet is in edit", gui.getInEdit() is not None)

        width, height = view().getSize()
        found = None
        for dy in range(4, 120, 2):
            for dx in range(0, 160, 3):
                for x in (width // 2 - dx, width // 2 + dx):
                    y = height - dy
                    if pick_kind(x, y) == "label" and behind(x, y):
                        found = (x, y)
                        break
                if found:
                    break
            if found:
                break
        note("INFO the view %s, a label pixel over the box %s" % ((width, height), found))
        if not check("a label pixel over the box is found by picking", found is not None):
            return

        # off the label: the middle of the view, the box under it (a corner
        # pixel can fall outside the view's widget and reach nobody)
        off = (width // 2, height // 2)
        FreeCADGui.Selection.clearSelection()
        move_to(*off)
        own = label_colours()
        move_to(*found)
        lit = label_colours()
        pre = preselected()
        hl = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").GetUnsigned(
            "HighlightColor", 0xe1e114ff)
        note("INFO label colours off %s, on %s; HighlightColor %08x" % (own, lit, hl))
        check("entry 196 point 2: the pointer over the label lights it", lit != own, (own, lit))
        check("... and nothing behind it is preselected", pre == "", pre)
        move_to(*off)
        check("... and off it the label has its own colour again", label_colours() == own,
              label_colours())

        before = arrow_colours()
        click(*found)
        sel = selection()
        after = arrow_colours()
        note("INFO arrows before %s, after the click %s" % (before, after))
        check("entry 196 point 3: a click on the label selects nothing behind it", sel == [], sel)
        check("... and makes its value active (the arrows change colour)",
              bool(after) and after != before, (before, after))
        gui.resetEdit()
        pump(40)
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
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
