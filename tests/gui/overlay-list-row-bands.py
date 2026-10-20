"""Every list in an overlay has the property view's alternating rows
(docs/HandsOnQueue.md entry 193).

The reporter, 2026-10-20 09:18: "style all list view in overlay panels with
alternating background with transparency just like property view. previous
request for transparent background is in, but not the alternating item
background"; and "193, one painter for all": the overlay paints the bands of
every list, tree and table of an overlaid panel, the property view and the
selection view included, which painted their own until then. Not the model
tree (the reporter: "Keep tree's look"): its ground behind each item's text
is overlay-see-through.py's.

Claims, under Dark, every panel overlaid and see-through:
  - a fillet in edit, its edge list of three rows in the task panel: each
    row is backed across the list at the overlays' opacity, the first and
    the third the items' ground, the second the palette's alternate colour;
    the list's empty part below them stays see-through (entry 184);
  - the property view, the box selected: its rows backed by the same two
    grounds at the same opacity, two rows one under the other never the
    same, as before (read off the picture: the heights the view reports for
    its rows are not those it paints with);
  - docked again, the task list's rows have no band at the overlays'
    opacity: the bands are the overlay's alone.

The lists are rendered through their parent onto a clear image: the widget
alone reads clear, or its ground twice. Runs with QT_QPA_PLATFORM=offscreen;
the pointer is not moved.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
STEPS = []
S = {}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()


def mw():
    return FreeCADGui.getMainWindow()


def cls(w):
    return w.metaObject().className()


def overlay_of(w):
    while w is not None:
        if cls(w) == "Gui::OverlayTabWidget":
            return w
        w = w.parentWidget()
    return None


def eye(tab, on):
    for b in tab.findChildren(QtWidgets.QAbstractButton):
        if b.objectName() == "OBTN Transparent":
            if b.isChecked() != on:
                b.click()
                settle(1000)
            return b.isChecked() == on
    return False


def front(tab, widget):
    for i in range(tab.count()):
        if tab.widget(i) is not None and tab.widget(i).isAncestorOf(widget):
            tab.setCurrentIndex(i)
    settle(400)


def rgb_of(packed):
    return ((packed >> 24) & 255, (packed >> 16) & 255, (packed >> 8) & 255)


def row_grounds(view, rows):
    """Each row's commonest colour across the list, (r, g, b, a), as the
    list's parent renders it onto a clear image; and the alpha of the
    list's empty part"""
    vp = view.viewport()
    parent = view.parentWidget()
    img = QtGui.QImage(parent.size(), QtGui.QImage.Format_ARGB32)
    img.fill(QtCore.Qt.transparent)
    parent.render(img, QtCore.QPoint(), QtGui.QRegion(),
                  QtWidgets.QWidget.RenderFlag.DrawChildren)
    got = []
    for rect in rows:
        seen = {}
        for y in (rect.top() + 1, rect.center().y(), rect.bottom() - 1):
            for x in range(2, vp.width() - 2, 2):
                at = vp.mapTo(parent, QtCore.QPoint(x, y))
                if img.rect().contains(at):
                    v = img.pixel(at)
                    seen[v] = seen.get(v, 0) + 1
        v = max(seen, key=seen.get) if seen else 0
        got.append(((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255))
    at = vp.mapTo(parent, QtCore.QPoint(vp.width() // 2, vp.height() - 4))
    empty = QtGui.QColor.fromRgba(img.pixel(at.x(), at.y())).alpha() \
        if img.rect().contains(at) else None
    return got, empty


def near(a, b, tol=6):
    return b is None or max(abs(a[i] - b[i]) for i in range(3)) <= tol


def start():
    check("the Dark theme applies", FreeCADGui.applyTheme("Dark"))
    settle(2000)
    doc = FreeCAD.newDocument("RowBands")
    body = doc.addObject("PartDesign::Body", "Body")
    box = body.newObject("PartDesign::AdditiveBox", "Box")
    doc.recompute()
    fillet = body.newObject("PartDesign::Fillet", "Fillet")
    fillet.Base = (box, ["Edge1", "Edge2", "Edge3"])
    fillet.Radius = 1
    doc.recompute()
    S["doc"], S["box"], S["fillet"] = doc, box, fillet
    FreeCADGui.runCommand("Std_DockOverlayAll")
    settle(2500)
    for tab in mw().findChildren(QtWidgets.QWidget):
        if cls(tab) == "Gui::OverlayTabWidget" and tab.count():
            eye(tab, True)
    settle(800)
    S["alpha"] = VIEW.GetInt("DockOverlayAlpha", 180)
    first = rgb_of(VIEW.GetUnsigned("BackgroundColor", 0x3C3C3CFF))
    if VIEW.GetBool("Gradient", False) or VIEW.GetBool("RadialGradient", False):
        note("INFO the background is a gradient here: the items' ground is not compared by colour")
        first = None
    S["first"] = first


def task_list():
    gdoc = FreeCADGui.getDocument(S["doc"].Name)
    gdoc.setEdit(S["fillet"], 0)
    settle(3000)
    view = None
    for w in mw().findChildren(QtWidgets.QAbstractItemView):
        p = w.parentWidget()
        while p is not None and cls(p) != "Gui::TaskView::TaskView":
            p = p.parentWidget()
        if p is not None and w.isVisible() and w.model() is not None \
                and w.model().rowCount() == 3:
            view = w
            break
    tab = overlay_of(view)
    if not check("a fillet's three edges listed in its task panel, overlaid",
                 view is not None and tab is not None, cls(view) if view else None):
        return
    front(tab, view)
    settle(800)
    S["task"] = view
    model = view.model()
    rows = [view.visualRect(model.index(r, 0)) for r in range(3)]
    got, empty = row_grounds(view, rows)
    alternate = view.palette().color(QtGui.QPalette.AlternateBase)
    alternate = (alternate.red(), alternate.green(), alternate.blue())
    alpha = S["alpha"]
    note("INFO task list rows %s, empty part alpha %s; overlay alpha %d, alternate %s, first %s"
         % (got, empty, alpha, alternate, S["first"]))
    for r, want in ((0, S["first"]), (1, alternate), (2, S["first"])):
        check("193: the task list's row %d is backed at the overlays' opacity, %s"
              % (r + 1, "the alternate colour" if r == 1 else "the items' ground"),
              abs(got[r][3] - alpha) <= 6 and near(got[r], want), got[r])
    check("193: its rows alternate", got[0][:3] != got[1][:3] and near(got[0], got[2], 2),
          got)
    check("184: the list's empty part stays see-through", empty is not None and empty < 64,
          empty)


def property_view():
    gdoc = FreeCADGui.getDocument(S["doc"].Name)
    gdoc.resetEdit()
    settle(1500)
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(S["doc"].Name, S["box"].Name)
    settle(1200)
    view = None
    for w in mw().findChildren(QtWidgets.QTreeView):
        if cls(w) == "Gui::PropertyEditor::PropertyEditor" and overlay_of(w) is not None                 and w.model() is not None and w.model().rowCount() > 0:
            view = w
            break
    if not check("the property view is overlaid, with rows", view is not None):
        return
    front(overlay_of(view), view)
    settle(800)
    # Read off the picture, line by line: the commonest colour of each line
    # of the view, and the runs of them. Its rows' heights as the view
    # reports them are not those it paints with (a row with a colour to show
    # is taller), so the bands are found where they are painted.
    vp = view.viewport()
    parent = view.parentWidget()
    img = QtGui.QImage(parent.size(), QtGui.QImage.Format_ARGB32)
    img.fill(QtCore.Qt.transparent)
    parent.render(img, QtCore.QPoint(), QtGui.QRegion(),
                  QtWidgets.QWidget.RenderFlag.DrawChildren)
    runs = []
    for y in range(vp.height()):
        seen = {}
        for x in range(2, vp.width() - 2, 2):
            at = vp.mapTo(parent, QtCore.QPoint(x, y))
            if img.rect().contains(at):
                v = img.pixel(at)
                seen[v] = seen.get(v, 0) + 1
        if not seen:
            continue
        v = max(seen, key=seen.get)
        c = ((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255)
        if runs and runs[-1][0] == c and runs[-1][2] == y - 1:
            runs[-1][2] = y
        else:
            runs.append([c, y, y])
    alpha = S["alpha"]
    alternate = view.palette().color(QtGui.QPalette.AlternateBase)
    alternate = (alternate.red(), alternate.green(), alternate.blue())
    bands = [r for r in runs if abs(r[0][3] - alpha) <= 6 and r[2] - r[1] >= 8]
    note("INFO property view bands (colour, from, to): %s" % ([tuple(r) for r in bands],))
    check("193: the property view's rows are backed at the overlays' opacity",
          len(bands) >= 3, len(bands))
    kinds = set()
    for r in bands:
        if near(r[0], alternate):
            kinds.add("alternate")
        elif near(r[0], S["first"]):
            kinds.add("ground")
        else:
            kinds.add(r[0])
    check("193: ... the items' ground and the alternate colour, nothing else",
          kinds == {"alternate", "ground"} or (S["first"] is None and "alternate" in kinds),
          kinds)
    # one under the other: at most the grid line between them
    touching = [(p[0], q[0]) for p, q in zip(bands, bands[1:]) if q[1] - p[2] <= 3]
    check("193: ... two rows one under the other alternate",
          touching and all(p[:3] != q[:3] for p, q in touching), touching)


def docked():
    FreeCADGui.runCommand("Std_DockOverlayAll")
    settle(2500)
    gdoc = FreeCADGui.getDocument(S["doc"].Name)
    gdoc.setEdit(S["fillet"], 0)
    settle(3000)
    view = None
    for w in mw().findChildren(QtWidgets.QAbstractItemView):
        p = w.parentWidget()
        while p is not None and cls(p) != "Gui::TaskView::TaskView":
            p = p.parentWidget()
        if p is not None and w.isVisible() and w.model() is not None \
                and w.model().rowCount() == 3:
            view = w
            break
    if not check("docked again, the task list", view is not None and overlay_of(view) is None):
        return
    model = view.model()
    got, _ = row_grounds(view, [view.visualRect(model.index(r, 0)) for r in range(2)])
    note("INFO docked task list rows %s" % (got,))
    alpha = S["alpha"]
    check("193: docked, no band at the overlays' opacity",
          all(abs(g[3] - alpha) > 6 for g in got), got)
    gdoc.resetEdit()
    settle(800)


def finish():
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
    QtCore.QTimer.singleShot(800, advance)


STEPS[:] = [start, task_list, property_view, docked]
QtCore.QTimer.singleShot(3000, advance)
