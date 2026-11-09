"""An undo puts a drawing back as it was, with nothing left to recompute
(docs/TechDrawStoredGeometry.md sec 6).

A box, a page, a part view of the box projected by the real hidden line
algorithm in its thread, a dimension of the box's length on the view, a
section through the box and a detail of the section. The box's length goes
from 15 to 20 and is recomputed; then undo and redo.

Claims:
  - after the undo the view draws the 15 box and after the redo the 20 one,
    without a recompute and with nothing marked for one: the projection
    the undo put back in ProjectedGeometry is the one drawn;
  - the dimension reads 15 and 20 with them, measured when read;
  - the detail of the section, recomputed alone after the undo, details the
    15 box: the section's cut, which is not stored, is made again rather
    than the cut of the 20 box it held being used;
  - an undo while the change is still being projected leaves no drawing
    showing the change and saying it is up to date: what was being made is
    dropped, and what the view stored before the change is drawn;
  - an undo past the steps kept in memory, taken from the log, draws what
    it put back too; and when the log's cache has let the projection go,
    the views are marked to recompute rather than shown as up to date;
  - the section's cut and the detail's piece are stored with them: a
    detail recomputed alone, after an undo or a reopen, cuts what is there
    and does not recompute the section.
"""
import os
import tempfile
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


class Recomputed:
    """The names of the objects recomputed while it watches"""

    def __init__(self):
        self.names = []
        FreeCAD.addDocumentObserver(self)

    def slotRecomputedObject(self, obj):
        self.names.append(obj.Name)

    def stop(self):
        FreeCAD.removeDocumentObserver(self)
        return self.names


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

    edges = view.getVisibleEdges()
    upright = max(range(len(edges)), key=lambda i: abs(edges[i].Vertexes[0].Point.y
                                                       - edges[i].Vertexes[-1].Point.y))
    dim = doc.addObject("TechDraw::DrawViewDimension", "Dimension")
    dim.Type = "Distance"
    dim.References2D = [(view, "Edge%d" % upright)]
    page.addView(dim)

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
        note("  %-8s length %s view %s dimension %.3f section %s detail %s touched %s"
             % (tag, box.Length.Value, tallest(view), dim.getRawValue(), longest(section),
                longest(detail), names))
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
    check("undo: the dimension reads 15", abs(dim.getRawValue() - 15) < 1e-6,
          "%.3f" % dim.getRawValue())

    doc.redo()
    settle(seconds=1.0)
    names = state("redone")
    check("redo: the view draws 20 with nothing to recompute",
          tallest(view) == 20 and "View" not in names, str(names))
    check("redo: the dimension reads 20", abs(dim.getRawValue() - 20) < 1e-6,
          "%.3f" % dim.getRawValue())

    doc.undo()
    settle(seconds=1.0)
    names = state("undone")
    check("undo: the section and its detail draw 15 with nothing to recompute",
          longest(section) == 15 and longest(detail) == 15
          and "Section" not in names and "Detail" not in names, str(names))
    detail.Radius = 60
    watch = Recomputed()
    doc.recompute()
    settle(seconds=3.0)
    ran = watch.stop()
    state("detail")
    check("the detail recomputed alone details the 15 box, its section not recomputed",
          longest(detail) == 15 and "Section" not in ran, "%s %s" % (longest(detail), ran))
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
    doc.recompute()
    settle(lambda: tallest(view) == 15 and longest(detail) == 15)
    settle(seconds=2.0)

    def push_cold():
        # past the 20 steps kept in memory: the next undo reads the log
        doc.openTransaction("filler")
        filler = doc.addObject("App::FeatureTest", "Filler")
        doc.commitTransaction()
        for i in range(21):
            doc.openTransaction("filler %d" % i)
            filler.Integer = i + 1
            doc.commitTransaction()
        for i in range(22):
            doc.undo()

    # an undo from the log puts the projection back, and the view draws it
    # (TransactionLogJournal.md sec 24.14) ...
    doc.openTransaction("Length 30")
    box.Length = 30
    doc.commitTransaction()
    doc.recompute()
    settle(lambda: tallest(view) == 30)
    settle(seconds=2.0)
    push_cold()
    doc.undo()
    settle(seconds=1.0)
    names = state("cold")
    check("an undo from the log draws 15 with nothing to recompute",
          box.Length.Value == 15 and tallest(view) == 15 and "View" not in names, str(names))

    # ... and once the cache has let it go -- it is derived (sec 10.2) --
    # the view is marked to recompute, through a run of steps (sec 24.15)
    doc.openTransaction("Length 35")
    box.Length = 35
    doc.commitTransaction()
    doc.recompute()
    settle(lambda: tallest(view) == 35)
    settle(seconds=2.0)
    push_cold()
    doc.resolveTransactionLog()
    evicted = doc.evictTransactionCache(size=1, values=0)
    doc.undo()
    settle(seconds=1.0)
    names = state("evicted")
    check("an undo from the log after the cache let go draws 15 or is marked",
          box.Length.Value == 15 and (tallest(view) == 15 or "View" in names),
          "%s %s" % (evicted, names))

    # what the views made from a view read is stored with it
    # (TechDrawStoredGeometry.md sec 8): a reopened document has it
    doc.recompute()
    settle(lambda: tallest(view) == 15 and longest(detail) == 15)
    settle(seconds=2.0)
    path = os.path.join(tempfile.mkdtemp(prefix="td-undo-"), "stored.FCStd")
    doc.saveAs(path)
    FreeCAD.closeDocument(doc.Name)
    settle(seconds=0.5)
    doc = FreeCAD.openDocument(path)
    view, section, detail = doc.View, doc.Section, doc.Detail
    settle(seconds=1.0)
    names = [o.Name for o in doc.Objects if "Touched" in o.State]
    check("reopened: the section's cut and the detail's piece are back, nothing to recompute",
          not section.CutShape.isNull() and not section.PreparedShape.isNull()
          and not detail.DetailShape.isNull() and not detail.ScaledShape.isNull() and not names,
          str(names))
    detail.Radius = 55
    watch = Recomputed()
    doc.recompute()
    settle(lambda: longest(detail) == 15, 10)
    ran = watch.stop()
    check("reopened: the detail recomputed alone details the 15 box, its section not recomputed",
          longest(detail) == 15 and "Section" not in ran, "%s %s" % (longest(detail), ran))

    FreeCAD.closeDocument(doc.Name)


def main():
    try:
        run()
    except Exception:
        note("FAIL exception\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, main)
