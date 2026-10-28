"""The 3D sketch's circles and arcs, phase S3a (docs/Sketch3D.md section 10).

Started by the Sketcher's own commands in a 3D sketch's edit and driven through
the view provider's input entry points, as tests/gui/sketch3d-tools.py does:

  - a circle from its centre lies in the working plane: XY, then YZ;
  - an arc from its centre runs the way the pointer swept: a quarter turn
    counterclockwise, three quarters clockwise;
  - from three points the circle or arc lies in the plane they span, off every
    frame plane, tied to the sketch points it was clicked on: the ends
    coincident, a point it runs through on it;
  - on a kept plane a circle is held in it (centre on the plane, parallel to
    it), 3 DOF of its 6, and one undo takes it all away;
  - Coincident of two circles makes them coaxial; Perpendicular of a line and a
    circle puts the line along the circle's axis;
  - Escape drops a half-placed circle and keeps the tool; a second ends it.

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
DOC = "Sketch3DArcs"
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


def click(vp, p):
    x, y = vp.toView(p)
    vp.move(x, y)
    return vp.press(x, y)


def hover(vp, p):
    vp.move(*vp.toView(p))


def parallel(a, b):
    return a.cross(b).Length < 1e-6 * a.Length * b.Length


def healthy(sk):
    return sk.Malformed == [] and sk.Conflicting == [] and sk.Redundant == []


def run():
    try:
        import Part

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.viewIsometric()
        settle(0.3)
        # the region drawn in, so it is in view at a useful size
        sk.addGeometry(Part.Point(V(-30, -70, -10)), True)
        sk.addGeometry(Part.Point(V(70, 50, 40)), True)
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject
        tol = 0.3
        # The rotation rings take a press on them (they tilt the manipulator), and a
        # rim is clicked near the centre the manipulator stands on: off for this.
        vp.setRotationHandles(False)

        # -- circle from its centre, XY then YZ -------------------------------
        cmd = FreeCADGui.Command.get("Sketcher_CreateCircle")
        check("Circle is active in a 3D sketch's edit", cmd.isActive())
        cmd.run()
        settle(0.2)
        check("and starts the 3D circle tool", vp.activeTool() == "Circle", vp.activeTool())
        n = len(sk.Geometry)
        vp.setManipulator(V(10, 10, 0), FreeCAD.Rotation())
        click(vp, V(10, 10, 0))
        check("the centre alone makes nothing", len(sk.Geometry) == n, len(sk.Geometry))
        click(vp, V(16, 18, 0))
        c1 = n
        g = sk.Geometry[c1] if len(sk.Geometry) > c1 else None
        ok = g is not None and g.TypeId == "Part::GeomCircle"
        check("the rim makes a circle", ok, g)
        if ok:
            check("centred where clicked", (g.Center - V(10, 10, 0)).Length < tol, g.Center)
            check("radius to the rim", abs(g.Radius - 10) < tol, g.Radius)
            check("in the XY plane", parallel(g.Axis, V(0, 0, 1)), g.Axis)

        vp.setManipulator(V(0, 40, 0), FreeCAD.Rotation())
        vp.cycleWorkingPlane()  # YZ
        click(vp, V(0, 40, 0))
        click(vp, V(0, 46, 8))
        c2 = n + 1
        g = sk.Geometry[c2] if len(sk.Geometry) > c2 else None
        ok = g is not None and g.TypeId == "Part::GeomCircle"
        check("a second circle, the working plane cycled", ok, g)
        if ok:
            check("in the YZ plane", parallel(g.Axis, V(1, 0, 0)), g.Axis)
            check("its radius", abs(g.Radius - 10) < tol, g.Radius)

        # Escape with the centre placed drops it and keeps the tool
        vp.setManipulator(V(-20, 0, 0), FreeCAD.Rotation())
        click(vp, V(-20, 0, 0))
        vp.finishTool()
        check(
            "Escape drops the half-placed circle and keeps the tool",
            vp.activeTool() == "Circle" and vp.placedCount() == 0 and len(sk.Geometry) == n + 2,
            (vp.activeTool(), vp.placedCount(), len(sk.Geometry)),
        )
        vp.finishTool()
        check("a second Escape ends it", vp.activeTool() == "None")

        # -- arc from its centre ----------------------------------------------
        FreeCADGui.Command.get("Sketcher_CreateArc").run()
        check("Arc from centre starts the 3D arc tool", vp.activeTool() == "Arc", vp.activeTool())
        vp.resetManipulator()  # back to the XY plane
        n = len(sk.Geometry)
        vp.setManipulator(V(50, 0, 0), FreeCAD.Rotation())
        click(vp, V(50, 0, 0))
        click(vp, V(60, 0, 0))
        click(vp, V(50, 10, 0))  # a quarter turn, counterclockwise
        a1 = n
        g = sk.Geometry[a1] if len(sk.Geometry) > a1 else None
        ok = g is not None and g.TypeId == "Part::GeomArcOfCircle"
        check("an arc is made", ok, g)
        if ok:
            check("from the start", (g.StartPoint - V(60, 0, 0)).Length < tol, g.StartPoint)
            check("to the end", (g.EndPoint - V(50, 10, 0)).Length < tol, g.EndPoint)
            # the angle, not the length: the centre is a click, a pixel off
            turn = g.toShape().Length / g.Radius
            check("the quarter the pointer swept", abs(turn - math.pi / 2) < 0.05, turn)

        vp.setManipulator(V(50, 30, 0), FreeCAD.Rotation())
        click(vp, V(50, 30, 0))
        click(vp, V(60, 30, 0))
        # round the other way: below, behind, then up to the end
        for p in (V(57, 23, 0), V(50, 20, 0), V(43, 23, 0), V(40, 30, 0), V(43, 37, 0)):
            hover(vp, p)
        click(vp, V(50, 40, 0))
        a2 = n + 1
        g = sk.Geometry[a2] if len(sk.Geometry) > a2 else None
        ok = g is not None and g.TypeId == "Part::GeomArcOfCircle"
        check("a second arc", ok, g)
        if ok:
            turn = g.toShape().Length / g.Radius
            check("three quarters, clockwise as swept", abs(turn - 1.5 * math.pi) < 0.05, turn)
            check("still from the start", (g.StartPoint - V(60, 30, 0)).Length < tol, g.StartPoint)
        vp.finishTool()
        check("the centres are tied where clicked", sk.Malformed == [], sk.Malformed)

        # -- three points, off every frame plane ------------------------------
        n = len(sk.Geometry)
        pa, pb, pm = V(0, -30, 0), V(20, -30, 20), V(0, -30, 20)
        for p in (pa, pb, pm):
            sk.addGeometry(Part.Point(p))
        settle(0.2)
        FreeCADGui.Command.get("Sketcher_Create3PointArc").run()
        check("Arc by three points", vp.activeTool() == "Arc3Point", vp.activeTool())
        for p in (pa, pb, pm):
            click(vp, p)
        a3 = n + 3
        g = sk.Geometry[a3] if len(sk.Geometry) > a3 else None
        ok = g is not None and g.TypeId == "Part::GeomArcOfCircle"
        check("an arc through the three points", ok, g)
        if ok:
            check("in the plane they span", parallel(g.Axis, V(0, 1, 0)), g.Axis)
            check("from the first", (g.StartPoint - pa).Length < 1e-6, g.StartPoint)
            check("to the second", (g.EndPoint - pb).Length < 1e-6, g.EndPoint)
            d = g.toShape().distToShape(Part.Vertex(pm))[0]
            check("through the third", d < 1e-6, d)
            c = cons(sk)
            check("its start coincident with the point", ("Coincident", a3, 1, n, 1) in c, c)
            check("its end with the second", ("Coincident", a3, 2, n + 1, 1) in c, c)
            check("the third point on it", ("PointOnObject", n + 2, 1, a3, 0) in c, c)
        vp.finishTool()

        n = len(sk.Geometry)
        pts = [V(30, -60, 0), V(40, -60, 10), V(30, -50, 10)]
        for p in pts:
            sk.addGeometry(Part.Point(p))
        settle(0.2)
        FreeCADGui.Command.get("Sketcher_Create3PointCircle").run()
        check("Circle by three points", vp.activeTool() == "Circle3Point", vp.activeTool())
        for p in pts:
            click(vp, p)
        c3 = n + 3
        g = sk.Geometry[c3] if len(sk.Geometry) > c3 else None
        ok = g is not None and g.TypeId == "Part::GeomCircle"
        check("a circle through three points", ok, g)
        if ok:
            ds = [g.toShape().distToShape(Part.Vertex(p))[0] for p in pts]
            check("through each", all(d < 1e-6 for d in ds), ds)
            normal = (pts[1] - pts[0]).cross(pts[2] - pts[0])
            check("tilted as they are", parallel(g.Axis, normal), g.Axis)
            on = [k for k in cons(sk) if k[0] == "PointOnObject" and k[3] == c3]
            check("the three points on it", len(on) == 3, on)
        vp.finishTool()
        check("the solver is happy", healthy(sk), (sk.Malformed, sk.Conflicting, sk.Redundant))

        # -- on a kept plane ----------------------------------------------------
        vp.resetManipulator()  # back to the XY plane
        rot = FreeCAD.Rotation(V(1, 0, 0), 30)
        origin = V(0, 0, 30)
        vp.setManipulator(origin, rot)
        plane = vp.keepPlane()
        dof = sk.DoF
        nc = len(sk.Constraints)
        FreeCADGui.Command.get("Sketcher_CreateCircle").run()
        n = len(sk.Geometry)
        # clear of the manipulator's plane handles, which take a press near it
        centre = origin + rot.multVec(V(25, 20, 0))
        click(vp, centre)
        click(vp, centre + rot.multVec(V(7, 0, 0)))
        vp.finishTool()
        g = sk.Geometry[n] if len(sk.Geometry) > n else None
        ok = g is not None and g.TypeId == "Part::GeomCircle"
        check("a circle on the kept plane", ok, g)
        if ok:
            check("in it", parallel(g.Axis, rot.multVec(V(0, 0, 1))), g.Axis)
            c = cons(sk)
            check("its centre held on the plane", ("PointOnObject", n, 3, plane, 0) in c, c)
            check("and it parallel to the plane", ("Parallel", n, 0, plane, 0) in c, c)
            check("3 of its 6 DOF left", sk.DoF - dof == 3, sk.DoF - dof)
        check("still healthy", healthy(sk), (sk.Malformed, sk.Conflicting, sk.Redundant))
        doc.undo()
        settle(0.3)
        check(
            "one undo takes the circle and its constraints",
            len(sk.Geometry) == n and len(sk.Constraints) == nc,
            (len(sk.Geometry), len(sk.Constraints)),
        )
        vp.activateKeptPlane(-1)
        vp.resetManipulator()

        # -- coaxial, a line along an axis -------------------------------------
        vp.clearSelection()
        vp.select(c1)
        vp.select(c2)
        nc = len(sk.Constraints)
        FreeCADGui.Command.get("Sketcher_ConstrainCoincidentUnified").run()
        settle(0.3)
        added = [k[0] for k in cons(sk)[nc:]]
        check("Coincident of two circles: coaxial", added == ["Coincident", "Parallel"], added)
        g1, g2 = sk.Geometry[c1], sk.Geometry[c2]
        check(
            "their centres together", (g1.Center - g2.Center).Length < 1e-6, (g1.Center, g2.Center)
        )
        check("their axes parallel", parallel(g1.Axis, g2.Axis), (g1.Axis, g2.Axis))

        line = sk.addGeometry(Part.LineSegment(V(-40, 0, 0), V(-40, 10, 5)))
        settle(0.2)
        vp.clearSelection()
        vp.select(line)
        vp.select(c1)
        FreeCADGui.Command.get("Sketcher_ConstrainPerpendicular").run()
        settle(0.3)
        g, g1 = sk.Geometry[line], sk.Geometry[c1]
        check(
            "Perpendicular of a line and a circle: along its axis",
            parallel(g.EndPoint - g.StartPoint, g1.Axis),
            (g.EndPoint - g.StartPoint, g1.Axis),
        )
        check("healthy at the end", healthy(sk), (sk.Malformed, sk.Conflicting, sk.Redundant))
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
