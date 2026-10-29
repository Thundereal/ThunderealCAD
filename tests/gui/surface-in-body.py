"""Surface features in a PartDesign Body, as a sketch is (docs/Surface.md):
the Surface commands put what they make into the active body.

A body with a pad and a SubShapeBinder of a network's six curves:
  - with the body active, Network from the binder makes a network in the
    body, OK keeps it; the pad stays the Tip;
  - an isocurve of its face goes into the body too;
  - with no body active, the same command leaves its isocurve outside.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import math
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SurfaceInBody"
V = FreeCAD.Vector
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        time.sleep(0.01)


def press(which):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            pump(40)
            return True
    return False


def f(u, v):
    return V(10 * u, 10 * v, 2 * math.sin(math.pi * u) * math.sin(math.pi * v))


def curve(alongU, fixed):
    c = Part.BSplineCurve()
    c.interpolate([f(t / 4, fixed) if alongU else f(fixed, t / 4) for t in range(5)])
    return c


def newest(doc, before, prefix):
    made = [o for o in doc.Objects if o.Name not in before and o.Name.startswith(prefix)]
    return made[0] if made else None


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        pump(50)
        doc = FreeCAD.newDocument(DOC)
        gdoc = FreeCADGui.getDocument(DOC)
        view = gdoc.ActiveView

        import TestSketcherApp

        body = doc.addObject("PartDesign::Body", "Body")
        src = doc.addObject("Part::Feature", "Curves")
        src.Shape = Part.Compound(
            [curve(a, x).toShape() for a in (True, False) for x in (0, 0.5, 1)]
        )
        binder = body.newObject("PartDesign::SubShapeBinder", "Binder")
        binder.Support = [(src, ["Edge%d" % i for i in range(1, 7)])]
        binder.MakeFace = False
        sketch = body.newObject("Sketcher::SketchObject", "Square")
        sketch.Support = (
            [o for o in body.Origin.OriginFeatures if o.Role == "XY_Plane"][0],
            [""],
        )
        sketch.MapMode = "FlatFace"
        TestSketcherApp.CreateRectangleSketch(sketch, (2, 2), (6, 6))
        doc.recompute()
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 1
        doc.recompute()
        src.Visibility = False
        view.setActiveObject("pdbody", body)
        pump(20)

        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(binder)
        FreeCADGui.runCommand("Surface_Network")
        pump(80)
        net = newest(doc, before, "Network")
        press(QtWidgets.QDialogButtonBox.Ok)
        check(
            "with the body active, the network goes into it, built",
            net is not None and net in body.Group and net.isValid() and gdoc.getInEdit() is None,
            (net and net.Name, net and net.getStatusString(), [o.Name for o in body.Group]),
        )
        if net is None:
            return finish()
        check("the pad stays the Tip", body.Tip == pad, body.Tip and body.Tip.Name)

        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(net, "Face1")
        FreeCADGui.runCommand("Surface_Isocurve")
        pump(40)
        iso = newest(doc, before, "Isocurve")
        check(
            "an isocurve of its face goes into the body too",
            iso is not None and iso in body.Group and iso.isValid(),
            (iso and iso.Name, [o.Name for o in body.Group]),
        )

        view.setActiveObject("pdbody", None)
        pump(20)
        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(net, "Face1")
        FreeCADGui.runCommand("Surface_Isocurve")
        pump(40)
        iso2 = newest(doc, before, "Isocurve")
        check(
            "with no body active, it is left outside",
            iso2 is not None and iso2 not in body.Group and iso2.isValid(),
            (iso2 and iso2.Name, iso2 and iso2.getStatusString()),
        )
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
