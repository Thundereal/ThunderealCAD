"""A two-button pan of a TechDraw page with the Blender style ends without the
page's context menu (docs/HandsOnQueue.md entry 237, the reporter
2026-10-28: "with blender navigation style, pan techdraw page with left and
right button button down drag works. but will releasing the button the page
context menu pops up").

The menu comes with the right button's release (Windows): a pan whose left
button was let go of first had nothing down by then that refused it. The
pointer is moved by QTest events on the main window, which go through the
platform's own context menu trigger (hidden desktop or screen). A menu that opens is closed from a timer and noted.

Reopened 2026-11-02, the reporter: "Two button press should not open menu"
-- left and right down together and let go of without a move opened the
menu when the left went up first: only a pan refused it.

Claims, on a page in the Blender style:
  - left and right down, dragged, the left let go of first: no menu, and
    the page panned;
  - the same with the right let go of first: no menu;
  - left and right down, NOT moved, let go of in either order, pressed in
    either order: no menu;
  - a right click alone: the menu, as before.
And in a 3D view in the Blender style: the two-button press without a move,
both release orders, opens no menu; a right click alone opens it.
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


def view3d_widget():
    """The document's 3D view, its sub window made active; the pointer goes
    to its middle, where the viewer's drawing widget is"""
    for w in mw().findChildren(QtWidgets.QWidget):
        if w.metaObject().className() != "Gui::View3DInventor":
            continue
        sub = w.parentWidget()
        while sub is not None and not isinstance(sub, QtWidgets.QMdiSubWindow):
            sub = sub.parentWidget()
        if sub is not None:
            sub.mdiArea().setActiveSubWindow(sub)
        pump(0.5)
        at = QtWidgets.QApplication.widgetAt(w.mapToGlobal(w.rect().center()))
        note("INFO the 3D view (visible %s), at its middle: %s"
             % (w.isVisible(), at.metaObject().className() if at else None))
        return w
    note("INFO no 3D view; MDI views: %s" % sorted(set(
        x.metaObject().className() for x in mw().findChildren(QtWidgets.QWidget)
        if x.inherits("Gui::MDIView"))))
    return None


def gesture(releases, drag=True, presses=None, target=None):
    """Left and right down at the page's middle (or target's), dragged, let
    go of in the order given; how many menus opened, and how far it scrolled"""
    v = page_view()
    win = mw().windowHandle()
    if target is None:
        c = v.viewport().mapTo(mw(), v.viewport().rect().center())
    else:
        c = target.mapTo(mw(), target.rect().center())
    def scrolled():
        if v is None:
            return None
        return (v.horizontalScrollBar().value(), v.verticalScrollBar().value())

    before = scrolled()
    MENUS.clear()
    QtTest.QTest.mouseMove(win, c)
    pump(0.1)
    held = list(presses or releases)
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
    return list(MENUS), before, scrolled()


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


def press_no_move():
    for presses, releases, what in (
            ([Qt.LeftButton, Qt.RightButton], [Qt.LeftButton, Qt.RightButton],
             "left then right down, left up first"),
            ([Qt.LeftButton, Qt.RightButton], [Qt.RightButton, Qt.LeftButton],
             "left then right down, right up first"),
            ([Qt.RightButton, Qt.LeftButton], [Qt.LeftButton, Qt.RightButton],
             "right then left down, left up first"),
            ([Qt.RightButton, Qt.LeftButton], [Qt.RightButton, Qt.LeftButton],
             "right then left down, right up first")):
        menus, _, _ = gesture(releases, drag=False, presses=presses)
        check("a two-button press, no move (%s): no menu" % what, not menus, menus)


def right_click():
    menus, _, _ = gesture([Qt.RightButton], drag=False)
    check("a right click alone: the page's menu", len(menus) == 1, menus)


def view3d():
    w = view3d_widget()
    if not check("the 3D view's GL widget", w is not None):
        return
    for releases, what in (([Qt.LeftButton, Qt.RightButton], "left up first"),
                           ([Qt.RightButton, Qt.LeftButton], "right up first")):
        menus, _, _ = gesture(releases, drag=False,
                              presses=[Qt.LeftButton, Qt.RightButton], target=w)
        note("INFO 3D view, two-button press, no move, %s: menus %s" % (what, menus))
        check("3D view: a two-button press, no move (%s): no menu" % what, not menus, menus)
    menus, _, _ = gesture([Qt.RightButton], drag=False, target=w)
    check("3D view: a right click alone: the menu", len(menus) == 1, menus)


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


STEPS[:] = [start, left_first, right_first, press_no_move, right_click, view3d]
QtCore.QTimer.singleShot(3000, advance)
