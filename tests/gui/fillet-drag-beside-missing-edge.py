"""A drag of a working edge's handle keeps its value beside a missing edge; a selected handle lights under the pointer.

docs/HandsOnQueue.md entry 196, the user's hands-on of 2026-10-20: "when
there is missing/failed edge, dragging any working edge revert back
immediately even though the fillet was made correctly"; "there is no hover
highlight when the handle is selected. add hover highlight. be careful to set
back the selected color when not hovered".

A PartDesign wedge with a fillet on its top edge (points of its own) and its
bottom edge (the default radius 2); the wedge then collapsed so the top edge's
reference is missing. In edit. Claims:
  - the top edge is missing; the fillet is in error for it alone;
  - the bottom edge's arrow, selected (Tab), has another colour than plain;
  - under the pointer it has another colour again, and the pointer gone, the
    selected one back;
  - a click on it, the pointer staying on it: the selected colour; off and
    on again: the hover's;
  - a drag of it toward its edge: after the release the radius is the
    dragged one (not 2 again), the top edge still the only failed one, and
    the result holds the bottom edge's fillet;
  - a drag cancelled by Esc with the button still down: the radius back, the
    arrow staying where the drag began while the pointer goes on, not shown
    dragged; released, the radius the start's and the edit still open.

Runs with QT_QPA_PLATFORM=offscreen; the drag goes through the window.
Coarse snapping is turned off, so a short drag changes the value.
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
DOC = "FilletDragBesideMissing"
RADIUS = 2.0

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


def key(k):
    widget = viewer_widget()
    QtTest.QTest.keyPress(widget, k, Qt.NoModifier)
    pump(5)
    target = QtWidgets.QApplication.focusWidget() or widget
    QtTest.QTest.keyRelease(target, k, Qt.NoModifier)
    pump(10)


def dragger_at(x, y):
    """The linear dragger picked at viewport pixel (x, y), or None"""
    from pivy import coin

    viewer = view().getViewer()
    rm = viewer.getSoRenderManager()
    action = coin.SoRayPickAction(rm.getViewportRegion())
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(1)
    action.apply(rm.getSceneGraph())
    picked = action.getPickedPoint()
    if picked is None:
        return None
    path = picked.getPath()
    # The dragger itself, whose colour is lit: a part of its container, which
    # is all a pick path shows of a kit (its own colour stays as it was made)
    for i in range(path.getLength()):
        node = path.getNode(i)
        if node.getTypeId().getName().getString().endswith("LinearDraggerContainer"):
            return node.getPart("dragger", False)
    return None


def picks_anything(x, y):
    from pivy import coin

    viewer = view().getViewer()
    rm = viewer.getSoRenderManager()
    action = coin.SoRayPickAction(rm.getViewportRegion())
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(4)
    action.apply(rm.getSceneGraph())
    return action.getPickedPoint() is not None


def pick_arrow(center, reach=70, step=2):
    """A viewport pixel well inside an arrow near `center`, and its dragger"""
    cx, cy = center
    best, found = -1, None
    for r in range(0, reach, step):
        for dx in range(-r, r + 1, step):
            for dy in (-r, r) if abs(dx) != r else range(-r, r + 1, step):
                if dragger_at(cx + dx, cy + dy) is None:
                    continue
                score = sum(dragger_at(cx + dx + i, cy + dy + j) is not None
                            for i in (-2, 0, 2) for j in (-2, 0, 2))
                if score > best:
                    best, found = score, (cx + dx, cy + dy)
        if found is not None and best >= 9:
            break
    return found


def colour(dragger):
    c = dragger.getField("color").getValue()
    return tuple(round(v, 3) for v in (c[0], c[1], c[2]))


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
    pump(30)


def wedge_edges(wedge):
    shape = wedge.Shape
    top = bottom = None
    for i, e in enumerate(shape.Edges):
        ends = e.Vertexes
        if len(ends) != 2:
            continue
        if (all(abs(v.Point.y - shape.BoundBox.YMax) < 1e-7 for v in ends)
                and all(abs(v.Point.z - wedge.Z2min.Value) < 1e-7 for v in ends)):
            top = "Edge%d" % (i + 1)
        if (all(abs(v.Point.y - shape.BoundBox.YMin) < 1e-7 for v in ends)
                and all(abs(v.Point.z - wedge.Zmin.Value) < 1e-7 for v in ends)):
            bottom = "Edge%d" % (i + 1)
    return top, bottom


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Gui/Gizmos").SetBool(
            "EnableCoarseSnap", False)
        mw().showNormal()
        mw().setGeometry(QtGui.QGuiApplication.primaryScreen().availableGeometry())
        pump(30)
        mw().activateWindow()
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        wedge = body.newObject("PartDesign::AdditiveWedge", "Wedge")
        doc.recompute()
        top, bottom = wedge_edges(wedge)
        bottomEdge = wedge.Shape.getElement(bottom)
        note("INFO the top edge %s, the bottom edge %s" % (top, bottom))
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (wedge, [top, bottom])
        fillet.Radius = RADIUS
        fillet.Segments = [(top, [(0.0, 1.0), (1.0, 1.5)])]
        doc.recompute()
        wedge.X2min = 5
        wedge.X2max = 5
        doc.recompute()
        failed = list(fillet.FailedEdges)
        check("the top edge is missing, the fillet in error for it alone",
              len(failed) == 1 and not fillet.isValid(), failed)

        FreeCADGui.SendMsgToActiveView("ViewFit")
        FreeCADGui.activeDocument().activeView().viewIsometric()
        pump()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(fillet, 0)
        pump(80)
        if not check("the fillet is in edit", gdoc.getInEdit() is not None):
            return

        middle = bottomEdge.valueAt((bottomEdge.FirstParameter + bottomEdge.LastParameter) / 2)
        edgeAt = view().getPointOnViewport(middle)
        arrow = pick_arrow(edgeAt)
        note("INFO the bottom edge at %s, its arrow at %s" % (tuple(edgeAt), arrow))
        if not check("the bottom edge's arrow is found", arrow is not None):
            return
        dragger = dragger_at(*arrow)
        # the pointer away from the model: inside the view, off its corners
        # (a corner's overlay takes the move from the view), nothing picked
        width, height = view().getSize()
        away = None
        for x, y in ((width // 2, 30), (30, height // 2), (width - 30, height // 2),
                     (width // 2, height - 30)):
            if not picks_anything(x, y):
                away = (x, y)
                break
        note("INFO away at %s" % (away,))
        if not check("a spot of the view with nothing on it", away is not None):
            return
        move(*away)
        plain = colour(dragger)

        # Tab through the values until the arrow is selected
        viewer_widget().setFocus()
        pump(10)
        selected = plain
        for _ in range(8):
            key(Qt.Key_Tab)
            pump(20)
            selected = colour(dragger)
            if selected != plain:
                break
        note("INFO colours: plain %s, selected %s" % (plain, selected))
        if not check("the arrow selected: another colour than plain", selected != plain,
                     (plain, selected)):
            return
        move(*arrow)
        hovered = colour(dragger)
        check("selected and under the pointer: another colour again (the hover's)",
              hovered != selected and hovered != plain, (plain, selected, hovered))
        move(*away)
        back = colour(dragger)
        check("the pointer gone: the selected colour back", back == selected, (selected, back))

        # A click on it, the pointer staying: the selected colour, not the
        # hover's (the user, 2026-10-20: "when click to select the handle,
        # its highlight should change from hover to selection")
        win = mw().windowHandle()
        move(*arrow)
        p = to_window(*arrow)
        QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
        pump(10)
        pressed = colour(dragger)
        QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, p)
        pump(30)
        clicked = colour(dragger)
        QtTest.QTest.mouseMove(win, to_window(arrow[0] + 1, arrow[1]))
        pump(20)
        still = colour(dragger)
        note("INFO colours: pressed %s, clicked %s, moved on it %s" % (pressed, clicked, still))
        check("a click on it, the pointer staying: the selected colour, not the hover's",
              clicked == selected and still == selected, (selected, clicked, still, hovered))
        key(Qt.Key_Escape)
        pump(20)
        move(*away)
        move(*arrow)
        again = colour(dragger)
        check("the pointer off and on again: the hover's colour again", again == hovered,
              (hovered, again))

        # A drag toward the edge: a smaller radius, which works (no Escape
        # more: with no value active it leaves the edit)
        move(*away)
        p = to_window(*arrow)
        dx, dy = edgeAt[0] - arrow[0], edgeAt[1] - arrow[1]
        length = max(1.0, (dx * dx + dy * dy) ** 0.5)
        steps = 30.0 / length
        offset = (dx * steps, dy * steps)
        QtTest.QTest.mouseMove(win, p)
        pump(5)
        QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
        pump(5)
        q = p
        for i in range(1, 11):
            q = QtCore.QPoint(int(p.x() + offset[0] * i / 10), int(p.y() - offset[1] * i / 10))
            QtTest.QTest.mouseMove(win, q)
            pump(5)
        # the value written as the drag's moves come in: waited for, not a
        # fixed pause (one run read 2 still, then 0.6 after the release)
        for _ in range(30):
            pump(10)
            if abs(fillet.Radius.Value - RADIUS) > 1e-6:
                break
        pump(60)
        during = fillet.Radius.Value
        QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, q)
        pump(150)
        after = fillet.Radius.Value
        note("INFO radius in the drag %g, after %g; failed %s, valid %s"
             % (during, after, list(fillet.FailedEdges), fillet.isValid()))
        check("in the drag the radius moved off 2", abs(during - RADIUS) > 1e-6, during)
        check("released: the dragged radius kept, not 2 again",
              abs(after - RADIUS) > 1e-6 and abs(after - during) < 1e-6, (during, after))
        check("released: the top edge still the only failed one",
              list(fillet.FailedEdges) == failed, list(fillet.FailedEdges))
        check("released: the result holds the bottom edge's fillet",
              fillet.Shape.isValid() and fillet.Shape.Volume < wedge.Shape.Volume - 1e-3,
              (fillet.Shape.Volume, wedge.Shape.Volume))
        check("still in edit", gdoc.getInEdit() is not None)

        # A drag cancelled by Esc, the button still down: the radius back,
        # and the arrow no longer dragged -- it stays where the drag began
        # however the pointer goes on (the user, 2026-10-20: "the dragging is
        # still active, no aborting")
        pump(60)
        arrow = pick_arrow(view().getPointOnViewport(middle))
        note("INFO the arrow for the cancel at %s" % (arrow,))
        if check("the arrow is found again", arrow is not None):
            dragger = dragger_at(*arrow)
            start = fillet.Radius.Value
            placed = tuple(round(v, 4) for v in dragger.getField("translation").getValue())
            p = to_window(*arrow)
            QtTest.QTest.mouseMove(win, p)
            pump(5)
            QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
            pump(5)
            q = p
            # away from the edge: a bigger radius (0.6 leaves little to go down)
            for i in range(1, 6):
                q = QtCore.QPoint(int(p.x() - offset[0] * i / 10),
                                  int(p.y() + offset[1] * i / 10))
                QtTest.QTest.mouseMove(win, q)
                pump(5)
            for _ in range(30):
                pump(10)
                if abs(fillet.Radius.Value - start) > 1e-6:
                    break
            pump(30)
            moved = fillet.Radius.Value
            QtTest.QTest.keyClick(viewer_widget(), Qt.Key_Escape)
            pump(60)
            # the pointer goes on, the button still down
            for i in range(6, 11):
                q = QtCore.QPoint(int(p.x() - offset[0] * i / 10),
                                  int(p.y() + offset[1] * i / 10))
                QtTest.QTest.mouseMove(win, q)
                pump(5)
            pump(60)
            held = tuple(round(v, 4) for v in dragger.getField("translation").getValue())
            active = dragger.getField("active").getValue()
            note("INFO cancel: radius start %g, moved %g, after Esc %g; arrow %s -> %s; active %s"
                 % (start, moved, fillet.Radius.Value, placed, held, active))
            check("the drag moved the radius before Esc", abs(moved - start) > 1e-6, (start, moved))
            check("Esc, the button down, the pointer moving on: the radius back",
                  abs(fillet.Radius.Value - start) < 1e-9, (start, fillet.Radius.Value))
            check("... the arrow where the drag began, not following the pointer",
                  held == placed, (placed, held))
            check("... and not shown dragged", not active, active)
            QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, q)
            pump(80)
            check("released: the radius still the start's, still in edit",
                  abs(fillet.Radius.Value - start) < 1e-9 and gdoc.getInEdit() is not None,
                  (fillet.Radius.Value, gdoc.getInEdit() is not None))
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
