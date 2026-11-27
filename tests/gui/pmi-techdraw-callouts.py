"""The 3D model's PMI on a TechDraw page (docs/GDandT.md sec 9, P4), driven
as a user drives it.

The PMI test part (PMITests.TestDrawing: a box with a hole, a flatness, a
position and a note facing up, datums on its bottom and left) on a page, seen
from the top. TechDraw_ImportModelAnnotations with the view selected makes
the callouts; the page draws each as the 3D view lays it out -- a frame of
strokes and filled lettering -- with its leader.

Claims:
  - the command makes a callout per PMI object that faces the view and whose
    feature it shows (the flatness, the position, the note), linked to it;
  - and a dimension per PMI dimension it shows (the hole's diameter, the
    width), on the edges it measures, linked to it: one value;
  - the page draws them: its SVG export gains paths, and the page's pixels
    darken where they stand;
  - the vg page renderer draws them too (they are captured from their
    items, not painted on their own);
  - the 3D object is the master: its value changed, the drawing changes;
  - run again, nothing is made twice.
"""
import os
import re
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
STEPS = []
STATE = {}


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


def callouts(doc):
    return [o for o in doc.Objects if o.isDerivedFrom("TechDraw::DrawPMICallout")]


def dimensions(doc):
    return [o for o in doc.Objects
            if o.isDerivedFrom("TechDraw::DrawViewDimension") and o.Source]


def export(tag):
    """The page as an SVG: its paths, and the text of them all"""
    import TechDrawGui
    svg = os.path.join(OUT, tag + ".svg")
    TechDrawGui.exportPageAsSvg(STATE["page"], svg)
    text = ""
    if os.path.exists(svg):
        with open(svg, encoding="utf-8", errors="replace") as f:
            text = f.read()
    return len(re.findall(r"<path\b", text)), text


def dark(img):
    """How many pixels are ink: dark grey to black, lines and lettering on
    the white sheet -- not the window's blue around it"""
    n = 0
    for y in range(img.height()):
        for x in range(img.width()):
            p = img.pixel(x, y)
            r, g, b = (p >> 16) & 255, (p >> 8) & 255, p & 255
            if r + g + b < 300 and max(r, g, b) - min(r, g, b) < 24:
                n += 1
    return n


def darker(a, b):
    """How many pixels of b are darker than a's by more than 60 (summed over r, g, b): ink
    added, however thin -- at the size the page is shown a callout's strokes come out a
    light grey that dark() does not count"""
    if a.size() != b.size():
        return -1
    n = 0
    for y in range(a.height()):
        for x in range(a.width()):
            p, q = a.pixel(x, y), b.pixel(x, y)
            if sum(((p >> s) & 255) - ((q >> s) & 255) for s in (0, 8, 16)) > 60:
                n += 1
    return n


def vg(tag):
    import TechDrawGui
    from PySide import QtGui
    path = os.path.join(OUT, tag + ".png")
    TechDrawGui.renderPageVg(STATE["page"], path, 1200, 850)
    img = QtGui.QImage(path)
    return dark(img) if not img.isNull() else -1


def page_view():
    views = [v for v in FreeCADGui.getMainWindow().findChildren(QtWidgets.QGraphicsView)
             if v.metaObject().className() == "TechDrawGui::QGVPage" and v.isVisible()]
    return views[-1] if views else None


@step(1000)
def make():
    import TechDrawGui  # noqa: F401 -- the view providers
    from PMITests.TestDrawing import TestImportModelAnnotations

    # Its commands, TechDrawTools' among them, are the workbench's
    FreeCADGui.activateWorkbench("TechDrawWorkbench")
    t = TestImportModelAnnotations("test_what_faces_the_view_and_shows")
    t.setUp()
    STATE["test"] = t
    STATE["doc"], STATE["page"], STATE["view"] = t.doc, t.page, t.view
    STATE["page"].ViewObject.show()
    FreeCADGui.SendMsgToActiveView("ViewFit")


@step(3000)
def before():
    STATE["paths0"], _ = export("0-before")
    STATE["vg0"] = vg("0-before-vg")
    v = page_view()
    STATE["img0"] = v.grab().toImage() if v else None
    if v is not None:
        STATE["img0"].save(os.path.join(OUT, "0-before.png"))
    check("the page is shown", v is not None)


@step(500)
def run_command():
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(STATE["view"])
    FreeCADGui.runCommand("TechDraw_ImportModelAnnotations")
    FreeCADGui.Selection.clearSelection()
    got = sorted(c.Source.Name for c in callouts(STATE["doc"]) if c.Source)
    check("the command makes the callouts that face the view and show",
          got == ["Flatness", "Note", "Position"], got)
    got = sorted((d.Source.Name, d.Type) for d in dimensions(STATE["doc"]))
    check("and the dimensions, linked", got == [("HoleSize", "Diameter"), ("Width", "DistanceX")],
          got)


@step(3000)
def drawn():
    paths, text = export("1-imported")
    STATE["text1"] = text
    check("the SVG export draws them", paths > STATE["paths0"] + 3,
          "%d paths, %d before" % (paths, STATE["paths0"]))
    v = page_view()
    if v is not None:
        img = v.grab().toImage()
        img.save(os.path.join(OUT, "1-imported.png"))
        n = darker(STATE["img0"], img)
        check("the page shows them", n > 150, "%d pixels darker than before" % n)
    n = vg("1-imported-vg")
    check("the vg renderer draws them", n > STATE["vg0"] + 50,
          "%d dark pixels, %d before" % (n, STATE["vg0"]))


@step(500)
def edit_3d():
    flat = STATE["doc"].getObject("Flatness")
    flat.Value = 0.125
    STATE["doc"].recompute()


@step(2000)
def follows():
    _, text = export("2-edited")
    check("the 3D object is the master: its value changed, the drawing changes",
          text != STATE["text1"])


@step(500)
def again():
    FreeCADGui.Selection.addSelection(STATE["view"])
    FreeCADGui.runCommand("TechDraw_ImportModelAnnotations")
    FreeCADGui.Selection.clearSelection()
    check("run again, nothing is made twice", len(callouts(STATE["doc"])) == 3,
          len(callouts(STATE["doc"])))
    check("nor a dimension", len(dimensions(STATE["doc"])) == 2, len(dimensions(STATE["doc"])))


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
