"""A capture of a shape that just appeared has its edges.

A shape is first tessellated on a coarse rung (Render/CoarseTessellation,
default 2) where the engine draws, and an object's edges wait for its exact
faces (BGFXFrame.cpp, gatedByContract): an edge set describes the shape
its faces only approximate. The exact rung is asked for by the level plan
about 300 ms after the camera settles, and lands some time after that.
View.saveImage() waits for a complete frame, but nothing told it the
picture still owed that refine: the plan sets frameOwes from its timer,
between frames, and the next frame's start clears it. So a capture taken
right after a shape appeared came out with shaded faces and no edges --
every fillet picture the OCCT pictures script made on the mac after the
2026-10-19 merge (the report: fcad-41, by way of the mac session).

Now a frame with a waiting capture asks the plan's question itself and
holds the capture while anything on screen would still be refined.

What is done, for a box, a box with four edges filleted and a cylinder,
each in a document of its own, with the defaults (coarse-first on): the
shape is made, the camera set (not animated), the GUI pumped three times,
then three shots at 360 x 300 on white -- one that does not wait (the
frame as it stands), one that does, and one more after 1.5 s of settling.
Then the shape is replaced by itself scaled by 1.1 and the same is done
again. An edge pixel is one darker than 110/110/120.

What is asserted of each:
  - the case arose: the shot that does not wait has under 80% of the edge
    pixels of the settled picture (the edges held for the coarse rung);
  - the waiting capture has at least 95% of the settled picture's edge
    pixels;
  - and it took under 5 s.
"""
import os
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
import Part
from PySide import QtGui

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
W, H = 360, 300
V = App.Vector


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def edge_pixels(png):
    img = QtGui.QImage(png)
    n = 0
    for y in range(img.height()):
        for x in range(img.width()):
            c = QtGui.QColor(img.pixel(x, y))
            if c.red() < 110 and c.green() < 110 and c.blue() < 120:
                n += 1
    return n


def settle(sec):
    end = time.time() + sec
    while time.time() < end:
        Gui.updateGui()
        time.sleep(0.05)


def camera(eye, up, center, height):
    # An orthographic camera looking at center from eye's direction, set
    # in one step (fitAll and the view-direction commands animate).
    z = V(eye)
    z.normalize()
    y = up - z * up.dot(z)
    y.normalize()
    x = y.cross(z)
    m = App.Matrix(x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0)
    r = App.Rotation(m)
    ax, ang = r.Axis, r.Angle
    pos = center + z * 1000
    return ("#Inventor V2.1 ascii\nOrthographicCamera {\n viewportMapping ADJUST_CAMERA\n"
            " position %g %g %g\n orientation %g %g %g %g\n nearDistance 1\n farDistance 3000\n"
            " aspectRatio 1\n focalDistance 1000\n height %g\n}\n"
            % (pos.x, pos.y, pos.z, ax.x, ax.y, ax.z, ang, height))


CAM = camera(V(1, -1, 1), V(0, 0, 1), V(5, 5, 5), 24)


def shapes():
    box = Part.makeBox(10, 10, 10)
    return [("box", box), ("fillet", box.makeFillet(2, box.Edges[:4])),
            ("cylinder", Part.makeCylinder(5, 10))]


def shoot(view, tag):
    for _ in range(3):
        Gui.updateGui()
    view.redraw()
    now = os.path.join(OUT, tag + "-nowait.png")
    view.saveImage(now, W, H, "#FFFFFF", "$MIBA", 0, 0)
    waited = os.path.join(OUT, tag + "-wait.png")
    t = time.time()
    view.saveImage(waited, W, H, "#FFFFFF")
    took = time.time() - t
    settle(1.5)
    settled = os.path.join(OUT, tag + "-settled.png")
    view.saveImage(settled, W, H, "#FFFFFF")
    a, b, c = edge_pixels(now), edge_pixels(waited), edge_pixels(settled)
    detail = "no wait %d, waiting %d, settled %d" % (a, b, c)
    check("%s: the case arose -- the frame as it stands lacks the edges" % tag,
          c > 0 and a < 0.8 * c, detail)
    check("%s: the waiting capture has the edges" % tag, c > 0 and b >= 0.95 * c, detail)
    check("%s: ... and took under 5 s" % tag, took < 5.0, "%.2f s" % took)


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetBool("ShowNaviCube", False)
        for name, shape in shapes():
            doc = App.newDocument("Capture")
            Gui.updateGui()
            obj = doc.addObject("Part::Feature", "S")
            obj.Shape = shape
            obj.ViewObject.LineColor = (0.15, 0.15, 0.2)
            obj.ViewObject.LineWidth = 1.5
            doc.recompute()
            view = Gui.ActiveDocument.ActiveView
            view.setCamera(CAM)
            shoot(view, name)
            edited = shape.copy()
            edited.scale(1.1, V(5, 5, 5))
            obj.Shape = edited
            doc.recompute()
            shoot(view, name + "-edited")
            App.closeDocument(doc.Name)
            Gui.updateGui()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    note("DONE")
    Gui.getMainWindow().close()


run()
