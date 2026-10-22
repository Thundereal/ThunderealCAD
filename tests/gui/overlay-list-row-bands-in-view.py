"""A task panel laid over its view bands its lists as an overlaid dock does
(docs/HandsOnQueue.md entry 208).

The user, 2026-10-21: "regression, there is one entry to fix the list view
item background in overlay mode. I tested fillet edit. and its fillet list
view does show correctly when in the task panel under overlay. but sill
shows uniform opaque background when moved to editing view". Entry 193's
painter banded the lists of an overlaid dock only; a task panel in its view,
over the picture (TaskPanelHost, overlay mode), looks as such a dock does but
is in none.

Claims, under Dark, the task panel in the view (View/TaskPanelInView), over
the picture on the left:
  - a fillet in edit, its edge list of three rows in the panel in the view:
    each row backed at the overlays' opacity, the first and the third alike,
    the second another (the bands), not one opaque ground;
  - the task box's own ground there see-through as the dock's: its content
    frame at the overlays' opacity, and the list on it (its own ground clear,
    as the sheets make a TaskView's) at that of the box and a band together;
  - the panel set beside the view (its own cell, a plain widget's look): no
    row backed at the overlays' opacity -- the bands are the overlay's alone.

The list is rendered through its parent onto a clear image (overlay-list-
row-bands.py). Runs with QT_QPA_PLATFORM=offscreen; the pointer is not moved.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
HOST = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/TaskView/Host")
S = {}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()


def mw():
    return FreeCADGui.getMainWindow()


def cls(w):
    return w.metaObject().className()


def host_of(w):
    while w is not None:
        if cls(w) == "Gui::TaskView::TaskPanelHost":
            return w
        w = w.parentWidget()
    return None


def row_grounds(view, rows, picture=None):
    """Each row's commonest colour across the list, (r, g, b, a), as the
    list's parent renders it onto a clear image"""
    vp = view.viewport()
    parent = view.parentWidget()
    img = QtGui.QImage(parent.size(), QtGui.QImage.Format_ARGB32)
    img.fill(QtCore.Qt.transparent)
    parent.render(img, QtCore.QPoint(), QtGui.QRegion(),
                  QtWidgets.QWidget.RenderFlag.DrawChildren)
    if picture:
        img.save(os.path.join(OUT, picture))
    note("INFO the list's own alternate %s (off while the bands are on); its parent %s, "
         "no system background %s, autofill %s" % (
             view.alternatingRowColors(), cls(parent),
             parent.testAttribute(QtCore.Qt.WA_NoSystemBackground), parent.autoFillBackground()))
    got = []
    for rect in rows:
        seen = {}
        for y in (rect.top() + 1, rect.center().y(), rect.bottom() - 1):
            for x in range(2, vp.width() - 2, 2):
                at = vp.mapTo(parent, QtCore.QPoint(x, y))
                if img.rect().contains(at):
                    v = img.pixel(at)
                    seen[v] = seen.get(v, 0) + 1
        v = max(seen, key=seen.get) if seen else 0
        got.append(((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255))
    return got


def box_ground(view):
    """(r, g, b, a) of the task box's content frame the list is in, with all
    it holds, rendered onto a clear image: at its left margin, and in the
    list's first row"""
    frame = view.parentWidget()
    while frame is not None and not (isinstance(frame, QtWidgets.QFrame)
                                     and frame.property("class") == "content"):
        frame = frame.parentWidget()
    if frame is None:
        return None, None
    img = QtGui.QImage(frame.size(), QtGui.QImage.Format_ARGB32)
    img.fill(QtCore.Qt.transparent)
    frame.render(img, QtCore.QPoint(), QtGui.QRegion(),
                 QtWidgets.QWidget.RenderFlag.DrawChildren)
    img.save(os.path.join(OUT, "box-over-the-picture.png"))

    def at(p):
        c = QtGui.QColor.fromRgba(img.pixel(p))
        return (c.red(), c.green(), c.blue(), c.alpha())

    row = view.visualRect(view.model().index(0, 0))
    inside = view.viewport().mapTo(frame, QtCore.QPoint(row.width() // 2, row.center().y()))
    # halfway between the frame's edge and the list's, clear of a border
    left = view.mapTo(frame, QtCore.QPoint(0, 0)).x()
    return at(QtCore.QPoint(max(left // 2, 1), inside.y())), at(inside)


def near(a, b, tol=2):
    return max(abs(a[i] - b[i]) for i in range(3)) <= tol


def task_list():
    """The fillet's edge list in the panel in its view, and that panel"""
    for w in mw().findChildren(QtWidgets.QAbstractItemView):
        if w.isVisible() and w.model() is not None and w.model().rowCount() == 3 \
                and host_of(w) is not None:
            return w, host_of(w)
    return None, None


def run():
    try:
        check("the Dark theme applies", FreeCADGui.applyTheme("Dark"))
        settle(2000)
        VIEW.SetBool("TaskPanelInView", True)
        HOST.SetString("Mode", "Overlay")
        HOST.SetString("Side", "Left")
        doc = FreeCAD.newDocument("RowBandsInView")
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1", "Edge2", "Edge3"])
        fillet.Radius = 1
        doc.recompute()
        alpha = VIEW.GetInt("DockOverlayAlpha", 180)
        gdoc = FreeCADGui.getDocument(doc.Name)
        gdoc.setEdit(fillet, 0)
        settle(3000)

        view, host = task_list()
        if not check("a fillet's three edges listed in the task panel in its view",
                     view is not None, None):
            return
        model = view.model()
        rows = [view.visualRect(model.index(r, 0)) for r in range(3)]
        got = row_grounds(view, rows, "rows-over-the-picture.png")
        # the panel on the screen, as the user sees it over the picture
        host.grab().save(os.path.join(OUT, "panel-over-the-picture.png"))
        note("INFO over the picture: rows %s; overlay alpha %d" % (got, alpha))
        check("208: over the picture, each row backed at the overlays' opacity",
              all(abs(g[3] - alpha) <= 6 for g in got), got)
        check("208: ... and the rows alternate, not one opaque ground",
              not near(got[0], got[1]) and near(got[0], got[2]), got)
        margin, under = box_ground(view)
        # the box's ground at the overlays' opacity, a row's band over it
        both = 255 - (255 - alpha) * (255 - alpha) // 255
        note("INFO the box's ground: margin %s, under the list %s; expected alpha %d and %d"
             % (margin, under, alpha, both))
        check("208: the task box's ground over the picture is see-through, as the dock's",
              margin is not None and abs(margin[3] - alpha) <= 6, margin)
        check("208: ... and so is the list on it, rows and all",
              under is not None and abs(under[3] - both) <= 8, under)

        # beside the view, in a cell of its own: a plain widget's look
        HOST.SetString("Mode", "Side")
        place = None
        for v in mw().findChildren(QtWidgets.QWidget):
            if cls(v) == "Gui::View3DInventor" and v.isVisible():
                place = v
        if place is not None:
            place.setProperty("Task_Mode", "Side")
        gdoc.resetEdit()
        settle(1500)
        gdoc.setEdit(fillet, 0)
        settle(3000)
        view, host = task_list()
        if view is None:
            note("INFO beside the view: no list found")
        elif host.testAttribute(QtCore.Qt.WA_NoSystemBackground):
            note("INFO the panel stayed over the picture: the beside claim not made")
        else:
            got = row_grounds(view, [view.visualRect(view.model().index(r, 0))
                                     for r in range(3)])
            note("INFO beside the view: rows %s" % (got,))
            check("208: beside the view, no row backed at the overlays' opacity",
                  not any(abs(g[3] - alpha) <= 6 for g in got), got)
        gdoc.resetEdit()
        settle(800)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        try:
            for name in list(FreeCAD.listDocuments().keys()):
                FreeCAD.closeDocument(name)
        except Exception:
            pass
        note("DONE")
        QtCore.QTimer.singleShot(500, mw().close)


QtCore.QTimer.singleShot(3000, run)
