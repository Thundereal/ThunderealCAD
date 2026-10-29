"""The curvature map (docs/Surface.md sec 7, U4): a matcap preset colouring
a surface by its curvature -- blue negative, green flat, red positive,
saturating at Render_MatcapCurvatureRange -- Gaussian, mean, least or
greatest by Render_MatcapCurvature.

The curvature is the surface's shape operator found in the shader from how
the normal turns across the screen, so it is as true as the view mesh;
the bodies here are meshed finely. Read from the backend's own frame:

  sphere    of radius R, its mean curvature 1/R: red outweighs green at a
            range of 1.5/R and green outweighs red at 3/R, which brackets
            what the shader found between 0.625/R and 1.25/R;
  cylinder  its Gaussian curvature 0: green;
  saddle    its Gaussian curvature negative at the middle: blue.

Scored against the tree before the preset existed: the first check fails
(there is no Curvature preset).
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
DOC = "CurvatureMap"
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


R = 25.0


def saddle():
    s = Part.BSplineSurface()
    a = 10.0
    s.interpolate(
        [
            [V(x, y, (x * x - y * y) / (2 * a)) for y in (-10 + 2.5 * j for j in range(9))]
            for x in (-10 + 2.5 * i for i in range(9))
        ]
    )
    return s.toShape()


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
            "Sphere": Part.makeSphere(R),
            "Cylinder": Part.makeCylinder(10, 40, V(80, 0, -20)),
            "Saddle": saddle(),
        }
        for name, shape in shapes.items():
            obj = doc.addObject("Part::Feature", name)
            obj.Shape = shape
            obj.ViewObject.DisplayMode = "Shaded"
            obj.ViewObject.Deviation = 0.01
            obj.ViewObject.AngularDeflection = 2.0
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
    for o in ("Sphere", "Cylinder", "Saddle"):
        gdoc.getObject(o).Visibility = o in names


def dominant(img, pts):
    """How many of the points read red-, green- and blue-dominant"""
    out = {"red": 0, "green": 0, "blue": 0}
    for p in pts:
        c = colour(img, p)
        rgb = {"red": c.red(), "green": c.green(), "blue": c.blue()}
        out[max(rgb, key=rgb.get)] += 1
    return out


def facing_front(radius, centre=V(0, 0, 0)):
    """Points of a sphere or cylinder facing the front camera (at -Y)"""
    pts = []
    for i in range(-2, 3):
        for j in range(-2, 3):
            n = V(0.15 * i, -1, 0.15 * j)
            n.normalize()
            pts.append(centre + n * radius)
    return pts


def measure():
    try:
        view = state["view"]
        try:
            view.Render_MatcapPreset = "Curvature"
            ok, detail = True, ""
        except Exception as e:
            ok, detail = False, str(e)
        if not check("the view has a Curvature matcap preset", ok, detail):
            finish()
            return

        show("Sphere")
        view.Render_MatcapCurvature = "Mean"
        pts = facing_front(R)
        view.viewFront()
        view.Render_MatcapCurvatureRange = 1.5 / R
        hot = dominant(grab("sphere-hot"), pts)
        view.Render_MatcapCurvatureRange = 3.0 / R
        cool = dominant(grab("sphere-cool"), pts)
        check(
            "a sphere's mean curvature reads between 0.625/R and 1.25/R",
            hot["red"] == len(pts) and cool["green"] == len(pts),
            (hot, cool),
        )

        show("Cylinder")
        view.Render_MatcapCurvature = "Gaussian"
        view.Render_MatcapCurvatureRange = 0.01
        cyl = dominant(grab("cylinder"), facing_front(10, V(80, 0, 0)))
        check("a cylinder's Gaussian curvature is 0: green", cyl["green"] == 25, cyl)

        show("Saddle")
        view.Render_MatcapCurvatureRange = 0.01
        view.viewTop()
        middle = [V(x, y, (x * x - y * y) / 20.0) for x in (-1, 0, 1) for y in (-1, 0, 1)]
        sad = dominant(grab("saddle"), middle)
        check("a saddle's Gaussian curvature is negative: blue", sad["blue"] == 9, sad)
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
