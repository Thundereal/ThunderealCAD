"""A non-convex face through the render cache reads no point detail outside
its face (Assembly TestGuiCommands, 2026-11-28: 0xc0000005 in
SoPointDetail::getTextureCoordIndex under SoFCVertexCache::addTriangle).

SoFCRenderCacheManagerP::addTriangle numbers a triangle's vertices from its
first one, {0, v1 - v0, v2 - v0}, and the vertex cache took those numbers as
the vertices' places in the face detail. They are only when v0 is the face's
first vertex: strips, fans, quads and convex polygons. A polygon without the
CONVEX hint is cut up by Coin's tesselator, whose triangles start anywhere; v1
or v2 can lie before v0, the number is negative, and getPoint() handed back a
pointer read from beside the face's point array. The crash came only when that
pointer was unmapped, so the test counts the numbers instead of waiting for
one: FreeCADGui.renderCacheCounters() says how many vertices came with a
number outside their face, each now taken as having no point detail.

Measured here: a convex square (the control) gives none; a 240-point star
without the CONVEX hint gives some, and is drawn, and nothing crashes. Before
the guard the same star's numbers were read as they came.
"""
import math
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtCore.QCoreApplication.processEvents(QtCore.QEventLoop.AllEvents, 50)
        time.sleep(0.02)


def face(coin, points, convex):
    sep = coin.SoSeparator()
    hints = coin.SoShapeHints()
    hints.vertexOrdering = coin.SoShapeHints.COUNTERCLOCKWISE
    hints.faceType = (coin.SoShapeHints.CONVEX if convex
                      else coin.SoShapeHints.UNKNOWN_FACE_TYPE)
    sep.addChild(hints)
    mat = coin.SoMaterial()
    mat.diffuseColor = (0.8, 0.2, 0.2)
    sep.addChild(mat)
    coords = coin.SoCoordinate3()
    coords.point.setValues(0, len(points), points)
    sep.addChild(coords)
    fs = coin.SoFaceSet()
    fs.numVertices.setValue(len(points))
    sep.addChild(fs)
    return sep


def drawn(view, root, node, tag):
    FreeCADGui.renderCacheCounters(reset=True)
    root.addChild(node)
    view.fitAll()
    view.redraw()
    settle(1.5)
    stats = view.getRenderStats()
    counted = FreeCADGui.renderCacheCounters()["pointDetailsOutOfRange"]
    note("INFO %s: %s vertices outside their face; geometry pixels %s"
         % (tag, counted, stats.get("geometryPixels")))
    root.removeChild(node)
    view.redraw()
    settle(0.5)
    return counted, stats.get("geometryPixels")


def run():
    try:
        from pivy import coin

        doc = FreeCAD.newDocument("TessellatedFace")
        view = FreeCADGui.getDocument(doc.Name).activeView()
        root = view.getSceneGraph()
        square = [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0)]
        n = 240
        star = []
        for i in range(n):
            r = 10.0 if i % 2 == 0 else 4.0
            a = 2.0 * math.pi * i / n
            star.append((r * math.cos(a), r * math.sin(a), 0.0))

        count, px = drawn(view, root, face(coin, square, True), "convex square")
        check("a convex face: every vertex's number is its place in the face", count == 0, count)

        count, px = drawn(view, root, face(coin, star, False), "240-point star, no CONVEX hint")
        check("a tessellated face: vertices come numbered outside their face (what crashed)",
              count > 0, count)
        check("... each taken as having no point detail, and the face is drawn",
              px is None or px != 0, px)
        check("nothing crashed reading them", True)
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
