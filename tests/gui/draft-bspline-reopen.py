"""A document with a Draft BSpline opens again in the GUI.

It did not (2026-11-26): opening data/examples/draft_test_objects.FCStd or
EngineBlock.FCStd, or any document saved with a Draft BSpline, ended the
GUI in a stack overflow. ViewProviderWire.updateData of Shape reads
obj.Placement.Rotation.inverted(). A rotation read off a property writes
itself back after a method call that may change it
(PyObjectBase::startNotify), and RotationPy.xml did not say that inverted()
and isSame() change nothing: the placement was written again, the Part
feature's shape followed it, and updateData of Shape ran again, inside
itself. Claims: a BSpline made, saved and opened again; the two example
documents opened.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
            "WarnRecomputeOnRestore", False)
        import Draft

        doc = FreeCAD.newDocument("BSplineReopen")
        v = FreeCAD.Vector
        spline = Draft.make_bspline([v(0, 0, 0), v(10, 5, 0), v(20, 0, 0), v(30, 8, 0)])
        spline.Placement = FreeCAD.Placement(v(1, 2, 0), FreeCAD.Rotation(v(0, 0, 1), 30))
        doc.recompute()
        pump(0.5)
        name = spline.Name
        path = os.path.join(OUT, "bspline.FCStd")
        doc.saveAs(path)
        FreeCAD.closeDocument(doc.Name)
        pump(0.5)
        doc = FreeCAD.openDocument(path)
        pump(1.0)
        spline = doc.getObject(name)
        check("a BSpline saved opens again", spline is not None and not spline.Shape.isNull()
              and "Touched" not in spline.State, spline.State if spline else None)
        FreeCAD.closeDocument(doc.Name)
        for rel in ("draft_test_objects.FCStd", "EngineBlock.FCStd"):
            path = os.path.join(FreeCAD.getHomePath(), "data", "examples", rel)
            if not os.path.exists(path):
                note("INFO not installed: " + rel)
                continue
            d = FreeCAD.openDocument(path)
            pump(2.0)
            check("%s opens" % rel, len(d.Objects) > 0, len(d.Objects))
            FreeCAD.closeDocument(d.Name)
            pump(0.5)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
