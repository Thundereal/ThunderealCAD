"""A detail of a section is drawn by the recompute that makes it, and
follows the section through the next.

A box, a page, a part view of the box, a section through the box and a
detail of the section, projected by the real hidden line algorithm in
threads. The section cuts in a thread too; the detail, recomputed while
the cut is still running, waits for it, and the section has it recomputed
when the cut lands (DrawViewSection::postSectionCutTasks). That recompute
was skipped -- nothing of the detail is touched, and OptimizeRecompute
skips an object with nothing touched -- so the detail was empty after the
recompute that made it, and one change behind after each one after.

Claims:
  - after the recompute that makes them the detail details the 15 box;
  - after the box's length goes to 20 and is recomputed, the 20 one.
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

    FreeCAD.closeDocument(doc.Name)


def main():
    try:
        run()
    except Exception:
        note("FAIL exception\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, main)
