"""A pre-selection macro's tip stands in its corner of what the overlay panels
leave of the view.

docs/HandsOnQueue.md entry 245: "measure preselection macro tooltip show
position ignores the overlay panels ... I want it to be aware of overlay
panels". The tip (Std_Macro_Presel_PartMeasure, shown by Selection through
ToolTip::showText in the corner ViewParams PreselectionToolTipCorner names)
was placed in that corner of the whole view, under an overlaid dock there.

A box in the one 3D view, the macro on; the pointer moved over the box (a
mouse move sent to the view, which preselects as the pointer does). For the
tip in the bottom right corner (the default) and in the top left, claims:
  - docked: the tip shows in that corner of the view -- it stood at twice
    the view's origin (top left) or in the main window's corner (the
    others), the offsets lost;
  - every dock overlaid: the tip shows clear of every overlaid dock, inside
    the view, in its corner's quarter of it;
  - docked again: back in the corner.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
MACRO = "Std_Macro_Presel_PartMeasure"
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
    for _ in range(5):
        QtCore.QCoreApplication.processEvents()


def on_screen(w):
    return QtCore.QRect(w.mapToGlobal(QtCore.QPoint(0, 0)), w.size())


def docks():
    out = {}
    for w in QtWidgets.QApplication.allWidgets():
        try:
            if w.metaObject().className() == "Gui::OverlayTabWidget" \
                    and w.isVisible() and w.count():
                out[w.objectName().replace("Overlay", "")] = on_screen(w)
        except RuntimeError:
            pass
    return out


def hover(at):
    """The pointer moved to \a at in the view's viewport, away first so that
    the preselection is new"""
    gl = S["gl"]
    for p in (QtCore.QPoint(2, gl.height() - 3), at):
        ev = QtGui.QMouseEvent(QtCore.QEvent.MouseMove, QtCore.QPointF(p),
                               QtCore.QPointF(gl.mapToGlobal(p)), QtCore.Qt.NoButton,
                               QtCore.Qt.NoButton, QtCore.Qt.NoModifier)
        QtWidgets.QApplication.sendEvent(gl, ev)
        settle(400)
    settle(800)


def tip():
    for w in QtWidgets.QApplication.allWidgets():
        try:
            if w.objectName() == "fc_overlay_tiplabel" and w.isVisible():
                return on_screen(w), w.text() if hasattr(w, "text") else ""
        except RuntimeError:
            pass
    return None, ""


CORNERS = {3: "bottom right", 0: "top left"}


def in_corner(rect, area, corner):
    """At the view's corner, the offsets being 0"""
    if rect is None:
        return False
    x = area.left() if corner in (0, 2) else area.right() - rect.width() + 1
    y = area.top() if corner in (0, 1) else area.bottom() - rect.height() + 1
    return abs(rect.left() - x) <= 2 and abs(rect.top() - y) <= 2


def in_quadrant(rect, area, corner):
    """In the quarter of the view its corner is in"""
    cx, cy = area.center().x(), area.center().y()
    x_ok = rect.left() >= cx if corner in (1, 3) else rect.right() <= cx
    y_ok = rect.top() >= cy if corner in (2, 3) else rect.bottom() <= cy
    return x_ok and y_ok


def view_now():
    """The view's rectangle on the screen and its centre in the viewport, as
    they are now: overlaying the docks gives the view the whole window"""
    gl = S["gl"]
    mdi = gl
    while mdi is not None and mdi.metaObject().className() != "Gui::View3DInventor":
        mdi = mdi.parentWidget()
    return on_screen(mdi), QtCore.QPoint(gl.width() // 2, gl.height() // 2)


def shown(corner):
    VIEW.SetInt("PreselectionToolTipCorner", corner)
    area, centre = view_now()
    hover(centre)
    rect, _ = tip()
    return rect, area


def docked(when):
    for corner, name in CORNERS.items():
        rect, area = shown(corner)
        note("INFO %s, %s: tip %s, view %s" % (name, when, rect and rect.getRect(),
                                               area.getRect()))
        check("%s, %s: the tip shows, in the view's %s corner" % (name, when, name),
              in_corner(rect, area, corner), rect and rect.getRect())


def overlaid():
    for corner, name in CORNERS.items():
        rect, area = shown(corner)
        all_docks = docks()
        under = {side: r.getRect() for side, r in all_docks.items()
                 if rect is not None and r.intersects(rect)}
        note("INFO %s, overlaid: tip %s, view %s, docks %s"
             % (name, rect and rect.getRect(), area.getRect(),
                {s: r.getRect() for s, r in all_docks.items()}))
        check("%s, overlaid: the tip shows clear of every overlaid dock" % name,
              rect is not None and all_docks and not under,
              under or (rect and rect.getRect()))
        check("%s, overlaid: inside the view, in its corner's quarter" % name,
              rect is not None and area.contains(rect) and in_quadrant(rect, area, corner),
              rect and rect.getRect())


def run():
    S["overlaid"] = False
    try:
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("PreselTip")
        doc.addObject("Part::Box", "Box")
        doc.recompute()
        settle(800)
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewIsometric()
        view.fitAll()
        settle(600)
        S["gl"] = view.graphicsView().viewport()
        FreeCADGui.runCommand(MACRO, 1)
        docked("docked")
        FreeCADGui.runCommand("Std_DockOverlayAll")
        S["overlaid"] = True
        settle(1500)
        view.fitAll()
        settle(600)
        overlaid()
        FreeCADGui.runCommand("Std_DockOverlayAll")
        S["overlaid"] = False
        settle(1500)
        view.fitAll()
        settle(600)
        docked("docked again")
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        try:
            if S["overlaid"]:
                FreeCADGui.runCommand("Std_DockOverlayAll")
            FreeCADGui.runCommand(MACRO, 0)
        except Exception:
            pass
        VIEW.RemInt("PreselectionToolTipCorner")
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
