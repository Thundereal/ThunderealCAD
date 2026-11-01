"""The 3D sketch's curves in edit, each in its own colour (docs/Sketch3D.md sec 7).

The curves are one line set, a polyline per curve, an arc many segments: the
colours are bound per polyline, so a curve after an arc takes its own colour,
not one shifted along by the arc's segments.
  - a free arc is one colour along, and not the external colour;
  - a construction line after it is in the construction colour, apart from
    the arc's and the external edge's;
  - a press on the arc selects the arc, on the line after it the line.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import os
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = App.Vector


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(s=0.4):
    end = time.monotonic() + s
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def run():
    try:
        import Part
        from PIL import Image

        note("RenderCache %s" % App.ParamGet("User parameter:BaseApp/Preferences/View")
             .GetInt("RenderCache", -1))
        doc = App.newDocument("Colours")
        e = doc.addObject("Part::Feature", "Edge")
        e.Shape = Part.LineSegment(V(0, -20, 0), V(30, -20, 0)).toShape()
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        doc.recompute()
        arc = sk.addGeometry(Part.ArcOfCircle(Part.Circle(V(0, 0, 0), V(0, 0, 1), 10), 0, 3))
        line = sk.addGeometry(Part.LineSegment(V(15, 0, 0), V(30, 10, 0)), True)
        sk.addExternal(e, "Edge1")
        doc.recompute()
        gdoc = Gui.getDocument(doc.Name)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.viewTop()
        view.fitAll()
        settle(1.0)
        vp = sk.ViewObject
        vp.setManipulator(V(-30, 30, 0), App.Rotation())
        settle(0.5)
        vpw = view.graphicsView().viewport()
        path = os.path.join(OUT, "curve-colours.png")
        view.saveImage(path, vpw.width(), vpw.height(), "Current")
        img = Image.open(path).convert("RGB")
        h = vpw.height()

        def at(p):
            x, y = vp.toView(p)
            best = None
            # the brightest/most saturated pixel within 2 px of the curve
            for dx in range(-2, 3):
                for dy in range(-2, 3):
                    c = img.getpixel((x + dx, h - 1 - y + dy))
                    score = max(c) - min(c) + sum(c) / 10
                    if best is None or score > best[0]:
                        best = (score, c)
            return best[1]

        import math

        def far(a, b):
            return sum((x - y) ** 2 for x, y in zip(a, b)) > 60**2

        mid = at(sk.Geometry[arc].value(1.5))
        start = at(sk.Geometry[arc].value(0.2))
        construction = at(V(22.5, 5, 0))
        external = at(V(15, -20, 0))
        check("the arc is one colour along", not far(mid, start), (mid, start))
        check("the arc is not in the external colour", far(mid, external), (mid, external))
        check(
            "the construction line after it is in its own colour",
            far(construction, external) and far(construction, mid),
            (construction, external, mid),
        )
        for geo, p in ((arc, sk.Geometry[arc].value(1.5)), (line, V(22.5, 5, 0))):
            vp.clearSelection()
            x, y = vp.toView(p)
            vp.press(x, y)
            vp.release(x, y)
            settle(0.2)
            sel = vp.getSelection()[0]
            check("a press on curve %d selects it" % geo, (geo, 0) in sel, sel)
        gdoc.resetEdit()
        App.closeDocument(doc.Name)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
