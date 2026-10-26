"""New automatic colours stand apart from those in use: PartDesign's tag
colours and the shapes' random colour.

docs/HandsOnQueue.md entry 210, the user 2026-10-21: "Make that auto color
selection reusable. Use it also in PartDesign tag coloring"; "And shape auto
color funciton if it fits". One function (Gui::farthestHue) picks the hue
farthest from the hues taken; entry 209's dress-up groups use it, and so now
do these two.

Claims:
  - a body that grows three solids (each new one's base feature cleared):
    the three tag colours, every two at least 0.1 apart in hue;
  - with RandomColor on, six boxes made one after another: every two of
    their colours at least 0.08 apart in hue (the hue is the one farthest
    from the shapes shown; saturation and value stay muted and random).

Runs with QT_QPA_PLATFORM=offscreen.
"""
import colorsys
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def hue(c):
    return colorsys.rgb_to_hsv(c[0], c[1], c[2])[0]


def gap(a, b):
    d = abs(hue(a) - hue(b))
    return min(d, 1.0 - d)


def closest(colours):
    pairs = [(i, j, round(gap(a, b), 3)) for i, a in enumerate(colours)
             for j, b in enumerate(colours) if i < j]
    return min(pairs, key=lambda p: p[2]) if pairs else None


def run():
    try:
        doc = FreeCAD.newDocument("AutoColorsSpread")
        body = doc.addObject("PartDesign::Body", "Body")
        boxes = []
        for k in range(3):
            box = body.newObject("PartDesign::AdditiveBox", "Box%d" % k)
            box.Placement.Base = FreeCAD.Vector(30 * k, 0, 0)
            if k:
                # a solid of its own
                box.BaseFeature = None
            doc.recompute()
            pump(10)
            boxes.append(box)
        tags = []
        for box in boxes:
            c = box.ViewObject.IconColor
            tags.append((c[0], c[1], c[2]))
        note("INFO tag colours %s" % (tags,))
        check("three solids, three tag colours", len(set(tags)) == 3 and all(any(t) for t in tags),
              tags)
        worst = closest(tags)
        check("every two tag colours at least 0.1 apart in hue", worst and worst[2] >= 0.1, worst)

        VIEW.SetBool("RandomColor", True)
        shapes = []
        for k in range(6):
            box = doc.addObject("Part::Box", "Shape%d" % k)
            box.Placement.Base = FreeCAD.Vector(30 * k, 50, 0)
            doc.recompute()
            pump(5)
            c = box.ViewObject.ShapeColor
            shapes.append((c[0], c[1], c[2]))
        VIEW.SetBool("RandomColor", False)
        note("INFO shape colours %s" % (shapes,))
        worst = closest(shapes)
        check("RandomColor on: every two shapes made in a row at least 0.08 apart in hue",
              worst and worst[2] >= 0.08, worst)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        VIEW.SetBool("RandomColor", False)
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
