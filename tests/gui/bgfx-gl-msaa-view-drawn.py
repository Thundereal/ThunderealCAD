"""The 3D view on bgfx - OpenGL shows the scene with anti-aliasing on.

With MSAA 4x -- View/AntiAliasing's default -- the GL blit's framebuffer for
the depth (a write-only multisampled target) failed its status check on
every frame, and the blit gave up the whole frame with it: the view stayed
one flat colour, "Framebuffer incomplete attachment" in the log at each
redraw. Seen under a clone of the reporter's profile in WaterFilmIsClear and
TranslucentShadows (docs/SmokeTesting.md, step 1); Direct3D, which reads its
frame back, and Legacy were not affected.

Measured here: a box filling a new document's view, the view read with
QOpenGLWidget.grabFramebuffer() (what the widget shows, multisampling
resolved). Legacy with MSAA 4x first, to show the instrument sees a box at
all; then bgfx - OpenGL with MSAA off and with 4x, PBR, water and
volumetrics off, and 4x with them on: each shows more than 20 colours, the
box covering part of the view. Before the fix both 4x cases are one colour.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets
from PySide6 import QtOpenGLWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
VIEW = "User parameter:BaseApp/Preferences/View"
GL = "bgfx - OpenGL"
state = {"done": False, "saved": None}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


EFFECTS = ("PBR", "WaterSurface", "WaterRefraction", "Volumetric")


def picture(tag, kind, samples, effects=False):
    """(colours, share unlike the corner) of a new view of a box"""
    prefs = FreeCAD.ParamGet(VIEW)
    for name in EFFECTS:
        prefs.GetGroup("Render").SetBool(name, effects)
    prefs.SetInt("RenderCache", 3)
    prefs.GetGroup("Render").SetString("Type", kind)
    prefs.SetInt("AntiAliasing", samples)
    doc = FreeCAD.newDocument("MsaaView" + tag)
    box = doc.addObject("Part::Box", "Box")
    box.Length = box.Width = box.Height = 10
    doc.recompute()
    settle(1.0)
    view = FreeCADGui.ActiveDocument.ActiveView
    view.viewIsometric()
    view.fitAll()
    settle(2.0)
    mdi = [w for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget)
           if w.metaObject().className() == "Gui::View3DInventor" and w.isVisible()]
    gl = mdi[-1].findChildren(QtOpenGLWidgets.QOpenGLWidget)
    img = gl[0].grabFramebuffer()
    img.save(os.path.join(OUT, tag + ".png"))
    corner = img.pixel(2, 2)
    colours, other, n = set(), 0, 0
    for y in range(0, img.height(), 4):
        for x in range(0, img.width(), 4):
            p = img.pixel(x, y)
            n += 1
            colours.add(p)
            other += p != corner
    FreeCAD.closeDocument(doc.Name)
    settle(0.5)
    return len(colours), round(100.0 * other / max(1, n), 1)


def shows(name, got):
    return check(name, got[0] > 20 and got[1] > 5.0, "%d colours, %.1f%% unlike the corner" % got)


def run():
    prefs = FreeCAD.ParamGet(VIEW)
    render = prefs.GetGroup("Render")
    state["effects"] = {n: (render.GetBool(n, False), n in render.GetBools()) for n in EFFECTS}
    state["saved"] = (prefs.GetInt("RenderCache", 0),
                      prefs.GetGroup("Render").GetString("Type", ""),
                      prefs.GetInt("AntiAliasing", 0))
    try:
        shows("the instrument: Legacy, MSAA 4x, shows the box", picture("legacy4x", "Legacy", 3))
        shows("bgfx - OpenGL, MSAA off, shows the box", picture("gl0", GL, 0))
        shows("bgfx - OpenGL, MSAA 4x, shows the box", picture("gl4x", GL, 3))
        shows("and with PBR, water and volumetrics on", picture("gl4xfx", GL, 3, True))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    if state["saved"]:
        prefs = FreeCAD.ParamGet(VIEW)
        prefs.SetInt("RenderCache", state["saved"][0])
        prefs.GetGroup("Render").SetString("Type", state["saved"][1])
        prefs.SetInt("AntiAliasing", state["saved"][2])
        for name, (value, there) in state.get("effects", {}).items():
            if there:
                prefs.GetGroup("Render").SetBool(name, value)
            else:
                prefs.GetGroup("Render").RemBool(name)
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
