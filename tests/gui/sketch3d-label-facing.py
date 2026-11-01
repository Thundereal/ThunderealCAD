"""The 3D sketch's dimension labels read from the front (docs/Sketch3D.md sec 7).

A label is drawn in a plane through what it measures; a radius's in its arc's
plane, an angle's in its lines'. Seen from behind, a plane mirrors its text
("mm 00.8"). So each label's plane faces the eye, whichever side the camera
is on:
  - two fillets' Radius labels, an Angle, and a Distance, from the isometric
    view and from the opposite side.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import os
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = App.Vector


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(s=0.4):
    end = time.monotonic() + s
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def run():
    try:
        import Part
        import Sketcher

        doc = App.newDocument("Label")
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        pts = [V(0, 0, 0), V(0, 0, 30), V(40, 0, 30), V(40, 35, 30)]
        for i in range(3):
            sk.addGeometry(Part.LineSegment(pts[i], pts[i + 1]))
        sk.addConstraint([
            Sketcher.Constraint("Coincident", -1, 1, 0, 1),
            Sketcher.Constraint("Coincident", 0, 2, 1, 1),
            Sketcher.Constraint("Coincident", 1, 2, 2, 1),
            Sketcher.Constraint("AlongZ", 0),
            Sketcher.Constraint("AlongX", 1),
            Sketcher.Constraint("AlongY", 2),
        ])
        for first, second in ((0, 1), (1, 2)):
            arc = sk.fillet(first, 2, second, 1, 8.0)
            sk.addConstraint(Sketcher.Constraint("Radius", arc, 8.0))
        sk.addConstraint(Sketcher.Constraint("Angle", 0, 2, 1.5707963267948966))
        sk.addConstraint(Sketcher.Constraint("Distance", 1, 40.0))
        doc.recompute()
        gdoc = Gui.getDocument(doc.Name)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.setCameraType("Orthographic")
        vp = sk.ViewObject
        for name, direction in (("isometric", None), ("the other side", (1, -1, 1))):
            if direction:
                view.setViewDirection(direction)
            else:
                view.viewIsometric()
            view.fitAll()
            settle(1.0)
            d = view.getViewDirection()
            toEye = V(-d[0], -d[1], -d[2])
            facing = {}
            for i, text in sorted(vp.getLabels().items()):
                plc = vp.getLabelPlane(i)
                n = plc.Rotation.multVec(V(0, 0, 1))
                facing["%d %s" % (i, sk.Constraints[i].Type)] = round(n.dot(toEye), 3)
            check(
                "from %s every label faces the eye" % name,
                len(facing) == 4 and all(v > 1e-6 for v in facing.values()),
                facing,
            )
        gdoc.resetEdit()
        App.closeDocument(doc.Name)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
