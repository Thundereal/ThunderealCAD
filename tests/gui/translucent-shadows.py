# SPDX-License-Identifier: LGPL-2.1-or-later
"""A translucent part casts a translucent shadow, tinted by its colour.

The user, of a glass tank in the fluid workbench's videos: "Coin definitely
supports translucent shadow. I added the code along with colorized shadow
tinted by the translucent object. There must be something wrong" -- and
then: "No need to mirror exact since bgfx is more powerful. Make it look
more natural the better".

The bgfx renderer cast every transparent part into the shadow map as if it
were opaque: the tank, drawn four fifths clear, darkened the water and the
floor like a solid box. A translucent part now casts through the tint map,
letting (1 - opacity) x mix(white, its colour, opacity) of the light
through, a part behind an opaque one as the light sees it casting nothing;
and a receiver takes that tint only behind the nearest translucent part
along the light, so a translucent part does not shade itself, nor a part
standing on a translucent floor take the floor's shadow.

The scene, from straight above, the light at 45 degrees so every shadow
falls beside what casts it, on a white floor:
  - an opaque block: its shadow is drawn;
  - a grey plate four fifths clear: most of the light passes;
  - a red plate half clear: red light passes;
  - the grey plate's own top is as bright as when it receives no shadow;
  - a translucent tray with an opaque cube on its floor: the cube's top is
    as bright as when the tray casts nothing;
  - a red plate half clear cut by a section plane: what is left of it
    casts red, what is cut away nothing.
And under a bulb (a body that is a light, Render_LightShadow), the
scene's own light pointing up, each shadow against the same render with the
parts casting nothing: the block's is dark, the clear plate's light and
the red plate's red.
"""
import os
import time
import traceback

import numpy as np

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
VIEW = "User parameter:BaseApp/Preferences/View"
HIGH = 40.0  # the casters' undersides above the floor


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
        time.sleep(0.005)


def pixels(path):
    image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB888)
    w, h = image.width(), image.height()
    raw = np.frombuffer(image.constBits(), dtype=np.uint8, count=h * image.bytesPerLine())
    return raw.reshape(h, image.bytesPerLine())[:, : w * 3].reshape(h, w, 3).astype(np.float64)


def linear(a):
    c = a / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def box(doc, name, size, at, color, transparency=0):
    obj = doc.addObject("Part::Box", name)
    obj.Length, obj.Width, obj.Height = size
    obj.Placement.Base = FreeCAD.Vector(*at)
    doc.recompute()
    obj.ViewObject.ShapeColor = color
    obj.ViewObject.Transparency = transparency
    obj.ViewObject.DisplayMode = "Shaded"
    return obj


