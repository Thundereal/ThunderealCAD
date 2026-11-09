"""A geometric hatch is trimmed once, not at each redraw
(docs/TechDrawStoredGeometry.md sec 7).

A plate with holes, a top view of it and a PAT hatch on the face with the
holes. Trimming the pattern to that face is a boolean that takes seconds on
a face with many holes, and drawing ran it on every redraw.

Claims:
  - the recompute keeps the trimmed lines (TrimmedLines), and a redraw
    reads them: far faster than one that trims;
  - a hatch changed and not yet recomputed is not drawn from lines trimmed
    for its old pattern: it trims, as before;
  - undo and redo put back the lines of the state they leave, with nothing
    to recompute;
  - the lines are saved, and a reopened document draws the hatch without
    trimming.
"""
import os
import tempfile
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
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
    start = time.time()
    while time.time() - start < seconds:
        QtCore.QCoreApplication.processEvents()
        if done():
            return True
        time.sleep(0.02)
    return False


def redraw(view):
    """Seconds one redraw of the view takes: it draws as it is asked"""
    start = time.time()
    view.requestPaint()
    return time.time() - start


def touched(doc):
    return [o.Name for o in doc.Objects if "Touched" in o.State]


def build(doc):
    plate = doc.addObject("Part::Box", "Plate")
    plate.Length, plate.Width, plate.Height = 120, 80, 10
    holes = doc.addObject("Part::Feature", "Holes")
    holes.Shape = Part.makeCompound([Part.makeCylinder(3, 30, V(12 + i * 16, 12 + j * 16, -10))
                                     for i in range(7) for j in range(4)])
    cut = doc.addObject("Part::Cut", "Cut")
    cut.Base, cut.Tool = plate, holes
    page = doc.addObject("TechDraw::DrawPage", "Page")
    tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    tmpl.Template = os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates",
                                 "A4_Landscape_blank.svg")
    page.Template = tmpl
    view = doc.addObject("TechDraw::DrawViewPart", "View")
    page.addView(view)
    view.Source = [cut]
    view.Direction = V(0, 0, 1)
    view.ScaleType = "Custom"
    view.Scale = 1.0
    page.ViewObject.doubleClicked()
    doc.recompute()
    settle(lambda: view.getVisibleEdges() and "Touched" not in view.State, 60)
    settle(seconds=1.0)
    hatch = doc.addObject("TechDraw::DrawGeomHatch", "Hatch")
    hatch.Source = (view, ["Face0"])
    hatch.ScalePattern = 1.0
    doc.recompute()
    settle(seconds=2.0)
    return page, view, hatch


def run():
    doc = FreeCAD.newDocument("TdHatch")
    page, view, hatch = build(doc)
    stored = hatch.TrimmedLines
    note("  built: trimmed lines %s" % (stored,))
    check("the recompute keeps the trimmed lines", stored[0] == 1 and stored[1] > 0, str(stored))

    fast = min(redraw(view) for _ in range(3))
    hatch.ScalePattern = 1.5   # not recomputed: the stored lines are not its
    slow = redraw(view)
    note("  redraw: %.3f s from the stored lines, %.3f s trimming" % (fast, slow))
    check("a redraw from the stored lines is far faster than one that trims",
          fast * 5 < slow, "%.3f vs %.3f" % (fast, slow))
    check("a hatch changed and not recomputed trims rather than use the old lines",
          slow > 0.05, "%.3f" % slow)
    hatch.ScalePattern = 1.0
    doc.recompute()
    settle(seconds=1.0)

    # a section's cut faces hatched with a PAT pattern: the same
    section = doc.addObject("TechDraw::DrawViewSection", "Section")
    page.addView(section)
    section.BaseView = view
    section.Source = view.Source
    section.SectionNormal = V(0, -1, 0)
    section.Direction = V(0, -1, 0)
    section.SectionOrigin = V(60, 36, 5)
    section.ScaleType = "Custom"
    section.Scale = 1.0
    section.CutSurfaceDisplay = "PatHatch"
    doc.recompute()
    settle(lambda: section.getVisibleEdges() and "Touched" not in section.State, 60)
    settle(seconds=2.0)
    cut = section.CutHatchLines
    fast = min(redraw(section) for _ in range(3))
    section.HatchScale = section.HatchScale * 0.5   # not recomputed
    slow = redraw(section)
    note("  section: cut hatch lines %s, redraw %.3f s stored, %.3f s trimming" % (cut, fast, slow))
    check("a section keeps its cut faces' trimmed lines and draws from them",
          cut[0] > 0 and cut[1] > 0 and fast < slow, "%s %.3f %.3f" % (cut, fast, slow))
    section.HatchScale = section.HatchScale * 2.0
    doc.recompute()
    settle(lambda: "Touched" not in section.State, 60)
    settle(seconds=2.0)

    doc.UndoMode = 1
    doc.clearUndos()
    before = hatch.TrimmedLines
    doc.openTransaction("Scale 2")
    hatch.ScalePattern = 2.0
    doc.commitTransaction()
    doc.recompute()
    settle(seconds=1.0)
    after = hatch.TrimmedLines
    note("  scale 1: %s, scale 2: %s" % (before, after))
    check("the lines follow the pattern's scale", after[1] < before[1], "%s %s" % (before, after))
    doc.undo()
    settle(seconds=0.5)
    names = touched(doc)
    check("undo puts back the lines of scale 1 with nothing to recompute",
          hatch.TrimmedLines == before and not names, "%s %s" % (hatch.TrimmedLines, names))
    check("and draws from them", redraw(view) * 5 < slow, "")
    doc.redo()
    settle(seconds=0.5)
    names = touched(doc)
    check("redo puts back the lines of scale 2 with nothing to recompute",
          hatch.TrimmedLines == after and not names, "%s %s" % (hatch.TrimmedLines, names))

    path = os.path.join(tempfile.mkdtemp(prefix="td-hatch-"), "hatch.FCStd")
    doc.saveAs(path)
    FreeCAD.closeDocument(doc.Name)
    settle(seconds=0.5)
    doc = FreeCAD.openDocument(path)
    view, hatch = doc.getObject("View"), doc.getObject("Hatch")
    doc.getObject("Page").ViewObject.doubleClicked()
    settle(seconds=1.0)
    names = touched(doc)
    t = redraw(view)
    note("  reopened: lines %s, redraw %.3f s, touched %s" % (hatch.TrimmedLines, t, names))
    check("the lines are saved and the reopened hatch draws from them",
          hatch.TrimmedLines == after and t * 5 < slow and not names,
          "%s %.3f %s" % (hatch.TrimmedLines, t, names))
    FreeCAD.closeDocument(doc.Name)


def main():
    try:
        run()
    except Exception:
        note("FAIL exception\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, main)
