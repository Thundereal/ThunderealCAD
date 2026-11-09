"""Which line shows where lines are highlighted over one another: a matrix of
cases, the same on the render engine as on "Legacy".

docs/HandsOnQueue.md entry 250, the reporter: "regression, edge highlight
still partially obscured by the selected line with bgfx backend ... the
selected edge is shown, then the edge and its selection highlight fights
for Z depth. just compare with render type legacy you'll see", then "just
compare all the cases (selection, preselection, unselected highlight over
one and other) with the legacy render". Legacy's rule (entry 243): the
pre-selection over the selection, a highlighted line over every plain line,
a face in front still over it.

Seen from the front (y -oo, orthographic), on a white ground:
  - box A (x 0..10) and box B (x 10..20), side by side: A's front top edge
    is "the same edge"; A's front right edge and B's front left edge lie on
    each other, "coinciding";
  - line F (y -5, z 5, x 30..50) in front of line K (y 5, x 40, z -5..15):
    "crossing", one in front of the other;
  - line H (y 20, z 5, x -5..15) behind both boxes: "hidden", its middle
    behind box A;
  - A's front top left vertex: "point".
Every cell is run twice: first as above, then with an App::Part's Origin
shown at x 70, clear of every cell. Its datums draw in an on-top order of
their own, and that runs the engine's late on-top pass, which draws the
selections' lines once more -- the pass entry 250 was in: it drew them in
list order, the plain whole-on-top line after the highlight it lies on,
covering it at equal depth. A scripted scene has no such order, which is
why the first round of this matrix passed everywhere.
Each object's plain lines in a colour of their own. For each cell the
selection and the pre-selection are set, the view drawn, and the pixels
along the cell's place classed by the nearest of the palette; the cell
holds where its expected colour wins at 90% of the places (the dash cell:
the selection at some, the face at others).

The crops of every cell are saved as cell-<n>.png in GT_OUT (cell-o<n>.png
with the Origin shown), and the
classes in cells.txt, for a side-by-side of the two renderers.
GT_RENDER_TYPE picks the renderer, GT_AA the anti-aliasing (3 = MSAA 4x),
GT_REPORTER_COLOURS=1 the reporter's colours.

Legacy on Mesa's d3d12 GL driver (a GPU under WSL, gui-test.sh --gl nvidia)
does not pass, and not for the engine's reasons: without MSAA it draws the
dashed line solid (no line stipple), with MSAA 4x every highlighted line
wider and washed out. Software GL and native Windows pass.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets
from PySide6 import QtOpenGLWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "HighlightMatrix"
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
RENDER = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render")
V = FreeCAD.Vector
STEPS = []
S = {}

LINES = {"A": (0.6, 0.0, 0.0), "B": (0.2, 0.2, 0.2), "F": (0.6, 0.35, 0.0),
         "K": (0.5, 0.0, 0.5), "H": (0.0, 0.4, 0.4)}


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
        gl = view().graphicsView().findChildren(QtOpenGLWidgets.QOpenGLWidget)
        if not gl and isinstance(view().graphicsView(), QtOpenGLWidgets.QOpenGLWidget):
            gl = [view().graphicsView()]
        pump(0.3)
        # the GL frame itself, its alpha ignored: grab() composites it
        # over the widget's white, where the frame's alpha is not 1 -- with
        # MSAA on a GPU, the faces gone and every colour washed out
        img = gl[0].grabFramebuffer()
        img.reinterpretAsFormat(QtGui.QImage.Format_RGB32)
        img.save(path)
    return QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)


def colour(key, default):
    c = VIEW.GetUnsigned(key, default)
    return ((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255)


def dist(a, b):
    return sum(abs(x - y) for x, y in zip(a, b))


def edge_where(obj, test):
    found = [i for i, e in enumerate(obj.Shape.Edges, 1) if test(e)]
    assert len(found) == 1, (obj.Name, found)
    return "Edge%d" % found[0]


def vertex_where(obj, p):
    found = [i for i, v in enumerate(obj.Shape.Vertexes, 1) if (v.Point - p).Length < 1e-6]
    assert len(found) == 1, (obj.Name, found)
    return "Vertex%d" % found[0]


def start():
    if os.environ.get("GT_AA"):
        VIEW.SetInt("AntiAliasing", int(os.environ["GT_AA"]))
    VIEW.SetBool("ShowNaviCube", False)
    VIEW.SetBool("UseNavigationAnimations", False)
    VIEW.SetBool("Gradient", False)
    VIEW.SetBool("RadialGradient", False)
    VIEW.SetUnsigned("BackgroundColor", 0xFFFFFFFF)
    if os.environ.get("GT_REPORTER_COLOURS"):
        VIEW.SetUnsigned("HighlightColor", 327679)
        VIEW.SetUnsigned("SelectionColor", 16711935)
    doc = FreeCAD.newDocument(DOC)
    a = doc.addObject("Part::Box", "A")
    b = doc.addObject("Part::Box", "B")
    a.Length = a.Width = a.Height = 10
    b.Length = b.Width = b.Height = 10
    b.Placement.Base = V(10, 0, 0)
    lines = {}
    for name, p1, p2 in (("F", V(30, -5, 5), V(50, -5, 5)),
                         ("K", V(40, 5, -5), V(40, 5, 15)),
                         ("H", V(-5, 20, 5), V(15, 20, 5))):
        line = doc.addObject("Part::Line", name)
        line.X1, line.Y1, line.Z1 = p1.x, p1.y, p1.z
        line.X2, line.Y2, line.Z2 = p2.x, p2.y, p2.z
        lines[name] = line
    doc.recompute()
    for obj in [a, b] + list(lines.values()):
        obj.ViewObject.LineColor = LINES[obj.Name]
        obj.ViewObject.LineWidth = 2
        obj.ViewObject.PointColor = LINES[obj.Name]
        obj.ViewObject.PointSize = 6
    S["top"] = edge_where(a, lambda e: all(abs(v.Point.y) < 1e-6 and abs(v.Point.z - 10) < 1e-6
                                           for v in e.Vertexes))
    S["aright"] = edge_where(a, lambda e: all(abs(v.Point.y) < 1e-6 and abs(v.Point.x - 10) < 1e-6
                                              for v in e.Vertexes))
    S["bleft"] = edge_where(b, lambda e: all(abs(v.Point.y) < 1e-6 and abs(v.Point.x - 10) < 1e-6
                                             for v in e.Vertexes))
    S["vertex"] = vertex_where(a, V(0, 0, 10))
    v = view()
    v.setCameraType("Orthographic")
    v.viewFront()
    v.fitAll()
    pump(0.8)
    note("INFO render type %r, AntiAliasing %d; edges top %s, A right %s, B left %s, vertex %s"
         % (RENDER.GetString("Type", "Default"), VIEW.GetInt("AntiAliasing", 3), S["top"],
            S["aright"], S["bleft"], S["vertex"]))


def palette():
    out = {"selection": colour("SelectionColor", 0x1CAD1CFF),
           "preselection": colour("HighlightColor", 0xE1E114FF)}
    for name, c in LINES.items():
        out["line " + name] = tuple(int(round(255 * x)) for x in c)
    return out


def places(where):
    """Screen points (x, row from the top) a cell is judged at"""
    h = S["height"]

    def at(p):
        x, y = view().getPointOnScreen(p)
        return int(round(x)), h - 1 - int(round(y))

    if where == "top":
        return [at(V(x, 0, 10)) for x in (2, 3, 4, 5, 6, 7, 8)]
    if where == "shared":
        return [at(V(10, 0, z)) for z in (2, 3, 4, 5, 6, 7, 8)]
    if where == "cross":
        return [at(V(40, 0, 5))]
    if where == "hidden":
        # every quarter unit: one unit apart, the samples can all land on
        # dashes (or all between them) for the dash cell
        return [at(V(1 + k / 4.0, 20, 5)) for k in range(33)]
    if where == "vertex":
        return [at(V(0, 0, 10))]
    raise ValueError(where)


def classify(img, pts, radius):
    """Per place, the palette name that wins its neighbourhood: the most
    pixels nearer to it than to the ground, the face and the rest"""
    pal = palette()
    ground = [(255, 255, 255)]
    w, h = img.width(), img.height()
    out = []
    for x, y in pts:
        votes = {}
        for dx in range(-radius, radius + 1):
            for dy in range(-radius, radius + 1):
                if not (0 <= x + dx < w and 0 <= y + dy < h):
                    continue
                c = QtGui.QColor(img.pixel(x + dx, y + dy))
                rgb = (c.red(), c.green(), c.blue())
                name, d = min(pal.items(), key=lambda kv: dist(rgb, kv[1])), None
                d = dist(rgb, name[1])
                if d < 120 and all(d < dist(rgb, g) for g in ground):
                    votes[name[0]] = votes.get(name[0], 0) + 1
        out.append(max(votes, key=votes.get) if votes else "other")
    return out


# (label, selected [(object, element)], pre-selected (object, element) or
#  None, where, expected winner, SelectionLinePattern)
def cells():
    top, ar, bl, vx = S["top"], S["aright"], S["bleft"], S["vertex"]
    return [
        ("same edge: selected", [("A", top)], None, "top", "selection", 0),
        ("same edge: selected and pre-selected", [("A", top)], ("A", top), "top",
         "preselection", 0),
        ("same edge: pre-selected", [], ("A", top), "top", "preselection", 0),
        ("coinciding: A selected", [("A", ar)], None, "shared", "selection", 0),
        ("coinciding: A pre-selected", [], ("A", ar), "shared", "preselection", 0),
        ("coinciding: A selected, B pre-selected", [("A", ar)], ("B", bl), "shared",
         "preselection", 0),
        ("coinciding: B selected, A pre-selected", [("B", bl)], ("A", ar), "shared",
         "preselection", 0),
        ("coinciding: both selected", [("A", ar), ("B", bl)], None, "shared", "selection", 0),
        ("coinciding: both selected, A pre-selected", [("A", ar), ("B", bl)], ("A", ar),
         "shared", "preselection", 0),
        ("crossing: the back line selected", [("K", "Edge1")], None, "cross", "selection", 0),
        ("crossing: the back line pre-selected", [], ("K", "Edge1"), "cross",
         "preselection", 0),
        ("crossing: the front selected, the back pre-selected", [("F", "Edge1")],
         ("K", "Edge1"), "cross", "preselection", 0),
        ("crossing: the back selected, the front pre-selected", [("K", "Edge1")],
         ("F", "Edge1"), "cross", "preselection", 0),
        ("crossing: the front selected", [("F", "Edge1")], None, "cross", "selection", 0),
        ("hidden: selected, solid", [("H", "Edge1")], None, "hidden", "selection", 0),
        ("hidden: selected and pre-selected, solid", [("H", "Edge1")], ("H", "Edge1"),
         "hidden", "preselection", 0),
        ("hidden: selected, dashed", [("H", "Edge1")], None, "hidden", "dashed", 0xF0F0),
        ("point: selected", [("A", vx)], None, "vertex", "selection", 0),
        ("point: selected and pre-selected", [("A", vx)], ("A", vx), "vertex",
         "preselection", 0),
    ]


def show_origin():
    """An App::Part at x 70 with its Origin shown: an on-top order in the feed"""
    doc = FreeCAD.getDocument(DOC)
    part = doc.addObject("App::Part", "Part")
    part.Placement.Base = V(70, 0, 5)
    doc.recompute()
    part.Origin.ViewObject.Visibility = True
    view().fitAll()
    pump(0.8)
    note("INFO the Origin shown")


def run_cells(tag=""):
    doc = FreeCAD.getDocument(DOC)
    img = dump("plain" + tag)
    S["height"] = img.height()
    rows = []
    for n, (label, sel, pre, where, want, pattern) in enumerate(cells(), 1):
        if tag:
            label = "the Origin shown, " + label
        VIEW.SetInt("SelectionLinePattern", pattern)
        FreeCADGui.Selection.clearPreselection()
        FreeCADGui.Selection.clearSelection()
        pump(0.3)
        for obj, sub in sel:
            FreeCADGui.Selection.addSelection(doc.getObject(obj), sub)
        if pre:
            FreeCADGui.Selection.setPreselection(doc.getObject(pre[0]), pre[1])
        pump(0.8)
        img = dump("cell-%s%02d-full" % (tag, n))
        pts = places(where)
        # the point's dot is 6 px: wider. Not the crossing: there the
        # arms of both lines outvote the pixels where they overlap
        radius = 3 if where == "vertex" else 1
        got = classify(img, pts, radius)
        # for the eye: the place, eight times
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        cx, cy = (min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2
        half = max(12, (max(max(xs) - min(xs), max(ys) - min(ys)) // 2) + 8)
        img.copy(cx - half, cy - half, 2 * half, 2 * half).scaled(
            200, 200, QtCore.Qt.KeepAspectRatio).save(
                os.path.join(OUT, "cell-%s%02d.png" % (tag, n)))
        os.remove(os.path.join(OUT, "cell-%s%02d-full.png" % (tag, n)))
        if want == "dashed":
            ok = got.count("selection") >= 0.2 * len(got) and got.count("selection") < len(got)
        else:
            ok = got.count(want) >= 0.9 * len(got)
        check("%s%02d %s: %s wins" % (tag, n, label, want), ok, got)
        rows.append("%s%02d\t%s\t%s\t%s" % (tag, n, label, want, ",".join(got)))
    FreeCADGui.Selection.clearPreselection()
    FreeCADGui.Selection.clearSelection()
    with open(os.path.join(OUT, "cells.txt"), "a", encoding="utf-8") as f:
        f.write("\n".join(rows) + "\n")


def finish():
    VIEW.RemInt("SelectionLinePattern")
    for name in ("Gradient", "RadialGradient", "ShowNaviCube", "UseNavigationAnimations"):
        VIEW.RemBool(name)
    for name in ("BackgroundColor", "HighlightColor", "SelectionColor"):
        VIEW.RemUnsigned(name)
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
STEPS[:] = [start, run_cells, show_origin, lambda: run_cells("o")]
QtCore.QTimer.singleShot(3000, advance)
