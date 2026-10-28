"""A two-button pan of a TechDraw page with the Blender style ends without the
page's context menu (docs/HandsOnQueue.md entry 237, the reporter
2026-10-28: "with blender navigation style, pan techdraw page with left and
right button button down drag works. but will releasing the button the page
context menu pops up").

The menu comes with the right button's release (Windows): a pan whose left
button was let go of first had nothing down by then that refused it. Runs
on a real screen (the platform's own context menu trigger); the pointer is
moved by Qt events. A menu that opens is closed from a timer and noted.

Claims, on a page in the Blender style:
  - left and right down, dragged, the left let go of first: no menu, and
    the page panned;
  - the same with the right let go of first: no menu;
  - a right click alone: the menu, as before.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets
from PySide6 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "BlenderPanMenu"
Qt = QtCore.Qt
STEPS = []
MENUS = []


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds=0.3):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def mw():
    return FreeCADGui.getMainWindow()


def page_view():
    for w in mw().findChildren(QtWidgets.QGraphicsView):
        if w.metaObject().className() == "TechDrawGui::QGVPage" and w.isVisible():
            return w
    return None


def sweep():
    """A context menu shown: noted, and closed"""
    for w in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(w, QtWidgets.QMenu) and w.isVisible():
            MENUS.append([a.text() for a in w.actions()][:3])
            w.close()


SWEEPER = QtCore.QTimer()
SWEEPER.setInterval(100)
SWEEPER.timeout.connect(sweep)


def gesture(releases, drag=True):
    """Left and right down at the page's middle, dragged, let go of in
    the order given; how many menus opened, and how far it scrolled"""
    v = page_view()
    win = mw().windowHandle()
    c = v.viewport().mapTo(mw(), v.viewport().rect().center())
    before = (v.horizontalScrollBar().value(), v.verticalScrollBar().value())
    MENUS.clear()
    QtTest.QTest.mouseMove(win, c)
    pump(0.1)
    held = [b for b in releases]
    for b in held:
        QtTest.QTest.mousePress(win, b, Qt.NoModifier, c)
        pump(0.05)
    end = c
    if drag:
        for i in range(1, 9):
            end = c + QtCore.QPoint(6 * i, 4 * i)
            QtTest.QTest.mouseMove(win, end)
            pump(0.03)
    for b in releases:
        QtTest.QTest.mouseRelease(win, b, Qt.NoModifier, end)
        pump(0.1)
    pump(0.8)
    after = (v.horizontalScrollBar().value(), v.verticalScrollBar().value())
    return list(MENUS), before, after


def start():
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetString(
        "NavigationStyle", "Gui::BlenderNavigationStyle")
    import TechDrawGui  # the view providers
    doc = FreeCAD.newDocument(DOC)
    page = doc.addObject("TechDraw::DrawPage", "Page")
    template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    template.Template = os.path.join(
        FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates", "A4_Landscape_blank.svg")
    page.Template = template
    doc.recompute()
    page.ViewObject.show()
    SWEEPER.start()


def left_first():
    v = page_view()
    if not check("the page's view is open", v is not None):
        STEPS[:] = []
        return
    v.scale(3.0, 3.0)
    pump(0.5)
    menus, before, after = gesture([Qt.LeftButton, Qt.RightButton])
    check("a pan with left and right, the left let go of first: no menu", not menus, menus)
    check("... and the page panned", before != after, (before, after))


def right_first():
    menus, before, after = gesture([Qt.RightButton, Qt.LeftButton])
    check("the same, the right let go of first: no menu", not menus, menus)


def right_click():
    menus, _, _ = gesture([Qt.RightButton], drag=False)
    check("a right click alone: the page's menu", len(menus) == 1, menus)


def finish():
    SWEEPER.stop()
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


def advance():
    if not STEPS:
        finish()
        return
    step = STEPS.pop(0)
    try:
        step()
    except Exception:
        note("ABORT in %s: %s" % (step.__name__, traceback.format_exc().replace("\n", " | ")))
        STEPS[:] = []
    QtCore.QTimer.singleShot(1500, advance)


STEPS[:] = [start, left_first, right_first, right_click]
QtCore.QTimer.singleShot(3000, advance)
