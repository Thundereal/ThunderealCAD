"""A TechDraw page from PMI views (docs/GDandT.md sec 9 item 6, P4), driven
as a user drives it.

The PMI test part (PMITests.TestDrawing) with three PMI views: the plan
showing the flatness, the position and the hole's size; the elevation showing
datum A; the top again clipped at half height, showing the position and the
hole's size. Selected, TechDraw_PageFromPMIViews makes the page.

Claims:
  - the command makes a page with a view per PMI view, named after it, and
    for the clipped one a section on a base view;
  - once the views are projected -- in the GUI on their threads -- each gets
    what its PMI view shows, and nothing else: the frames, the datum, the
    linked dimension; the section has the hole's, not the top's flatness;
  - the page draws them (vg renderer).
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
STEPS = []
STATE = {}
V = FreeCAD.Vector


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name
         + (" | " + str(detail) if detail else ""))
    return cond


def step(delay_ms):
    def deco(fn):
        STEPS.append((delay_ms, fn))
        return fn
    return deco


def on(view):
    """What is drawn on \\a view from the model: its callouts' and its
    dimensions' 3D objects"""
    doc = STATE["doc"]
    calls = sorted(c.Source.Name for c in doc.Objects
                   if c.isDerivedFrom("TechDraw::DrawPMICallout") and c.SourceView == view
                   and c.Source)
    dims = sorted(d.Source.Name for d in doc.Objects
                  if d.isDerivedFrom("TechDraw::DrawViewDimension") and d.Source
                  and d.References2D and d.References2D[0][0] == view)
    return calls, dims


@step(1000)
def make():
    import TechDrawGui  # noqa: F401 -- the view providers
    from PMITests.TestDrawing import TestImportModelAnnotations

    FreeCADGui.activateWorkbench("TechDrawWorkbench")
    t = TestImportModelAnnotations("test_what_faces_the_view_and_shows")
    t.setUp()
    STATE["test"] = t
    doc = STATE["doc"] = t.doc
    pset = doc.getObject("PMI")

    def pmi_view(name, standard):
        v = doc.addObject("PMI::View", name)
        pset.addObject(v)
        v.Standard = standard
        return v

    top, front, cut = pmi_view("Plan", "Top"), pmi_view("Elevation", "Front"), \
        pmi_view("Cut", "Top")
    cut.Clip = True
    cut.ClipPlane = FreeCAD.Placement(V(20, 10, 5), FreeCAD.Rotation())
    for name in ("Flatness", "Position", "HoleSize"):
        doc.getObject(name).Views = [top]
    for name in ("Position", "HoleSize"):
        o = doc.getObject(name)
        o.Views = o.Views + [cut]
    doc.getObject("DatumA").Views = [front]
    doc.recompute()
    STATE["pviews"] = [top, front, cut]


@step(500)
def run_command():
    FreeCADGui.Selection.clearSelection()
    for v in STATE["pviews"]:
        FreeCADGui.Selection.addSelection(v)
    before = set(o.Name for o in STATE["doc"].Objects)
    FreeCADGui.runCommand("TechDraw_PageFromPMIViews")
    FreeCADGui.Selection.clearSelection()
    pages = [o for o in STATE["doc"].Objects
             if o.isDerivedFrom("TechDraw::DrawPage") and o.Name not in before]
    if not check("the command makes a page", len(pages) == 1, len(pages)):
        raise RuntimeError("no page")
    page = STATE["page"] = pages[0]
    views = {o.Label: o for o in page.Views if o.isDerivedFrom("TechDraw::DrawViewPart")}
    STATE["views"] = views
    check("a view per PMI view, and the section's base",
          sorted(views) == ["Cut base", "Cut view", "Elevation view", "Plan view"], sorted(views))
    check("the clipped one a section on it",
          views["Cut view"].isDerivedFrom("TechDraw::DrawViewSection")
          and views["Cut view"].BaseView == views["Cut base"])
    STATE["t0"] = time.time()


def imported():
    v = STATE["views"]
    return {"Plan": on(v["Plan view"]), "Elevation": on(v["Elevation view"]),
            "Cut": on(v["Cut view"])}


WANT = {"Plan": (["Flatness", "Position"], ["HoleSize"]),
        "Elevation": (["DatumA"], []),
        "Cut": (["Position"], ["HoleSize"])}


@step(500)
def projected():
    # The projections run on threads; the command imports as each is back
    while time.time() - STATE["t0"] < 30 and imported() != WANT:
        loop = QtCore.QEventLoop()
        QtCore.QTimer.singleShot(100, loop.quit)
        loop.exec_()
    got = imported()
    check("each view gets what its PMI view shows, once projected", got == WANT, got)
    note("waited %.1f s" % (time.time() - STATE["t0"]))


@step(2000)
def drawn():
    import TechDrawGui
    from PySide import QtGui

    path = os.path.join(OUT, "page-vg.png")
    TechDrawGui.renderPageVg(STATE["page"], path, 1200, 850)
    img = QtGui.QImage(path)
    n = 0
    if not img.isNull():
        for y in range(img.height()):
            for x in range(img.width()):
                p = img.pixel(x, y)
                r, g, b = (p >> 16) & 255, (p >> 8) & 255, p & 255
                if r + g + b < 300 and max(r, g, b) - min(r, g, b) < 24:
                    n += 1
    check("the page draws them", n > 3000, "%d dark pixels" % n)


def finish():
    for name in list(FreeCAD.listDocuments()):
        try:
            FreeCAD.closeDocument(name)
        except Exception:
            pass
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


def advance():
    if not STEPS:
        finish()
        return
    delay, fn = STEPS.pop(0)

    def run():
        try:
            fn()
        except Exception:
            note("ABORT in %s: %s" % (
                fn.__name__, traceback.format_exc().replace("\n", " | ")))
            STEPS.clear()
        advance()

    QtCore.QTimer.singleShot(delay, run)


advance()
