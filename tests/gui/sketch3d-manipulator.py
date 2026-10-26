"""The 3D sketch's manipulator, phase S2b (docs/Sketch3D.md section 7).

Driven through the view provider's input entry points (press, move, pointAt,
toView), which are what the mouse handlers call, so the clicks are the edit's
own and no synthetic Qt event is needed:

  - the manipulator starts at the sketch frame's origin, plane XY, rotation
    handles shown; the Sketcher 3D toolbar is up in the edit only, its
    commands enabled;
  - a click lands on the active plane: a point's view position maps back to
    it, on XY, on YZ, and on a plane of a tilted manipulator;
  - Sketcher_Sketch3DCyclePlane cycles XY -> YZ -> ZX -> XY;
  - along an axis, beyond the gizmo, a click locks to the axis, a couple of
    pixels off it included;
  - Lock Height keeps the in-plane position and moves along the normal;
  - Reset puts the frame back;
  - a press on a plane handle picks that plane; a press on a sketch point
    moves the manipulator there.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "Sketch3DManipulator"
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


def toolbar_visible(name):
    bar = FreeCADGui.getMainWindow().findChild(QtWidgets.QToolBar, name)
    return bool(bar and bar.isVisible())


def dragger_scale():
    from pivy import coin

    view = FreeCADGui.getDocument(DOC).ActiveView
    sa = coin.SoSearchAction()
    sa.setName("Sketch3DManipulatorDragger")
    sa.setInterest(coin.SoSearchAction.FIRST)
    sa.apply(view.getViewer().getSoRenderManager().getSceneGraph())
    path = sa.getPath()
    return path.getTail().autoScaleResult.getValue() if path else None


def roundtrip(vp, p):
    xy = vp.toView(p)
    q = vp.pointAt(*xy) if xy else None
    return q, xy


def run():
    try:
        import Part

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        line = sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(40, 10, 5)))
        doc.recompute()
        settle(0.5)
        check(
            "outside an edit the manipulator commands are inactive",
            not FreeCADGui.Command.get("Sketcher_Sketch3DCyclePlane").isActive(),
        )
        check("and its toolbar is not up", not toolbar_visible("Sketcher 3D"))

        FreeCADGui.getDocument(DOC).setEdit(sk)
        settle(1.0)
        view = FreeCADGui.getDocument(DOC).ActiveView
        view.viewIsometric()
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject
        m = vp.getManipulator()
        check(
            "the manipulator starts at the origin, plane XY",
            m and m["Origin"].Length < 1e-9 and m["Plane"] == "XY",
            m,
        )
        check("with its rotation handles", m and m["RotationHandles"])
        check("the Sketcher 3D toolbar is up in the edit", toolbar_visible("Sketcher 3D"))
        # The edit's task dialog disables a command not made for an edit
        # (Command::testActive): its button and shortcut would do nothing.
        enabled = {
            n: FreeCADGui.Command.get(n).getAction()[0].isEnabled()
            for n in (
                "Sketcher_Sketch3DCyclePlane",
                "Sketcher_Sketch3DLockHeight",
                "Sketcher_Sketch3DKeepPlane",
                "Sketcher_Sketch3DResetManipulator",
                "Sketcher_Sketch3DRotationHandles",
                "Sketcher_Constrain3DAlongX",
                "Sketcher_ConstrainDistanceZ",
            )
        }
        check("and its commands are enabled", all(enabled.values()), enabled)
        check(
            "and the commands are active",
            FreeCADGui.Command.get("Sketcher_Sketch3DCyclePlane").isActive(),
        )

        def near(a, b, tol):
            return a is not None and (a - b).Length < tol

        tol = 0.3  # mm: the fit puts a pixel well under this on a 40 mm line
        p = V(25, 35, 0)
        q, xy = roundtrip(vp, p)
        check("a click lands on the XY plane", near(q, p, tol), (p, q, xy))
        # exactly on it, not a float's micron off: a point placed on XY has z = 0
        zs = [abs(vp.pointAt(xy[0] + dx, xy[1] + dy).z) for dx in range(-7, 8, 3) for dy in (-5, 4)]
        check("and exactly on it", max(zs) < 1e-12, max(zs))

        FreeCADGui.Command.get("Sketcher_Sketch3DCyclePlane").run()
        settle(0.2)
        check("Cycle goes to YZ", vp.getManipulator()["Plane"] == "YZ")
        p = V(0, 25, 35)
        q, xy = roundtrip(vp, p)
        check("and a click lands on the YZ plane", near(q, p, tol), (p, q, xy))
        FreeCADGui.Command.get("Sketcher_Sketch3DCyclePlane").run()
        check("then ZX", vp.getManipulator()["Plane"] == "ZX")
        FreeCADGui.Command.get("Sketcher_Sketch3DCyclePlane").run()
        check("then XY again", vp.getManipulator()["Plane"] == "XY")

        # A tilted frame: 30 deg about X, off the origin.
        rot = FreeCAD.Rotation(V(1, 0, 0), 30)
        origin = V(10, 0, 0)
        vp.setManipulator(origin, rot)
        settle(0.2)
        p = origin + rot.multVec(V(20, 15, 0))
        q, xy = roundtrip(vp, p)
        check("a click lands on the tilted plane", near(q, p, tol), (p, q, xy))
        normal = rot.multVec(V(0, 0, 1))
        offs = [
            abs((vp.pointAt(xy[0] + dx, xy[1] + dy) - origin).dot(normal))
            for dx in range(-7, 8, 3)
            for dy in (-5, 4)
        ]
        check("and exactly on it", max(offs) < 1e-12, max(offs))

        # Along the X axis beyond the gizmo: the axis, also two pixels off it.
        axis = rot.multVec(V(1, 0, 0))
        p = origin + axis * 30
        xy = vp.toView(p)
        q = vp.pointAt(xy[0], xy[1] + 2)
        off = (q - origin).cross(axis).Length if q else None
        check("along an axis a click locks to it", q is not None and off < 1e-6, (q, off))

        # Height lock: the position under the pointer, then along the normal.
        normal = rot.multVec(V(0, 0, 1))
        base = origin + rot.multVec(V(15, 20, 0))
        bxy = vp.toView(base)
        vp.move(*bxy)
        FreeCADGui.Command.get("Sketcher_Sketch3DLockHeight").run()
        check("Lock Height locks", vp.getManipulator()["HeightLock"])
        # the position it keeps is the plane's point under the pointer
        held = vp.getManipulator()["HeightBase"]
        check(
            "at the in-plane position under the pointer", (held - base).Length < tol, (held, base)
        )
        base = held
        target = base + normal * 12
        txy = vp.toView(target)
        q = vp.pointAt(*txy)
        dev = (q - base).cross(normal).Length if q else None
        check(
            "a locked click moves along the normal through the point",
            q is not None and dev < 1e-6,
            (q, dev),
        )
        check(
            "to the height asked",
            q is not None and abs((q - base).dot(normal) - 12) < tol,
            (q - base).dot(normal) if q else None,
        )
        FreeCADGui.Command.get("Sketcher_Sketch3DLockHeight").run()
        check("and again releases", not vp.getManipulator()["HeightLock"])

        FreeCADGui.Command.get("Sketcher_Sketch3DResetManipulator").run()
        m = vp.getManipulator()
        check(
            "Reset puts the frame back",
            m["Rotation"].isSame(FreeCAD.Rotation(), 1e-9) and m["Plane"] == "XY",
            m,
        )

        # Plane handles: the YZ square's middle, scaled as the gizmo is.
        vp.setManipulator(V(0, 0, 0), FreeCAD.Rotation())
        settle(0.5)
        scale = dragger_scale()
        check("the gizmo scales itself", scale is not None and scale > 0, scale)
        if scale:
            hxy = vp.toView(V(0, 4, 4) * scale)
            took = vp.press(*hxy)
            check(
                "a press on the YZ handle takes it",
                took and vp.getManipulator()["Plane"] == "YZ",
                (took, vp.getManipulator()["Plane"], hxy),
            )

        # A sketch point: the manipulator goes there.
        end = sk.Geometry[line].EndPoint
        exy = vp.toView(end)
        took = vp.press(*exy)
        m = vp.getManipulator()
        check(
            "a press on a sketch point moves the manipulator there",
            took and (m["Origin"] - end).Length < 1e-4,
            (took, m["Origin"]),
        )

        FreeCADGui.Command.get("Sketcher_Sketch3DRotationHandles").run()
        check("the rotation handles hide", not vp.getManipulator()["RotationHandles"])

        FreeCADGui.getDocument(DOC).resetEdit()
        settle(0.5)
        check("leaving takes the Sketcher 3D toolbar down", not toolbar_visible("Sketcher 3D"))
        check("and the manipulator with the edit", vp.getManipulator() is None)
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
