"""The 3D view is drawn while a recompute runs (entry 247 (a)).

The reporter 2026-11-03: "opening scanner.FCStd and proceed with migration
recompute still gives black background during recomputation". The view
skipped every frame while its document was recomputing (actualRedraw's
Native case, since 2021), and a recompute pumps the event loop -- its
progress does, every 200 ms -- so each paint it let through drew nothing:
QOpenGLWidget presents a cleared buffer, black, until the recompute ended.
The migration prompt's recompute runs over a load that is still building
the scene, so the whole model appeared at once at the end.

A document of BOXES boxes and a Python feature whose execute asks the view
for a picture (saveRenderDump, which pumps frames until one is drawn), so
the picture is taken in the middle of a recompute. Claims:
  - a picture taken with no recompute shows the boxes (the control);
  - the picture asked for inside the recompute is drawn -- before, the
    frame never came and the capture timed out;
  - and it shows the boxes as the control does.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "DrawnDuringRecompute"
BOXES = 6
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


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def covered(path):
    """Share of the picture that differs from its corner, the background"""
    img = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
    w, h = img.width(), img.height()
    ground = QtGui.QColor(img.pixel(1, 1))
    hit = total = 0
    for y in range(0, h, max(1, h // 60)):
        for x in range(0, w, max(1, w // 60)):
            c = QtGui.QColor(img.pixel(x, y))
            total += 1
            if (abs(c.red() - ground.red()) + abs(c.green() - ground.green())
                    + abs(c.blue() - ground.blue())) > 30:
                hit += 1
    return hit / float(total)


class Snapper:
    """execute() takes the view's picture: the recompute is running"""

    def __init__(self, obj):
        obj.Proxy = self
        obj.addProperty("App::PropertyInteger", "Count")

    def execute(self, obj):
        if not STATE.get("armed"):
            return
        path = os.path.join(OUT, "during.png")
        try:
            view().saveRenderDump(path, metadata=False)
            STATE["during"] = path
        except Exception as e:
            STATE["duringError"] = str(e)

    def dumps(self):
        return None

    def loads(self, state):
        return None


def start():
    VIEW.SetBool("ShowNaviCube", False)
    VIEW.SetBool("UseNavigationAnimations", False)
    # a flat ground, so that what differs from it is the boxes
    VIEW.SetBool("Gradient", False)
    VIEW.SetBool("RadialGradient", False)
    VIEW.SetUnsigned("BackgroundColor", 0xFFFFFFFF)
    doc = FreeCAD.newDocument(DOC)
    for i in range(BOXES):
        box = doc.addObject("Part::Box", "Box%d" % i)
        box.Placement.Base = FreeCAD.Vector(15 * i, 0, 0)
    snap = doc.addObject("App::FeaturePython", "Snap")
    Snapper(snap)
    doc.recompute()
    v = view()
    v.viewIsometric()
    v.fitAll()


def control():
    path = os.path.join(OUT, "control.png")
    view().saveRenderDump(path, metadata=False)
    STATE["control"] = covered(path)
    check("with no recompute the picture shows the boxes",
          0.02 < STATE["control"] < 0.9,
          "%.3f of the picture" % STATE["control"])


def during():
    doc = FreeCAD.getDocument(DOC)
    snap = doc.getObject("Snap")
    snap.Count = snap.Count + 1
    STATE["armed"] = True
    doc.recompute()
    STATE["armed"] = False
    drawn = "during" in STATE
    check("the picture asked for inside the recompute is drawn", drawn,
          STATE.get("duringError", STATE.get("during")))
    if drawn:
        share = covered(STATE["during"])
        check("and it shows the boxes as the control does",
              share > 0.5 * STATE["control"],
              "%.3f of the picture, the control %.3f" % (share, STATE["control"]))


def finish():
    for name in ("ShowNaviCube", "UseNavigationAnimations", "Gradient", "RadialGradient"):
        VIEW.RemBool(name)
    VIEW.RemUnsigned("BackgroundColor")
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


STEPS[:] = [start, control, during]
QtCore.QTimer.singleShot(3000, advance)
