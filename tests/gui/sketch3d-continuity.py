"""The 3D sketch's continuity to model edges, phase S3e (docs/Sketch3D.md section 4.4).

  - Tangent of a line's end and a model edge, selected in the edit: the end on the
    edge, tangent to it there (G1 to the edge);
  - Smooth (Sketcher 3D toolbar) of a spline's end and an arc's end: joined,
    tangent and the arc's curvature (G2); nothing redundant;
  - Smooth with one end selected is refused.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
import math
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "Sketch3DContinuity"
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


def cons(sk):
    return [(c.Type, c.First, c.FirstPos, c.Second, c.SecondPos) for c in sk.Constraints]


def healthy(sk):
    return sk.Malformed == [] and sk.Conflicting == [] and sk.Redundant == []


def run():
    try:
        import Part
        import Sketcher

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        edge = doc.addObject("Part::Feature", "Edge")
        edge.Shape = Part.ArcOfCircle(Part.Circle(V(0, 0, 0), V(0, 0, 1), 10), 0.2, 2.5).toShape()
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        doc.recompute()
        ext = sk.addExternal(edge, "Edge1")
        line = sk.addGeometry(Part.LineSegment(V(15, 12, 1), V(6, 8, 0.5)))
        arc = sk.addGeometry(Part.ArcOfCircle(Part.Circle(V(30, 0, 0), V(0, 0, 1), 8), 0, 1.2))
        sk.addConstraint(Sketcher.Constraint("Block", arc))
        a = sk.Geometry[arc].EndPoint
        bs = Part.BSplineCurve()
        bs.interpolate([a + V(-1, 1, 0.5), a + V(-6, 4, 2), a + V(-12, 4, 5), a + V(-18, 1, 6)])
        spline = sk.addGeometry(bs)
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(sk)
        settle(1.0)
        vp = sk.ViewObject

        # -- Tangent to a model edge -------------------------------------------
        vp.clearSelection()
        vp.select(line, 2)
        vp.select(ext)
        FreeCADGui.Command.get("Sketcher_ConstrainTangent").run()
        settle(0.2)
        c = cons(sk)
        check("Tangent of an end and a model edge", ("Tangent", line, 2, ext, 0) in c, c)
        l = sk.Geometry[line]
        e = l.EndPoint
        check("the end on the edge", abs(math.hypot(e.x, e.y) - 10) < 1e-7 and abs(e.z) < 1e-7, e)
        d = l.EndPoint - l.StartPoint
        radial = V(e.x, e.y, 0)
        check(
            "tangent to it there",
            abs(d.dot(radial)) < 1e-7 * d.Length * radial.Length and abs(d.z) < 1e-7,
            d,
        )

        # -- Smooth --------------------------------------------------------------
        cmd = FreeCADGui.Command.get("Sketcher_Constrain3DSmooth")
        check("Smooth is active in the edit", cmd.isActive())
        vp.clearSelection()
        vp.select(spline, 1)
        nc = len(sk.Constraints)
        cmd.run()
        settle(0.2)
        check("one end alone is refused", len(sk.Constraints) == nc, cons(sk)[nc:])
        vp.clearSelection()
        vp.select(arc, 2)
        vp.select(spline, 1)
        cmd.run()
        settle(0.2)
        c = cons(sk)
        check("Smooth of two ends", ("Smooth", arc, 2, spline, 1) in c, c)
        g = sk.Geometry[spline]
        check("joined", (g.StartPoint - a).Length < 1e-7, g.StartPoint)
        k = g.curvature(g.FirstParameter)
        check("with the arc's curvature", abs(k - 1 / 8) < 1e-6, k)
        check("nothing redundant", healthy(sk), (sk.Malformed, sk.Conflicting, sk.Redundant))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    gdoc = FreeCADGui.getDocument(DOC) if FreeCAD.listDocuments().get(DOC) else None
    if gdoc and gdoc.getInEdit():
        gdoc.resetEdit()
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
