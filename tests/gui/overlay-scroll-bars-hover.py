"""The thin overlay scroll bars show when the pointer comes near them, also
in a see-through panel whose clear part passes the pointer on.

docs/HandsOnQueue.md entry 233, the reporter 2026-10-28: "the overlay auto
scrollbar does not work properly. it does not auto reveal when mouse hover,
only after mouse wheel scrolls a bit will it show. Also need to test
overlays with transparent background see if the mouse over reveal works or
not. need to check before pass through."

Runs only with QT_QPA_PLATFORM=offscreen: it moves the pointer (the
platform's own, not the screen's).

The sheet and the page open in cells of the 3D view's tab, split to the
right, as the reporter has them. Claims, each bar faded out first:
  - a spreadsheet's vertical bar shows with the pointer moved near it over
    the cells, and the horizontal one near the bottom edge;
  - a TechDraw page's (zoomed in, so that it scrolls) the same;
  - both again with every dock overlaid, the pointer near a bar from the
    view's side and from over the dock beside it; and neither bar stands
    under a dock (a dock at the side was taken for one at the top too, and
    both bars went back under the docks);
  - a task panel shown over the page's view, touching none of its sides:
    neither bar stands under it, and they are back when it closes (entry
    233 (c): placed "aware of overlay widgets", sliding clear "just like
    cell menu/handler");
  - every dock overlaid, the tree's panel in transparent mode with more rows
    than it shows: the pointer over a clear spot near the tree's vertical
    bar -- one passed on to the view under the panel -- shows that bar;
  - a bar does not fade while the pointer stays near it.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ScrollBarsHover"
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
OPEN = VIEW.GetGroup("OpenView")
STEPS = []
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


def pointer(gpos):
    """The pointer to a point of the screen, by two moves"""
    QtGui.QCursor.setPos(gpos + QtCore.QPoint(-3, 0))
    settle(0.1)
    QtGui.QCursor.setPos(gpos)
    settle(0.4)


def faded(area, name):
    """The pointer to the middle of the area and the bars left to fade"""
    pointer(area.mapToGlobal(area.rect().center()))
    settle(2.2)
    v, h = ours(area, "OverlayScrollBarV"), ours(area, "OverlayScrollBarH")
    return check("%s: the bars are faded out to begin with" % name,
                 v is not None and not shown(v) and (h is None or not shown(h)),
                 (v and shown(v), h and shown(h)))


def under(gpos):
    w = QtWidgets.QApplication.widgetAt(gpos)
    return "%s %r" % (cls(w), w.objectName()) if w is not None else None


def near_bars(area, name, outside=False):
    """Near the vertical bar, then near the horizontal one, over the area;
    with outside, also from past the bar's outer side (over a dock beside
    it, when there is one)"""
    v, h = ours(area, "OverlayScrollBarV"), ours(area, "OverlayScrollBarH")
    note("INFO %s: area %s on the screen, bars %s %s" % (
        name, QtCore.QRect(area.mapToGlobal(QtCore.QPoint(0, 0)), area.size()).getRect(),
        v and v.geometry().getRect(), h and h.geometry().getRect()))
    sides = [("vertical", v, lambda r: QtCore.QPoint(r.left() - THICK, r.center().y()))]
    if h is not None and h.maximum() > h.minimum():
        sides.append(("horizontal", h, lambda r: QtCore.QPoint(r.center().x(), r.top() - THICK)))
    if outside:
        sides.append(("vertical, from past it,", v,
                      lambda r: QtCore.QPoint(r.right() + THICK, r.center().y())))
        if h is not None and h.maximum() > h.minimum():
            sides.append(("horizontal, from past it,", h,
                          lambda r: QtCore.QPoint(r.center().x(), r.bottom() + THICK)))
    screen = QtGui.QGuiApplication.primaryScreen().geometry()
    for which, bar, at in sides:
        r = bar.geometry()
        spot = area.mapToGlobal(at(r))
        if not area.rect().contains(area.mapFromGlobal(spot)) or not screen.contains(spot):
            note("INFO %s: near the %s bar is off the area or the screen, not tried"
                 % (name, which))
            continue
        if not faded(area, name):
            return
        pointer(spot)
        check("%s: the pointer near the %s bar shows it" % (name, which), shown(bar),
              "pointer %s over %s, bar %s" % (area.mapFromGlobal(spot), under(spot),
                                             r.getRect()))
        if which == "vertical":
            settle(2.0)
            check("%s: ... and it stays while the pointer is near" % name, shown(bar))


def start():
    platform = QtGui.QGuiApplication.platformName()
    if platform != "offscreen":
        note("NOT RUN: the platform is %r; this script moves the pointer and runs only with "
             "QT_QPA_PLATFORM=offscreen" % platform)
        STEPS[:] = []
        return
    VIEW.SetBool("UseViewArea", True)
    VIEW.SetBool("ShowNaviCube", False)
    OPEN.SetString("DocViewTarget", "Split")
    OPEN.SetString("SplitDirection", "Right")
    doc = FreeCAD.newDocument(DOC)
    sheet = doc.addObject("Spreadsheet::Sheet", "Sheet")
    sheet.set("A1", "1")
    sheet.set("Z300", "2")
    for i in range(60):
        doc.addObject("App::FeaturePython", "Item%02d" % i)
    doc.recompute()
    sheet.ViewObject.doubleClicked()


def sheet_hover():
    view = find("SpreadsheetGui::SheetTableView")
    if check("the sheet's view is open", view is not None):
        near_bars(view, "the sheet")


def sheet_hover_overlaid():
    view = find("SpreadsheetGui::SheetTableView")
    if view is None:
        note("INFO the sheet's view is not shown now, not tried")
        return
    bars_clear(view, "the sheet, docks overlaid")
    near_bars(view, "the sheet, docks overlaid", True)


def global_rect(w):
    return QtCore.QRect(w.mapToGlobal(QtCore.QPoint(0, 0)), w.size())


class Box:
    """A cover that is not a widget of its own: a box a see-through Tasks
    panel shows"""

    def __init__(self, widget):
        self.widget = widget

    def rect(self):
        return QtCore.QRect(self.widget.mapToGlobal(QtCore.QPoint(0, 0)), self.widget.size())

    def isAncestorOf(self, w):
        return False


def transparent(tab):
    for button in tab.findChildren(QtWidgets.QAbstractButton):
        if button.objectName() == "OBTN Transparent":
            return button.isChecked()
    return False


def covers():
    """What stands over the views: the overlaid docks and the task panels
    over a view; a see-through dock that shows task views by what they show,
    the boxes in their panels (an empty Tasks panel is no cover, entry 233)"""
    out = []
    for w in QtWidgets.QApplication.allWidgets():
        try:
            if not w.isVisible():
                continue
            if cls(w) == "Gui::OverlayTabWidget" and w.count():
                views = [t for t in w.findChildren(QtWidgets.QWidget)
                         if cls(t) == "Gui::TaskView::TaskView" and t.isVisible()]
                if views and transparent(w):
                    for t in views:
                        for p in t.findChildren(QtWidgets.QWidget):
                            if cls(p) == "Gui::TaskView::TaskPanel" and p.isVisible():
                                out += [Box(c) for c in p.children()
                                        if isinstance(c, QtWidgets.QWidget) and c.isVisible()]
                else:
                    out.append(w)
            elif cls(w) == "Gui::TaskView::TaskPanelHost":
                out.append(w)
        except RuntimeError:
            pass
    return out


def cover_rect(c):
    return c.rect() if isinstance(c, Box) else global_rect(c)


def cover_name(c):
    return "task box" if isinstance(c, Box) else cls(c)


def bars_clear(area, name):
    """Neither bar where a dock or a panel over the view stands"""
    under = []
    for bar in (ours(area, "OverlayScrollBarV"), ours(area, "OverlayScrollBarH")):
        if bar is None or bar.geometry().isEmpty():
            continue
        r = QtCore.QRect(area.mapToGlobal(bar.geometry().topLeft()), bar.geometry().size())
        for c in covers():
            if c.isAncestorOf(area):
                continue  # the panel the area is in
            if r.intersects(cover_rect(c)):
                under.append((bar.objectName(), r.getRect(), cover_name(c), cover_rect(c).getRect()))
    note("INFO %s: covers %s" % (name, [(cover_name(c), cover_rect(c).getRect()) for c in covers()
                                        if not c.isAncestorOf(area)]))
    return check("%s: neither bar stands under a dock or a panel" % name, not under, under)


def page():
    import TechDrawGui  # the view providers
    doc = FreeCAD.getDocument(DOC)
    page = doc.addObject("TechDraw::DrawPage", "Page")
    template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    template.Template = os.path.join(
        FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates", "A4_Landscape_blank.svg")
    page.Template = template
    doc.recompute()
    page.ViewObject.show()


def page_hover():
    view = find("TechDrawGui::QGVPage")
    if not check("the page's view is open", view is not None):
        return
    view.scale(4.0, 4.0)
    settle(0.5)
    near_bars(view, "the page")


def page_hover_overlaid():
    view = find("TechDrawGui::QGVPage")
    if check("the page's view is still shown", view is not None):
        bars_clear(view, "the page, docks overlaid")
        near_bars(view, "the page, docks overlaid", True)


class Panel:
    def __init__(self):
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle("A panel over the page")
        layout = QtWidgets.QVBoxLayout(self.form)
        layout.addWidget(QtWidgets.QLabel("a line", self.form))
        layout.addWidget(QtWidgets.QLineEdit(self.form))

    def accept(self):
        return True

    def reject(self):
        return True


STATE = {}


def page_panel():
    view = find("TechDrawGui::QGVPage")
    if view is None:
        STEPS[:] = [s for s in STEPS if s not in (page_panel_shown, page_panel_closed)]
        return
    VIEW.SetBool("TaskPanelInView", True)
    mdi = FreeCADGui.getDocument(DOC).mdiViewsOfType("TechDrawGui::MDIViewPage")
    if mdi:
        mw().setActiveWindow(mdi[0])
    # the layout settled first: where the bars are without the panel
    settle(1.5)
    STATE["home"] = [ours(view, n).geometry() for n in ("OverlayScrollBarV",
                                                         "OverlayScrollBarH")]
    STATE["panel"] = Panel()
    FreeCADGui.Control.showDialog(STATE["panel"])


def page_panel_shown():
    view = find("TechDrawGui::QGVPage")
    host = None
    for w in covers():
        if not isinstance(w, Box) and cls(w) == "Gui::TaskView::TaskPanelHost":
            host = w
    if check("a task panel is shown over the page's view",
             host is not None and global_rect(view).contains(global_rect(host)),
             host and (global_rect(host).getRect(), global_rect(view).getRect())):
        r = global_rect(host).translated(-view.mapToGlobal(QtCore.QPoint(0, 0)))
        note("INFO the panel at %s in the view %s" % (r.getRect(), view.rect().getRect()))
        bars_clear(view, "a task panel over the page")
        note("INFO bars at %s %s" % (ours(view, "OverlayScrollBarV").geometry().getRect(),
                                     ours(view, "OverlayScrollBarH").geometry().getRect()))
    FreeCADGui.Control.closeDialog()


def page_panel_closed():
    view = find("TechDrawGui::QGVPage")
    settle(1.0)
    now = [ours(view, n).geometry() for n in ("OverlayScrollBarV", "OverlayScrollBarH")]
    check("the panel closed: the bars are back where they were", now == STATE["home"],
          ([r.getRect() for r in now], [r.getRect() for r in STATE["home"]]))
    VIEW.SetBool("TaskPanelInView", False)


def overlay_all():
    # a configuration with the docks overlaid already (GT_SEED_CFG) keeps them
    tree = find("Gui::TreeWidget")
    if tree is None or overlay_of(tree) is None:
        FreeCADGui.runCommand("Std_DockOverlayAll")


def eye(tab, on):
    """The transparent mode of one overlay, by its own button"""
    for button in tab.findChildren(QtWidgets.QAbstractButton):
        if button.objectName() == "OBTN Transparent":
            if button.isChecked() != on:
                button.click()
                settle(0.8)
            return button.isChecked() == on
    return False


def overlay_of(w):
    while w is not None:
        if cls(w) == "Gui::OverlayTabWidget":
            return w
        w = w.parentWidget()
    return None


def tree_hover():
    tree = find("Gui::TreeWidget")
    tab = overlay_of(tree) if tree is not None else None
    if not check("the tree view is in an overlay", tab is not None):
        return
    tree.expandAll()
    settle(0.5)
    if not check("the tree's overlay has its transparent mode on", eye(tab, True)):
        return
    v = ours(tree, "OverlayScrollBarV")
    if not check("the tree has the thin bars and more rows than it shows",
                 v is not None and v.maximum() > v.minimum(),
                 v and (v.minimum(), v.maximum())):
        return
    if not faded(tree, "the tree"):
        return
    # the pointer off the panel, and the focus, the panel showing itself
    # solid while it has either
    view = find("Gui::View3DInventor")
    if view is not None:
        view.setFocus()
        pointer(view.mapToGlobal(view.rect().center()))
        settle(1.0)
    focus = QtWidgets.QApplication.focusWidget()
    note("INFO the focus on %s; the pointer over %s" % (
        focus and cls(focus), under(QtGui.QCursor.pos())))
    # a clear spot near the bar: one the panel passes on
    r = v.geometry()
    image = tab.grab().toImage().convertToFormat(QtGui.QImage.Format_ARGB32)

    def clear(gp):
        at = tab.mapFromGlobal(gp)
        for dx in range(-2, 3):
            for dy in range(-2, 3):
                p = at + QtCore.QPoint(dx, dy)
                if not image.rect().contains(p) or (image.pixel(p) >> 24) & 255:
                    return False
        return True

    spot = None
    for y in range(r.top() + 4, r.bottom() - 4, 3):
        for dx in (THICK + 4, 2 * THICK, 2 * THICK + 5):
            gp = tree.mapToGlobal(QtCore.QPoint(r.left() - dx, y))
            if clear(gp):
                spot = gp
                break
        if spot is not None:
            break
    if spot is None:
        # a narrow dock may have no spot clear all round beside the bar: the
        # claim is then left to a run with a wider one (the reporter's)
        note("INFO no clear spot near the tree's bar; the pass-through claim not tried")
        alphas = sorted({(image.pixel(tab.mapFromGlobal(tree.mapToGlobal(
            QtCore.QPoint(r.left() - THICK, y)))) >> 24) & 255
            for y in range(r.top() + 4, r.bottom() - 4, 3)})
        note("INFO alpha beside the bar: %s" % alphas[:20])
        return
    pointer(spot)
    check("the pointer over a clear spot of a see-through panel near its bar shows the bar",
          shown(v), "pointer %s over %s, bar %s" % (tree.mapFromGlobal(spot), under(spot),
                                                   r.getRect()))


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
        STEPS[:] = []
    QtCore.QTimer.singleShot(2500, advance)


STEPS[:] = [start, sheet_hover, page, page_hover, overlay_all, page_hover_overlaid,
            sheet_hover_overlaid, tree_hover, page_panel, page_panel_shown,
            page_panel_closed]
QtCore.QTimer.singleShot(3000, advance)
