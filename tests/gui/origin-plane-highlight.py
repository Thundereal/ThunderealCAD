"""An origin plane, highlighted, keeps its size, keeps the pointer across its
whole face, and is highlighted as a face is (docs/HandsOnQueue.md entry 235,
the reporter 2026-10-28: "the plane then shows in full, but no highlight
color. and also when the mouse move off the edge, it immediate remove the
pre-selection, i.e. the plane goes back to quater size ... I want the plane
to have whole face high picking priority once it is already highlighted,
and I want highlight like other object, following the outline/color
setting, and do not change the plane size when (pre)selected. same for
selection behavior").

A body's origin seen from the top, a box under it (behind the XY plane,
which a pick through the plane's face would find). The pointer is moved by
Qt events. Runs on a real screen: the pictures are saveRenderDump's.

Claims:
  - the pointer on the XY plane's edge preselects it;
  - the plane keeps its size: outside the corner it is drawn as, the
    picture is as it was;
  - it is highlighted in the preselection colour along all four sides of
    that corner (its outline);
  - the pointer brought across the face to its middle, over the box behind
    it, keeps it preselected;
  - clicked, it is selected, keeps its size, and the selection colour runs
    along all four sides;
  - the pointer taken far off, nothing is preselected.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets
from PySide6 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "OriginPlaneHighlight"
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
Qt = QtCore.Qt
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
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def mw():
    return FreeCADGui.getMainWindow()


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def to_window(x, y):
    """A viewport pixel (y up) as a point of the main window"""
    width, height = view().getSize()
    w = view().graphicsView()
    local = QtCore.QPoint(int(x), int(height - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def move(x, y):
    QtTest.QTest.mouseMove(mw().windowHandle(), to_window(x, y))
    pump(0.15)


def glide(a, b, steps=10):
    for i in range(1, steps + 1):
        move(a[0] + (b[0] - a[0]) * i / steps, a[1] + (b[1] - a[1]) * i / steps)


def picked(x, y):
    info = view().getObjectInfo((int(x), int(y)))
    return info["Object"] if info else None


def presel():
    """The preselection as a path: the top object and its subname (a body's
    origin plane is Body.Origin.XY_Plane.)"""
    p = FreeCADGui.Selection.getPreselection()
    try:
        if not p or not p.Object:
            return None
        subs = p.SubElementNames
        return p.Object.Name + "." + (subs[0] if subs else "")
    except Exception:
        return None


def dump(name):
    path = os.path.join(OUT, name + ".png")
    try:
        view().saveRenderDump(path, metadata=False)
    except RuntimeError:
        # no render engine (Render/Type Legacy): what the screen shows of the
        # view, the window in front
        mw().raise_()
        mw().activateWindow()
        pump(0.5)
        w = view().graphicsView()
        w = w.viewport() if hasattr(w, "viewport") else w
        g = w.mapToGlobal(QtCore.QPoint(0, 0))
        screen = QtGui.QGuiApplication.screenAt(g) or QtGui.QGuiApplication.primaryScreen()
        sg = screen.geometry()
        screen.grabWindow(0, g.x() - sg.x(), g.y() - sg.y(), w.width(), w.height()).save(path)
    return QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)


def colour(key, default):
    c = VIEW.GetUnsigned(key, default)
    return ((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255)


def near_colour(px, rgb, tol=60):
    c = QtGui.QColor(px)
    return abs(c.red() - rgb[0]) + abs(c.green() - rgb[1]) + abs(c.blue() - rgb[2]) <= tol


def changed_outside(a, b, box, margin=12):
    """Pixels that differ outside the box (image coordinates, y down), and
    within as far again of it each way: where the whole plane would be drawn
    if it grew (the rest of a screen grab may differ by the frame)"""
    n = 0
    around = box.adjusted(-box.width(), -box.height(), box.width(), box.height())
    for y in range(max(0, around.top()), min(a.height(), around.bottom()), 2):
        for x in range(max(0, around.left()), min(a.width(), around.right()), 2):
            if (box.left() - margin <= x <= box.right() + margin
                    and box.top() - margin <= y <= box.bottom() + margin):
                continue
            if a.pixel(x, y) != b.pixel(x, y):
                n += 1
    return n


def sides(img, box, rgb):
    """For each side of the box, whether pixels of the colour lie along it"""
    out = {}
    for name, pts in (
            ("top", [(x, box.top()) for x in range(box.left() + 4, box.right() - 4, 2)]),
            ("bottom", [(x, box.bottom()) for x in range(box.left() + 4, box.right() - 4, 2)]),
            ("left", [(box.left(), y) for y in range(box.top() + 4, box.bottom() - 4, 2)]),
            ("right", [(box.right(), y) for y in range(box.top() + 4, box.bottom() - 4, 2)])):
        hits = 0
        for x, y in pts:
            if any(near_colour(img.pixel(x + dx, y + dy), rgb)
                   for dx in range(-5, 6) for dy in range(-5, 6)
                   if 0 <= x + dx < img.width() and 0 <= y + dy < img.height()):
                hits += 1
        out[name] = (hits, len(pts))
    return out


def coloured(img, box, rgb, margin=8):
    """How many pixels of the colour on the plane's corner and around it"""
    n = 0
    for y in range(max(0, box.top() - margin), min(img.height(), box.bottom() + margin)):
        for x in range(max(0, box.left() - margin), min(img.width(), box.right() + margin)):
            if near_colour(img.pixel(x, y), rgb):
                n += 1
    return n


