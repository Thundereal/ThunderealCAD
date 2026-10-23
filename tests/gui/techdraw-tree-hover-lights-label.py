"""A tree hover lights a TechDraw dimension's label, and a balloon's, not only its lines.

docs/HandsOnQueue.md entry 218 (the reporter, 2026-10-23): "tree view hover
of techdraw dimension only highlight the arrows but not its label".

A box; a page with a view of it at scale 1; a distance dimension on its top
edge, its label placed well above the view's geometry; a balloon beside
the view. The page's view is grabbed (pixels only: a script that walks a
page's scene items takes them out of it). Around each label, normally its
text is in the dark normal colour. Claims:
  - preselected as a tree hover does (Selection's preselect), the
    dimension's label has no dark text left: all in the preselect colour;
  - the preselect removed, the dark text is back;
  - the same for the balloon's label;
  - selected from the tree, the dimension's label is lit too.

Runs offscreen as well.
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
DOC = "TreeHoverLabel"
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
        time.sleep(0.02)


def mw():
    return FreeCADGui.getMainWindow()


def page_view():
    found = []
    for w in mw().findChildren(QtWidgets.QGraphicsView):
        found.append((w.metaObject().className(), w.isVisible(), w.width(), w.height()))
        if w.isVisible() and w.metaObject().className() == "TechDrawGui::QGVPage":
            note("INFO the page's view %dx%d, scene rect %s" % (w.width(), w.height(), w.sceneRect()))
            return w
    note("INFO graphics views: %s" % found)
    return None


def value(q):
    return q.Value if hasattr(q, "Value") else q


def dark_near(gv, scene_pt, half, name):
    """Dark pixels in a box around a scene point of the page's view"""
    img = gv.viewport().grab().toImage()
    img.save(os.path.join(OUT, name + ".png"))
    c = gv.mapFromScene(QtCore.QPointF(*scene_pt))
    count = 0
    for y in range(c.y() - half[1], c.y() + half[1]):
        for x in range(c.x() - half[0], c.x() + half[0]):
            if 0 <= x < img.width() and 0 <= y < img.height():
                p = QtGui.QColor(img.pixel(x, y))
                if p.red() + p.green() + p.blue() < 160:
                    count += 1
    return count


def preselect(obj):
    FreeCADGui.Selection.setPreselection(obj, "")
    settle(1)


def unpreselect():
    FreeCADGui.Selection.clearPreselection()
    settle(1)


def run():
    try:
        doc = FreeCAD.newDocument(DOC)
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
        view = doc.addObject("TechDraw::DrawViewPart", "View")
        page.addView(view)
        view.Source = [box]
        view.Direction = FreeCAD.Vector(0, 0, 1)
        view.ScaleType = "Custom"
        view.Scale = 1.0
        view.X, view.Y = 120, 120
        doc.recompute()
        settle(5)
        dim = doc.addObject("TechDraw::DrawViewDimension", "Dimension")
        dim.Type = "Distance"
        dim.References2D = [(view, "Edge0")]
        page.addView(dim)
        dim.X, dim.Y = 0, 40
        balloon = doc.addObject("TechDraw::DrawViewBalloon", "Balloon")
        balloon.SourceView = view
        balloon.OriginX, balloon.OriginY = 20, 10
        page.addView(balloon)
        balloon.X, balloon.Y = 60, 40
        balloon.Text = "88"
        doc.recompute()
        settle(3)
        page.ViewObject.doubleClicked()
        settle(3)
        gv = page_view()
        if not check("the page's view is shown", gv is not None):
            return
        # one scene unit a pixel: the label's text some 35 px high
        gv.resetTransform()
        vx, vy = value(view.X), value(view.Y)
        dim_at = (REZ * (vx + value(dim.X)), -REZ * (vy + value(dim.Y)))
        gv.centerOn(QtCore.QPointF(*dim_at))
        settle(1)
        bal_at = (REZ * (vx + value(balloon.X)), -REZ * (vy + value(balloon.Y)))
        pre = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/TechDraw/Colors").GetUnsigned(
            "PreSelectColor", 0)
        note("INFO PreSelectColor %08x; label at scene %s, viewport %s"
             % (pre, dim_at, gv.mapFromScene(QtCore.QPointF(*dim_at))))
        half = (90, 30)

        normal = dark_near(gv, dim_at, half, "dimension-normal")
        check("the dimension's label is drawn, dark", normal > 20, normal)
        preselect(dim)
        lit = dark_near(gv, dim_at, half, "dimension-preselected")
        check("a tree preselect lights the dimension's label: no dark text left",
              lit <= max(3, normal // 20), (normal, lit))
        unpreselect()
        back = dark_near(gv, dim_at, half, "dimension-after")
        check("... and the preselect removed, the dark text is back",
              abs(back - normal) <= max(3, normal // 10), (normal, back))

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(dim)
        settle(1)
        chosen = dark_near(gv, dim_at, half, "dimension-selected")
        check("selected from the tree, the dimension's label is lit too",
              chosen <= max(3, normal // 20), (normal, chosen))
        FreeCADGui.Selection.clearSelection()
        settle(1)

        bhalf = (25, 20)
        gv.centerOn(QtCore.QPointF(*bal_at))
        settle(1)
        bnormal = dark_near(gv, bal_at, bhalf, "balloon-normal")
        check("the balloon's label is drawn, dark", bnormal > 5, bnormal)
        preselect(balloon)
        blit = dark_near(gv, bal_at, bhalf, "balloon-preselected")
        check("a tree preselect lights the balloon's label too",
              blit <= max(2, bnormal // 10), (bnormal, blit))
        unpreselect()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
