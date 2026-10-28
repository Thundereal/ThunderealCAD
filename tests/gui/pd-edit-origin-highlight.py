"""The origin a PartDesign box's edit shows is highlighted under the pointer
whether or not the Origin group is visible (docs/HandsOnQueue.md entry 239,
the reporter 2026-10-28: "when create/edit partdesign box, the origin plane
is shown regardless of its visibility ... But when the origin group is
invisible, the origin shown when editing box does not react to highlight.
it only reacts when origin group is made visible").

The box panel shows the body's origin for picking a reference
(setTemporaryVisibility, an edit visibility): the origin's own Visibility
is not touched. Runs on a real screen: the pictures are saveRenderDump's.

Claims, a box in its edit seen from the top, the Origin hidden and then
visible:
  - the XY plane can be pointed at, and the pointer on its edge
    preselects it;
  - the highlight is drawn on it: the plane changes on the screen.
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
DOC = "EditOriginHighlight"
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
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
    width, height = view().getSize()
    w = view().graphicsView()
    local = QtCore.QPoint(int(x), int(height - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def move(x, y):
    QtTest.QTest.mouseMove(mw().windowHandle(), to_window(x, y))
    pump(0.15)


def picked(x, y):
    """What a pick finds, as object.subname: in a body's edit the origin
    may come as the body's sub-object"""
    info = view().getObjectInfo((int(x), int(y)))
    if not info:
        return None
    return "%s.%s" % (info.get("Object", ""), info.get("SubName", "") or "")


def presel():
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
    view().saveRenderDump(path, metadata=False)
    return QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)


def colour(key, default):
    c = VIEW.GetUnsigned(key, default)
    return ((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255)


def count(img, rgb, box, tol=60):
    n = 0
    for y in range(max(0, box.top()), min(img.height(), box.bottom())):
        for x in range(max(0, box.left()), min(img.width(), box.right())):
            c = QtGui.QColor(img.pixel(x, y))
            if abs(c.red() - rgb[0]) + abs(c.green() - rgb[1]) + abs(c.blue() - rgb[2]) <= tol:
                n += 1
    return n


def start():
    VIEW.SetBool("ShowNaviCube", False)
    VIEW.SetBool("UseNavigationAnimations", False)
    doc = FreeCAD.newDocument(DOC)
    body = doc.addObject("PartDesign::Body", "Body")
    box = body.newObject("PartDesign::AdditiveBox", "Box")
    # under the origin, the origin in the middle of the view
    box.Length, box.Width, box.Height = 100, 100, 10
    box.Placement.Base = FreeCAD.Vector(-50, -50, -40)
    doc.recompute()
    body.Origin.ViewObject.Visibility = False
    v = view()
    v.viewTop()
    v.fitAll()
    FreeCADGui.getDocument(DOC).setEdit(box, 0)


def try_plane(tag):
    """The pointer from the origin out across the XY plane's corner (picks
    find nothing in an edit: the preselection says what is there)"""
    FreeCADGui.Selection.clearSelection()
    move(5, 5)
    note("INFO %s: the Origin %s" % (tag, "visible" if FreeCAD.getDocument(DOC).getObject(
        "Body").Origin.Visibility else "hidden"))
    ox, oy = view().getPointOnViewport(FreeCAD.Vector(0, 0, 0))
    rest = dump("%s-rest" % tag)
    found = None
    for d in range(4, 300, 2):
        move(ox + d, oy + d)
        if "XY_Plane" in (presel() or ""):
            found = (ox + d, oy + d)
            break
    if not check("%s: the pointer over the XY plane preselects it" % tag, found is not None,
                 presel()):
        return
    pump(0.3)
    img = dump("%s-preselected" % tag)
    h = img.height()
    box = QtCore.QRect(QtCore.QPoint(int(ox) - 10, h - 1 - int(oy) - 320),
                       QtCore.QPoint(int(ox) + 320, h - 1 - int(oy) + 10))
    # the highlight drawn: the plane changes on the screen (its colour is
    # mixed with the plane's own and with what lies behind it)
    changed = 0
    for yy in range(max(0, box.top()), min(img.height(), box.bottom())):
        for xx in range(max(0, box.left()), min(img.width(), box.right())):
            if img.pixel(xx, yy) != rest.pixel(xx, yy):
                changed += 1
    check("%s: the highlight is drawn on it" % tag, changed > 100,
          "%d pixels changed round the plane" % changed)
    move(5, 5)


def hidden():
    try_plane("Origin hidden")
    FreeCAD.getDocument(DOC).getObject("Body").Origin.ViewObject.Visibility = True
    pump(0.5)


def visible():
    try_plane("Origin visible")


def finish():
    try:
        FreeCADGui.Control.closeDialog()
    except Exception:
        pass
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
    QtCore.QTimer.singleShot(2000, advance)


STEPS[:] = [start, hidden, visible]
QtCore.QTimer.singleShot(3000, advance)
