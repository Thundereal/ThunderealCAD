"""An undo puts a drawing back as it was, with nothing left to recompute
(docs/TechDrawStoredGeometry.md sec 6).

A box, a page, a part view of the box projected by the real hidden line
algorithm in its thread, a section through the box and a detail of the
section. The box's length goes
from 15 to 20 and is recomputed; then undo and redo.

Claims:
  - after the undo the view draws the 15 box and after the redo the 20 one,
    without a recompute and with nothing marked for one: the projection
    the undo put back in ProjectedGeometry is the one drawn;
  - an undo while the change is still being projected leaves no drawing
    showing the change and saying it is up to date: what was being made is
    dropped, and what the view stored before the change is drawn.
"""
import os
import time
import traceback

import FreeCAD
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = FreeCAD.Vector
FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


FAILED = []


def check(name, cond, detail=""):
    note("%s %s%s" % ("PASS" if cond else "FAIL", name, (" -- " + detail) if detail else ""))
    if not cond:
        FAILED.append(name)


def settle(done=lambda: False, seconds=10.0):
    """Run the event loop until done() or the time is up"""
    start = time.time()
    while time.time() - start < seconds:
        QtCore.QCoreApplication.processEvents()
        if done():
            return True
        time.sleep(0.02)
    return False


def tallest(view):
    """The longest vertical edge the view draws: the box's length, as shown"""
    best = 0.0
    for edge in view.getVisibleEdges():
        a, b = edge.Vertexes[0].Point, edge.Vertexes[-1].Point
        if abs(a.x - b.x) < 1e-6:
            best = max(best, abs(a.y - b.y))
    return round(best, 3)


def longest(view):
    lengths = [edge.Length for edge in view.getVisibleEdges()]
    return round(max(lengths), 3) if lengths else 0.0


def run():
    doc = FreeCAD.newDocument("TdUndoStore")
    box = doc.addObject("Part::Box", "Box")
    box.Length, box.Width, box.Height = 15, 10, 10
    page = doc.addObject("TechDraw::DrawPage", "Page")
    tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    tmpl.Template = os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates",
                                 "A4_Landscape_blank.svg")
    page.Template = tmpl
    view = doc.addObject("TechDraw::DrawViewPart", "View")
    page.addView(view)
    view.Source = [box]
    view.Direction = V(0, -1, 0)
    view.XDirection = V(0, 0, 1)   # the length shows upright
    view.ScaleType = "Custom"
    view.Scale = 1.0
    doc.recompute()
    settle(lambda: tallest(view) == 15)

    section = doc.addObject("TechDraw::DrawViewSection", "Section")
    page.addView(section)
    section.Source = [box]
    section.BaseView = view
    section.SectionNormal = V(0, 0, 1)
    section.Direction = V(0, 0, 1)
    section.SectionOrigin = V(5, 5, 5)
    section.ScaleType = "Custom"
    section.Scale = 1.0
    detail = doc.addObject("TechDraw::DrawViewDetail", "Detail")
    page.addView(detail)
    detail.BaseView = section
    detail.Source = [box]
    detail.AnchorPoint = V(0, 0, 0)
    detail.Radius = 50   # the whole of the cut
    detail.ScaleType = "Custom"
    detail.Scale = 1.0
    doc.recompute()
    settle(lambda: longest(detail) == 15)
    settle(seconds=2.0)   # what is drawn is stored once the faces are found
    doc.UndoMode = 1
    doc.clearUndos()

    def state(tag):
        names = [o.Name for o in doc.Objects if "Touched" in o.State]
        note("  %-8s length %s view %s section %s detail %s touched %s"
             % (tag, box.Length.Value, tallest(view), longest(section), longest(detail),
                names))
        return names

    state("built")
    check("built: the view draws 15 and the detail details 15",
          tallest(view) == 15 and longest(detail) == 15)

    doc.openTransaction("Length")
    box.Length = 20
    doc.commitTransaction()
    doc.recompute()
    settle(lambda: tallest(view) == 20 and longest(detail) == 20)
    settle(seconds=2.0)
    state("edited")
    check("edited: the view draws 20 and the detail details 20",
          tallest(view) == 20 and longest(detail) == 20)

    doc.undo()
    settle(seconds=1.0)
    names = state("undone")
    check("undo: the view draws 15 with nothing to recompute",
          tallest(view) == 15 and "View" not in names, str(names))

    doc.redo()
    settle(seconds=1.0)
    names = state("redone")
    check("redo: the view draws 20 with nothing to recompute",
          tallest(view) == 20 and "View" not in names, str(names))

    doc.undo()
    settle(seconds=1.0)
    names = state("undone")
    check("undo: the section and its detail draw 15 with nothing to recompute",
          longest(section) == 15 and longest(detail) == 15
          and "Section" not in names and "Detail" not in names, str(names))
    settle(seconds=2.0)

    doc.openTransaction("Length 25")
    box.Length = 25
    doc.commitTransaction()
    doc.recompute()
    doc.undo()   # at once, the projections still running
    settle(seconds=3.0)
    names = state("raced")
    shown = {"View": tallest(view), "Section": longest(section), "Detail": longest(detail)}
    stale = [n for n, v in shown.items() if v != 15 and n not in names]
    check("an undo during the projection leaves each drawing right or marked",
          box.Length.Value == 15 and not stale, "stale %s" % stale)

    FreeCAD.closeDocument(doc.Name)


def main():
    try:
        run()
    except Exception:
        note("FAIL exception\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, main)
