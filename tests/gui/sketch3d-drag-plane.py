"""The 3D sketch's drag and kept planes, phase S2e (docs/Sketch3D.md sections 6 and 7).

  - a press on a sketch point and a move with the button held drags it on the
    working plane through it, the constraints kept: a line's end at a fixed
    length from a fixed start swings round; the drag is one undo step;
  - a free line taken by its middle moves with the pointer;
  - Keep Plane stores the working plane (tilted here) as a fixed workplane and
    makes it the kept plane: a polyline drawn then has every point held on it,
    and stays on it when a point is dragged;
  - Reset lets the kept plane go; Ctrl+press on its outline takes it again;
  - Ctrl+press on a planar face of the model takes that face's plane.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "Sketch3DDragPlane"
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


def dragger_scale():
    from pivy import coin

    view = FreeCADGui.getDocument(DOC).ActiveView
    sa = coin.SoSearchAction()
    sa.setName("Sketch3DManipulatorDragger")
    sa.setInterest(coin.SoSearchAction.FIRST)
    sa.apply(view.getViewer().getSoRenderManager().getSceneGraph())
    return sa.getPath().getTail().autoScaleResult.getValue()


def drag(vp, start, path):
    """A press at start, moves with the button held along path, a release."""
    x, y = vp.toView(start)
    vp.press(x, y)
    for p in path:
        x, y = vp.toView(p)
        vp.dragTo(x, y)
    return vp.release(x, y)


def run():
    try:
        import Part
        import Sketcher

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        box = doc.addObject("Part::Box", "Box")
        box.Placement.Base = V(40, -10, 0)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(20, 0, 0)))
        sk.addGeometry(Part.LineSegment(V(-30, 10, 0), V(-20, 10, 0)))
        sk.addConstraint(Sketcher.Constraint("Coincident", -1, 1, 0, 1))
        sk.addConstraint(Sketcher.Constraint("Distance", 0, 20.0))
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.viewIsometric()
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject

        # -- drag a constrained end -------------------------------------------
        undo = doc.UndoCount
        dragged = drag(vp, V(20, 0, 0), [V(18, 6, 0), V(12, 14, 0), V(1, 22, 0)])
        end = sk.Geometry[0].EndPoint
        check("a press and a move drag the end", dragged, dragged)
        check(
            "it swung round at its length",
            abs(end.Length - 20) < 1e-6 and end.y > 15 and abs(end.z) < 1e-9,
            end,
        )
        check("the drag is one undo step", doc.UndoCount == undo + 1, doc.UndoCount - undo)
        doc.undo()
        settle(0.3)
        check(
            "which undo takes back",
            (sk.Geometry[0].EndPoint - V(20, 0, 0)).Length < 1e-6,
            sk.Geometry[0].EndPoint,
        )

        # -- drag a free line by its middle ------------------------------------
        drag(vp, V(-25, 10, 0), [V(-25, 15, 0), V(-25, 20, 0)])
        g = sk.Geometry[1]
        check(
            "a free line moves with the pointer",
            abs(g.StartPoint.y - 20) < 0.3
            and abs(g.EndPoint.y - 20) < 0.3
            and abs((g.EndPoint - g.StartPoint).Length - 10) < 1e-6,
            (g.StartPoint, g.EndPoint),
        )

        # -- keep a tilted plane, draw on it -----------------------------------
        rot = FreeCAD.Rotation(V(1, 0, 0), 30)
        origin = V(0, -20, 10)
        vp.setManipulator(origin, rot)
        settle(0.3)
        cmd = FreeCADGui.Command.get("Sketcher_Sketch3DKeepPlane")
        check("Keep Plane is active in the edit", cmd.isActive())
        n = len(sk.Geometry)
        cmd.run()
        settle(0.3)
        plane = n
        check(
            "it stores a workplane",
            len(sk.Geometry) == n + 1 and type(sk.Geometry[plane]).__name__ == "Plane",
            type(sk.Geometry[-1]).__name__,
        )
        check("and keeps it", vp.activeKeptPlane() == plane, vp.activeKeptPlane())
        normal = rot.multVec(V(0, 0, 1))
        on = [origin + rot.multVec(V(u, v, 0)) for u, v in ((-15, 8), (-5, 14), (6, 9))]
        FreeCADGui.Command.get("Sketcher_CreatePolyline").run()
        for p in on:
            x, y = vp.toView(p)
            vp.move(x, y)
            vp.press(x, y)
        vp.finishTool()
        vp.finishTool()
        made = [i for i in range(len(sk.Geometry)) if i > plane]
        held = [c for c in sk.Constraints if c.Type == "PointOnObject" and c.Second == plane]
        check(
            "every point of the polyline is held on the kept plane",
            len(made) == 2 and len(held) == 3,
            (made, [(c.First, c.FirstPos) for c in held]),
        )

        def off_plane():
            pts = []
            for i in made:
                pts += [sk.Geometry[i].StartPoint, sk.Geometry[i].EndPoint]
            return max(abs((p - origin).dot(normal)) for p in pts)

        check("on it", off_plane() < 1e-9, off_plane())
        # a drag on another plane: the point stays on the kept one
        FreeCADGui.Command.get("Sketcher_Sketch3DResetManipulator").run()
        check("Reset lets the kept plane go", vp.activeKeptPlane() == -1, vp.activeKeptPlane())
        p0 = sk.Geometry[made[0]].StartPoint
        drag(vp, p0, [p0 + V(0, 3, 0), p0 + V(0, 6, 0)])
        check("a dragged point stays on it", off_plane() < 1e-9, off_plane())
        check(
            "the solver is happy",
            sk.Malformed == [] and sk.Conflicting == [],
            (sk.Malformed, sk.Conflicting),
        )

        # -- Ctrl+press: the kept plane again, a face of the model -------------
        vp.activateKeptPlane(plane)  # draws its outline at today's size
        vp.activateKeptPlane(-1)
        settle(0.3)
        h = 1.5 * 10 * dragger_scale()
        # the outline's lower edge: its right one lies over the polyline on screen
        edge = origin + rot.multVec(V(0, -h, 0))
        x, y = vp.toView(edge)
        took = vp.press(x, y, True)
        check(
            "Ctrl+press on its outline takes it again",
            took and vp.activeKeptPlane() == plane,
            (took, vp.activeKeptPlane()),
        )
        face = None
        for i, f in enumerate(box.Shape.Faces):
            if abs(f.CenterOfMass.x - 50) < 1e-9:
                face = "Face%d" % (i + 1)
        point = V(50, -5, 5)
        FreeCADGui.Selection.setPreselection(box, face, point.x, point.y, point.z)
        x, y = vp.toView(point)
        took = vp.press(x, y, True)
        FreeCADGui.Selection.clearPreselection()
        m = vp.getManipulator()
        n = m["WorkingPlane"].Rotation.multVec(V(0, 0, 1))
        check(
            "Ctrl+press on a planar face takes its plane",
            took and abs(abs(n.x) - 1) < 1e-9,
            (took, n, face),
        )
        check("through the point pressed", abs(m["Origin"].x - 50) < 1e-6, m["Origin"])
        check("and lets the kept plane go", vp.activeKeptPlane() == -1)
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
