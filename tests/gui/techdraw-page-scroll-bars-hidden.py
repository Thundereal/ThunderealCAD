"""A TechDraw page view shows no scroll bars, and still pans.

docs/HandsOnQueue.md entry 221 (the reporter, 2026-10-23): "hide techdraw
vertical and horizontal scrollbar to not cause confusion with overlay
panels". General/HideScrollBars, on by default, applied at once.

A page in its view, zoomed in so that it is larger than the view. Claims:
  - neither scroll bar is shown;
  - an arrow key still pans the page (the hidden bar's value moves);
  - the setting turned off: both bars shown at once;
  - turned on again: hidden again.

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


def bars(gv):
    return (gv.horizontalScrollBar().isVisible(), gv.verticalScrollBar().isVisible())


def run():
    try:
        GENERAL.RemBool("HideScrollBars")
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
        settle(1)
        hsb = gv.horizontalScrollBar()
        note("INFO horizontal range %d..%d, value %d" % (hsb.minimum(), hsb.maximum(), hsb.value()))
        check("zoomed in, the page is larger than the view", hsb.maximum() > hsb.minimum())
        check("by default neither scroll bar is shown", bars(gv) == (False, False), bars(gv))

        gv.setFocus()
        before = hsb.value()
        for _ in range(5):
            QtTest.QTest.keyClick(gv, Qt.Key_Right)
            settle(0.1)
        check("an arrow key still pans the page", hsb.value() != before, (before, hsb.value()))

        GENERAL.SetBool("HideScrollBars", False)
        settle(1)
        check("the setting off: both bars shown at once", bars(gv) == (True, True), bars(gv))
        GENERAL.SetBool("HideScrollBars", True)
        settle(1)
        check("on again: hidden again", bars(gv) == (False, False), bars(gv))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    GENERAL.RemBool("HideScrollBars")
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