def start():
    # GT_RENDER_TYPE: "Legacy" draws through the render cache's own OpenGL
    # renderer, unset through the render engine (bgfx)
    kind = os.environ.get("GT_RENDER_TYPE")
    if kind:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString("Type", kind)
    note("INFO render type %r" % FreeCAD.ParamGet(
        "User parameter:BaseApp/Preferences/View/Render").GetString("Type", "Default"))
    VIEW.SetBool("ShowNaviCube", False)
    VIEW.SetBool("UseNavigationAnimations", False)
    doc = FreeCAD.newDocument(DOC)
    body = doc.addObject("PartDesign::Body", "Body")
    box = doc.addObject("Part::Box", "Under")
    box.Length, box.Width, box.Height = 200, 200, 5
    box.Placement.Base = FreeCAD.Vector(-100, -100, -40)
    doc.recompute()
    body.Origin.ViewObject.Visibility = True
    for f in body.Origin.OriginFeatures:
        f.ViewObject.Visibility = True
    v = view()
    v.viewTop()
    v.fitAll()


def find_plane():
    width, height = view().getSize()
    hits = []
    for y in range(0, height, 4):
        for x in range(0, width, 4):
            if (picked(x, y) or "").startswith("XY_Plane"):
                hits.append((x, y))
    if not check("the XY plane is somewhere to point at", len(hits) > 4, len(hits)):
        STEPS[:] = []
        return
    xs = [h[0] for h in hits]
    ys = [h[1] for h in hits]
    STATE["bbox"] = (min(xs), min(ys), max(xs), max(ys))
    # on its edge, where its line is: the left side, halfway up (the face
    # in from the edges has the box behind it, which is picked there until
    # the plane is highlighted)
    STATE["edge"] = (min(xs) + 1, (min(ys) + max(ys)) // 2)
    STATE["middle"] = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    note("INFO the XY plane picked at %d points, from %s to %s; middle %s, the box there: %s" % (
        len(hits), (min(xs), min(ys)), (max(xs), max(ys)), STATE["middle"],
        picked(*STATE["middle"])))
    FreeCADGui.Selection.clearSelection()
    move(5, 5)
    STATE["rest"] = dump("0-rest")


def img_box():
    """The plane's corner in image coordinates (y down), from its picks"""
    x0, y0, x1, y1 = STATE["bbox"]
    h = STATE["rest"].height()
    return QtCore.QRect(QtCore.QPoint(x0, h - 1 - y1), QtCore.QPoint(x1, h - 1 - y0))


def hover_edge():
    # glided in from outside, then a pixel at a time across the edge's line
    # (found on a grid of four)
    x, y = STATE["edge"]
    glide((x - 40, y), (x - 4, y), 6)
    for dx in range(-4, 5):
        move(x + dx, y)
        if "XY_Plane" in (presel() or ""):
            STATE["edge"] = (x + dx, y)
            break
    check("the pointer on the XY plane's edge preselects it",
          "XY_Plane" in (presel() or ""), presel())
    img = dump("1-preselected")
    box = img_box()
    n = changed_outside(STATE["rest"], img, box)
    check("preselected, the plane keeps its size: outside its corner the picture is as it was",
          n < 40, "%d pixels changed outside %s" % (n, box.getRect()))
    hl = colour("HighlightColor", 0xE1E114FF)
    s = sides(img, box, hl)
    note("INFO preselection colour %s along the sides: %s; %d pixels of it on the plane" % (
        hl, s, coloured(img, box, hl)))
    check("preselected, the preselection colour runs along all four sides",
          all(h >= t * 0.5 for h, t in s.values()), s)


def across():
    glide(STATE["edge"], STATE["middle"])
    check("the pointer brought across the face to its middle keeps it preselected",
          "XY_Plane" in (presel() or ""),
          "%s; a pick there now finds %s" % (presel(), picked(*STATE["middle"])))


def click():
    win = mw().windowHandle()
    p = to_window(*STATE["edge"])
    QtTest.QTest.mouseMove(win, p)
    pump(0.1)
    QtTest.QTest.mouseClick(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(0.5)
    sel = [o.Name for o in FreeCADGui.Selection.getSelection()]
    check("clicked, it is selected", any(n.startswith("XY_Plane") for n in sel), sel)
    move(5, 5)
    pump(0.5)
    img = dump("2-selected")
    box = img_box()
    n = changed_outside(STATE["rest"], img, box)
    check("selected, the plane keeps its size", n < 40,
          "%d pixels changed outside %s" % (n, box.getRect()))
    sc = colour("SelectionColor", 0x1CAD1CFF)
    s = sides(img, box, sc)
    note("INFO selection colour %s along the sides: %s; %d pixels of it on the plane" % (
        sc, s, coloured(img, box, sc)))
    check("selected, the selection colour runs along all four sides",
          all(h >= t * 0.5 for h, t in s.values()), s)
    check("the pointer taken off, nothing is preselected", presel() is None, presel())


def box_face():
    """For the eye: the box's top face preselected and selected, under the
    same settings, beside the plane's pictures (crops of both saved)"""
    FreeCADGui.Selection.clearSelection()
    x0, y0, x1, y1 = STATE["bbox"]
    spot = (x0 - 3 * (x1 - x0) // 2, (y0 + y1) // 2)
    move(*spot)
    note("INFO the box's face under the pointer: %s" % presel())
    img = dump("3-box-preselected")
    win = mw().windowHandle()
    p = to_window(*spot)
    QtTest.QTest.mouseClick(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(0.5)
    move(5, 5)
    pump(0.5)
    img2 = dump("4-box-selected")
    h = img.height()
    box = QtCore.QRect(QtCore.QPoint(spot[0] - 60, h - 1 - spot[1] - 60), QtCore.QSize(120, 120))
    plane = img_box().adjusted(-20, -20, 20, 20)
    for name, im, r in (("crop-plane-preselected", QtGui.QImage(os.path.join(OUT, "1-preselected.png")), plane),
                        ("crop-plane-selected", QtGui.QImage(os.path.join(OUT, "2-selected.png")), plane),
                        ("crop-box-preselected", img, box), ("crop-box-selected", img2, box)):
        im.copy(r).save(os.path.join(OUT, name + ".png"))
    FreeCADGui.Selection.clearSelection()


def finish():
    FreeCADGui.Selection.clearSelection()
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


def advance():
    if not STEPS:
        finish()
        return
    step = STEPS.pop(0)
    try:
        step()
    except Exception:
        note("ABORT in %s: %s" % (step.__name__, traceback.format_exc().replace("\n", " | ")))
        STEPS[:] = []
    QtCore.QTimer.singleShot(1500, advance)


STEPS[:] = [start, find_plane, hover_edge, across, click, box_face]
QtCore.QTimer.singleShot(3000, advance)
