"""A TechDraw page view shows the spreadsheet's thin overlaid scroll bars.

docs/HandsOnQueue.md entry 221 (the reporter, 2026-10-23): "hide techdraw
vertical and horizontal scrollbar to not cause confusion with overlay
panels"; entry 224: "add the same auto scroll bar to techdraw page. it
looks nice", and on 221's option: "Keep, default off". The option is
General/HidePageScrollBars now, off by default; the 221 key HideScrollBars,
true wherever stage29's preference page saved its old default, is no longer
read.

A page in its view, zoomed in so that it is larger than the view, with the
221 key set true. Claims:
  - Qt's own bars are not shown; the overlaid ones are there, hidden at rest;
  - an arrow key pans the page and shows the overlaid bars, which follow;
  - moving an overlaid bar pans the page;
  - they fade out a while after;
  - HidePageScrollBars on: no overlaid bars, Qt's own still hidden, and an
    arrow key still pans;
  - off again: the overlaid bars are back.

Runs offscreen as well.
"""
import glob
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
GENERAL = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/TechDraw/General")

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


def page_view():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QGraphicsView):
        if w.isVisible() and w.metaObject().className() == "TechDrawGui::QGVPage":
            return w
    return None


def own_bars(gv):
    return (gv.horizontalScrollBar().isVisible(), gv.verticalScrollBar().isVisible())


def overlay_bars(gv):
    found = {}
    for b in gv.findChildren(QtWidgets.QScrollBar):
        if b.objectName() in ("OverlayScrollBarH", "OverlayScrollBarV"):
            found[b.objectName()] = b
    return found.get("OverlayScrollBarH"), found.get("OverlayScrollBarV")


def pan_right(gv, times=5):
    gv.setFocus()
    for _ in range(times):
        QtTest.QTest.keyClick(gv, Qt.Key_Right)
        settle(0.05)


def run():
    try:
        GENERAL.RemBool("HidePageScrollBars")
        GENERAL.SetBool("HideScrollBars", True)
        doc = FreeCAD.newDocument("PageScrollBars")
        templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                           "Templates", "**", "*A4*Landscape*.svg"), recursive=True)
        page = doc.addObject("TechDraw::DrawPage", "Page")
        tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        if templates:
            tmpl.Template = templates[0]
        page.Template = tmpl
        doc.recompute()
        page.ViewObject.doubleClicked()
        settle(3)
        gv = page_view()
        if not check("the page's view is shown", gv is not None):
            return
        gv.resetTransform()
        gv.scale(2.0, 2.0)
        settle(2.5)
        hsb = gv.horizontalScrollBar()
        note("INFO horizontal range %d..%d, value %d" % (hsb.minimum(), hsb.maximum(), hsb.value()))
        check("zoomed in, the page is larger than the view", hsb.maximum() > hsb.minimum())
        check("Qt's own bars are not shown", own_bars(gv) == (False, False), own_bars(gv))
        oh, ov = overlay_bars(gv)
        if not check("the overlaid bars are there, the 221 key true notwithstanding",
                     oh is not None and ov is not None):
            return
        check("at rest they are hidden", not oh.isVisible() and not ov.isVisible(),
              (oh.isVisible(), ov.isVisible()))

        before = hsb.value()
        pan_right(gv)
        check("an arrow key pans the page", hsb.value() != before, (before, hsb.value()))
        check("panning shows the overlaid bars", oh.isVisible() and ov.isVisible(),
              (oh.isVisible(), ov.isVisible()))
        check("the horizontal one follows", oh.value() == hsb.value(), (oh.value(), hsb.value()))
        g = oh.geometry()
        check("it is thin and lies over the page", 0 < g.height() <= 12
              and gv.viewport().geometry().contains(g.center()), g)

        target = (oh.minimum() + oh.maximum()) // 2
        if target == hsb.value():
            target = oh.minimum()
        oh.setValue(target)
        settle(0.1)
        check("moving the overlaid bar pans the page", hsb.value() == target, (target, hsb.value()))

        settle(2.5)
        check("a while after, they have faded out", not oh.isVisible() and not ov.isVisible(),
              (oh.isVisible(), ov.isVisible()))

        GENERAL.SetBool("HidePageScrollBars", True)
        settle(0.5)
        oh, ov = overlay_bars(gv)
        check("the option on: no overlaid bars", oh is None and ov is None, (oh, ov))
        check("the option on: Qt's own still hidden", own_bars(gv) == (False, False), own_bars(gv))
        before = hsb.value()
        pan_right(gv)
        check("the option on: an arrow key still pans", hsb.value() != before,
              (before, hsb.value()))

        GENERAL.SetBool("HidePageScrollBars", False)
        settle(0.5)
        oh, ov = overlay_bars(gv)
        check("off again: the overlaid bars are back", oh is not None and ov is not None)
        check("off again: Qt's own not shown", own_bars(gv) == (False, False), own_bars(gv))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    GENERAL.RemBool("HidePageScrollBars")
    GENERAL.RemBool("HideScrollBars")
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
