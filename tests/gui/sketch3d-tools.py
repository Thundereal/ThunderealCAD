"""The 3D sketch's drawing tools, phase S2c (docs/Sketch3D.md section 7).

Started by the Sketcher's own commands in a 3D sketch's edit and driven through
the view provider's input entry points (press, pressRight, finishTool), which
are what the mouse handlers and Escape call:

  - Polyline: segments joined end to start; a segment drawn along a frame axis
    (here through the manipulator's axis lock) gets AlongX / AlongZ; Escape
    finishes the chain and a second Escape ends the tool;
  - Line: a start on an existing end is coincident with it;
  - Point: on a curve, point on object; on a model vertex (the preselection),
    the vertex becomes an external reference and the point coincident with it;
  - the spline through points makes the spline and its knot points; the spline
    by control points its poles;
  - Toggle Construction makes construction geometry, kept out of the shape;
  - a right press finishes as Escape does; an addition is one undo step.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "Sketch3DTools"
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
    out = []
    for c in sk.Constraints:
        out.append((c.Type, c.First, c.FirstPos, c.Second, c.SecondPos))
    return out


def click(vp, p):
    x, y = vp.toView(p)
    vp.move(x, y)
    return vp.press(x, y)


def run():
    try:
        import Part

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        box = doc.addObject("Part::Box", "Box")
        box.Placement.Base = V(40, 0, 0)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.viewIsometric()
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject
        tol = 0.3

        # -- polyline --------------------------------------------------------
        cmd = FreeCADGui.Command.get("Sketcher_CreatePolyline")
        check("Polyline is active in a 3D sketch's edit", cmd.isActive())
        cmd.run()
        settle(0.2)
        check("and starts the 3D polyline tool", vp.activeTool() == "Polyline", vp.activeTool())
        click(vp, V(5, 5, 0))
        click(vp, V(25, 5, 0))
        check("a segment made", len(sk.Geometry) == 1, len(sk.Geometry))
        # the manipulator is at the end now: up its Z axis, beyond the gizmo
        took = click(vp, V(25, 5, 20))
        check("a second segment made", took and len(sk.Geometry) == 2, len(sk.Geometry))
        c = cons(sk)
        check("joined end to start", ("Coincident", 0, 2, 1, 1) in c, c)
        check("the first along X", ("AlongX", 0, 0, -2000, 0) in c, c)
        check("the second along Z", ("AlongZ", 1, 0, -2000, 0) in c, c)
        g = sk.Geometry
        check("where it was clicked", (g[1].EndPoint - V(25, 5, 20)).Length < tol, g[1].EndPoint)
        vp.finishTool()
        check("Escape finishes the chain and keeps the tool", vp.activeTool() == "Polyline")
        vp.finishTool()
        check("a second Escape ends it", vp.activeTool() == "None")

        # -- line from an existing end ---------------------------------------
        FreeCADGui.Command.get("Sketcher_CreateLine").run()
        end = sk.Geometry[1].EndPoint
        click(vp, end)
        click(vp, end + V(0, 25, 0))
        c = cons(sk)
        check("a line made", len(sk.Geometry) == 3, len(sk.Geometry))
        check("its start coincident with the end it began on", ("Coincident", 2, 1, 1, 2) in c, c)
        check("and along Y", ("AlongY", 2, 0, -2000, 0) in c, c)
        vp.finishTool()

        # -- point on a curve, point on a model vertex -----------------------
        FreeCADGui.Command.get("Sketcher_CreatePoint").run()
        n = len(sk.Geometry)
        click(vp, V(15, 5, 0))
        c = cons(sk)
        check("a point on the first segment is on it", ("PointOnObject", n, 1, 0, 0) in c, c)
        vertex = None
        for i, v in enumerate(box.Shape.Vertexes):
            if (v.Point - V(40, 0, 10)).Length < 1e-9:
                vertex = "Vertex%d" % (i + 1)
        FreeCADGui.Selection.setPreselection(box, vertex, 40, 0, 10)
        x, y = vp.toView(V(40, 0, 10))
        vp.press(x, y)
        FreeCADGui.Selection.clearPreselection()
        c = cons(sk)
        ext = sk.ExternalGeometry
        check("the vertex became an external reference", ext and ext[0][1][0] == vertex, ext)
        check("and the point is coincident with it", ("Coincident", n + 1, 1, -7, 1) in c, c)
        check(
            "where the vertex is",
            (sk.Geometry[n + 1].toShape().Point - V(40, 0, 10)).Length < 1e-6,
            sk.Geometry[n + 1].toShape().Point,
        )
        vp.finishTool()

        # -- splines -----------------------------------------------------------
        FreeCADGui.Command.get("Sketcher_CreateBSplineByInterpolation").run()
        check(
            "the spline through points tool", vp.activeTool() == "BSplineThrough", vp.activeTool()
        )
        n = len(sk.Geometry)
        pts = [V(0, 40, 0), V(10, 48, 0), V(22, 42, 0), V(30, 52, 0)]
        # From the box vertex, the first point would lie on the manipulator's Z axis on
        # screen in this view, and lock to it: start the manipulator there instead.
        vp.setManipulator(pts[0], FreeCAD.Rotation())
        for p in pts:
            click(vp, p)
        check(
            "its points are held until it is made",
            vp.placedCount() == 4 and len(sk.Geometry) == n,
            (vp.placedCount(), len(sk.Geometry)),
        )
        took = vp.pressRight(*vp.toView(V(30, 60, 0)))
        check("a right press makes it", took and len(sk.Geometry) == n + 5, len(sk.Geometry))
        kinds = [c[0] for c in cons(sk) if c[3] == n]
        check("with its four knot points", kinds.count("InternalAlignment") == 4, kinds)
        spl = sk.Geometry[n]
        dists = [spl.toShape().distToShape(Part.Vertex(p))[0] for p in pts]
        check("through the points", all(d < tol for d in dists), dists)
        vp.finishTool()

        FreeCADGui.Command.get("Sketcher_CreateBSpline").run()
        n = len(sk.Geometry)
        vp.setManipulator(V(0, 70, 0), FreeCAD.Rotation())
        for p in [V(0, 70, 0), V(15, 80, 0), V(30, 70, 0)]:
            click(vp, p)
        vp.finishTool()
        check(
            "the spline by poles: the spline and three control points",
            len(sk.Geometry) == n + 4 and sk.Geometry[n].Degree == 2,
            (len(sk.Geometry) - n, getattr(sk.Geometry[n], "Degree", None)),
        )
        vp.finishTool()
        check(
            "the solver is happy",
            sk.Malformed == [] and sk.Conflicting == [],
            (sk.Malformed, sk.Conflicting),
        )

        # -- construction, undo ----------------------------------------------
        doc.recompute()
        edges = len(sk.Shape.Edges)
        FreeCADGui.Command.get("Sketcher_ToggleConstruction").run()
        check("Toggle Construction switches the tools to construction", vp.constructionMode())
        FreeCADGui.Command.get("Sketcher_CreateLine").run()
        n = len(sk.Geometry)
        vp.setManipulator(V(-10, -10, 0), FreeCAD.Rotation())
        click(vp, V(-10, -10, 0))
        click(vp, V(-10, -30, 0))
        vp.finishTool()
        doc.recompute()
        check("a construction line is made", len(sk.Geometry) == n + 1)
        check(
            "and kept out of the shape", len(sk.Shape.Edges) == edges, (len(sk.Shape.Edges), edges)
        )
        FreeCADGui.Command.get("Sketcher_ToggleConstruction").run()
        doc.undo()
        settle(0.3)
        check("one undo takes the line away", len(sk.Geometry) == n, len(sk.Geometry))
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
