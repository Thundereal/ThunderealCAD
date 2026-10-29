"""Geometry picked for a Surface feature is taken as a sketch takes external
geometry (docs/Surface.md): through Part::SubShapeBinder::import, which
links what is in the feature's scope directly and binds anything else into
the feature's body -- an import made once, reused, no face made of curves.

A body with a pad, and six curves outside it (one Part::Feature):
  - the body active, Network from the curves: its links are a binder in the
    body, of all six curves (no face made of them), the network built; the
    body moved, the network stays on the curves where they are;
  - an isocurve of the pad's top face links the pad itself (in scope);
  - the network's panel: a curve of the outside object added as a guide is
    linked through a binder in the body; picked again in Remove, it comes
    out;
  - the blend panel of a blend in the body: an edge of an outside plane
    picked as its first edge is linked through a binder;
  - the Sections panel of a Sections in the body: an outside curve added is
    linked through a binder;
  - with no body active, a network of the outside curves links them
    directly.

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
DOC = "SurfaceImport"
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


def panel(name):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget, name):
        if w.isVisible():
            return w
    return None


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


def linked(prop):
    return [o for o, _ in prop]


def is_binder(o):
    return o is not None and o.isDerivedFrom("Part::SubShapeBinder")


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
        extra = doc.addObject("Part::Feature", "Extra")
        extra.Shape = Part.LineSegment(V(0, 2, 0), V(10, 2, 0)).toShape()
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
        plane = doc.addObject("Part::Plane", "Plane")
        plane.Length = plane.Width = 10
        plane.Placement.Base = V(0, 20, 0)
        doc.recompute()
        view.setActiveObject("pdbody", body)
        pump(20)

        # Network from outside curves, the body active
        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(src)
        FreeCADGui.runCommand("Surface_Network")
        pump(80)
        net = newest(doc, before, "Network")
        press(QtWidgets.QDialogButtonBox.Ok)
        links = linked(net.Profiles) if net else []
        binder = links[0] if links else None
        check(
            "Network from outside curves links a binder in the body, of all six",
            net is not None
            and len(links) == 1
            and is_binder(binder)
            and binder in body.Group
            and len(binder.Shape.Edges) == 6
            and not binder.Shape.Faces
            and net.isValid(),
            (links, binder and len(binder.Shape.Edges), net and net.getStatusString()),
        )
        if net is None or not is_binder(binder):
            return finish()
        box = net.Shape.BoundBox
        body.Placement = FreeCAD.Placement(V(0, 0, 5), FreeCAD.Rotation())
        doc.recompute()
        world = net.Shape.transformed(net.getGlobalPlacement().toMatrix())
        check(
            "the body moved, the network stays on the curves",
            net.isValid() and abs(world.BoundBox.ZMax - box.ZMax) < 1e-6,
            (world.BoundBox, box),
        )

        # in scope: the pad's face is linked as it is
        top = [i for i, fc in enumerate(pad.Shape.Faces) if abs(fc.BoundBox.ZMin - 1) < 1e-9]
        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(pad, "Face%d" % (top[0] + 1))
        FreeCADGui.runCommand("Surface_Isocurve")
        pump(40)
        iso = newest(doc, before, "Isocurve")
        check(
            "an isocurve of the pad's face links the pad itself",
            iso is not None and iso.Face[0] == pad and iso in body.Group and iso.isValid(),
            (iso and iso.Face, iso and iso.getStatusString()),
        )

        # the network's panel: an outside curve added and taken out by picks
        gdoc.setEdit(net, 0)
        pump(60)
        p = panel("SurfaceGui_Network")
        add = p.findChild(QtWidgets.QToolButton, "buttonAddGuide") if p else None
        remove = p.findChild(QtWidgets.QToolButton, "buttonRemove") if p else None
        if add is not None:
            add.setChecked(True)
            pump(10)
            FreeCADGui.Selection.addSelection(src, "Edge1")
            pump(40)
            add.setChecked(False)
        guides = list(net.Guides)
        # import reuses a binder only where it can map the element through
        # it; this shape, set from Python, has no element map to map by, so
        # the edge gets a binder of its own, as a sketch's would
        check(
            "a curve added in the panel is linked through a binder in the body",
            len(guides) == 1 and is_binder(guides[0][0]) and guides[0][0] in body.Group,
            (guides, [o.Name for o in doc.Objects if is_binder(o)]),
        )
        if remove is not None:
            remove.setChecked(True)
            pump(10)
            FreeCADGui.Selection.addSelection(src, "Edge1")
            pump(40)
            remove.setChecked(False)
        check(
            "picked again in Remove, it comes out",
            len(net.Guides) == 0,
            list(net.Guides),
        )
        press(QtWidgets.QDialogButtonBox.Cancel)

        # a tool panel: the blend's first edge, an outside plane's
        blend = doc.addObject("Surface::BlendSurface", "Blend")
        body.addObject(blend)
        doc.recompute()
        gdoc.setEdit(blend, 0)
        pump(60)
        p = panel("SurfaceGui_BlendSurface")
        pick = p.findChild(QtWidgets.QToolButton, "pickEdge1") if p else None
        if pick is not None:
            pick.setChecked(True)
            pump(10)
            FreeCADGui.Selection.addSelection(plane, "Edge1")
            pump(40)
        edge1 = blend.Edge1
        check(
            "an outside edge picked for the blend is linked through a binder in the body",
            edge1 is not None and is_binder(edge1[0]) and edge1[0] in body.Group,
            (edge1, pick is not None),
        )
        press(QtWidgets.QDialogButtonBox.Cancel)

        # an older panel: Sections
        sec = doc.addObject("Surface::Sections", "Sections")
        body.addObject(sec)
        doc.recompute()
        gdoc.setEdit(sec, 0)
        pump(60)
        add = None
        for w in mw.findChildren(QtWidgets.QToolButton, "buttonEdgeAdd"):
            if w.isVisible():
                add = w
        if add is not None:
            add.setChecked(True)
            pump(10)
            FreeCADGui.Selection.addSelection(extra, "Edge1")
            pump(40)
        sections = list(sec.NSections)
        check(
            "an outside curve added to Sections is linked through a binder in the body",
            len(sections) == 1 and is_binder(sections[0][0]) and sections[0][0] in body.Group,
            (sections, add is not None),
        )
        press(QtWidgets.QDialogButtonBox.Cancel)

        # no body active: as it was
        view.setActiveObject("pdbody", None)
        pump(20)
        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(src)
        FreeCADGui.runCommand("Surface_Network")
        pump(80)
        net2 = newest(doc, before, "Network")
        press(QtWidgets.QDialogButtonBox.Ok)
        check(
            "with no body active, a network of the outside curves links them directly",
            net2 is not None and linked(net2.Profiles) == [src] and net2.isValid(),
            (net2 and net2.Profiles,),
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
