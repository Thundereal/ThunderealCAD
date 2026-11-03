"""Two selected edges of one solid, drawn on top as a selection is by default: is
the one in front as bright as the one behind (docs/HandsOnQueue.md entry 250,
the reporter: "two edge selected the Edge10 much dimmer due to z fight with the
original edge. Edge3 much brigher because the original edge is obscured by the
face")?

A PartDesign box in a Body, made as the reporter made it: the Body and box
commands, the box's task panel left with OK. That edit showed the Body's Origin
and left it in the render feed hidden; its datums' on-top order then ran the
engine's late pass that drew the selection's lines again -- in list order, the
box's plain line after the highlight over it. A box made by the API had no such
order in the feed, which is why three rounds of this found nothing.
GT_CREATE=origin is the box made by the API with the Origin SHOWN, no edit: the
same late pass, and the same fault, by the other way in; GT_CREATE=script is
the box made by the API alone.

The reporter's camera (GT_ISO=1: an exact isometric) and MSAA 4x (the GUI test
profile starts at 0). The box's front top edge is in front of everything, its
rear left vertical edge behind the front and right faces. Both are selected
through the Body: by addSelection, then as a click comes (the edge preselected
first, the preselection gone after).

Measured along each edge, away from its ends and from where the two cross on the
screen: at each sample the pixel nearest to the selection colour within 3 px,
and its distance to it (sum of the channel differences) -- the median of those
is the edge's reading; and whether the plain line's colour is within 1 px of the
edge's centre (the reporter's picture: the plain line down the middle of the
front edge, green only at its sides).

Claims, the render engine and "Legacy" (GT_RENDER_TYPE):
  - the edge in front reads as the selection colour (median distance < 90);
  - so does the edge behind;
  - the plain line shows on neither;
  - the two are alike (medians within 60 of each other).
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SelectedEdgesOnTop"
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
RENDER = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render")
SELECTION = 0x00FF00FF  # the reporter's
LINE_F = (0.098, 0.098, 0.098)  # the reporter's line colour
LINE = (25, 25, 25)
# How the box is made (GT_CREATE): "gui" as the reporter did, the default; "origin"
# by the API with the Body's Origin shown; "script" by the API alone.
MODE = os.environ.get("GT_CREATE", "gui")
GUI = MODE == "gui"
# The reporter's camera, as their session had it. Near and far are its own: how two
# coincident lines resolve in depth depends on them, and the exact isometric of
# viewIsometric() put the rear edge through the front corner.
CAMERA = """#Inventor V2.1 ascii
OrthographicCamera {
  viewportMapping ADJUST_CAMERA
  position 48.881508 -63.280117 61.174068
  orientation 0.86492187 0.23175442 0.44519675  1.0835806
  nearDistance 89.808411
  farDistance 107.47161
  aspectRatio 1
  focalDistance 100
  height 24.6597
}
"""
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
        view().saveRenderDump(path, metadata=True)
    except RuntimeError:
        # no render engine ("Legacy"): the GL widget's own frame buffer
        gl = [c for c in view().graphicsView().findChildren(QtWidgets.QWidget)
              if c.inherits("QOpenGLWidget")]
        if not gl and view().graphicsView().inherits("QOpenGLWidget"):
            gl = [view().graphicsView()]
        pump(0.3)
        gl[0].grab().save(path)
    return QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)


def dist(a, b):
    return sum(abs(x - y) for x, y in zip(a, b))


def edge_where(shape, test):
    found = [i for i, e in enumerate(shape.Edges, 1) if test(e.Vertexes[0].Point,
                                                            e.Vertexes[-1].Point)]
    assert len(found) == 1, found
    return found[0]


def start():
    VIEW.SetBool("ShowNaviCube", False)
    VIEW.SetBool("UseNavigationAnimations", False)
    VIEW.SetBool("Gradient", False)
    VIEW.SetBool("RadialGradient", False)
    VIEW.SetUnsigned("BackgroundColor", 0x3C3C3CFF)  # the reporter's
    VIEW.SetUnsigned("SelectionColor", SELECTION)
    # MSAA 4x, the default and the reporter's: the GUI test profile starts at 0
    # (scripts/gui-test-profile.py), and without multisampling the two lines
    # never fought -- three probes of this entry ran so and found nothing.
    VIEW.SetInt("AntiAliasing", int(os.environ.get("GT_AA", "3")))
    doc = FreeCAD.newDocument(DOC)
    if GUI:
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        FreeCADGui.runCommand("PartDesign_Body")
        return
    body = doc.addObject("PartDesign::Body", "Body")
    box = body.newObject("PartDesign::AdditiveBox", "Box")
    box.Length = box.Width = box.Height = 10
    doc.recompute()
    for vo in (body.ViewObject, box.ViewObject):  # the reporter's
        vo.ShapeColor = (0.8, 0.8, 0.8)
        vo.LineColor = LINE_F
        vo.LineWidth = 2
    if MODE == "origin":
        # the Body's Origin shown: its datums draw in an on-top order of their own
        body.Origin.ViewObject.Visibility = True
        note("INFO the Origin shown")


def gui_box():
    # the toolbar's additive primitive, its first entry: the box, in its task panel
    if GUI:
        FreeCADGui.runCommand("PartDesign_CompPrimitiveAdditive", 0)
        note("INFO the box command; a task panel %s" % FreeCADGui.Control.activeDialog())


def gui_ok():
    if not GUI:
        return
    for bb in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        ok = bb.button(QtWidgets.QDialogButtonBox.Ok)
        if bb.isVisible() and ok is not None and ok.isVisible():
            ok.click()
            note("INFO OK clicked")
            return
    raise RuntimeError("no OK button in the task panel")


def setup():
    doc = FreeCAD.getDocument(DOC)
    box = doc.getObject("Box")
    note("INFO in edit %s, a task panel %s" % (FreeCADGui.getDocument(DOC).getInEdit(),
                                              FreeCADGui.Control.activeDialog()))
    v = view()
    if os.environ.get("GT_ISO"):
        v.viewIsometric()
        v.fitAll()
    else:
        v.setCamera(CAMERA)
    s = box.Shape
    # the front top edge (y 0, z 10) and the rear left one (x 0, y 10, vertical)
    STATE["front"] = edge_where(s, lambda a, b: abs(a.y) < 1e-6 and abs(b.y) < 1e-6
                                and abs(a.z - 10) < 1e-6 and abs(b.z - 10) < 1e-6)
    STATE["rear"] = edge_where(s, lambda a, b: abs(a.x) < 1e-6 and abs(b.x) < 1e-6
                               and abs(a.y - 10) < 1e-6 and abs(b.y - 10) < 1e-6)
    STATE["ends"] = {k: (s.Edges[STATE[k] - 1].Vertexes[0].Point,
                         s.Edges[STATE[k] - 1].Vertexes[-1].Point)
                     for k in ("front", "rear")}
    note("INFO render type %r, AntiAliasing %d; front Edge%d, rear Edge%d" % (
        RENDER.GetString("Type", "Default"), VIEW.GetInt("AntiAliasing", -1),
        STATE["front"], STATE["rear"]))


def screen(p, h):
    x, y = view().getPointOnScreen(p)
    return x, h - 1 - y


def seg_dist(p, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / float(dx * dx + dy * dy)))
    return ((p[0] - ax - t * dx) ** 2 + (p[1] - ay - t * dy) ** 2) ** 0.5


def reading(img, key, other):
    h = img.height()
    sc = ((SELECTION >> 24) & 255, (SELECTION >> 16) & 255, (SELECTION >> 8) & 255)
    a, b = STATE["ends"][key]
    oa, ob = (screen(p, h) for p in STATE["ends"][other])
    got = []
    fought = 0
    for i in range(3, 18):
        t = i / 20.0
        sx, sy = screen(a + (b - a) * t, h)
        if seg_dist((sx, sy), oa, ob) < 12:
            continue
        best = None
        plain = False
        for dy in range(-3, 4):
            for dx in range(-3, 4):
                x, y = int(round(sx)) + dx, int(round(sy)) + dy
                if 0 <= x < img.width() and 0 <= y < h:
                    c = QtGui.QColor(img.pixel(x, y))
                    rgb = (c.red(), c.green(), c.blue())
                    d = dist(rgb, sc)
                    if best is None or d < best[0]:
                        best = (d, rgb)
                    # the plain line at the edge's centre, over its highlight
                    if abs(dx) <= 1 and abs(dy) <= 1 and dist(rgb, LINE) < 40:
                        plain = True
        got.append(best)
        fought += plain
    got.sort()
    return got[len(got) // 2] if got else None, len(got), fought


def select_and_look(clicks):
    def fn():
        look(clicks)
    fn.__name__ = "select_and_look_%s" % ("clicks" if clicks else "plain")
    return fn


def look(clicks):
    doc = FreeCAD.getDocument(DOC)
    body = doc.getObject("Body")
    sel = FreeCADGui.Selection
    sel.clearSelection()
    sel.clearPreselection()
    pump(0.3)
    for key in ("front", "rear"):
        sub = "Box.Edge%d" % STATE[key]
        if clicks:
            # as a click comes: the edge under the mouse first, then selected
            sel.setPreselection(body, sub)
            pump(0.3)
        sel.addSelection(body, sub)
        pump(0.3)
    if clicks:
        # the mouse leaves for somewhere empty
        sel.clearPreselection()
    pump(0.8)
    tag = "clicks" if clicks else "plain"
    note("INFO %s: selected %s" % (tag, [list(s.SubElementNames)
                                         for s in sel.getSelectionEx("", 0)]))
    img = dump("selected-" + tag)
    front, nf, ff = reading(img, "front", "rear")
    rear, nr, fr = reading(img, "rear", "front")
    note("INFO %s: front: median %s of %d samples, plain line at %d; rear: median %s of %d, "
         "plain line at %d" % (tag, front, nf, ff, rear, nr, fr))
    check(tag + ": the edge in front reads as the selection colour",
          front is not None and front[0] < 90, front)
    check(tag + ": so does the edge behind", rear is not None and rear[0] < 90, rear)
    check(tag + ": the plain line shows on neither", ff == 0 and fr == 0,
          "front %d of %d, rear %d of %d" % (ff, nf, fr, nr))
    check(tag + ": the two are alike", front is not None and rear is not None
          and abs(front[0] - rear[0]) < 60, (front, rear))
    sel.clearSelection()


def finish():
    for name in ("ShowNaviCube", "UseNavigationAnimations", "Gradient", "RadialGradient"):
        VIEW.RemBool(name)
    VIEW.RemUnsigned("BackgroundColor")
    VIEW.RemUnsigned("SelectionColor")
    VIEW.SetInt("AntiAliasing", 0)  # as the profile had it
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
STEPS[:] = [start, gui_box, gui_ok, setup, select_and_look(False), select_and_look(True)]
QtCore.QTimer.singleShot(3000, advance)
