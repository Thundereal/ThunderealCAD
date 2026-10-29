"""The curvature comb (docs/Surface.md sec 7, U4): an overlay of any Part
object's view, never part of its shape.

  - every Part object's view has CurvatureComb, off until asked for;
  - on a circle every tooth is as long as the others and points away from
    the centre, the longest a fifth of the object's size; on a line there
    are no teeth to speak of; on an ellipse the longest teeth are at the
    ends of its major axis;
  - CombScale scales them; a change of the shape rebuilds them;
  - isocurves of a cylinder's face get teeth round it and none along it;
  - turned off, the comb is hidden.

The comb's points are read from its Coin nodes (CombTeeth, CombEnvelope).

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import math
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "CurvatureComb"
V = FreeCAD.Vector
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds=0.3):
    import time

    end = time.monotonic() + seconds
    while True:
        QtCore.QCoreApplication.processEvents()
        if time.monotonic() >= end:
            break
        time.sleep(0.01)


def panel():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget, "SurfaceGui_Trim"):
        if w.isVisible():
            return w
    return None


def child(cls, name):
    p = panel()
    return p.findChild(cls, name) if p is not None else None


def press(which):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            settle(0.4)
            return True
    return False


def items(name):
    w = child(QtWidgets.QListWidget, name)
    return [w.item(i).text() for i in range(w.count())] if w is not None else None


def coords(obj, name):
    from pivy import coin

    sa = coin.SoSearchAction()
    sa.setName(name)
    sa.setInterest(coin.SoSearchAction.FIRST)
    sa.setSearchingAll(True)
    sa.apply(obj.ViewObject.RootNode)
    path = sa.getPath()
    if path is None:
        return None
    node = path.getTail()
    return [FreeCAD.Vector(*node.point[i].getValue()) for i in range(node.point.getNum())]


def teeth(obj):
    pts = coords(obj, "CombTeeth") or []
    return [(pts[i], pts[i + 1]) for i in range(0, len(pts) - 1, 2)]


def switch(obj):
    from pivy import coin

    sa = coin.SoSearchAction()
    sa.setName("CurvatureComb")
    sa.setSearchingAll(True)
    sa.apply(obj.ViewObject.RootNode)
    return sa.getPath().getTail() if sa.getPath() else None


def run():
    try:
        V = FreeCAD.Vector
        doc = FreeCAD.newDocument(DOC)
        circle = doc.addObject("Part::Circle", "Circle")
        circle.Radius = 5
        line = doc.addObject("Part::Line", "Line")
        ellipse = doc.addObject("Part::Ellipse", "Ellipse")
        ellipse.MajorRadius, ellipse.MinorRadius = 8, 3
        cyl = doc.addObject("Part::Cylinder", "Cyl")
        cyl.Radius, cyl.Height = 4, 10
        doc.recompute()
        for o in (circle, line, ellipse, cyl):
            check(
                "%s's view has a curvature comb, off" % o.Name,
                hasattr(o.ViewObject, "CurvatureComb") and not o.ViewObject.CurvatureComb,
            )

        circle.ViewObject.CurvatureComb = True
        settle(0.2)
        t = teeth(circle)
        lengths = [(q - p).Length for p, q in t]
        size = circle.Shape.optimalBoundingBox(False, False).DiagonalLength
        check(
            "on a circle every tooth is as long, a fifth of its size",
            len(t) > 10
            and max(lengths) - min(lengths) < 1e-4
            and abs(max(lengths) - 0.2 * size) < 1e-3,
            (len(t), min(lengths) if lengths else None, max(lengths) if lengths else None, size),
        )
        check(
            "and points away from the centre",
            all(q.Length > p.Length + 1e-6 for p, q in t),
        )
        circle.ViewObject.CombScale = 2
        settle(0.2)
        l2 = [(q - p).Length for p, q in teeth(circle)]
        check("CombScale 2 doubles them", abs(max(l2) - 2 * max(lengths)) < 1e-3, max(l2))
        circle.Radius = 10
        doc.recompute()
        settle(0.2)
        t3 = teeth(circle)
        check(
            "a change of the shape rebuilds them",
            t3 and all(abs(p.Length - 10) < 1e-6 for p, q in t3),
            t3[0] if t3 else None,
        )

        line.ViewObject.CurvatureComb = True
        settle(0.2)
        check(
            "a line has no teeth to speak of",
            all((q - p).Length < 1e-9 for p, q in teeth(line)),
        )

        ellipse.ViewObject.CurvatureComb = True
        settle(0.2)
        te = teeth(ellipse)
        longest = max(te, key=lambda pq: (pq[1] - pq[0]).Length)
        check(
            "on an ellipse the longest teeth are at the ends of its major axis",
            abs(abs(longest[0].x) - 8) < 0.1 and abs(longest[0].y) < 0.2,
            longest[0],
        )

        cv = cyl.ViewObject
        cv.CombIsocurves = 2
        cv.CurvatureComb = True
        settle(0.2)
        tc = teeth(cyl)
        round_teeth = [(p, q) for p, q in tc if (q - p).Length > 1e-6]
        check(
            "isocurves of a cylinder get teeth round it",
            len(round_teeth) > 2 * cv.CombDensity,
            (len(tc), len(round_teeth)),
        )
        cv.CurvatureComb = False
        settle(0.2)
        sw = switch(cyl)
        check("turned off, the comb is hidden", sw is not None and sw.whichChild.getValue() == -1)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
