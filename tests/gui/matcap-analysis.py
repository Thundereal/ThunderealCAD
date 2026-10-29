"""The surface analysis matcaps (docs/Surface.md sec 7, U4): draft analysis,
and the zebra fixed to the model.

Draft colours a face by how it releases from a mold pulled along the view's
Render_MatcapDirection: green past Render_MatcapDraftAngle, red an undercut
past its negative, yellow in between. The fixed zebra (Render_MatcapFixed)
looks at its room of stripes along that direction instead of the camera,
so a stripe stays where it is on the surface as the view turns.

Read from the backend's own framebuffer (saveRenderDump, "renderer"):

  draft     a frustum on z = 0 pulled along +Z with a least draft of 1 deg:
            its top and a side leaning in by 5 deg are green, a side
            leaning out by 5 deg is red; an upright box's side is yellow.
  fixed     points of a sphere seen from the front and from the right:
            fixed to the model, nearly every one keeps its tone between
            the two views; fixed to the camera, far fewer do.

Scored against the tree before the presets existed: the first check fails
(there is no Draft preset) and nothing else can be measured.
"""

import math
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtGui

V = FreeCAD.Vector
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "MatcapAnalysis"
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtCore.QCoreApplication.processEvents()
        time.sleep(0.01)


TAN5 = math.tan(math.radians(5))
SPHERE_R = 25.0


def frustum():
    """A 20 x 20 base, 10 high: the front (y = 0) and right (x = 20) sides
    leaning in by 5 deg, the left (x = 0) leaning out by 5 deg, the back
    upright"""
    d = 10 * TAN5
    bottom = Part.makePolygon([V(0, 0, 0), V(20, 0, 0), V(20, 20, 0), V(0, 20, 0), V(0, 0, 0)])
    top = Part.makePolygon(
        [V(-d, d, 10), V(20 - d, d, 10), V(20 - d, 20, 10), V(-d, 20, 10), V(-d, d, 10)]
    )
    return Part.makeLoft([bottom, top], True, True)


def run():
    try:
        render = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render")
        render.SetBool("Matcap", True)
        render.SetFloat("MatcapTint", 0.0)
        for off in ("AO", "Cavity", "Bloom", "Light", "Shadow", "PBR", "Volumetric"):
            render.SetBool(off, False)
        FreeCADGui.getMainWindow().showMaximized()
        doc = FreeCAD.newDocument(DOC)
        shapes = {
            "Frustum": frustum(),
            "Upright": Part.makeBox(10, 2, 10, V(30, 0, 0)),
            "Sphere": Part.makeSphere(SPHERE_R, V(0, 0, 0)),
        }
        for name, shape in shapes.items():
            obj = doc.addObject("Part::Feature", name)
            obj.Shape = shape
            obj.ViewObject.DisplayMode = "Shaded"
            obj.ViewObject.Deviation = 0.02
            obj.ViewObject.AngularDeflection = 4.0
        doc.recompute()
        view = FreeCADGui.activeDocument().activeView()
        view.setAnimationEnabled(False)
        state["view"] = view
        QtCore.QTimer.singleShot(2000, measure)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
        finish()


def show(*names):
    gdoc = FreeCADGui.getDocument(DOC)
    for o in ("Frustum", "Upright", "Sphere"):
        gdoc.getObject(o).Visibility = o in names


def grab(name):
    view = state["view"]
    view.fitAll()
    view.redraw()
    settle(0.5)
    view.waitFrameComplete()
    path = os.path.join(OUT, name + ".png")
    view.saveRenderDump(path, "renderer")
    return QtGui.QImage(path)


def colour(img, point):
    vx, vy = state["view"].getPointOnViewport(point)
    return QtGui.QColor(img.pixel(int(vx), int(img.height() - 1 - vy)))


def kind(c):
    r, g, b = c.red(), c.green(), c.blue()
    if g > r + 40 and g > b + 40:
        return "green"
    if r > g + 60 and r > b + 60:
        return "red"
    if r > 140 and g > 120 and b < r - 60:
        return "yellow"
    return "other (%d, %d, %d)" % (r, g, b)


def sphere_points():
    """Points of the sphere facing both the front camera (at -Y) and the
    right one (at +X)"""
    pts = []
    for i in range(7):
        for j in range(7):
            n = V(0.35 + 0.08 * i, -0.35 - 0.08 * j, -0.4 + 0.13 * j)
            n.normalize()
            pts.append(n * SPHERE_R)
    return pts


def tones(img, pts):
    out = []
    for p in pts:
        c = colour(img, p)
        t = (c.red() + c.green() + c.blue()) / 3.0
        out.append(None if 80 <= t <= 175 else t > 175)
    return out


def agreement(fixed):
    view = state["view"]
    view.Render_MatcapFixed = fixed
    pts = sphere_points()
    view.viewFront()
    a = tones(grab("sphere-front-%d" % fixed), pts)
    view.viewRight()
    b = tones(grab("sphere-right-%d" % fixed), pts)
    pairs = [(x, y) for x, y in zip(a, b) if x is not None and y is not None]
    if len(pairs) < 10:
        return None, "only %d points read clearly" % len(pairs)
    same = sum(1 for x, y in pairs if x == y) / float(len(pairs))
    return same, "%d of %d points keep their tone" % (
        sum(1 for x, y in pairs if x == y),
        len(pairs),
    )


def measure():
    try:
        view = state["view"]
        try:
            view.Render_MatcapPreset = "Draft"
            ok, detail = True, ""
        except Exception as e:
            ok, detail = False, str(e)
        if not check("the view has a Draft matcap preset", ok, detail):
            finish()
            return
        view.Render_MatcapDirection = V(0, 0, 1)
        view.Render_MatcapDraftAngle = 1.0
        show("Frustum", "Upright")
        view.setViewDirection((1, 1, -1))
        img = grab("draft")
        d = 10 * TAN5
        reads = {
            "the top": (V(10 - d, 10 + d / 2, 10), "green"),
            "the side leaning in": (V(10 - d / 2, d / 2, 5), "green"),
            "the side leaning out": (V(-d / 2, 10 + d / 2, 5), "red"),
            "the upright side": (V(35, 0, 5), "yellow"),
        }
        for what, (p, want) in reads.items():
            got = kind(colour(img, p))
            check("draft: %s is %s" % (what, want), got == want, got)

        view.Render_MatcapPreset = "Zebra"
        view.Render_MatcapStripes = 8
        view.Render_MatcapDirection = V(0, -1, 0)
        show("Sphere")
        fixed, detail = agreement(True)
        check(
            "fixed to the model, the stripes stay on the surface as the view turns",
            fixed is not None and fixed >= 0.9,
            detail,
        )
        free, detail2 = agreement(False)
        check(
            "fixed to the camera, they do not",
            free is not None and fixed is not None and free <= fixed - 0.25,
            detail2,
        )
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
