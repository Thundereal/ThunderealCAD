"""How long a step of a pan takes, in a 3D view and on a TechDraw page, and
how much of it the overlay scroll bars take (docs/HandsOnQueue.md entry
238, the reporter 2026-10-28: "pan causing noticable delay in both 3d view
and techdraw page (worse)").

A diagnostic, not a pass/fail test. The Blender style (the reporter's): a
pan with the left and the right button down, the pointer moved by Qt
events, each step followed by the redraw it asks for. With
FC_TRACE_OVERLAY_BARS set to a file, a build carrying the TRACE238 lines
writes the time spent in the bars' pointerMoved, reveal and place there.
Runs on a real screen.
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
TRACE = os.environ.get("FC_TRACE_OVERLAY_BARS", "")
DOC = "PanTiming"
Qt = QtCore.Qt
STEPS = []


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def pump(seconds=0.3):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


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


def trace_line():
    if not TRACE or not os.path.exists(TRACE):
        return "(no trace)"
    with open(TRACE) as f:
        lines = f.read().splitlines()
    return lines[-1] if lines else "(empty trace)"


def pan(widget, name, steps=60):
    win = mw().windowHandle()
    c = widget.mapTo(mw(), widget.rect().center())
    QtTest.QTest.mouseMove(win, c)
    pump(0.5)
    before = trace_line()
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, c)
    QtTest.QTest.mousePress(win, Qt.RightButton, Qt.NoModifier, c)
    pump(0.1)
    times = []
    for i in range(1, steps + 1):
        t = time.perf_counter()
        QtTest.QTest.mouseMove(win, c + QtCore.QPoint(3 * i, 2 * i))
        QtWidgets.QApplication.processEvents()
        times.append((time.perf_counter() - t) * 1000.0)
    QtTest.QTest.mouseRelease(win, Qt.RightButton, Qt.NoModifier, c + QtCore.QPoint(3 * steps, 2 * steps))
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, c + QtCore.QPoint(3 * steps, 2 * steps))
    pump(1.5)
    # a context menu after the pan (entry 237) closed
    for m in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(m, QtWidgets.QMenu) and m.isVisible():
            m.close()
    times.sort()
    note("RESULT %s: a pan step %.2f ms median, %.2f ms at the 90th percentile, %.1f ms in all (%d steps)"
         % (name, times[len(times) // 2], times[int(len(times) * 0.9)], sum(times), steps))
    note("INFO %s: overlay bars trace before %s" % (name, before))
    note("INFO %s: overlay bars trace after  %s" % (name, trace_line()))


def start():
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetBool("ShowNaviCube", False)
    doc = FreeCAD.newDocument(DOC)
    for i in range(30):
        b = doc.addObject("Part::Box", "Box%02d" % i)
        b.Placement.Base = FreeCAD.Vector(15 * (i % 6), 15 * (i // 6), 0)
    doc.recompute()
    v = FreeCADGui.getDocument(DOC).ActiveView
    v.setNavigationType("Gui::BlenderNavigationStyle")
    v.viewIsometric()
    v.fitAll()


def pan3d():
    w = FreeCADGui.getDocument(DOC).ActiveView.graphicsView()
    pan(w.viewport() if hasattr(w, "viewport") else w, "3D view")


def page():
    import TechDrawGui  # the view providers
    doc = FreeCAD.getDocument(DOC)
    p = doc.addObject("TechDraw::DrawPage", "Page")
    t = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    t.Template = os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw", "Templates",
                              "A4_Landscape_blank.svg")
    p.Template = t
    doc.recompute()
    p.ViewObject.show()


def panpage():
    v = find("TechDrawGui::QGVPage")
    if v is None:
        note("ABORT no page view")
        return
    v.scale(3.0, 3.0)
    pump(0.5)
    pan(v.viewport(), "TechDraw page")


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


STEPS[:] = [start, pan3d, page, panpage]
QtCore.QTimer.singleShot(3000, advance)
