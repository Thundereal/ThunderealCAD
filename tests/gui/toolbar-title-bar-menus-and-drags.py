"""Toolbars in the custom title bar: their own context menus, their grips, and
docked toolbars that still drag after the title bar has been used.

The reporter, 2026-11-26: "right click toolbar in titlebar does not show
toolbars own context menu, for example the workbench toolbar"; and two
drag problems -- the title bar's toolbars "locked" at start until the lock
is toggled by hand, and "under some unknown state, i cannot drag a normally
docked ... unlocked toolbar".

The menu: ToolBarManager answered the right button's RELEASE with the
title area's toolbar list. QTabBar ignores a right button release, so it
climbed from the workbench tab bar to the area and the list was shown there,
modally, before the tab bar's own menu was asked for. Now the context menu
event is answered, which reaches the area only when nothing under the mouse
took it, and a toolbar in the title bar gets the window's toolbar menu, as
it does docked.

The drags were not reproduced with synthetic input (a real mouse is not
available on the hidden desktop); the claims below hold the state they need:
a grip on each title bar toolbar that takes it out, and docked toolbars that
start a drag by their handle after each suspect state change.

Claims, the custom title bar on, the window maximized, File and Workbench
in the title bar's left area:
  - a right click on the workbench selector (tabs or combo box) brings ITS
    menu (Show text /
    Hide text, Collapse), and no other menu before it;
  - a right click on a tool button of File brings the window's toolbar
    menu (its Lock toolbars submenu), not the area's list;
  - each title bar toolbar is movable, has its grip, and a drag by the grip
    takes it out of the title bar;
  - a docked toolbar starts a drag by its handle: as set up, after a toolbar
    has been out of the title bar and docked again, after a workbench
    switch and back, after the title bar lock and the default lock have
    each been switched on and off, and after a Part Design Extensions toggle
    has shown and hidden its toolbar (where that toggle exists).

Runs on the hidden desktop (scripts/gui-test.cmd); GT_EXPECT_OLD=1 for a
build without the fix, where the menus are the area's list.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
state = {"done": False}
menus = []


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


def sweep():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(w, QtWidgets.QMenu) and w.isVisible():
            texts = [a.text() for a in w.actions()]
            subs = [a.text() for a in w.actions() if a.menu()]
            menus.append((texts[:10], subs))
            w.close()
        elif isinstance(w, QtWidgets.QMessageBox) and w.isVisible():
            note("INFO box: " + w.text())
            w.close()


SWEEPER = QtCore.QTimer()
SWEEPER.timeout.connect(sweep)
SWEEPER.start(100)


def mw():
    return FreeCADGui.getMainWindow()


def toolbar(name):
    for tb in mw().findChildren(QtWidgets.QToolBar):
        if tb.objectName() == name and not tb.isHidden():
            return tb
    return None


def grips(tb):
    return [w for w in tb.findChildren(QtWidgets.QWidget)
            if w.metaObject().className() == "Gui::ToolBarGrip"]


def in_title(tb):
    return tb is not None and tb.parentWidget() is not None \
        and tb.parentWidget().objectName() in ("MenuBarLeftArea", "MenuBarRightArea")


def send(w, kind, local, button, buttons):
    QtWidgets.QApplication.sendEvent(w, QtGui.QMouseEvent(
        kind, QtCore.QPointF(local), QtCore.QPointF(w.mapToGlobal(local)),
        button, buttons, QtCore.Qt.NoModifier))


def right_click(target, local):
    """As Windows delivers it: press, release, then the context menu event"""
    del menus[:]
    send(target, QtCore.QEvent.MouseButtonPress, local, QtCore.Qt.RightButton,
         QtCore.Qt.RightButton)
    send(target, QtCore.QEvent.MouseButtonRelease, local, QtCore.Qt.RightButton,
         QtCore.Qt.NoButton)
    settle(0.4)
    QtWidgets.QApplication.sendEvent(target, QtGui.QContextMenuEvent(
        QtGui.QContextMenuEvent.Mouse, local, target.mapToGlobal(local)))
    settle(0.6)
    return list(menus)


def handle_point(tb):
    opt = QtWidgets.QStyleOptionToolBar()
    opt.initFrom(tb)
    opt.features = QtWidgets.QStyleOptionToolBar.Movable
    if tb.orientation() == QtCore.Qt.Horizontal:
        opt.state |= QtWidgets.QStyle.State_Horizontal
    return tb.style().subElementRect(QtWidgets.QStyle.SE_ToolBarHandle, opt, tb).center()


def drags_docked(name):
    """Whether a docked toolbar floats on a press on its handle and a move;
    docked again where it was afterwards"""
    tb = toolbar(name)
    if tb is None or tb.parentWidget() is not mw() or tb.isFloating():
        return None
    area = mw().toolBarArea(tb)
    p = handle_point(tb)
    send(tb, QtCore.QEvent.MouseButtonPress, p, QtCore.Qt.LeftButton, QtCore.Qt.LeftButton)
    send(tb, QtCore.QEvent.MouseMove, p + QtCore.QPoint(40, 60), QtCore.Qt.NoButton,
         QtCore.Qt.LeftButton)
    settle(0.3)
    floated = tb.isFloating()
    send(tb, QtCore.QEvent.MouseButtonRelease, p + QtCore.QPoint(40, 60), QtCore.Qt.LeftButton,
         QtCore.Qt.NoButton)
    settle(0.3)
    if tb.isFloating():
        mw().addToolBar(area, tb)
        settle(0.3)
    return floated


def docked():
    return sorted(tb.objectName() for tb in mw().findChildren(QtWidgets.QToolBar)
                  if tb.parentWidget() is mw() and tb.isVisible() and tb.objectName()
                  and not tb.isFloating() and tb.isMovable())


def check_docked_drag(when):
    names = docked()
    stuck = [n for n in names if drags_docked(n) is False]
    check("docked toolbars drag %s" % when, names and not stuck,
          "stuck %s of %s" % (stuck, names))


def lock_action(text):
    menu = mw().createPopupMenu()
    for a in menu.actions():
        if a.menu() and "Lock" in a.text():
            for b in a.menu().actions():
                if text in b.text():
                    return menu, b
    return menu, None


def toggle_lock(text):
    for _ in range(2):
        menu, act = lock_action(text)
        if act is None:
            note("INFO no lock action %s" % text)
            return
        act.setChecked(not act.isChecked())
        settle(0.5)


def run():
    try:
        main = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/MainWindow")
        main.SetBool("CustomTitleBar", True)
        settle(1)
        mw().showMaximized()
        settle(1)
        left = FreeCAD.ParamGet("User parameter:BaseApp/MainWindow/MenuBarLeft")
        if not (in_title(toolbar("File")) and in_title(toolbar("Workbench"))):
            left.SetInt("File", 0)
            left.SetInt("Workbench", 1)
            settle(1.5)
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        settle(2)
        old = bool(os.environ.get("GT_EXPECT_OLD"))
        if not check("set up: File and Workbench in the title bar",
                     in_title(toolbar("File")) and in_title(toolbar("Workbench")),
                     [(n, toolbar(n).parentWidget().objectName() if toolbar(n) else None)
                      for n in ("File", "Workbench")]):
            return

        # the workbench tab bar's own menu
        # tabs or a combo box, by the profile; the same menu (WorkbenchGroup::
        # onContextMenuRequested), Show/Hide text only for the tabs
        wb = toolbar("Workbench")
        selectors = [w for w in wb.findChildren(QtWidgets.QTabBar) if w.isVisible()] \
            + [w for w in wb.findChildren(QtWidgets.QComboBox) if w.isVisible()]
        if check("the workbench toolbar has its selector", bool(selectors),
                 [w.metaObject().className() for w in wb.findChildren(QtWidgets.QWidget)]):
            bar = selectors[0]
            got = right_click(bar, bar.rect().center())
            note("INFO workbench selector %s: menus %s" % (bar.metaObject().className(), got))
            first = got[0][0] if got else []
            own = any(t in ("Collapse", "Expand") for t in first)
            if old:
                check("before the fix: the area's list comes first",
                      bool(got) and not own, got)
            else:
                check("a right click on the workbench selector brings its own menu, first",
                      own, got)
                check("and no other menu", len(got) == 1, got)

        # a tool button of File: the window's toolbar menu
        fb = toolbar("File")
        buttons = [b for b in fb.findChildren(QtWidgets.QToolButton) if b.isVisible()
                   and b.defaultAction() is not None]
        if check("File has a tool button", bool(buttons)):
            got = right_click(buttons[0], buttons[0].rect().center())
            note("INFO File's tool button: menus %s" % got)
            window_menu = bool(got) and any("Lock" in s for s in got[0][1])
            if old:
                check("before the fix: not the window's menu", not window_menu, got)
            else:
                check("a right click on File brings the window's toolbar menu",
                      window_menu and len(got) == 1, got)

        # the title bar toolbars' grips
        for name in ("File", "Workbench"):
            tb = toolbar(name)
            g = grips(tb)
            check("%s in the title bar: movable, with its grip" % name,
                  tb.isMovable() and len(g) == 1 and g[0].isVisible() and g[0].width() > 4,
                  (tb.isMovable(), [(x.width(), x.isVisible()) for x in g]))
        check_docked_drag("as set up")

        # File out of the title bar by its grip, docked again
        fb = toolbar("File")
        g = grips(fb)[0]
        c = g.rect().center()
        send(g, QtCore.QEvent.MouseButtonPress, c, QtCore.Qt.LeftButton, QtCore.Qt.LeftButton)
        send(g, QtCore.QEvent.MouseMove, c + QtCore.QPoint(0, 300), QtCore.Qt.NoButton,
             QtCore.Qt.LeftButton)
        settle(0.6)
        fb = toolbar("File")
        out = fb is not None and not in_title(fb)
        send(fb, QtCore.QEvent.MouseButtonRelease, QtCore.QPoint(5, 5), QtCore.Qt.LeftButton,
             QtCore.Qt.NoButton)
        settle(0.5)
        check("a drag by its grip takes File out of the title bar", out,
              fb.parentWidget().metaObject().className() if fb else None)
        if fb is not None and (fb.isFloating() or fb.parentWidget() is mw()):
            mw().addToolBar(QtCore.Qt.TopToolBarArea, fb)
            settle(0.5)
        check("File docked again drags by its handle", drags_docked("File") is True)
        check_docked_drag("after a toolbar has left the title bar")

        FreeCADGui.activateWorkbench("PartWorkbench")
        settle(2)
        check_docked_drag("in another workbench")
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        settle(2)
        check_docked_drag("back in PartDesign")

        toggle_lock("Menu bar")
        check_docked_drag("after the title bar lock on and off")
        toggle_lock("Default")
        check_docked_drag("after the default lock on and off")

        try:
            act = FreeCADGui.Command.get("Std_ToolContext_SheetMetal").getAction()[0]
        except Exception:
            act = None
        if act is not None:
            act.trigger()
            settle(1)
            act.trigger()
            settle(1)
            check_docked_drag("after a Part Design Extensions toggle on and off")
        else:
            note("INFO no Part Design Extensions toggles in this build")
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/ToolContext/Pinned/"
                     "PartDesignWorkbench").RemBool("SheetMetal")
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
