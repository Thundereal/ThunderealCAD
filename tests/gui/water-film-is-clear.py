# SPDX-License-Identifier: LGPL-2.1-or-later
"""Thin water over a part is next to clear; deep water is coloured.

The user, of the fluid workbench's videos: "tune down the water color and
if possible, make color follow absorption law so that thinner [film] is
nearly transparent".

The water surface shader (fc_water_surface.sh) tints what is seen through
the water by Beer-Lambert over the column from the surface to the water's
back face. Two things kept a film from being clear: a tint of a fifth of
the water's colour laid on whatever the depth, and the back face being
the FARTHEST of the water along the ray, so that a film over a part
standing in deeper water was tinted as the whole column past the part,
down to the floor. The column now ends at the first opaque surface behind
the water, and the fixed tint is left to where the depth is not known.

The scene, seen from straight above: a body of water 56 deep over a white
floor, and in it a white plate whose top is 2 under the surface. Each spot
is drawn with the water and without it:
  - the floor under the deep water is tinted, and clearly (the absorption
    runs at all);
  - the plate under the film is tinted under a fifth of that.
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
DEPTH = 56.0
FILM = 2.0


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


def waterBody():
    """A box 120 x 120 x DEPTH of water, as the fluid workbench draws one:
    a closed surface under the node that says it is water."""
    from pivy import coin

    root = coin.SoSeparator()
    material = coin.SoMaterial()
    material.diffuseColor = (0.10, 0.45, 0.80)
    root.addChild(material)
    hints = coin.SoShapeHints()
    hints.vertexOrdering = coin.SoShapeHints.COUNTERCLOCKWISE
    hints.shapeType = coin.SoShapeHints.SOLID
    root.addChild(hints)
    kind = coin.SoType.fromName("SoFCRenderMaterial")
    node = kind.createInstance()
    node.set("water TRUE waterDensity 0")
    root.addChild(node)
    lo, hi, top = -60.0, 60.0, DEPTH
    corners = [(x, y, z) for z in (0.0, top) for y in (lo, hi) for x in (lo, hi)]
    coords = coin.SoCoordinate3()
    coords.point.setValues(0, len(corners), corners)
    root.addChild(coords)
    # the six sides, wound outwards
    quads = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    faces = coin.SoIndexedFaceSet()
    index = []
    for a, b, c, d in quads:
        index += [a, b, c, -1, a, c, d, -1]
    faces.coordIndex.setValues(0, len(index), index)
    root.addChild(faces)
    return root


def run():
    try:
        prefs = FreeCAD.ParamGet(VIEW)
        render = prefs.GetGroup("Render")
        prefs.SetInt("RenderCache", 3)
        render.SetString("Type", "bgfx - OpenGL")
        for name in ("PBR", "WaterSurface", "WaterRefraction", "Volumetric"):
            render.SetBool(name, True)
        render.SetBool("WaterReflection", False)
        render.SetBool("WaterPlanarReflection", False)
        render.SetFloat("WaterWaveStrength", 0.0)
        render.SetFloat("VolumetricDensity", 1e-6)
        render.SetFloat("WaterAbsorption", 0.012)
        render.SetFloat("WaterInscatter", 1.0)
        doc = FreeCAD.newDocument("WaterFilm")
        floor = doc.addObject("Part::Box", "Floor")
        floor.Length, floor.Width, floor.Height = 120, 120, 4
        floor.Placement.Base = FreeCAD.Vector(-60, -60, -4)
        plate = doc.addObject("Part::Box", "Plate")
        plate.Length, plate.Width, plate.Height = 40, 40, 4
        plate.Placement.Base = FreeCAD.Vector(10, -20, DEPTH - FILM - 4)
        doc.recompute()
        for obj in (floor, plate):
            obj.ViewObject.ShapeColor = (0.85, 0.85, 0.85)
            obj.ViewObject.DisplayMode = "Shaded"
        settle(1.0)
        view = FreeCADGui.ActiveDocument.ActiveView
        note("NOTE renderer %s" % render.GetString("Type", ""))
        view.setCameraType("Orthographic")
        view.viewTop()
        view.Render_Light = True
        # oblique: the light's glint off a level surface seen from above
        # would land in the middle of the picture
        view.Render_LightDirection = (-0.6, 0.3, -0.7)
        water = waterBody()
        graph = view.getSceneGraph()
        graph.addChild(water)
        view.fitAll()
        settle(3.0)
        shots = {}
        for tag in ("water", "dry"):
            if tag == "dry":
                graph.removeChild(water)
            view.redraw()
            settle(2.0)
            path = os.path.join(OUT, tag + ".png")
            view.saveRenderDump(path, metadata=False)
            shots[tag] = linear(pixels(path))
        h = shots["water"].shape[0]

        def spot(point):
            x, y = view.getPointOnViewport(FreeCAD.Vector(*point))
            x, y = int(x), int(h - 1 - y)
            return {tag: shot[y - 4 : y + 5, x - 4 : x + 5].reshape(-1, 3).mean(axis=0)
                    for tag, shot in shots.items()}

        deep = spot((-30.0, 0.0, DEPTH))
        film = spot((30.0, 0.0, DEPTH))
        tint = lambda s: float(np.abs(s["water"] - s["dry"]).max())
        note("NOTE deep %s over %s; film %s over %s" % (
            np.round(deep["water"], 3), np.round(deep["dry"], 3),
            np.round(film["water"], 3), np.round(film["dry"], 3)))
        check("the floor under %.0f of water is tinted" % DEPTH, tint(deep) > 0.15,
              "the most a channel moved: %.3f (linear)" % tint(deep))
        check("the plate under %.0f of water is next to clear" % FILM,
              tint(film) < 0.2 * tint(deep),
              "the most a channel moved: %.3f, against %.3f under the deep water"
              % (tint(film), tint(deep)))
        FreeCAD.closeDocument(doc.Name)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        note("DONE")
        QtCore.QTimer.singleShot(0, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, run)
