"""The 3D sketch's Spline on Face, phase S3d (docs/Sketch3D.md, decision f).

Points clicked on a model face (the view's preselection, as a click on it gives),
then Escape: a spline in the face's (u, v) through them, a Part.CurveOnSurface on
the face exactly.

  - the command starts the tool; a click off the face is refused;
  - the spline goes through the clicked points and lies on the face; the face
    is made a reference and the spline put on it; its points are sketch points
    at its knots; nothing is redundant; one undo takes it all away;
  - a click on another face of the same model is refused;
  - a knot point moved drags the spline, which stays on the face;
  - the model changed (the cylinder's radius), the spline follows the face;
  - its edge in the sketch's shape carries it on the cylinder.

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
DOC = "Sketch3DFaceSpline"
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


def click_on(vp, obj, sub, p):
    """A click on the model at p, the view preselecting obj.sub there as it would."""
    FreeCADGui.Selection.setPreselection(obj, sub, p.x, p.y, p.z)
    x, y = vp.toView(p)
    vp.move(x, y)
    took = vp.press(x, y)
    FreeCADGui.Selection.clearPreselection()
    return took


def on_cylinder(g, radius):
    worst = 0
    for i in range(17):
        u = g.FirstParameter + (g.LastParameter - g.FirstParameter) * i / 16
        p = g.value(u)
        worst = max(worst, abs(math.hypot(p.x, p.y) - radius))
    return worst


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        cyl = doc.addObject("Part::Cylinder", "Cyl")
        cyl.Radius, cyl.Height = 20, 30
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        doc.recompute()
        side = [
            "Face%d" % (i + 1)
            for i, f in enumerate(cyl.Shape.Faces)
            if f.Surface.TypeId == "Part::GeomCylinder"
        ][0]
        top = [
            "Face%d" % (i + 1)
            for i, f in enumerate(cyl.Shape.Faces)
            if f.Surface.TypeId == "Part::GeomPlane" and f.CenterOfMass.z > 1
        ][0]
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.viewIsometric()
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject
        vp.setRotationHandles(False)

        cmd = FreeCADGui.Command.get("Sketcher_Sketch3DSplineOnFace")
        check("Spline on Face is active in a 3D sketch's edit", cmd.isActive())
        cmd.run()
        settle(0.2)
        check("and starts its tool", vp.activeTool() == "SplineOnFace", vp.activeTool())

        # off the face: refused
        vp.setManipulator(V(-60, -60, 0), FreeCAD.Rotation())
        x, y = vp.toView(V(-50, -40, 0))
        vp.move(x, y)
        vp.press(x, y)
        check("a click off any face is refused", vp.placedCount() == 0, vp.placedCount())

        # four points on the side that faces the view
        pts = [
            V(20 * math.cos(math.radians(a)), 20 * math.sin(math.radians(a)), z)
            for a, z in ((15, 6), (35, 12), (55, 17), (75, 24))
        ]
        for p in pts:
            click_on(vp, cyl, side, p)
        check("the points are held until it is made", vp.placedCount() == 4, vp.placedCount())
        # the top face is another face
        click_on(vp, cyl, top, V(0, 0, 30))
        check("a click on another face is refused", vp.placedCount() == 4, vp.placedCount())

        n = len(sk.Geometry)
        undo = doc.UndoCount
        vp.finishTool()
        settle(0.3)
        g = sk.Geometry[n] if len(sk.Geometry) > n else None
        ok = g is not None and type(g).__name__ == "CurveOnSurface"
        check("Escape makes a spline on the face", ok, g)
        check("and keeps the tool", vp.activeTool() == "SplineOnFace", vp.activeTool())
        if ok:
            check("on the cylinder exactly", on_cylinder(g, 20) < 1e-9, on_cylinder(g, 20))
            import Part

            ds = [g.toShape().distToShape(Part.Vertex(p))[0] for p in pts]
            check("through the points clicked", max(ds) < 0.5, ds)
            ext = sk.ExternalGeometry
            check("the face made a reference", ext and ext[0][1][0] == side, ext)
            c = cons(sk)
            check("the spline put on it", ("PointOnObject", n, 0, -7, 0) in c, c)
            knots = [k for k in c if k[0] == "InternalAlignment" and k[3] == n]
            check("its four points at its knots", len(knots) == 4, knots)
            check(
                "nothing redundant or malformed",
                sk.Malformed == [] and sk.Redundant == [] and sk.Conflicting == [],
                (sk.Malformed, sk.Redundant, sk.Conflicting),
            )
            check("one undo step", doc.UndoCount == undo + 1, doc.UndoCount - undo)
        vp.finishTool()
        check("a second Escape ends the tool", vp.activeTool() == "None", vp.activeTool())

        # a knot point moved: the spline follows, on the face
        if ok:
            k = knots[1][1]
            q = sk.Geometry[k]
            target = V(q.X, q.Y, q.Z + 3)
            moved = sk.movePoint(k, 1, target)
            q = sk.Geometry[k]
            check(
                "a knot point moved drags the spline",
                moved == 0 and abs(q.Z - target.z) < 1e-6,
                (moved, q.Z, target.z),
            )
            g = sk.Geometry[n]
            check("which stays on the face", on_cylinder(g, 20) < 1e-9, on_cylinder(g, 20))

            # the model changed: the spline follows the face
            cyl.Radius = 26
            doc.recompute()
            settle(0.3)
            g = sk.Geometry[n]
            check(
                "the face grown, the spline follows it",
                on_cylinder(g, 26) < 1e-9,
                on_cylinder(g, 26),
            )
            edges = sk.Shape.Edges
            check(
                "its edge carries it on the cylinder",
                len(edges) == 1 and type(edges[0].curveOnSurface(0)[1]).__name__ == "Cylinder",
                [e.curveOnSurface(0) for e in edges],
            )

            while doc.UndoCount > undo:
                doc.undo()
            settle(0.3)
            check("undo takes the spline away", len(sk.Geometry) == n, len(sk.Geometry))
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