def bulb():
    """The same casters under a bulb above them, the scene light off."""
    doc = FreeCAD.newDocument("BulbShadows")
    box(doc, "Floor", (400, 300, 4), (-200, -150, -4), (0.9, 0.9, 0.9))
    parts = [
        box(doc, "Block", (30, 30, 2), (-60, -15, HIGH), (0.6, 0.6, 0.6)),
        box(doc, "Clear", (30, 30, 2), (30, -15, HIGH), (0.55, 0.57, 0.60), 80),
        box(doc, "Red", (30, 30, 2), (-15, 30, HIGH), (0.9, 0.1, 0.1), 50),
    ]
    import Part

    lamp = doc.addObject("Part::Feature", "Bulb")
    lamp.Shape = Part.makeSphere(4, FreeCAD.Vector(0, 0, 100))
    doc.recompute()
    vo = lamp.ViewObject
    for name, kind, value in (("Render_Light", "App::PropertyBool", True),
                              ("Render_LightShadow", "App::PropertyBool", True),
                              ("Render_LightRange", "App::PropertyFloat", 800.0),
                              ("Render_LightIntensity", "App::PropertyFloat", 0.4)):
        if not hasattr(vo, name):
            vo.addProperty(kind, name)
        setattr(vo, name, value)
    settle(1.0)
    view = FreeCADGui.ActiveDocument.ActiveView
    view.setCameraType("Orthographic")
    view.viewTop()
    # bulb shadows run with the scene light's: it points up, lighting and
    # shading nothing seen from above
    view.Render_Light = True
    view.Render_LightDirection = (0.0, 0.0, 1.0)
    view.fitAll()
    settle(3.0)
    h = [0]

    def shot(tag):
        view.redraw()
        settle(2.0)
        path = os.path.join(OUT, tag + ".png")
        view.saveRenderDump(path, metadata=False)
        image = linear(pixels(path))
        h[0] = image.shape[0]
        return image

    def spot(image, point):
        x, y = view.getPointOnViewport(FreeCAD.Vector(*point))
        x, y = int(x), int(h[0] - 1 - y)
        return image[y - 3 : y + 4, x - 3 : x + 4].reshape(-1, 3).mean(axis=0)

    # where each part's shadow falls: away from the bulb, past the part
    # (the floor's point on the line from the bulb through the part's
    # far side, a little in)
    def under(x, y):
        k = 100.0 / (100.0 - HIGH)
        return (x * k, y * k, 0.0)

    spots = {"block": under(-52.0, 0.0), "clear": under(52.0, 0.0), "red": under(0.0, 52.0)}
    cast = shot("bulb")
    for part in parts:
        part.ViewObject.ShadowStyle = "Shadowed"
    bare = shot("bulb-bare")
    share = {name: spot(cast, at) / np.maximum(spot(bare, at), 1e-6) for name, at in spots.items()}
    block = share["block"]
    passed = lambda c: (c - block) / np.maximum(1.0 - block, 1e-6)
    faint, tinted = passed(share["clear"]), passed(share["red"])
    note("NOTE under the bulb: the block keeps %s of the light; the clear plate lets through %s, "
         "the red %s" % (np.round(block, 3), np.round(faint, 3), np.round(tinted, 3)))
    if not check("an opaque block's shadow under a bulb is drawn", float(block.max()) < 0.9,
                 "%.2f of the light" % float(block.max())):
        FreeCAD.closeDocument(doc.Name)
        return
    check("under a bulb a plate four fifths clear lets most of the light through",
          0.5 < float(faint.mean()) < 0.95, "%.2f of it" % float(faint.mean()))
    check("under a bulb a red plate half clear lets red through",
          float(tinted[0]) > 1.5 * float(tinted[1]) and float(tinted[0]) > 1.5 * float(tinted[2]),
          "red %.2f, green %.2f, blue %.2f of it" % tuple(float(v) for v in tinted))
    FreeCAD.closeDocument(doc.Name)


