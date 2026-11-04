"""A surface group in a PartDesign Body (docs/SheetMetal.md sec 8): an aux
group that gathers the body's Surface features, and brings the Surface tool
bar into PartDesign while it is active.

In PartDesign:
  - there is no command to make a surface group: the body makes one;
  - a body made with PartDesign_Body has the auto aux groups (Sketches,
    Datums, Misc) from the start, and no Surfaces group yet; a body made by
    script has none;
  - the Surface tool bar is there but not offered (no toggle in the menus);
  - the first Surface feature in the body brings a group named Surfaces,
    with the surface icon overlaid on the group's, that holds it; Misc does
    not hold it too; a later one goes in the same group; not made active;
  - a double-click on the group makes it active, which offers the tool bar,
    the body staying active; a second lets it go and takes the tool bar away;
  - the tool bar stays offered across a workbench switch and back;
  - with no group active, editing a Surface feature offers the tool bar and
    leaving the edit takes it away; with a group active, leaving the edit
    keeps it; editing a PartDesign feature offers nothing;
  - a sketch cannot be dropped on the group;
  - the auto aux groups turned off, the Surfaces group goes too and the
    surfaces stay in the body; turned on, it comes back holding them;
  - in a body with no auto aux groups, a Surface feature brings no group;
  - in a body on a base feature, the groups go after the base, which the
    body keeps;
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


def auxGroups(body):
    """The kinds of auto aux group the body holds, by name"""
    kinds = ("Sketches", "Datums", "Misc", "Surfaces")
    return sorted(
        k for o in body.Group if o.TypeId == "PartDesign::AuxGroup" for k in kinds if o.Name.startswith(k)
    )


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

        check("there is no command to make a surface group",
              "PartDesign_SurfaceGroup" not in FreeCADGui.listCommands())

        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.runCommand("PartDesign_Body")
        pump(40)
        body = newest(doc, before, "Body")
        if not check("PartDesign_Body makes a body",
                     body is not None and body.TypeId == "PartDesign::Body"):
            return finish()
        check("with the auto aux groups from the start, and no Surfaces group yet",
              body.hasAutoAuxGroup() and auxGroups(body) == ["Datums", "Misc", "Sketches"],
              [o.Name for o in body.Group])
        view.setActiveObject("pdbody", body)
        pump(20)

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
        pump(20)

        check("in PartDesign the Surface tool bar is there and not offered", hidden(),
              toolbar() and (toolbar().isVisible(), toolbar().toggleViewAction().isVisible()))

        before = set(o.Name for o in doc.Objects)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(pad, "Face6")
        FreeCADGui.runCommand("Surface_Isocurve")
        pump(40)
        doc.recompute()
        iso = newest(doc, before, "Isocurve")
        groups = surfaceGroups(body)
        group = groups[0] if groups else None
        if not check("the first Surface feature in the body brings a group named Surfaces",
                     iso is not None and iso in body.Group and len(groups) == 1,
                     (iso and iso.Name, [o.Name for o in body.Group])):
            return finish()
        check("that holds it", iso in group.Group, [o.Name for o in group.Group])
        misc = [o for o in body.Group if o.Name.startswith("Misc")][0]
        check("Misc does not hold it too", iso not in misc.Group, [o.Name for o in misc.Group])
        check("it is not made active", view.getActiveObject("pdsurface") is None and hidden())

        icon = image(group.ViewObject.Icon)
        check("its icon is the surface group's",
              icon == image(FreeCADGui.getIcon("PartDesign_GroupSurface.svg"))
              and icon != image(FreeCADGui.getIcon("PartDesign_Group.svg")))

        fill = doc.addObject("Surface::Filling", "Fill")
        body.addObject(fill)
        doc.recompute()
        check("a later one goes in the same group",
              surfaceGroups(body) == [group] and fill in group.Group and fill not in misc.Group,
              ([o.Name for o in body.Group], [o.Name for o in group.Group]))

        group.ViewObject.doubleClicked()
        pump(20)
        check("a double-click makes the group active",
              view.getActiveObject("pdsurface") == group, view.getActiveObject("pdsurface"))
        check("which offers the Surface tool bar", offered())
        check("the body stays active", view.getActiveObject("pdbody") == body)
        group.ViewObject.doubleClicked()
        pump(20)
        check("a second lets it go and takes the tool bar away",
              view.getActiveObject("pdsurface") is None and hidden())
        group.ViewObject.doubleClicked()
        pump(20)

        FreeCADGui.activateWorkbench("PartWorkbench")
        pump(30)
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        pump(30)
        check("the tool bar stays offered across a workbench switch and back", offered())

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
        check("a sketch cannot be dropped on the group",
              not group.ViewObject.canDropObject(sketch))

        body.setAutoAuxGroup(False)
        doc.recompute()
        pump(40)
        check("the auto aux groups turned off, the Surfaces group goes too",
              not body.hasAutoAuxGroup() and auxGroups(body) == [],
              [o.Name for o in body.Group])
        check("and the surfaces stay in the body", iso in body.Group and fill in body.Group)
        check("(its tool bar with it)", view.getActiveObject("pdsurface") is None and hidden())
        body.setAutoAuxGroup(True)
        doc.recompute()
        pump(20)
        groups = surfaceGroups(body)
        group = groups[0] if groups else None
        check("turned on, it comes back holding them",
              auxGroups(body) == ["Datums", "Misc", "Sketches", "Surfaces"]
              and sorted(o.Name for o in group.Group) == sorted([iso.Name, fill.Name]),
              ([o.Name for o in body.Group], group and [o.Name for o in group.Group]))

        plain = doc.addObject("PartDesign::Body", "Plain")
        fill2 = doc.addObject("Surface::Filling", "Fill")
        plain.addObject(fill2)
        doc.recompute()
        check("a body made by script has no auto aux groups, and a surface brings none",
              not plain.hasAutoAuxGroup() and auxGroups(plain) == [] and fill2 in plain.Group,
              [o.Name for o in plain.Group])

        box = doc.addObject("Part::Box", "Box")
        based = doc.addObject("PartDesign::Body", "Based")
        based.BaseFeature = box
        doc.recompute()
        based.setAutoAuxGroup(True)
        doc.recompute()
        check("in a body on a base feature, the groups go after the base, which it keeps",
              based.BaseFeature == box and based.Group[0].isDerivedFrom("PartDesign::FeatureBase")
              and auxGroups(based) == ["Datums", "Misc", "Sketches"],
              (based.BaseFeature and based.BaseFeature.Name, [o.Name for o in based.Group]))

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
