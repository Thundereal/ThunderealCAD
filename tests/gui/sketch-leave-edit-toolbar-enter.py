"""Leaving a sketch's edit while a toolbar it changes takes the pointer's
Enter does not crash, and a second view leaving the edit leaves the first
one listening.

The crash (found on the Mac mini, sketch-undo-during-drag.py's teardown):
Gui::Document::_resetEditSession takes the view out of the edit first
(ViewProviderSketch::unsetEditViewer: edit->viewer = nullptr), then ends
the edit (unsetEdit), whose first step SketcherGui::Workbench::leaveEditMode
shows the "Sketcher" toolbar in PartDesign -- a new row of toolbars. Under
the pointer it takes Qt's synthetic Enter; Gui::OverlayManager's filter,
on an Enter to a widget that is not a viewer with a preselection standing,
drops the preselection (rmvPreselect); the sketch, still listening,
updated its highlight and read the absent viewer (getEditZDir):
SIGSEGV. By hand: a constraint label or an element preselected near the top
of the 3D view, where that toolbar row appears, and the edit left without
moving the pointer (Escape).

Now the sketch stops listening as its view goes (unsetEditViewer), listens
again when the edit moves to another view (setEditViewer), and
getEditZDir() takes no viewer for no view.

Where the pointer rests is not left to chance (the real cursor sits
anywhere on a test box, Xvfb or a hidden desktop): the test sends the Enter
itself, to whichever toolbar leaving the edit shows or hides, from its
visibilityChanged -- synchronously, inside leaveEditMode, as Qt does. The
overlay's mouse-through, which reads the real cursor, is off.

Claims:
  - in a sketch's edit, a second view made (Std_ViewCreate, it joins the
    edit) and closed: the first view still highlights a preselected line
    (its pixels turn the preselection colour) and drops it again;
  - a constraint preselected, the edit left by resetEdit: a toolbar
    changed under the Enter, the Enter sent, and the process lives on --
    the edit left, the preselection gone.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SketchLeaveEnter"
# ViewProviderSketch::PreselectColor, #E1E100
PRESELECT = (225, 225, 0)

state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds=0.0):
    import time
    end = time.monotonic() + seconds
    while True:
        QtCore.QCoreApplication.processEvents()
        if time.monotonic() >= end:
            break
        time.sleep(0.01)


def views3d():
    """The 3D views open: a closed one is hidden at once, deleted later"""
    return [w for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget)
            if w.metaObject().className() == "Gui::View3DInventor" and w.isVisible()]


def preselect_pixels(mdi, view, world, reach=6):
    """How many pixels near `world` in the view drawn by `mdi` are the
    preselection colour"""
    # QWidget.grab() on the view's GL widget: what it shows (a
    # QOpenGLWidget renders its framebuffer for it)
    gl = [w for w in mdi.findChildren(QtWidgets.QWidget)
          if w.metaObject().className() == "QOpenGLWidget"]
    if not gl:
        return None
    view.redraw()
    settle(0.3)
    img = gl[0].grab().toImage()
    x, y = view.getPointOnViewport(FreeCAD.Vector(*world))
    dpr = img.width() / max(1, view.getSize()[0])
    cx, cy = int(x * dpr), int(img.height() - 1 - y * dpr)
    found = 0
    for j in range(cy - reach, cy + reach + 1):
        for i in range(cx - reach, cx + reach + 1):
            if 0 <= i < img.width() and 0 <= j < img.height():
                c = QtGui.QColor(img.pixel(i, j))
                if (abs(c.red() - PRESELECT[0]) < 30 and abs(c.green() - PRESELECT[1]) < 30
                        and c.blue() < 60):
                    found += 1
    return found


class EnterOnToolbarChange(QtCore.QObject):
    """Sends Qt's Enter to a toolbar as it is shown or hidden: where the
    pointer would get it if it rested there. An event filter, not the
    toolbar's visibilityChanged: Gui::ToolBarManager blocks the toolbar's
    signals while it changes it"""

    def __init__(self):
        super().__init__()
        self.changed = []
        self.toolbars = FreeCADGui.getMainWindow().findChildren(QtWidgets.QToolBar)
        for tb in self.toolbars:
            tb.installEventFilter(self)

    def eventFilter(self, obj, ev):
        if ev.type() in (QtCore.QEvent.Show, QtCore.QEvent.Hide) and \
                isinstance(obj, QtWidgets.QToolBar):
            self.changed.append((obj.objectName(), ev.type() == QtCore.QEvent.Show))
            pos = QtCore.QPointF(4, 4)
            QtWidgets.QApplication.sendEvent(
                obj, QtGui.QEnterEvent(pos, pos,
                                       QtCore.QPointF(obj.mapToGlobal(pos.toPoint()))))
        return False

    def disconnect(self):
        for tb in self.toolbars:
            try:
                tb.removeEventFilter(self)
            except Exception:
                pass


def run():
    try:
        import Part
        import Sketcher

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        view_prefs = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
        view_prefs.SetBool("ShowNaviCube", False)
        view_prefs.SetBool("DockOverlayAutoMouseThrough", False)
        FreeCAD.ParamGet(
            "User parameter:BaseApp/Preferences/Mod/Sketcher/Tools").SetInt(
                "OnViewParameterVisibility", 0)
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        settle(0.5)

        doc = FreeCAD.newDocument(DOC)
        sk = doc.addObject("Sketcher::SketchObject", "Sketch")
        sk.addGeometry(Part.LineSegment(FreeCAD.Vector(-20, 0, 0),
                                        FreeCAD.Vector(20, 0, 0)), False)
        sk.addConstraint(Sketcher.Constraint("Distance", 0, 40.0))
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        settle(0.3)
        note("INFO toolbars shown before the edit: %s" % sorted(
            tb.objectName() for tb in FreeCADGui.getMainWindow().findChildren(QtWidgets.QToolBar)
            if tb.isVisible()))
        gdoc.setEdit(sk)
        settle(0.5)
        first = gdoc.ActiveView
        first.viewTop()
        first.fitAll()
        settle(0.5)
        first_mdi = views3d()
        first_mdi = first_mdi[-1] if len(first_mdi) == 1 else None
        check("one 3D view to begin with", first_mdi is not None, len(views3d()))

        # A second view joins the edit, and closes
        before = set(views3d())
        FreeCADGui.runCommand("Std_ViewCreate", 0)
        settle(1.0)
        made = [w for w in views3d() if w not in before]
        note("INFO %d 3D view(s) with the second made" % len(views3d()))
        check("a second view made", len(made) == 1, len(made))
        area = FreeCADGui.getMainWindow().findChild(QtWidgets.QMdiArea)
        for w in made:
            sub = w.parentWidget()
            (sub if isinstance(sub, QtWidgets.QMdiSubWindow) else w).close()
        # Deleted as the event loop would delete it, now rather than at the
        # quit: this runs inside one timer callback, which holds deferred
        # deletes back until it returns -- a view closed and left undeleted
        # until after the session it joined has ended is not a state a user
        # reaches, and its destructor then unbinds a root already gone
        settle(0.3)
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        settle(0.7)
        check("the second view closed", len(views3d()) == len(before), len(views3d()))
        sub = first_mdi.parentWidget() if first_mdi is not None else None
        if area is not None and isinstance(sub, QtWidgets.QMdiSubWindow):
            area.setActiveSubWindow(sub)
        settle(0.5)
        check("the second view closed, the sketch still in edit",
              gdoc.getInEdit() is not None, gdoc.getInEdit())
        line_mid = (0.0, 0.0, 0.0)
        idle = preselect_pixels(first_mdi, first, line_mid)
        FreeCADGui.Selection.setPreselection(sk, "Edge1")
        settle(0.5)
        lit = preselect_pixels(first_mdi, first, line_mid)
        FreeCADGui.Selection.clearPreselection()
        settle(0.5)
        dropped = preselect_pixels(first_mdi, first, line_mid)
        note("INFO preselection-coloured pixels at the line: before %s, preselected %s,"
             " cleared %s" % (idle, lit, dropped))
        check("the first view highlights the preselected line",
              lit is not None and lit > 3 and lit > (idle or 0), (idle, lit))
        check("and drops it again", dropped is not None and dropped <= (idle or 0) + 1,
              (lit, dropped))

        # Leaving with a constraint preselected, the Enter sent to the
        # toolbar the leaving changes
        FreeCADGui.Selection.setPreselection(sk, "Constraint1")
        settle(0.3)
        pre = FreeCADGui.Selection.getPreselection()
        try:
            standing = list(pre.SubElementNames) if pre.Object else []
        except Exception:
            standing = []
        check("a constraint preselected before leaving", standing == ["Constraint1"], standing)
        def shown_toolbars():
            return sorted(tb.objectName() for tb in
                          FreeCADGui.getMainWindow().findChildren(QtWidgets.QToolBar)
                          if tb.isVisible())
        bars_before = shown_toolbars()
        enter = EnterOnToolbarChange()
        try:
            gdoc.resetEdit()
            settle(0.5)
        finally:
            enter.disconnect()
        note("INFO toolbars changed by leaving: %s; shown before %s; after %s; workbench %s"
             % (enter.changed, bars_before, shown_toolbars(),
                FreeCADGui.activeWorkbench().name()))
        check("leaving the edit changed a toolbar, and it took the Enter",
              len(enter.changed) > 0, enter.changed)
        check("the edit left", gdoc.getInEdit() is None, gdoc.getInEdit())
        try:
            gone = not FreeCADGui.Selection.getPreselection().Object
        except Exception:
            gone = True
        check("and the preselection gone", gone)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        try:
            FreeCADGui.getDocument(name).resetEdit()
            settle(0.2)
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