def run():
    try:
        prefs = FreeCAD.ParamGet(VIEW)
        render = prefs.GetGroup("Render")
        prefs.SetInt("RenderCache", 3)
        render.SetString("Type", "bgfx - OpenGL")
        render.SetBool("PBR", True)
        doc = FreeCAD.newDocument("TranslucentShadows")
        floor = box(doc, "Floor", (400, 160, 4), (-100, -80, -4), (0.9, 0.9, 0.9))
        block = box(doc, "Block", (40, 40, 6), (-80, -20, HIGH), (0.6, 0.6, 0.6))
        clear = box(doc, "Clear", (40, 40, 2), (0, -20, HIGH), (0.55, 0.57, 0.60), 80)
        red = box(doc, "Red", (40, 40, 2), (80, -20, HIGH), (0.9, 0.1, 0.1), 50)
        # an open tray, its floor 2 thick, walls 6 high; a cube on its floor
        import Part

        tray = doc.addObject("Part::Feature", "Tray")
        tray.Shape = Part.makeBox(80, 60, 8, FreeCAD.Vector(180, -30, 0)).cut(
            Part.makeBox(76, 56, 8, FreeCAD.Vector(182, -28, 2)))
        doc.recompute()
        tray.ViewObject.ShapeColor = (0.55, 0.57, 0.60)
        tray.ViewObject.Transparency = 80
        tray.ViewObject.DisplayMode = "Shaded"
        cube = box(doc, "Cube", (10, 10, 4), (205, -5, 2), (0.6, 0.6, 0.6))
        # a red plate cut by a section plane of its own: x < 20 kept
        from pivy import coin

        cut = box(doc, "Cut", (40, 30, 2), (0, 30, HIGH), (0.9, 0.1, 0.1), 50)
        plane = coin.SoClipPlane()
        plane.plane.setValue(coin.SbPlane(coin.SbVec3f(-1, 0, 0), coin.SbVec3f(20, 0, 0)))
        cut.ViewObject.RootNode.insertChild(plane, 0)
        settle(1.0)
        view = FreeCADGui.ActiveDocument.ActiveView
        view.setCameraType("Orthographic")
        view.viewTop()
        view.Render_Light = True
        view.Render_LightDirection = (1.0, 0.0, -1.0)
        view.fitAll()
        settle(3.0)
        h = [0]

        def shot(tag):
            view.redraw()
            settle(2.0)
            path = os.path.join(OUT, tag + ".png")
            view.saveRenderDump(path, metadata=False)
            image = linear(pixels(path))
            h[0] = image.shape[0]
            return image

        def spot(image, point):
            x, y = view.getPointOnViewport(FreeCAD.Vector(*point))
            x, y = int(x), int(h[0] - 1 - y)
            return image[y - 3 : y + 4, x - 3 : x + 4].reshape(-1, 3).mean(axis=0)

        shadows = shot("shadows")
        lit = spot(shadows, (-60.0, 60.0, 0.0))
        # the light goes +x as it falls: a caster's shadow is HIGH along x
        under = lambda x: spot(shadows, (x + 20.0 + HIGH + 3.0, 0.0, 0.0))
        # the scene light is a part of what lights the floor, the
        # headlight and the environment the rest: what a shadow lets
        # through is measured against the opaque block's, 0 there and 1
        # in the open
        block = under(-80.0)
        passed = lambda c: (c - block) / np.maximum(lit - block, 1e-6)
        faint = passed(under(0.0))
        tinted = passed(under(80.0))
        note("NOTE lit floor %s, under the block %s; let through by the clear plate %s, the red "
             "plate %s" % (np.round(lit, 3), np.round(block, 3), np.round(faint, 3),
                           np.round(tinted, 3)))
        check("an opaque block's shadow is drawn", float((lit - block).min()) > 0.05,
              "the floor %.3f darker under it" % float((lit - block).min()))
        check("a plate four fifths clear lets most of the light through",
              0.6 < float(faint.mean()) < 0.95, "%.2f of it" % float(faint.mean()))
        check("a red plate half clear lets red through",
              float(tinted[0]) > 1.5 * float(tinted[1]) and float(tinted[0]) > 1.5 * float(tinted[2]),
              "red %.2f, green %.2f, blue %.2f of it" % tuple(float(v) for v in tinted))
        # the plate's top 2 above HIGH: its shadow HIGH + 2 along x; the
        # kept half's from 0 to 20, the cut half's from 20 to 40
        kept = passed(spot(shadows, (10.0 + HIGH + 2.0, 45.0, 0.0)))
        gone = passed(spot(shadows, (30.0 + HIGH + 2.0, 45.0, 0.0)))
        note("NOTE the cut plate lets through %s where it is kept, %s where it is cut away"
             % (np.round(kept, 3), np.round(gone, 3)))
        check("what a section plane leaves of a translucent part casts",
              float(kept[0]) > 1.5 * float(kept[1]),
              "red %.2f, green %.2f, blue %.2f of the light" % tuple(float(v) for v in kept))
        check("what a section plane cuts away of a translucent part casts nothing",
              float(gone.min()) > 0.9,
              "red %.2f, green %.2f, blue %.2f of the light" % tuple(float(v) for v in gone))
        clearTop = spot(shadows, (20.0, 0.0, HIGH + 2.0))
        cubeTop = spot(shadows, (210.0, 0.0, 6.0))
        # the same, the plate not receiving and the tray not casting
        clear.ViewObject.ShadowStyle = "Cast shadow"
        tray.ViewObject.ShadowStyle = "Shadowed"
        alone = shot("alone")
        clearAlone = spot(alone, (20.0, 0.0, HIGH + 2.0))
        cubeAlone = spot(alone, (210.0, 0.0, 6.0))
        note("NOTE clear plate's top %s, receiving nothing %s; cube's top %s, the tray casting "
             "nothing %s" % (np.round(clearTop, 3), np.round(clearAlone, 3),
                             np.round(cubeTop, 3), np.round(cubeAlone, 3)))
        check("a translucent plate does not shade itself",
              float(np.abs(clearTop - clearAlone).max()) < 0.03 * float(clearAlone.max()),
              "its top moved %.4f" % float(np.abs(clearTop - clearAlone).max()))
        check("a part on a translucent floor takes no shadow from it",
              float(np.abs(cubeTop - cubeAlone).max()) < 0.03 * float(cubeAlone.max()),
              "the cube's top moved %.4f" % float(np.abs(cubeTop - cubeAlone).max()))
        FreeCAD.closeDocument(doc.Name)
        bulb()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        note("DONE")
        QtCore.QTimer.singleShot(0, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, run)
