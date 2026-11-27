"""TechDraw hover names what is under the mouse, and a dimension's arrows take the mouse all over.

The reporter, 2026-11-26: "make TechDraw preselection calls
Selection().setPreselect() so the status bar can see which object is under
mouse without actually select it"; and, of a dimension over a section view's
face, "the highlight flickering only happens for arrows. it works fine on
dimension label text". An arrow head's shape was its outline with its fill
appended, and where the two wound the opposite ways they cancelled: holes in
the head, where the face under it took the mouse (QGIArrow::shape()).

A box; a page, a view of it at scale 1, a horizontal distance across the
middle of the view's face with big arrows. Claims:
  - the mouse over an edge preselects that edge (View.EdgeN), and the status
    bar says so; off the view, on the bare page, nothing is preselected;
  - over every point sampled inside each arrow head the dimension is what is
    preselected, never the face under it, and a click there selects it;
  - nothing is selected by hovering.

Runs on the hidden desktop (scripts/gui-test.cmd).
"""
import glob
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "TDHoverPreselect"
REZ = 10.0
state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


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


def page_view():
    for v in mw().findChildren(QtWidgets.QGraphicsView):
        if v.metaObject().className() == "TechDrawGui::QGVPage" and v.isVisible():
            return v
    return None


def mouse(kind, pos, button=QtCore.Qt.NoButton, buttons=QtCore.Qt.NoButton):
    vp = page_view().viewport()
    ev = QtGui.QMouseEvent(kind, QtCore.QPointF(pos), QtCore.QPointF(vp.mapToGlobal(pos)),
                           button, buttons, QtCore.Qt.NoModifier)
    QtWidgets.QApplication.sendEvent(vp, ev)


def hover(scene_point):
    p = page_view().mapFromScene(scene_point)
    # from a pixel aside, so the move is one
    mouse(QtCore.QEvent.MouseMove, QtCore.QPoint(p.x() + 3, p.y() + 3))
    QtWidgets.QApplication.processEvents()
    mouse(QtCore.QEvent.MouseMove, p)
    QtWidgets.QApplication.processEvents()
    return p


def presel():
    s = FreeCADGui.Selection.getPreselection()
    try:
        if not s.ObjectName:
            return ""
        return "%s.%s" % (s.ObjectName, s.SubElementNames[0] if s.SubElementNames else "")
    except Exception:
        return ""


def run():
    try:
        doc = FreeCAD.newDocument(DOC)
        box = doc.addObject("Part::Box", "Box")
        box.Length, box.Width, box.Height = 40, 40, 40
        templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                           "Templates", "**", "*A4*Landscape*.svg"), recursive=True)
        page = doc.addObject("TechDraw::DrawPage", "Page")
        tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        if templates:
            tmpl.Template = templates[0]
        page.Template = tmpl
        view = doc.addObject("TechDraw::DrawViewPart", "View")
        page.addView(view)
        view.Source = [box]
        view.ScaleType = "Custom"
        view.Scale = 1.0
        view.X, view.Y = 140, 110
        doc.recompute()
        settle(4)
        # a horizontal edge: a distance along it, moved down across the face
        edge = None
        n = 0
        while edge is None and n < 40:
            e = view.getEdgeByIndex(n)
            a, b = e.Vertexes[0].Point, e.Vertexes[-1].Point
            if abs(a.y - b.y) < 1e-6 and abs(a.x - b.x) > 30:
                edge = "Edge%d" % n
            n += 1
        if not check("a horizontal edge of the view", edge is not None):
            return
        dim = doc.addObject("TechDraw::DrawViewDimension", "Dimension")
        dim.Type = "DistanceX"
        dim.References2D = [(view, edge)]
        page.addView(dim)
        dim.X, dim.Y = 0.0, 5.0
        doc.recompute()
        dim.ViewObject.Arrowsize = 6.0
        doc.recompute()
        settle(2)
        page.ViewObject.doubleClicked()
        settle(4)
        gv = page_view()
        if not check("the page shown", gv is not None):
            return
        gv.resetTransform()
        gv.scale(2.0, 2.0)
        gv.centerOn(QtCore.QPointF(view.X.Value * REZ, -view.Y.Value * REZ))
        settle(2)
        FreeCADGui.Selection.clearSelection()

        # (B) an edge, then the bare page
        e = view.getEdgeByIndex(int(edge[4:]))
        a, b = e.Vertexes[0].Point, e.Vertexes[-1].Point
        mid = QtCore.QPointF((view.X.Value + (a.x + b.x) / 2.0) * REZ,
                             -(view.Y.Value + (a.y + b.y) / 2.0) * REZ)
        hover(mid)
        settle(0.3)
        check("over an edge: it is preselected", presel() == "View.%s" % edge, presel())
        # MainWindow::showMessage writes the status bar's action label
        bar = " | ".join(w.text() for w in mw().statusBar().findChildren(QtWidgets.QLabel)
                         if w.text())
        check("and the status bar names it", edge in bar and "View" in bar, bar)
        check("hovering selects nothing", not FreeCADGui.Selection.getSelectionEx())
        hover(QtCore.QPointF((view.X.Value + 120.0) * REZ, -(view.Y.Value + 60.0) * REZ))
        settle(0.3)
        check("on the bare page: nothing preselected", presel() == "", presel())

        # (A) inside each arrow head
        tips = dim.getArrowPositions()
        inner = tips[1] - tips[0]
        length = inner.Length
        inner.normalize()
        across = FreeCAD.Vector(-inner.y, inner.x, 0)
        size = 6.0
        missed = {0: [], 1: []}
        total = {0: 0, 1: 0}
        for i, tip in enumerate(tips):
            along = inner if i == 0 else -inner
            # inside the head: from near its tip to near its base, on and off its axis
            for f in (0.25, 0.4, 0.55, 0.7, 0.85):
                for g in (-0.4, -0.2, 0.0, 0.2, 0.4):
                    halfwidth = size / 6.0 * f      # the head widens from its tip
                    pt = tip + along * (size * f) + across * (halfwidth * g)
                    sp = QtCore.QPointF((view.X.Value + pt.x) * REZ, -(view.Y.Value + pt.y) * REZ)
                    hover(sp)
                    total[i] += 1
                    if presel() != "Dimension.":
                        missed[i].append("%.2f,%.2f:%s" % (f, g, presel()))
        note("INFO dimension length %.2f, tips %s" % (length, [tuple(round(c, 2) for c in t)
                                                             for t in tips]))
        for i in (0, 1):
            check("arrow %d: the dimension preselected at all %d points inside it" % (i, total[i]),
                  not missed[i], missed[i][:8])
        # a click inside the head selects the dimension
        tip = tips[0]
        pt = tip + inner * (size * 0.5)
        sp = QtCore.QPointF((view.X.Value + pt.x) * REZ, -(view.Y.Value + pt.y) * REZ)
        got = []
        for _ in range(4):
            FreeCADGui.Selection.clearSelection()
            p = hover(sp)
            mouse(QtCore.QEvent.MouseButtonPress, p, QtCore.Qt.LeftButton, QtCore.Qt.LeftButton)
            mouse(QtCore.QEvent.MouseButtonRelease, p, QtCore.Qt.LeftButton, QtCore.Qt.NoButton)
            settle(0.3)
            got.append(",".join(s.ObjectName for s in FreeCADGui.Selection.getSelectionEx()))
        check("a click inside an arrow head selects the dimension, every time",
              got == ["Dimension"] * 4, got)
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
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
