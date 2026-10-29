"""A dress-up's handle made active selects its edge, as a click on its row does.

docs/HandsOnQueue.md entry 213 (the reporter, 2026-10-23): "when dress up
feature active handle is selected, select the corresonding edge (or preview
face if the edge is missing)"; then "no just select the corresponding edge,
as the right click context menu is meant to apply to that edge which may
change its mode".

A 10 mm PartDesign box, a fillet on two edges far apart on the screen, the
second with a radius of its own, the first on the default radius (whose
value has an arrow on every edge that takes it). In edit; an arrow's pixels
are found by picking the scene and clicked through the window. Claims:
  - a click on the first edge's arrow: that edge's row the selected one,
    and the edge alone the selection;
  - on the second's: the same for it;
  - on the first's again: back to it;
  - the same for a chamfer's size handle.

Clicks in the 3D view: run on a real screen.
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
DOC = "DressUpHandleSelects"

state = {"done": False, "doc": DOC}


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
    return FreeCADGui.getDocument(state["doc"]).ActiveView


def is_handle(x, y):
    from pivy import coin

    viewer = view().getViewer()
    region = viewer.getSoRenderManager().getViewportRegion()
    action = coin.SoRayPickAction(region)
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(1)
    action.apply(viewer.getSoRenderManager().getSceneGraph())
    picked = action.getPickedPoint()
    if picked is None:
        return False
    path = picked.getPath()
    return any("Dragger" in path.getNode(i).getTypeId().getName().getString()
               for i in range(path.getLength()))


def handle_pixel(center, reach=40, step=2):
    """The handle pixel nearest `center` with the most of the handle around it"""
    cx, cy = center
    best = None
    for r in range(0, reach, step):
        for dx in range(-r, r + 1, step):
            for dy in (-r, r) if abs(dx) != r else range(-r, r + 1, step):
                x, y = cx + dx, cy + dy
                if not is_handle(x, y):
                    continue
                score = sum(is_handle(x + i, y + j) for i in (-2, -1, 0, 1, 2)
                            for j in (-2, -1, 0, 1, 2))
                if score >= 10:
                    return (x, y)
                if best is None or score > best[0]:
                    best = (score, (x, y))
    return best[1] if best else None


def to_window(x, y):
    width, height = view().getSize()
    w = view().graphicsView()
    local = QtCore.QPoint(int(x), int(height - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def click(x, y):
    win = mw().windowHandle()
    p = to_window(x, y)
    QtTest.QTest.mouseMove(win, p)
    pump(5)
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(5)
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(60)
    # off the handle, so that the next click is not a double click on it
    QtTest.QTest.mouseMove(win, to_window(5, 5))
    pump(40)


def selected_edges():
    out = []
    for s in FreeCADGui.Selection.getSelectionEx("", 0):
        for sub in s.SubElementNames:
            out.append(sub.split(".")[-1])
    return out


def tree():
    for w in mw().findChildren(QtWidgets.QTreeWidget):
        if w.objectName() == "treeWidgetReferences" and w.isVisible():
            return w
    return None


def selected_rows():
    w = tree()
    if w is None:
        return None
    out = []
    for it in w.selectedItems():
        it = it.parent() or it
        if it.text(0) not in out:
            out.append(it.text(0))
    return out


def middle_on_screen(shape, name):
    edge = shape.getElement(name)
    middle = edge.valueAt((edge.FirstParameter + edge.LastParameter) / 2)
    return tuple(view().getPointOnViewport(middle))


def far_edge(box, first):
    """The box edge whose middle is farthest from `first`'s on the screen,
    among those in the middle of the view: a corner has the navigation cube
    over it, which a scene pick does not see and takes the click"""
    width, height = view().getSize()
    x0, y0 = middle_on_screen(box.Shape, first)
    best = None
    for i in range(len(box.Shape.Edges)):
        name = "Edge%d" % (i + 1)
        if name == first:
            continue
        x, y = middle_on_screen(box.Shape, name)
        if not (0.2 * width < x < 0.8 * width and 0.2 * height < y < 0.8 * height):
            continue
        d = (x - x0) ** 2 + (y - y0) ** 2
        if best is None or d > best[0]:
            best = (d, name)
    note("INFO view %dx%d; %s's middle at %s; the other edge %s"
         % (width, height, first, (x0, y0), best[1] if best else None))
    return best[1] if best else "Edge3"


def make(kind):
    name = DOC + kind
    doc = FreeCAD.newDocument(name)
    state["doc"] = name
    body = doc.addObject("PartDesign::Body", "Body")
    box = body.newObject("PartDesign::AdditiveBox", "Box")
    box.Length = box.Width = box.Height = 10
    doc.recompute()
    FreeCADGui.SendMsgToActiveView("ViewFit")
    FreeCADGui.activeDocument().activeView().viewIsometric()
    pump()
    return doc, body, box


def try_edges(box, a, b, what):
    for edge in (a, b, a):
        center = middle_on_screen(box.Shape, edge)
        pixel = handle_pixel((int(center[0]), int(center[1])))
        note("INFO %s: %s's middle at %s, a handle pixel %s" % (what, edge, center, pixel))
        if not check("%s: a handle on %s found by picking" % (what, edge), pixel is not None):
            continue
        click(*pixel)
        check("%s: a click on %s's handle selects its row" % (what, edge),
              selected_rows() == [edge], selected_rows())
        check("%s: ... and the edge alone" % what, selected_edges() == [edge], selected_edges())


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        mw().activateWindow()

        doc, body, box = make("F")
        a = "Edge1"
        b = far_edge(box, a)
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, [a, b])
        fillet.Radius = 1
        fillet.Segments = [(b, [(0.0, 1.5)])]
        doc.recompute()
        gdoc = FreeCADGui.getDocument(state["doc"])
        gdoc.setEdit(fillet, 0)
        pump(80)
        FreeCADGui.Selection.clearSelection()
        pump(10)
        if check("the fillet is in edit (%s on the default radius, %s its own)" % (a, b),
                 gdoc.getInEdit() is not None and tree() is not None):
            try_edges(box, a, b, "fillet")
        gdoc.resetEdit()
        pump(40)
        FreeCAD.closeDocument(doc.Name)
        pump()

        doc, body, box = make("C")
        b = far_edge(box, a)
        chamfer = body.newObject("PartDesign::Chamfer", "Chamfer")
        chamfer.Base = (box, [a, b])
        chamfer.Size = 1
        doc.recompute()
        gdoc = FreeCADGui.getDocument(state["doc"])
        gdoc.setEdit(chamfer, 0)
        pump(80)
        FreeCADGui.Selection.clearSelection()
        pump(10)
        if check("the chamfer is in edit", gdoc.getInEdit() is not None and tree() is not None):
            try_edges(box, a, b, "chamfer")
        gdoc.resetEdit()
        pump(40)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


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
