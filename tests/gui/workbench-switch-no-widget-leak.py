"""Switching workbench leaves no widgets behind, nor does a sketch's edit.

Every workbench switch clears the menu bar and builds its menus anew
(MenuManager::setup). The clear took the menus' actions out but left the
menus themselves, the menu bar's children, hidden and unreachable, with all
their submenus: 356 widgets for each switch into Sketcher and back, and so
for each sketch edit, which switches there and back. After a few hundred
edits that is a hundred thousand widgets (docs/SmokeTesting.md, step 1).

Counted with the main loop running between the steps, so the deferred
deletes have run: QApplication.allWidgets() after the second cycle and after
the fifth, for a switch to Sketcher and back and for a sketch's edit; the
growth over those three cycles is under 30 each. Before the change: 1065.
And every menu the menu bar owns is one it shows.

What is deleted is only what the switch made: an addon's menu put on the bar
with menuBar().addMenu() outlives the switch and can be put back, and a
switch started while one of the bar's menus is open crashes nothing. Run on
the test profile (the native bar) and on a clone of the reporter's (the
custom title bar, where the kit's guard carries an addon's menu over).
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
CYCLES = 5
LIMIT = 30
state = {"done": False, "phase": 0, "cycle": 0, "counts": [], "doc": None, "sk": None,
         "wb": None, "legs": set()}
PHASES = ("a switch to Sketcher and back", "a sketch's edit")
# Every leg, in order. One that an exception keeps from finishing -- its own, or
# one before it, which ends the script -- is a FAIL by name in finish(), not
# lost behind the one ABORT line: on the Mac mini the last leg had never run.
LEGS = PHASES + ("an addon's menu on the bar", "a switch with a menu of the bar open")


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def ptr(obj):
    import shiboken6

    return shiboken6.getCppPointer(obj)[0]


def widgets():
    return len(QtWidgets.QApplication.allWidgets())


def menus_owned_and_shown():
    bar = FreeCADGui.getMainWindow().menuBar()
    # Titled ones: every menu the workbenches put on the bar has one. One untitled
    # menu the bar keeps of its own is there from the start and never grows.
    owned = [m for m in bar.children() if isinstance(m, QtWidgets.QMenu) and m.title()]
    shown = {ptr(a.menu()) for a in bar.actions() if a.menu() is not None}
    hidden = [m.objectName() for m in owned if ptr(m) not in shown]
    return len(owned), hidden


def enter():
    try:
        import Part
        import SketcherGui  # noqa: F401  -- registers the sketch's edit

        if state["doc"] is None:
            state["doc"] = FreeCAD.newDocument("WorkbenchSwitchNoLeak")
            state["wb"] = FreeCADGui.activeWorkbench().name()
            if state["wb"] == "SketcherWorkbench":
                FreeCADGui.activateWorkbench("PartWorkbench")
                state["wb"] = "PartWorkbench"
            sk = state["doc"].addObject("Sketcher::SketchObject", "Sketch")
            sk.addGeometry(Part.LineSegment(FreeCAD.Vector(2, 3, 0), FreeCAD.Vector(8, 7, 0)),
                           False)
            state["doc"].recompute()
            state["sk"] = sk
        if state["phase"] == 0:
            FreeCADGui.activateWorkbench("SketcherWorkbench")
        else:
            FreeCADGui.ActiveDocument.setEdit(state["sk"], 0)
        QtCore.QTimer.singleShot(300, leave)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
        finish()


def leave():
    try:
        if state["phase"] == 0:
            FreeCADGui.activateWorkbench(state["wb"])
        else:
            FreeCADGui.ActiveDocument.resetEdit()
        # back to the main loop, where the deferred deletes run
        QtCore.QTimer.singleShot(700, counted)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
        finish()


def counted():
    try:
        state["cycle"] += 1
        state["counts"].append(widgets())
        if state["cycle"] < CYCLES:
            QtCore.QTimer.singleShot(200, enter)
            return
        counts = state["counts"]
        growth = counts[-1] - counts[1]
        check("%s, cycles 2 to %d: fewer than %d widgets more" % (PHASES[state["phase"]], CYCLES,
                                                                  LIMIT),
              growth < LIMIT, "%d more; after each cycle %s" % (growth, counts))
        owned, hidden = menus_owned_and_shown()
        check("and every menu the menu bar owns is one it shows", not hidden,
              "%d owned, %d not shown: %s" % (owned, len(hidden), hidden[:8]))
        state["legs"].add(PHASES[state["phase"]])
        state["phase"] += 1
        state["cycle"] = 0
        state["counts"] = []
        if state["phase"] < len(PHASES):
            QtCore.QTimer.singleShot(200, enter)
            return
        QtCore.QTimer.singleShot(200, addon_menu)
        return
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


# An addon's own menu, put on the bar with menuBar().addMenu(): the switch takes
# it out, as it always did, but must not delete it -- the addon may hold it and
# put it back at its next activation. In the custom title bar the kit's guard
# carries such a menu over to the application's bar first.
def addon_menu():
    try:
        bar = FreeCADGui.getMainWindow().menuBar()
        menu = bar.addMenu("GtAddonMenu")
        menu.addAction("GtAddonAction")
        state["addon"] = menu
        FreeCADGui.activateWorkbench("SketcherWorkbench")
        QtCore.QTimer.singleShot(300, addon_menu_back)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
        finish()


def addon_menu_back():
    try:
        FreeCADGui.activateWorkbench(state["wb"])
        QtCore.QTimer.singleShot(700, addon_menu_check)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
        finish()


def addon_menu_check():
    import shiboken6

    try:
        menu = state["addon"]
        alive = shiboken6.isValid(menu)
        check("an addon's menu on the bar outlives a switch", alive)
        if alive:
            bar = FreeCADGui.getMainWindow().menuBar()
            bar.addAction(menu.menuAction())
            back = [a for a in bar.actions() if a.menu() is not None and ptr(a.menu()) == ptr(menu)]
            check("and the addon can put it back", len(back) == 1,
                  [a.text() for a in bar.actions()])
            bar.removeAction(menu.menuAction())
            menu.deleteLater()
        state["addon"] = None
        state["legs"].add(LEGS[2])
        QtCore.QTimer.singleShot(300, from_open_menu)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
        finish()


# A switch started while one of the bar's menus is open -- as from the View
# menu's workbench list: that menu is one the switch replaces, and is deleted
# later, once it has closed, not under the click.
def from_open_menu():
    try:
        bar = FreeCADGui.getMainWindow().menuBar()
        # Each menu's name read as it is fetched: on the Mac mini's PySide 6.11.1
        # a wrapper QAction.menu() gave reads as deleted once menu() is called
        # again, the menu itself alive on the bar. PySide 6.11.2 (Windows,
        # 2026-11-28) does not: the old script ran 8/0 there, so it is 6.11.1's
        named = [(a, a.menu().objectName()) for a in bar.actions()
                 if a.menu() is not None and a.isVisible()]
        action, name = ([(a, n) for a, n in named if n == "&View"] or named)[0]
        action.menu().popup(bar.mapToGlobal(QtCore.QPoint(10, bar.height())))
        QtCore.QTimer.singleShot(300, lambda: from_open_menu_switch(name))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
        finish()


def from_open_menu_switch(name):
    try:
        popup = QtWidgets.QApplication.activePopupWidget()
        state["popup"] = popup is not None
        FreeCADGui.activateWorkbench("SketcherWorkbench")
        FreeCADGui.activateWorkbench(state["wb"])
        QtCore.QTimer.singleShot(200, from_open_menu_close)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
        finish()


def from_open_menu_close():
    try:
        popup = QtWidgets.QApplication.activePopupWidget()
        while popup is not None:
            popup.close()
            popup = QtWidgets.QApplication.activePopupWidget()
        QtCore.QTimer.singleShot(700, from_open_menu_check)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
        finish()


def from_open_menu_check():
    try:
        check("a switch with a menu of the bar open: the menu was open", state["popup"])
        owned, hidden = menus_owned_and_shown()
        check("and after it every menu the menu bar owns is one it shows", not hidden,
              "%d owned, %d not shown: %s" % (owned, len(hidden), hidden[:8]))
        mw = FreeCADGui.getMainWindow()
        slot = QtWidgets.QMainWindow.menuWidget(mw)
        note("INFO the window's menu slot holds %s: %s" % (
            slot.metaObject().className() if slot is not None else None,
            "the native bar" if slot is not None and ptr(slot) == ptr(mw.menuBar())
            else "not the bar -- the custom title bar's spacer"))
        state["legs"].add(LEGS[3])
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for leg in LEGS:
        if leg not in state["legs"]:
            note("FAIL %s: not run to its end -- an exception in it or before it" % leg)
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, enter)
