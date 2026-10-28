"""The thin scroll bars of overlay panels show with the real pointer near
them, and an empty Tasks panel is no cover for a view's bars
(docs/HandsOnQueue.md entry 233, the reporter 2026-10-28: "overlay panels,
all of it (even for in view panels), does not response to mouse over/near
highlight for any scroll bar. The view scroll bar works well. overlay
panels only show scroll bar when scrolling with mouse wheel"; "if Task
overlay panel is empty, then do not slide out affected scroll bar").

The REAL screen and the SYSTEM pointer (QCursor.setPos): what OverlayManager
does with a panel under the pointer -- its pass-through, the panel's look --
it decides by where the system pointer is, which Qt events sent to a window
do not move. The pointer is put back where it was at the end. Best run
with a copy of the reporter's configuration (GT_SEED_CFG).

Claims, each bar faded first:
  - the tree's bar, in its overlay panel, the pointer brought over the rows
    to next to it: shown, and on the screen;
  - the same over a clear spot near it, if the panel has one;
  - a list in a task panel shown over the view (TaskPanelInView): the same;
  - a see-through Tasks panel covers what it shows, not its whole dock:
    with no task, a page's vertical bar keeps to the page's edge, under
    the dock, clear of any box the panel shows (a watcher's); with a task
    panel in it, the bar is clear of that too.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "PanelBarPointer"
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
STEPS = []
STATE = {}
THICK = 10


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


def mw():
    return FreeCADGui.getMainWindow()


def cls(w):
    return w.metaObject().className()


def find(class_name):
    for w in QtWidgets.QApplication.allWidgets():
        try:
            if cls(w) == class_name and w.isVisible():
                return w
        except RuntimeError:
            pass
    return None


def ours(area, name):
    for b in area.findChildren(QtWidgets.QScrollBar):
        if b.objectName() == name:
            return b
    return None


def shown(bar):
    # the bar paints itself at this opacity (no graphics effect)
    o = bar.property("overlayOpacity")
    return bar.isVisible() and o is not None and o > 0.0


def overlay_of(w):
    while w is not None:
        if cls(w) == "Gui::OverlayTabWidget":
            return w
        w = w.parentWidget()
    return None


def screen_shot(area, bar):
    r = bar.geometry()
    g = area.mapToGlobal(r.topLeft())
    screen = QtGui.QGuiApplication.screenAt(g) or QtGui.QGuiApplication.primaryScreen()
    sg = screen.geometry()
    return screen.grabWindow(0, g.x() - sg.x(), g.y() - sg.y(), r.width(), r.height()).toImage()


def differ(a, b):
    if a.size() != b.size():
        return -1
    return sum(1 for y in range(a.height()) for x in range(a.width())
               if a.pixel(x, y) != b.pixel(x, y))


def glide(to, steps=8):
    """The system pointer brought to a point in steps, as a hand would"""
    start = QtGui.QCursor.pos()
    for i in range(1, steps + 1):
        QtGui.QCursor.setPos(start + (to - start) * i / steps)
        settle(0.03)
    settle(0.6)


def faded(area, name):
    glide(mw().mapToGlobal(mw().rect().center()))
    settle(2.2)
    v = ours(area, "OverlayScrollBarV")
    return check("%s: faded to begin with" % name, v is not None and not shown(v))


def front(spot):
    """This window in front where the pointer goes: a window of another
    program over it takes the pointer's moves"""
    for _ in range(3):
        w = QtWidgets.QApplication.widgetAt(spot)
        if w is not None and w.window() == mw():
            return True
        if mw().isMinimized():
            mw().showNormal()
        mw().raise_()
        mw().activateWindow()
        settle(0.8)
    note("INFO another window is over %s: %s" % (spot, QtWidgets.QApplication.widgetAt(spot)))
    return False


def near(area, name, spot):
    v = ours(area, "OverlayScrollBarV")
    if not front(spot):
        check("%s: this window is in front at the spot" % name, False)
        return
    if not faded(area, name):
        return
    before = screen_shot(area, v)
    glide(spot)
    after = screen_shot(area, v)
    tag = name.replace(" ", "-").replace(",", "")
    before.save(os.path.join(OUT, tag + "-before.png"))
    after.save(os.path.join(OUT, tag + "-after.png"))
    w = QtWidgets.QApplication.widgetAt(spot)
    check("%s: the pointer near the bar shows it" % name, shown(v),
          "pointer over %s" % (w and cls(w)))
    n = differ(before, after)
    check("%s: ... on the screen" % name, n > 20,
          "%d of %d pixels changed where the bar is" % (n, v.width() * v.height()))


