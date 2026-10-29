"""The 3D sketch's corner fillet, phase S3c (docs/Sketch3D.md section 10).

The Sketcher's Fillet command in a 3D sketch's edit:

  - two selected lines that meet: their corner rounded by an arc in their plane,
    tangent to both, and a radius dimension whose value is typed next; the
    fillet and its value are one undo step;
  - the corner point selected does the same;
  - with nothing selected it starts a tool, and a click on a corner fillets it;
  - two lines that do not meet are refused, and nothing changes;
  - a new radius keeps the arc tangent to both lines.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "Sketch3DFillet"
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


def types(sk):
    return [c.Type for c in sk.Constraints]


def healthy(sk):
    return sk.Malformed == [] and sk.Conflicting == [] and sk.Redundant == []


def tangent_at_ends(sk, arc, a, b):
    """The arc's ends on the lines and its tangents along them, from either line."""
    return tangent_from(sk, arc, a, b) or tangent_from(sk, arc, b, a)


def tangent_from(sk, arc, a, b):
    g, la, lb = sk.Geometry[arc], sk.Geometry[a], sk.Geometry[b]
    out = []
    for end, param, line in (
        (g.StartPoint, g.FirstParameter, la),
        (g.EndPoint, g.LastParameter, lb),
    ):
        d = line.EndPoint - line.StartPoint
        t = g.tangent(param)[0]
        on = min((end - line.StartPoint).Length, (end - line.EndPoint).Length)
        out.append(on < 1e-6 and t.cross(d).Length < 1e-6 * d.Length)
    return all(out)


def run():
    try:
        import Part
        import Sketcher

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        # a route of three lines, its two bends off the frame's planes
        pts = [V(0, 0, 0), V(30, 0, 5), V(32, 25, 15), V(10, 30, 35)]
        for i in range(3):
            sk.addGeometry(Part.LineSegment(pts[i], pts[i + 1]))
        sk.addConstraint(Sketcher.Constraint("Coincident", 0, 2, 1, 1))
        sk.addConstraint(Sketcher.Constraint("Coincident", 1, 2, 2, 1))
        # an unconnected line, for the refusal
        sk.addGeometry(Part.LineSegment(V(-10, 20, 0), V(-10, 40, 0)))
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.viewIsometric()
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject
        vp.setRotationHandles(False)
        cmd = FreeCADGui.Command.get("Sketcher_CreateFillet")
        check("Fillet is active in a 3D sketch's edit", cmd.isActive())

        # -- two lines selected -------------------------------------------------
        undo = doc.UndoCount
        dof = sk.DoF
        vp.select(0)
        vp.select(1)
        cmd.run()
        settle(0.3)
        arc = 4
        g = sk.Geometry[arc] if len(sk.Geometry) > arc else None
        ok = g is not None and g.TypeId == "Part::GeomArcOfCircle"
        check("two lines selected: their corner rounded", ok, g)
        if ok:
            check("tangent to both, at their trimmed ends", tangent_at_ends(sk, arc, 0, 1))
            normal = (pts[1] - pts[0]).cross(pts[2] - pts[1])
            check("in their plane", g.Axis.cross(normal).Length < 1e-6 * normal.Length, g.Axis)
            check(
                "with a radius dimension",
                ("Radius", arc) in [(c.Type, c.First) for c in sk.Constraints],
                types(sk),
            )
            check("whose value is typed next", vp.isEditingDatum())
        vp.finishDatumEdit(True)
        settle(0.2)
        check("no DOF more: the radius is the one it adds", sk.DoF == dof, (sk.DoF, dof))
        check("one undo step", doc.UndoCount == undo + 1, doc.UndoCount - undo)
        check("healthy", healthy(sk), (sk.Malformed, sk.Conflicting, sk.Redundant))

        # a new radius keeps it tangent
        r = [i for i, c in enumerate(sk.Constraints) if c.Type == "Radius"][0]
        doc.openTransaction("radius")
        sk.setDatum(r, 4.0)
        doc.commitTransaction()
        settle(0.2)
        check(
            "a new radius keeps it tangent",
            abs(sk.Geometry[arc].Radius - 4) < 1e-7 and tangent_at_ends(sk, arc, 0, 1),
            sk.Geometry[arc].Radius,
        )

        doc.undo()  # the radius
        doc.undo()  # the fillet
        settle(0.3)
        check(
            "undo brings the corner back",
            len(sk.Geometry) == 4
            and (sk.Geometry[0].EndPoint - sk.Geometry[1].StartPoint).Length < 1e-9
            and types(sk) == ["Coincident", "Coincident"],
            (len(sk.Geometry), types(sk)),
        )

        # -- the corner point selected ------------------------------------------
        vp.clearSelection()
        vp.select(1, 2)
        cmd.run()
        settle(0.2)
        vp.finishDatumEdit(True)
        g = sk.Geometry[4] if len(sk.Geometry) > 4 else None
        ok = g is not None and g.TypeId == "Part::GeomArcOfCircle"
        check("the corner point selected: rounded too", ok and tangent_at_ends(sk, 4, 1, 2), g)

        # -- the tool, a click on the corner --------------------------------------
        vp.clearSelection()
        cmd.run()
        check(
            "nothing selected starts the fillet tool", vp.activeTool() == "Fillet", vp.activeTool()
        )
        n = len(sk.Geometry)
        x, y = vp.toView(sk.Geometry[0].EndPoint)
        vp.press(x, y)
        settle(0.2)
        vp.finishDatumEdit(True)
        g = sk.Geometry[n] if len(sk.Geometry) > n else None
        ok = g is not None and g.TypeId == "Part::GeomArcOfCircle"
        check("a click on a corner fillets it", ok and tangent_at_ends(sk, n, 0, 1), g)
        vp.finishTool()
        check("Escape ends the tool", vp.activeTool() == "None", vp.activeTool())

        # -- refused ----------------------------------------------------------------
        n, nc = len(sk.Geometry), len(sk.Constraints)
        vp.clearSelection()
        vp.select(0)
        vp.select(3)
        cmd.run()
        settle(0.2)
        check(
            "two lines that do not meet are refused",
            len(sk.Geometry) == n and len(sk.Constraints) == nc,
            (len(sk.Geometry) - n, len(sk.Constraints) - nc),
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
