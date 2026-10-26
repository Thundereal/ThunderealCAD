"""The 3D sketch's icons (docs/Sketch3D.md section 7).

The Sketcher's tools serve both sketches; while a 3D sketch is in edit they are
drawn in its teal instead of the 2D red (SketcherGui::setSketch3DToolIcons, on
BitmapFactory::setIconColorMapping). Claims, an icon read as its count of red
and of teal pixels:

  - the 3D sketch shows its own icon, teal, and its commands have theirs;
  - before the edit the Sketcher's tools are red;
  - in a 3D sketch's edit they are teal: a plain command, a group of them and
    a dimension; the commands that make a sketch stay red; its own commands
    keep the manipulator's axis colours;
  - construction mode switches a tool to its construction icon, teal too;
  - leaving the edit brings the red back, and a 2D sketch's edit is red.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "Sketch3DIcons"
V = FreeCAD.Vector
state = {"done": False}

TOOLS = ["Sketcher_CreateLine", "Sketcher_CompLine", "Sketcher_ConstrainDistance"]


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds=0.3):
    import time

    end = time.monotonic() + seconds
    while True:
        QtCore.QCoreApplication.processEvents()
        if time.monotonic() >= end:
            break
        time.sleep(0.01)


def colours(icon):
    """(red, teal) pixel counts of an icon drawn at 32 px; None for no icon."""
    if icon is None or icon.isNull():
        return None
    image = icon.pixmap(32, 32).toImage()
    red = teal = 0
    for y in range(image.height()):
        for x in range(image.width()):
            c = image.pixelColor(x, y)
            if c.alpha() < 128:
                continue
            if c.red() > 150 and c.green() < 90 and c.blue() < 90:
                red += 1
            elif c.red() < 80 and c.green() > 120 and c.blue() > 120:
                teal += 1
    return red, teal


def command_icon(name):
    cmd = FreeCADGui.Command.get(name)
    actions = cmd.getAction() if cmd else None
    return actions[0].icon() if actions else None


def tools():
    return {name: colours(command_icon(name)) for name in TOOLS}


def all_red(found):
    return all(c and c[0] > 0 and c[1] == 0 for c in found.values())


def all_teal(found):
    return all(c and c[0] == 0 and c[1] > 0 for c in found.values())


def run():
    try:
        import Part

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SketcherWorkbench")
        settle(0.5)
        doc = FreeCAD.newDocument(DOC)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(10, 0, 5)))
        sk2d = doc.addObject("Sketcher::SketchObject", "Sketch")
        sk2d.addGeometry(Part.LineSegment(V(0, 0, 0), V(10, 0, 0)))
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        settle(0.3)

        own = colours(sk.ViewObject.Icon)
        check("the 3D sketch's own icon is teal", own and own[0] == 0 and own[1] > 0, own)
        made = {
            n: colours(command_icon(n))
            for n in (
                "Sketcher_NewSketch3D",
                "Sketcher_Constrain3DAlongX",
                "Sketcher_ConstrainDistanceZ",
                "Sketcher_Sketch3DCyclePlane",
                "Sketcher_Sketch3DRotationHandles",
            )
        }
        # the rotation rings are in the axis colours, the rest teal
        check("its commands have their icons", all(c and sum(c) > 0 for c in made.values()), made)

        before = tools()
        check("before the edit the tools are red", all_red(before), before)

        gdoc.setEdit(sk)
        settle(1.0)
        inside = tools()
        check("in a 3D sketch's edit they are teal", all_teal(inside), inside)
        # already teal, they are not recoloured: the X axis keeps its red
        axes = {
            n: colours(command_icon(n))
            for n in ("Sketcher_Sketch3DRotationHandles", "Sketcher_Constrain3DAlongX")
        }
        check("and keep their axis colours", all(c and c[0] > 0 for c in axes.values()), axes)
        new = colours(command_icon("Sketcher_NewSketch"))
        check("New Sketch stays red", new and new[0] > 0 and new[1] == 0, new)

        normal = command_icon("Sketcher_CreateLine").pixmap(32, 32).toImage()
        FreeCADGui.Command.get("Sketcher_ToggleConstruction").run()
        settle(0.3)
        icon = command_icon("Sketcher_CreateLine")
        line = colours(icon)
        check(
            "construction mode shows the construction icon",
            icon.pixmap(32, 32).toImage() != normal,
        )
        check("teal too", line and line[0] == 0 and line[1] > 0, line)
        FreeCADGui.Command.get("Sketcher_ToggleConstruction").run()
        settle(0.3)

        gdoc.resetEdit()
        settle(0.8)
        after = tools()
        check("leaving the edit brings the red back", all_red(after), after)

        gdoc.setEdit(sk2d)
        settle(1.0)
        flat = tools()
        check("a 2D sketch's edit is red", all_red(flat), flat)
        gdoc.resetEdit()
        settle(0.5)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    gdoc = FreeCADGui.getDocument(DOC) if FreeCAD.listDocuments().get(DOC) else None
    if gdoc and gdoc.getInEdit():
        gdoc.resetEdit()
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