def start():
    STATE["pointer"] = QtGui.QCursor.pos()
    VIEW.SetBool("UseViewArea", True)
    OPEN = VIEW.GetGroup("OpenView")
    OPEN.SetString("DocViewTarget", "Split")
    OPEN.SetString("SplitDirection", "Right")
    VIEW.SetBool("TaskPanelInView", False)
    doc = FreeCAD.newDocument(DOC)
    for i in range(60):
        doc.addObject("App::FeaturePython", "Item%02d" % i)
    import TechDrawGui  # the view providers
    page = doc.addObject("TechDraw::DrawPage", "Page")
    template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    template.Template = os.path.join(
        FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates", "A4_Landscape_blank.svg")
    page.Template = template
    doc.recompute()
    page.ViewObject.show()
    mw().raise_()
    mw().activateWindow()


def overlays():
    tree = find("Gui::TreeWidget")
    if tree is None or overlay_of(tree) is None:
        FreeCADGui.runCommand("Std_DockOverlayAll")


def tree_bar():
    tree = find("Gui::TreeWidget")
    if not check("the tree is in an overlay panel", tree is not None and overlay_of(tree)):
        return
    tree.expandAll()
    settle(0.5)
    v = ours(tree, "OverlayScrollBarV")
    if not check("the tree has the thin bar, with a range",
                 v is not None and v.maximum() > v.minimum()):
        return
    r = v.geometry()
    near(tree, "the tree, over a row",
         tree.mapToGlobal(QtCore.QPoint(r.left() - THICK, r.top() + 50)))


class Panel:
    def __init__(self, rows):
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle("A list in a panel")
        layout = QtWidgets.QVBoxLayout(self.form)
        self.list = QtWidgets.QListWidget(self.form)
        self.list.addItems(["row %d" % i for i in range(rows)])
        self.list.setMinimumHeight(200)
        layout.addWidget(self.list)

    def accept(self):
        return True

    def reject(self):
        return True


def page_bar_empty_tasks():
    view = find("TechDrawGui::QGVPage")
    if not check("the page's view is shown", view is not None):
        return
    view.scale(4.0, 4.0)
    settle(1.0)
    v = ours(view, "OverlayScrollBarV")
    vp = view.viewport().geometry()
    STATE["empty"] = v.geometry()
    # what each shown overlay holds: a Tasks panel with nothing in it is no
    # cover for the bars
    for w in QtWidgets.QApplication.allWidgets():
        try:
            if cls(w) != "Gui::OverlayTabWidget" or not w.isVisible() or not w.count():
                continue
            shown_docks = [w.widget(i).objectName() for i in range(w.count())]
            tv = [t for t in w.findChildren(QtWidgets.QWidget)
                  if cls(t) == "Gui::TaskView::TaskView"]
            boxes = [cls(b) + ":" + (b.windowTitle() or b.objectName())
                     for t in tv for b in t.findChildren(QtWidgets.QWidget)
                     if b.isVisible() and cls(b) in ("Gui::TaskView::TaskBox",
                                                     "Gui::TaskView::TaskPanel")]
            note("INFO overlay %s at %s: docks %s, current %r, task view %s, boxes shown %s" % (
                w.objectName(), QtCore.QRect(w.mapToGlobal(QtCore.QPoint(0, 0)),
                                             w.size()).getRect(),
                shown_docks, w.currentWidget() and w.currentWidget().objectName(),
                [t.isVisible() for t in tv], boxes[:6]))
        except RuntimeError:
            pass
    note("INFO no task: the page's bar at %s in %s" % (v.geometry().getRect(), vp.getRect()))
    check("no task: the page's vertical bar at the page's edge",
          v.geometry().right() >= vp.right() - 1, v.geometry().getRect())
    clear_of_tasks(view, v, "no task")
    STATE["docked"] = Panel(10)
    FreeCADGui.Control.showDialog(STATE["docked"])


def task_boxes():
    """What the Tasks panel shows, on the screen: the boxes in its panels"""
    out = []
    for w in QtWidgets.QApplication.allWidgets():
        try:
            if cls(w) == "Gui::TaskView::TaskPanel" and w.isVisible():
                for c in w.children():
                    if isinstance(c, QtWidgets.QWidget) and c.isVisible():
                        out.append(QtCore.QRect(c.mapToGlobal(QtCore.QPoint(0, 0)), c.size()))
        except RuntimeError:
            pass
    return out


def clear_of_tasks(view, v, name):
    g = QtCore.QRect(view.mapToGlobal(v.geometry().topLeft()), v.geometry().size())
    boxes = task_boxes()
    note("INFO %s: the Tasks panel shows %s" % (name, [b.getRect() for b in boxes]))
    check("%s: the page's bar is clear of what the Tasks panel shows" % name,
          not any(g.intersects(b) for b in boxes), (g.getRect(), [b.getRect() for b in boxes]))


def page_bar_full_tasks():
    view = find("TechDrawGui::QGVPage")
    v = ours(view, "OverlayScrollBarV")
    settle(0.5)
    note("INFO a task panel in the Tasks panel: the page's bar at %s" % (v.geometry().getRect(),))
    clear_of_tasks(view, v, "a task panel")
    FreeCADGui.Control.closeDialog()


def in_view_panel():
    VIEW.SetBool("TaskPanelInView", True)
    view = find("Gui::View3DInventor")
    mdi = FreeCADGui.getDocument(DOC).mdiViewsOfType("Gui::View3DInventor")
    if mdi:
        mw().setActiveWindow(mdi[0])
    settle(1.0)
    STATE["inview"] = Panel(80)
    FreeCADGui.Control.showDialog(STATE["inview"])


def in_view_bar():
    lst = STATE["inview"].list
    host = lst
    while host is not None and cls(host) != "Gui::TaskView::TaskPanelHost":
        host = host.parentWidget()
    note("INFO the panel's list is %s a panel host over the view" % (
        "in" if host is not None else "NOT in"))
    v = ours(lst, "OverlayScrollBarV")
    if check("the in-view panel's list has the thin bar, with a range",
             v is not None and v.maximum() > v.minimum(),
             v and (v.minimum(), v.maximum())):
        r = v.geometry()
        near(lst, "the in-view panel's list",
             lst.mapToGlobal(QtCore.QPoint(r.left() - THICK, r.top() + 30)))
    FreeCADGui.Control.closeDialog()
    VIEW.SetBool("TaskPanelInView", False)


def finish():
    if "pointer" in STATE:
        QtGui.QCursor.setPos(STATE["pointer"])
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
    QtCore.QTimer.singleShot(2500, advance)


STEPS[:] = [start, overlays, tree_bar, page_bar_empty_tasks, page_bar_full_tasks,
            in_view_panel, in_view_bar]
QtCore.QTimer.singleShot(3000, advance)
