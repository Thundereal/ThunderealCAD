"""A projection group with Auto Distribution lays its views out, and follows its spacing.

docs/HandsOnQueue.md entry 216: "X/Y spacing doesn't apply with Auto
Distribution on until it is toggled". Measured on r213: not even a new
group was laid out -- every view at (0, 0) until the toggle. The group asks
for its own recompute once its views have projected (reportReady()), and
that recompute of an untouched object ran nothing here (OptimizeRecompute).

A 20 x 10 x 5 box, a page, a projection group of Front, Right and Top at
scale 1, Auto Distribution on. The views project in threads, so this runs
with the GUI's event loop. Claims:
  - laid out without a toggle: Right beside Front, Top above or below it;
  - X spacing 15 -> 40: Right 25 mm farther from Front, Top where it was;
  - Y spacing 15 -> 30: Top 15 mm farther, Right where it was;
  - the group is not left touched.

Runs offscreen as well.
"""
import glob
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))

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


def value(q):
    return q.Value if hasattr(q, "Value") else q


def positions(grp):
    return {v.Type: (round(value(v.X), 2), round(value(v.Y), 2)) for v in grp.Views}


def wait_change(grp, old, seconds=15):
    """Until the positions differ from `old` and stay put a moment"""
    end = time.time() + seconds
    last = None
    while time.time() < end:
        settle(0.5)
        now = positions(grp)
        if now != old and now == last:
            return now
        last = now
    return positions(grp)


def run():
    try:
        doc = FreeCAD.newDocument("ProjGroupAutoDistribute")
        box = doc.addObject("Part::Box", "Box")
        box.Length, box.Width, box.Height = 20, 10, 5
        doc.recompute()
        page = doc.addObject("TechDraw::DrawPage", "Page")
        tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                           "Templates", "**", "*A4*Landscape*.svg"), recursive=True)
        if templates:
            tmpl.Template = templates[0]
        page.Template = tmpl
        grp = doc.addObject("TechDraw::DrawProjGroup", "ProjGroup")
        page.addView(grp)
        grp.Source = [box]
        grp.addProjection("Front")
        grp.addProjection("Right")
        grp.addProjection("Top")
        grp.ScaleType = "Custom"
        grp.Scale = 1.0
        grp.AutoDistribute = True
        zero = {"Front": (0.0, 0.0), "Right": (0.0, 0.0), "Top": (0.0, 0.0)}
        doc.recompute()
        laid = wait_change(grp, zero)
        note("INFO spacing %s, %s; laid out %s" % (grp.spacingX, grp.spacingY, laid))
        check("a new group is laid out without a toggle",
              laid["Right"][0] != 0.0 and laid["Top"][1] != 0.0, laid)

        grp.spacingX = 40
        doc.recompute()
        wider = wait_change(grp, laid)
        note("INFO X spacing 40: %s" % wider)
        check("X spacing 15 -> 40: Right 25 mm farther from Front, Top where it was",
              abs(abs(wider["Right"][0]) - abs(laid["Right"][0]) - 25.0) < 0.01
              and wider["Top"] == laid["Top"], (laid, wider))

        grp.spacingY = 30
        doc.recompute()
        taller = wait_change(grp, wider)
        note("INFO Y spacing 30: %s" % taller)
        check("Y spacing 15 -> 30: Top 15 mm farther, Right where it was",
              abs(abs(taller["Top"][1]) - abs(wider["Top"][1]) - 15.0) < 0.01
              and taller["Right"] == wider["Right"], (wider, taller))
        check("the group is not left touched", "Touched" not in grp.State, grp.State)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
