"""PartDesign's Part Design Extensions tool bar: a toggle per tool context, the design generators, and 3D sketch.

The reporter, 2026-11-26: "suggest a general way to expose PMI, sheetmetal,
surface, 3d sketch, shaft design, involute gear, sprocket features. maybe add
those in one new toolbar for extra funcionality, For PMI, sheetmetal,
surface, etc., clicking will activate relevant toolbars." Built as agreed:
- an always-shown tool bar, a toggle on it for each Gui::ToolContext of
  PartDesign (Sheet Metal, Surface, PMI), the same in Part Design >
  Extensions; a toggle shows its tool bars and hides them again, is checked
  whenever they are offered, makes nothing, and is kept per workbench across
  sessions;
- a Design Generators drop-down (involute gear, sprocket, shaft wizard, each
  if its command is there);
- Sketcher_NewSketch3D beside Create Sketch.

Claims, for each toggle: off and its tool bars hidden to begin with; a
click shows them, checks it, makes no object and stores the pin; another
workbench and back -- they are still shown and it still checked; a click
hides them, unchecks it and takes the pin away. And: the drop-down holds the
generators; 3D sketch is on the Helper tool bar after Create Sketch; the
Extensions submenu holds the toggles.

Runs on the hidden desktop (scripts/gui-test.cmd).
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
PINS = "User parameter:BaseApp/Preferences/ToolContext/Pinned/PartDesignWorkbench"
TOGGLES = (("Std_ToolContext_SheetMetal", "Sheet Metal"),
           ("Std_ToolContext_Surface", "Surface"),
           ("Std_ToolContext_PMI", "PMI"))
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


def toolbar(name):
    for bar in mw().findChildren(QtWidgets.QToolBar):
        if bar.objectName() == name:
            return bar
    return None


def shown(name):
    bar = toolbar(name)
    return bar is not None and bar.isVisible()


def action(cmd):
    try:
        acts = FreeCADGui.Command.get(cmd).getAction()
    except Exception:
        return None
    return acts[0] if acts else None


def bar_commands(name):
    """The commands of a tool bar's buttons, by their actions' data"""
    bar = toolbar(name)
    if bar is None:
        return []
    out = []
    for a in bar.actions():
        out.append(a.data() if a.data() else a.text())
    return out


def activate(wb):
    FreeCADGui.activateWorkbench(wb)
    settle(2)


def run():
    try:
        pins = FreeCAD.ParamGet(PINS)
        for cmd, _ in TOGGLES:
            pins.RemBool(cmd.split("_")[-1])
        doc = FreeCAD.newDocument("PDExtensions")
        activate("PartDesignWorkbench")
        check("the Part Design Extensions tool bar shown", shown("Part Design Extensions"),
              [b.objectName() for b in mw().findChildren(QtWidgets.QToolBar) if b.isVisible()])
        names = [a.text() for a in toolbar("Part Design Extensions").actions()] \
            if toolbar("Part Design Extensions") else []
        note("INFO the tool bar's buttons: %s" % names)
        for cmd, bar in TOGGLES:
            act = action(cmd)
            if not check("%s: its toggle there" % bar, act is not None and act.isCheckable()):
                continue
            check("%s: off to begin with, its tool bar hidden" % bar,
                  not act.isChecked() and not shown(bar), (act.isChecked(), shown(bar)))
            objects = len(doc.Objects)
            act.trigger()
            settle(1)
            check("%s: a click shows its tool bar" % bar, shown(bar))
            check("%s: and checks the toggle" % bar, act.isChecked())
            check("%s: and makes nothing" % bar, len(doc.Objects) == objects, len(doc.Objects))
            check("%s: and keeps the pin" % bar, pins.GetBool(cmd.split("_")[-1], False))
            activate("PartWorkbench")
            check("%s: not in another workbench" % bar, not shown(bar))
            activate("PartDesignWorkbench")
            act = action(cmd)
            check("%s: back in PartDesign, still shown" % bar, shown(bar))
            check("%s: and still checked" % bar, act is not None and act.isChecked())
            act.trigger()
            settle(1)
            check("%s: a second click hides it" % bar, not shown(bar))
            check("%s: unchecks it" % bar, not act.isChecked())
            check("%s: and takes the pin away" % bar, not pins.GetBool(cmd.split("_")[-1], False))

        # the generators
        # a group command's getAction() is its items (CommandPy::getAction)
        try:
            items = [a.text() for a in FreeCADGui.Command.get(
                "PartDesign_DesignGenerators").getAction()]
        except Exception:
            items = []
        if check("the Design Generators drop-down there", bool(items), items):
            check("it holds the involute gear and the sprocket",
                  any("nvolute" in t for t in items) and any("procket" in t for t in items), items)
            bar = toolbar("Part Design Extensions")
            buttons = [b for b in bar.findChildren(QtWidgets.QToolButton)
                       if b.menu() is not None] if bar else []
            menus = [[a.text() for a in b.menu().actions()] for b in buttons]
            check("its button on the tool bar drops down the same items",
                  any(all(t in m for t in items) for m in menus), menus)
        # 3D sketch beside Create Sketch
        helper = [a.text() for a in toolbar("Part Design Helper").actions()] \
            if toolbar("Part Design Helper") else []
        sketch3d = action("Sketcher_NewSketch3D")
        create = action("PartDesign_NewSketch")
        acts = toolbar("Part Design Helper").actions() if toolbar("Part Design Helper") else []
        idx = [i for i, a in enumerate(acts) if sketch3d is not None and a.text() == sketch3d.text()]
        idc = [i for i, a in enumerate(acts) if create is not None and a.text() == create.text()]
        check("3D sketch on the Helper tool bar, after Create Sketch",
              idx and idc and idx[0] == idc[0] + 1, helper)
        # the Extensions submenu
        found = []
        for m in mw().menuBar().actions():
            if m.menu() and "Part Design" in m.text().replace("&", ""):
                for a in m.menu().actions():
                    if a.menu() and a.text().replace("&", "") == "Extensions":
                        found = [x.text() for x in a.menu().actions()]
        check("Part Design > Extensions holds the three toggles",
              all(any(bar in t for t in found) for _, bar in TOGGLES), found)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    pins = FreeCAD.ParamGet(PINS)
    for cmd, _ in TOGGLES:
        pins.RemBool(cmd.split("_")[-1])
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
