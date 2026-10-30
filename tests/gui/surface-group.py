"""A surface group in a PartDesign Body (docs/SheetMetal.md sec 8): an aux
group that holds Surface features, and brings the Surface tool bar into
PartDesign while it is active.

In PartDesign, with a body active:
  - the Surface tool bar is there but not offered (no toggle in the menus);
  - PartDesign_SurfaceGroup makes a group named Surfaces in the body, with
    the surface icon overlaid on the group's, and makes it active, which
    offers the tool bar;
  - a Surface command run while it is active puts its feature in it;
  - a double-click on the group lets it go and takes the tool bar away; a
    second makes it active again;
  - the tool bar stays offered across a workbench switch and back;
  - with no group active, editing a Surface feature offers the tool bar and
    leaving the edit takes it away; with a group active, leaving the edit
    keeps it; editing a PartDesign feature offers nothing;
  - a feature in a surface group is not in the body's Misc group too, and a
    Surface feature dropped on the group moves there; a sketch cannot be
    dropped on it;
  - saved and reopened, the group is a surface group with its members;
  - deleting the active group takes the tool bar away.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import os
import tempfile
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SurfaceGroup"
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


def toolbar():
    for tb in FreeCADGui.getMainWindow().findChildren(QtWidgets.QToolBar, "Surface"):
        return tb
    return None


def offered():
    """Whether the Surface tool bar is shown and has its toggle in the menus"""
    tb = toolbar()
    return tb is not None and tb.isVisible() and tb.toggleViewAction().isVisible()


def hidden():
    """Whether the Surface tool bar is there and neither shown nor in the menus"""
    tb = toolbar()
    return tb is not None and not tb.isVisible() and not tb.toggleViewAction().isVisible()


def image(icon):
    """The icon at tree size, as the factory makes it (FreeCADGui.getIcon)"""
    return icon.pixmap(32, 32).toImage()


def newest(doc, before, prefix):
    made = [o for o in doc.Objects if o.Name not in before and o.Name.startswith(prefix)]
    return made[0] if made else None


def surfaceGroups(body):
    return [o for o in body.Group if o.TypeId == "PartDesign::AuxGroup" and o.Name.startswith("Surfaces")]


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        pump(50)
        doc = FreeCAD.newDocument(DOC)
        gdoc = FreeCADGui.getDocument(DOC)
        view = gdoc.ActiveView

        import TestSketcherApp

        body = doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Square")
        sketch.Support = (
            [o for o in body.Origin.OriginFeatures if o.Role == "XY_Plane"][0],
            [""],
        )
        sketch.MapMode = "FlatFace"
        TestSketcherApp.CreateRectangleSketch(sketch, (0, 0), (6, 6))
        doc.recompute()
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 2
        doc.recompute()
        view.setActiveObject("pdbody", body)
        pump(20)

        check("in PartDesign the Surface tool bar is there and not offered", hidden(),
              toolbar() and (toolbar().isVisible(), toolbar().toggleViewAction().isVisible()))

        if not check("the command to make a surface group is registered",
                     "PartDesign_SurfaceGroup" in FreeCADGui.listCommands()):
            return finish()

        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.runCommand("PartDesign_SurfaceGroup")
        pump(40)
        group = newest(doc, before, "Surfaces")
        check("it makes a group named Surfaces in the active body",
              group is not None and group.TypeId == "PartDesign::AuxGroup" and group in body.Group,
              (group and group.Name, [o.Name for o in body.Group]))
        if group is None:
            return finish()
        check("and makes it active", view.getActiveObject("pdsurface") == group,
              view.getActiveObject("pdsurface"))
        check("which offers the Surface tool bar", offered())

        icon = image(group.ViewObject.Icon)
        check("its icon is the surface group's",
              icon == image(FreeCADGui.getIcon("PartDesign_GroupSurface.svg"))
              and icon != image(FreeCADGui.getIcon("PartDesign_Group.svg")))

        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(pad, "Face6")
        FreeCADGui.runCommand("Surface_Isocurve")
        pump(40)
        iso = newest(doc, before, "Isocurve")
        check("a Surface command run while it is active puts its feature in it",
              iso is not None and iso in body.Group and iso in group.Group,
              (iso and iso.Name, [o.Name for o in group.Group]))

        group.ViewObject.doubleClicked()
        pump(20)
        check("a double-click lets the group go", view.getActiveObject("pdsurface") is None)
        check("and takes the tool bar away", hidden())
        check("the body stays active", view.getActiveObject("pdbody") == body)
        group.ViewObject.doubleClicked()
        pump(20)
        check("a second makes it active again",
              view.getActiveObject("pdsurface") == group and offered())

        FreeCADGui.activateWorkbench("PartWorkbench")
        pump(30)
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        pump(30)
        check("the tool bar stays offered across a workbench switch and back", offered())

        fill = doc.addObject("Surface::Filling", "Fill")
        body.addObject(fill)
        doc.recompute()
        group.ViewObject.doubleClicked()
        pump(20)
        check("(no group active)", view.getActiveObject("pdsurface") is None and hidden())
        gdoc.setEdit(fill)
        pump(40)
        check("with no group active, editing a Surface feature offers the tool bar",
              gdoc.getInEdit() is not None and offered())
        gdoc.resetEdit()
        pump(40)
        check("and leaving the edit takes it away", gdoc.getInEdit() is None and hidden())

        gdoc.setEdit(pad)
        pump(40)
        check("editing a PartDesign feature offers nothing",
              gdoc.getInEdit() is not None and hidden())
        gdoc.resetEdit()
        pump(40)

        group.ViewObject.doubleClicked()
        pump(20)
        gdoc.setEdit(fill)
        pump(40)
        gdoc.resetEdit()
        pump(40)
        check("with a group active, leaving the edit keeps the tool bar",
              view.getActiveObject("pdsurface") == group and offered())

        misc = doc.addObject("PartDesign::AuxGroup", "Misc")
        misc.Label = "Misc"
        children = body.Group
        children.insert(0, misc)
        body.Group = children
        misc._Body = body
        doc.recompute()
        pump(20)
        check("a feature in a surface group is not in the Misc group too",
              iso not in misc.Group and fill in misc.Group,
              ([o.Name for o in misc.Group], [o.Name for o in group.Group]))
        check("a sketch cannot be dropped on the surface group",
              not group.ViewObject.canDropObject(sketch))
        dropped = group.ViewObject.canDropObject(fill)
        if dropped:
            group.ViewObject.dropObject(fill)
            doc.recompute()
            pump(20)
        check("a Surface feature dropped on it moves there, out of Misc",
              dropped and fill in group.Group and fill not in misc.Group,
              (dropped, [o.Name for o in group.Group], [o.Name for o in misc.Group]))

        path = os.path.join(tempfile.mkdtemp(dir=OUT), "SurfaceGroup.FCStd")
        doc.saveAs(path)
        name = group.Name
        members = sorted([iso.Name, fill.Name])
        FreeCAD.closeDocument(doc.Name)
        pump(30)
        check("closing the document takes the tool bar away", hidden())
        doc = FreeCAD.openDocument(path)
        pump(40)
        gdoc = FreeCADGui.getDocument(doc.Name)
        view = gdoc.ActiveView
        group = doc.getObject(name)
        check("saved and reopened, the group is a surface group with its members",
              group is not None
              and sorted(o.Name for o in group.Group) == members
              and image(group.ViewObject.Icon)
              == image(FreeCADGui.getIcon("PartDesign_GroupSurface.svg")),
              group and [o.Name for o in group.Group])
        check("and it is not active", view.getActiveObject("pdsurface") is None and hidden())

        group.ViewObject.doubleClicked()
        pump(20)
        check("(made active)", view.getActiveObject("pdsurface") == group and offered())
        doc.removeObject(name)
        pump(40)
        check("deleting the active group takes the tool bar away",
              view.getActiveObject("pdsurface") is None and hidden())
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
