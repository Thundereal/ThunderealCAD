"""Gui.getMainWindow().menuBar() is the menu bar the user sees, in the custom
title bar too.

CustomTitleBarWindow::menuBar() hides QMainWindow's without being virtual,
and PySide binds the base class: from Python, getMainWindow().menuBar() was
Qt's, which in the custom title bar -- the menu-widget slot holding the kit's
spacer, the menus in the title bar -- made an empty, hidden QMenuBar of its
own and returned that. Under a clone of the reporter's profile (custom title
bar on) SheetMetalCommands, SheetMetalConvert and PMIWorkbench found no Part
Design menu through it (docs/SmokeTesting.md, step 1); any macro or addon
asking for the menu bar was in the same place.

Measured here, with the custom title bar switched on: menuBar() is visible,
holds the menus (File among them), is the one bar of the window that has
any, and asking for it makes no new QMenuBar; the same with the platform's
title bar again. Before the fix, the first three fail under the custom one.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
PARAMS = "User parameter:BaseApp/Preferences/MainWindow"
state = {"done": False, "was": None}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


def title_bar():
    for w in QtWidgets.QApplication.allWidgets():
        if w.metaObject().className() == "TitleBarWidget" and w.isVisible():
            return w
    return None


def bars():
    """The window's QMenuBars that hold menus (findChildren makes none)"""
    mw = FreeCADGui.getMainWindow()
    return [b for b in mw.findChildren(QtWidgets.QMenuBar) if b.actions()]


def judge(where):
    mw = FreeCADGui.getMainWindow()
    before = len(mw.findChildren(QtWidgets.QMenuBar))
    bar = mw.menuBar()
    titles = [a.text().replace("&", "") for a in bar.actions()]
    check(where + ": menuBar() is visible", bar.isVisible())
    check(where + ": and holds the menus", "File" in titles, titles)
    full = bars()
    check(where + ": it is the window's menu bar", len(full) == 1 and full[0] is bar,
          [b.objectName() for b in full])
    settle(0.2)
    after = len(mw.findChildren(QtWidgets.QMenuBar))
    check(where + ": asking for it made no new bar", after == before, (before, after))


def run():
    params = FreeCAD.ParamGet(PARAMS)
    state["was"] = params.GetBool("CustomTitleBar", False)
    try:
        params.SetBool("CustomTitleBar", True)
        settle(1.0)
        if title_bar() is None:
            note("NOT RUN: no custom title bar on this platform")
        else:
            judge("custom title bar")
        params.SetBool("CustomTitleBar", False)
        settle(1.0)
        judge("the platform's title bar")
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    if state["was"] is not None:
        FreeCAD.ParamGet(PARAMS).SetBool("CustomTitleBar", state["was"])
        settle(1.0)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
