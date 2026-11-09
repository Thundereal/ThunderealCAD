"""A new TechDraw page has its frames off, ShowFrames is in the property
editor, and a saved page keeps what it saved (docs/HandsOnQueue.md entry
230, the reporter 2026-10-28: "I want to make it default off"; "make the
property visible, leave existing object as it is").

Claims:
  - a new page's ShowFrames is false, and the property is not hidden;
  - ShowFrames set as the property editor sets it redraws the page (the
    views' frames and the template's editable text marks appear), and set
    back the page is as it opened: so the marks are off on a new page too,
    as Toggle Frames leaves them;
  - TechDraw_ToggleFrame still flips it;
  - a page saved with frames on opens with them on, and a page made after
    that in the same document starts with them off.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "FramesDefaultOff"
PATH = os.path.join(OUT, DOC + ".FCStd")
GEN = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/TechDraw/General")
HAD = GEN.GetBool("PageRendererVg", False)
STEPS = []
STATE = {}


def note(msg):
    with open(RESULT, "a") as f:
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


def page_view():
    for v in FreeCADGui.getMainWindow().findChildren(QtWidgets.QGraphicsView):
        if v.metaObject().className() == "TechDrawGui::QGVPage" and v.isVisible():
            return v
    return None


def page_vp(name="Page"):
    return FreeCADGui.getDocument(DOC).getObject(name)


def grab(name):
    FreeCADGui.Selection.clearSelection()
    img = page_view().grab().toImage().convertToFormat(QtGui.QImage.Format_RGB32)
    img.save(os.path.join(OUT, name + ".png"))
    return img


def differ(a, b):
    if a.size() != b.size():
        return -1
    n = 0
    for y in range(0, a.height(), 2):
        for x in range(0, a.width(), 2):
            if a.pixel(x, y) != b.pixel(x, y):
                n += 1
    return n


@step(1000)
def make():
    import TechDrawGui  # the view providers
    GEN.SetBool("PageRendererVg", False)
    doc = FreeCAD.newDocument(DOC)
    box = doc.addObject("Part::Box", "Box")
    page = doc.addObject("TechDraw::DrawPage", "Page")
    template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    # a template with editable texts, whose marks go with the frames
    template.Template = os.path.join(
        FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates", "A4_LandscapeTD.svg")
    page.Template = template
    view = doc.addObject("TechDraw::DrawViewPart", "View")
    page.addView(view)
    view.Source = [box]
    view.Direction = FreeCAD.Vector(0, 0, 1)
    view.X = 120
    view.Y = 110
    doc.recompute()
    page_vp().show()


@step(5000)
def opened():
    vp = page_vp()
    if not check("the page has its view", page_view() is not None):
        raise RuntimeError("no page view")
    check("a new page has its frames off", vp.ShowFrames is False, vp.ShowFrames)
    status = vp.getPropertyStatus("ShowFrames")
    check("ShowFrames is shown in the property editor", "Hidden" not in status, status)
    STATE["off"] = grab("1-off")
    vp.ShowFrames = True


@step(1500)
def frames_on():
    STATE["on"] = grab("2-on")
    n = differ(STATE["off"], STATE["on"])
    check("ShowFrames set on redraws the page with frames and marks", n > 50,
          "%d pixels differ" % n)
    page_vp().ShowFrames = False


@step(1500)
def frames_off_again():
    n = differ(STATE["off"], grab("3-off-again"))
    check("ShowFrames set off again: the page as it opened, marks off too", n == 0,
          "%d pixels differ" % n)
    # with any other window active the command raises a message box
    active = FreeCADGui.activeView()
    shown = active.getPage().Name if hasattr(active, "getPage") else None
    if check("the page is the active window", shown == "Page", shown):
        FreeCADGui.runCommand("TechDraw_ToggleFrame")


@step(1500)
def toggled():
    vp = page_vp()
    check("Toggle Frames turns them on", vp.ShowFrames is True, vp.ShowFrames)
    n = differ(STATE["on"], grab("4-toggled"))
    check("Toggle Frames draws them", n == 0, "%d pixels differ from 2-on" % n)
    FreeCAD.getDocument(DOC).saveAs(PATH)
    FreeCAD.closeDocument(DOC)


@step(1500)
def reopen():
    FreeCAD.openDocument(PATH)


@step(3000)
def reopened():
    vp = page_vp()
    check("a page saved with frames on opens with them on", vp.ShowFrames is True,
          vp.ShowFrames)
    doc = FreeCAD.getDocument(DOC)
    doc.addObject("TechDraw::DrawPage", "Page2")
    check("a page made in that document starts with them off",
          page_vp("Page2").ShowFrames is False, page_vp("Page2").ShowFrames)


def finish():
    GEN.SetBool("PageRendererVg", HAD)
    try:
        FreeCAD.closeDocument(DOC)
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
