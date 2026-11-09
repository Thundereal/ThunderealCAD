"""A selected edge's part hidden behind a face, where a plain line lies in
front of it: is the hidden part drawn over the line (docs/HandsOnQueue.md
entry 243 b, the reporter: "when a selected edge is behind and shows dashed
lines, does the dashed line show on top of the original edge? if the
hidden selection edge style does not override, i.e. shows solid edge, it
should be shown on top")?

Seen from the front (y -oo): a box in front (x 0..20, z 10..20, y 0..5) hides the
part x 10..20 of the front bottom edge of a box behind it (x 10..30,
z 15..30, y 20..25), and a red line in front of both (y -5) lies over that
hidden part, x 12..18 at z 15. The edge is selected by itself.

Measured along the line, column by column: the pixel on the row of the
line nearest to the selection colour, classed as selection (its colour or
a dimmed one: nearer to it than to the line's red), line (red), or other.

Claims, render engine and "Legacy", SelectionLinePattern 0 (solid, the
default) and a dash pattern:
  - pattern 0: the hidden part of the selected edge is drawn over the line
    along all of it;
  - a dash pattern: its dashes are drawn over the line, between them the
    line shows (both seen along it).
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "HiddenSelOverLine"
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
RENDER = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render")
STEPS = []
STATE = {}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds=0.3):
    import time

    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def dump(name):
    path = os.path.join(OUT, name + ".png")
    try:
        view().saveRenderDump(path, metadata=False)
    except RuntimeError:
        # no render engine ("Legacy"): the GL widget's own frame buffer
        gl = [c for c in view().graphicsView().findChildren(QtWidgets.QWidget)
              if c.inherits("QOpenGLWidget")]
        if not gl and view().graphicsView().inherits("QOpenGLWidget"):
            gl = [view().graphicsView()]
        pump(0.3)
        gl[0].grab().save(path)
    return QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)


def colour(key, default):
    c = VIEW.GetUnsigned(key, default)
    return ((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255)


def dist(a, b):
    return sum(abs(x - y) for x, y in zip(a, b))


def start():
    VIEW.SetBool("ShowNaviCube", False)
    VIEW.SetBool("UseNavigationAnimations", False)
    VIEW.SetBool("Gradient", False)
    VIEW.SetBool("RadialGradient", False)
    VIEW.SetUnsigned("BackgroundColor", 0xFFFFFFFF)
    doc = FreeCAD.newDocument(DOC)
    front = doc.addObject("Part::Box", "Front")
    front.Length, front.Width, front.Height = 20, 5, 10
    front.Placement.Base = FreeCAD.Vector(0, 0, 10)
    back = doc.addObject("Part::Box", "Back")
    back.Length, back.Width, back.Height = 20, 5, 15
    back.Placement.Base = FreeCAD.Vector(10, 20, 15)
    line = doc.addObject("Part::Line", "Line")
    line.X1, line.Y1, line.Z1 = 12, -5, 15
    line.X2, line.Y2, line.Z2 = 18, -5, 15
    doc.recompute()
    line.ViewObject.LineColor = (1.0, 0.0, 0.0)
    line.ViewObject.LineWidth = 2
    for o in (front, back):
        o.ViewObject.LineColor = (0.2, 0.2, 0.2)
    v = view()
    # from y -oo: the front box nearest, the line in front of it
    v.viewFront()
    v.fitAll()
    # the edge of the back box at y 20, z 15
    for i, e in enumerate(back.Shape.Edges, 1):
        a, b = e.Vertexes[0].Point, e.Vertexes[-1].Point
        if abs(a.y - 20) < 1e-6 and abs(b.y - 20) < 1e-6 and abs(a.z - 15) < 1e-6 \
                and abs(b.z - 15) < 1e-6:
            STATE["edge"] = "Edge%d" % i
    note("INFO render type %r; the edge %s" % (RENDER.GetString("Type", "Default"),
                                                STATE.get("edge")))


def measure(tag):
    """Columns along the line: (selection, line, other)"""
    img = dump(tag)
    h = img.height()
    sc = colour("SelectionColor", 0x1CAD1CFF)
    red = (255, 0, 0)
    a = view().getPointOnScreen(FreeCAD.Vector(12.5, -5, 15))
    b = view().getPointOnScreen(FreeCAD.Vector(17.5, -5, 15))
    row = h - 1 - int(round(a[1]))
    out = {"selection": 0, "line": 0, "other": 0}
    x0, x1 = sorted((int(round(a[0])), int(round(b[0]))))
    # for the eye: the line and a little around it, four times
    img.copy(x0 - 40, row - 15, x1 - x0 + 80, 30).scaled(4 * (x1 - x0 + 80), 120).save(
        os.path.join(OUT, "zoom-" + tag + ".png"))
    for x in range(x0, x1 + 1):
        column = []
        for dy in range(-3, 4):
            y = row + dy
            if 0 <= y < h:
                c = QtGui.QColor(img.pixel(x, y))
                column.append((c.red(), c.green(), c.blue()))
        if any(dist(p, sc) < dist(p, red) and dist(p, sc) < 300 for p in column):
            out["selection"] += 1
        # the line's red, its 2 px blended with the ground: reddish
        elif any(p[0] > p[1] + 60 and p[0] > p[2] + 40 for p in column):
            out["line"] += 1
        else:
            out["other"] += 1
    return out


def select_and_look(pattern):
    def fn():
        VIEW.SetInt("SelectionLinePattern", pattern)
        FreeCADGui.Selection.clearSelection()
        pump(0.3)
        FreeCADGui.Selection.addSelection(FreeCAD.getDocument(DOC).getObject("Back"),
                                          STATE["edge"])
        pump(0.8)
        got = measure("pattern-%x" % pattern)
        note("INFO pattern %x: along the line %s" % (pattern, got))
        total = sum(got.values())
        if pattern == 0:
            check("pattern 0: the hidden part of the selected edge over the line, all along",
                  total > 0 and got["selection"] >= 0.9 * total, got)
        else:
            check("a dash pattern: its dashes over the line, the line between them",
                  total > 0 and got["selection"] >= 0.3 * total
                  and got["line"] >= 0.1 * total, got)
        FreeCADGui.Selection.clearSelection()
    return fn


def finish():
    VIEW.RemInt("SelectionLinePattern")
    for name in ("Gradient", "RadialGradient"):
        VIEW.RemBool(name)
    VIEW.RemUnsigned("BackgroundColor")
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


def advance():
    if not STEPS:
        finish()
        return
    step = STEPS.pop(0)
    try:
        step()
    except Exception:
        note("ABORT in %s: %s" % (getattr(step, "__name__", step),
                                  traceback.format_exc().replace("\n", " | ")))
        STEPS[:] = []
    QtCore.QTimer.singleShot(1000, advance)


kind = os.environ.get("GT_RENDER_TYPE")
if kind:
    RENDER.SetString("Type", kind)
STEPS[:] = [start, select_and_look(0), select_and_look(0xF0F0)]
QtCore.QTimer.singleShot(3000, advance)
