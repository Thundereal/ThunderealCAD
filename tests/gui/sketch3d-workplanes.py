"""The 3D sketch's workplanes solved for, phase S3b (docs/Sketch3D.md section 6, v2).

Driven through the view provider's entry points and the Sketcher's own commands:

  - Keep Plane stores the working plane blocked: the v1 plane, fixed;
  - Horizontal and Vertical on a line drawn on the active kept plane run along
    its X and Y axes, with nothing redundant; with no plane active and none
    selected they are refused;
  - Block on the selected plane frees it (6 DOF more) and blocks it again;
  - a free plane moved carries the geometry held on it, and the manipulator
    stays on the active plane;
  - Symmetric of two points and a plane mirrors them in it;
  - Angle of two planes is a dimension with its label.

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
DOC = "Sketch3DWorkplanes"
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


def healthy(sk):
    return sk.Malformed == [] and sk.Conflicting == [] and sk.Redundant == []


def run_cmd(name):
    FreeCADGui.Command.get(name).run()
    settle(0.2)


def run():
    try:
        import Part
        import Sketcher

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
        sk.addGeometry(Part.Point(V(-20, -20, 0)), True)
        sk.addGeometry(Part.Point(V(60, 60, 50)), True)
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject
        vp.setRotationHandles(False)

        # -- Keep Plane: blocked ------------------------------------------------
        rot = FreeCAD.Rotation(V(1, 0, 0), 30)
        origin = V(0, 0, 20)
        vp.setManipulator(origin, rot)
        plane = vp.keepPlane()
        c = cons(sk)
        check("Keep Plane makes a plane", sk.Geometry[plane].TypeId == "Part::GeomPlane")
        check("and blocks it: the v1 plane", ("Block", plane, 0, -2000, 0) in c, c)

        # -- a line on it, Horizontal and Vertical on it ------------------------
        run_cmd("Sketcher_CreateLine")
        n = len(sk.Geometry)
        a = origin + rot.multVec(V(25, 10, 0))
        b = origin + rot.multVec(V(45, 16, 0))
        click(vp, a)
        click(vp, b)
        e = b + rot.multVec(V(3, 18, 0))
        click(vp, b)
        click(vp, e)
        vp.finishTool()
        check("two lines drawn on the kept plane", len(sk.Geometry) == n + 2, len(sk.Geometry))
        on = [k for k in cons(sk) if k[0] == "PointOnObject" and k[3] == plane]
        check("their ends held on it", len(on) >= 3, on)
        vp.clearSelection()
        vp.select(n)
        run_cmd("Sketcher_ConstrainHorizontal")
        c = cons(sk)
        check("Horizontal on the active kept plane", ("Horizontal", n, 0, plane, 0) in c, c)
        vp.select(n + 1)
        run_cmd("Sketcher_ConstrainVertical")
        c = cons(sk)
        check("Vertical too", ("Vertical", n + 1, 0, plane, 0) in c, c)
        x, y = rot.multVec(V(1, 0, 0)), rot.multVec(V(0, 1, 0))
        g0, g1 = sk.Geometry[n], sk.Geometry[n + 1]
        d0, d1 = g0.EndPoint - g0.StartPoint, g1.EndPoint - g1.StartPoint
        check("the first along the plane's X", d0.cross(x).Length < 1e-6 * d0.Length, d0)
        check("the second along its Y", d1.cross(y).Length < 1e-6 * d1.Length, d1)
        check("nothing redundant", healthy(sk), (sk.Malformed, sk.Conflicting, sk.Redundant))

        vp.activateKeptPlane(-1)
        vp.clearSelection()
        vp.select(n)
        nc = len(sk.Constraints)
        run_cmd("Sketcher_ConstrainHorizontal")
        check(
            "with no plane active or selected, Horizontal is refused",
            len(sk.Constraints) == nc,
            cons(sk)[nc:],
        )
        vp.clearSelection()

        # -- Block toggles: the plane freed, then fixed again -------------------
        dof = sk.DoF
        vp.select(plane)
        run_cmd("Sketcher_ConstrainBlock")
        blocks = [k for k in cons(sk) if k[0] == "Block" and k[1] == plane]
        check("Block on a blocked plane frees it", blocks == [], blocks)
        check("6 DOF more: its frame", sk.DoF - dof == 6, sk.DoF - dof)
        vp.select(plane)
        run_cmd("Sketcher_ConstrainBlock")
        blocks = [k for k in cons(sk) if k[0] == "Block" and k[1] == plane]
        check("and again blocks it", len(blocks) == 1 and sk.DoF == dof, (blocks, sk.DoF, dof))
        vp.select(plane)
        run_cmd("Sketcher_ConstrainBlock")
        settle(0.2)

        # -- a free plane moved carries its lines; the manipulator stays on it --
        vp.activateKeptPlane(plane)
        before = sk.Geometry[n].StartPoint
        moved = sk.movePoint(plane, 0, V(0, 0, 7), True)
        settle(0.3)
        pl = sk.Geometry[plane]
        check("the free plane moves", moved == 0 and abs(pl.Position.z - 27) < 1e-6, pl.Position)
        q = sk.Geometry[n].StartPoint
        check("its line with it", abs((q - pl.Position).dot(pl.Axis)) < 1e-6, (before, q))
        m = vp.getManipulator()
        off = (m["Origin"] - pl.Position).dot(pl.Axis)
        check("and the manipulator on it", abs(off) < 1e-6, off)
        vp.activateKeptPlane(-1)

        # -- symmetric about a plane ---------------------------------------------
        base = sk.addGeometry(Part.Plane(V(0, 0, -10), V(0, 0, 1)))
        sk.addConstraint(Sketcher.Constraint("Block", base))
        p1 = sk.addGeometry(Part.Point(V(30, -5, -2)))
        p2 = sk.addGeometry(Part.Point(V(31, -4, -19)))
        settle(0.2)
        vp.clearSelection()
        vp.select(p1, 1)
        vp.select(p2, 1)
        vp.select(base)
        run_cmd("Sketcher_ConstrainSymmetric")
        g1, g2 = sk.Geometry[p1], sk.Geometry[p2]
        mid = (V(g1.X, g1.Y, g1.Z) + V(g2.X, g2.Y, g2.Z)) * 0.5
        check("Symmetric of two points and a plane: the middle on it", abs(mid.z + 10) < 1e-6, mid)
        check(
            "and the two across it",
            V(g2.X - g1.X, g2.Y - g1.Y, 0).Length < 1e-6,
            (V(g1.X, g1.Y, g1.Z), V(g2.X, g2.Y, g2.Z)),
        )

        # -- the angle of two planes ---------------------------------------------
        vp.clearSelection()
        vp.select(base)
        vp.select(plane)
        nc = len(sk.Constraints)
        run_cmd("Sketcher_ConstrainAngle")
        if vp.isEditingDatum():
            vp.finishDatumEdit(True)
        settle(0.2)
        added = cons(sk)[nc:]
        check("Angle of two planes", added and added[0][0] == "Angle", added)
        value = sk.Constraints[nc].Value if added else None
        tilt = math.degrees(sk.Geometry[plane].Axis.getAngle(V(0, 0, 1)))
        check("what they make now", value is not None and abs(math.degrees(value) - tilt) < 1e-6)
        check("with its label", nc in vp.getLabels(), vp.getLabels())
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
