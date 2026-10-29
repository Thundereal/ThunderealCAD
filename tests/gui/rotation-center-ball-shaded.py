"""The ball that shows the spin centre while the view is rotated is shaded,
not flat (docs/HandsOnQueue.md entry 234, the reporter 2026-10-28: "make the
view spin center indicating ball shaded. right now it seems to be render as
flat").

It was made flat on purpose in 201889b931 (SoLightModel BASE_COLOR), for the
render-cache backend, whose scene light lit it unevenly and whose shadow pass
darkened it. The light comes back; the shadow pass and the screen-space
effects stay off it.

GT_RENDER=coin draws through Coin alone (RenderCache 0), anything else
through the render-cache backend (RenderCache 3). Runs on a real screen:
the picture is saveRenderDump's.

Claims, the ball made large and opaque red, the view rotated by a left drag
(the Inventor style) about the window centre:
  - mid-drag the ball is in the picture, at the centre;
  - it is shaded: its red runs from light to dark across it, where a flat
    one is one red;
  - after the release it is gone.
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
COIN = os.environ.get("GT_RENDER", "") == "coin"
DOC = "SpinBall"
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
Qt = QtCore.Qt
STEPS = []


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
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


def reds(path, cx, cy, reach):
    """The red of each pixel of the ball near (cx, cy) (image coordinates):
    red well above green and blue"""
    img = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
    out = []
    for y in range(max(0, cy - reach), min(img.height(), cy + reach)):
        for x in range(max(0, cx - reach), min(img.width(), cx + reach)):
            c = QtGui.QColor(img.pixel(x, y))
            if c.red() > 60 and c.red() > c.green() + 50 and c.red() > c.blue() + 50:
                out.append(c.red())
    return out


def start():
    VIEW.SetInt("RenderCache", 0 if COIN else 3)
    VIEW.SetBool("ShowRotationCenter", True)
    VIEW.SetFloat("RotationCenterSize", 15.0)
    VIEW.SetUnsigned("RotationCenterColor", 0xFF0000FF)  # opaque red
    VIEW.SetInt("RotationMode", 0)  # about the window centre
    VIEW.SetBool("ShowNaviCube", False)
    VIEW.SetBool("UseNavigationAnimations", False)
    doc = FreeCAD.newDocument(DOC)
    box = doc.addObject("Part::Box", "Box")
    box.Placement.Base = FreeCAD.Vector(60, 60, 0)  # off the centre: the ball on the ground
    doc.recompute()
    v = view()
    v.setNavigationType("Gui::InventorNavigationStyle")
    v.viewIsometric()
    v.fitAll()


def drag():
    width, height = view().getSize()
    cx, cy = width // 2, height // 2
    win = mw().windowHandle()
    p = to_window(cx - 120, cy)
    QtTest.QTest.mouseMove(win, p)
    pump(5)
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(5)
    for i in range(1, 6):
        QtTest.QTest.mouseMove(win, to_window(cx - 120 + 4 * i, cy))
        pump(5)
    pump(30)
    path = os.path.join(OUT, "dragging.png")
    view().saveRenderDump(path, metadata=False)
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, to_window(cx - 100, cy))
    pump(40)
    img = QtGui.QImage(path)
    note("INFO %s path, picture %dx%d, view %dx%d" % (
        "Coin" if COIN else "backend", img.width(), img.height(), width, height))
    reach = 200
    ball = reds(path, img.width() // 2, img.height() // 2, reach)
    # the whole ball: none of it on the rim of what is looked at
    if not check("mid-drag the ball is in the picture, at the centre, whole",
                 200 < len(ball) < (2 * reach) ** 2 // 2,
                 "%d red pixels of %d looked at" % (len(ball), (2 * reach) ** 2)):
        return
    ball.sort()
    low = ball[len(ball) // 20]
    high = ball[-1 - len(ball) // 20]
    check("the ball is shaded: its red runs from light to dark", high - low >= 40,
          "red from %d to %d (5th to 95th percentile), %d pixels" % (low, high, len(ball)))


def released():
    path = os.path.join(OUT, "released.png")
    view().saveRenderDump(path, metadata=False)
    img = QtGui.QImage(path)
    ball = reds(path, img.width() // 2, img.height() // 2, 200)
    check("after the release the ball is gone", len(ball) < 20, "%d red pixels" % len(ball))


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


STEPS[:] = [start, drag, released]
QtCore.QTimer.singleShot(3000, advance)
