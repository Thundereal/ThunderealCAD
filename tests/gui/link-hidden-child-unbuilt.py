"""A hidden object a link does not show is not meshed at an open.

Found 2026-11-17 on the reporter's scanner.FCStd (docs/WorkerProcess.md
sec 18): of its 143 hidden Part features 138 had their visual built after
an open, though no view drew one. Two causes:
  - a Link to an App::Part makes a LinkInfo for every child down the
    tree (Part -> Body -> each feature), and each LinkInfo forced its
    object's visual up to date (forceUpdate) -- also a hidden child, whose
    snapshot then draws nothing;
  - the progressive load's drain built every visual parked during the
    restore, though the object was hidden by the time its turn came. A
    view provider whose record names a file of the archive -- a colour
    list, in a file saved at schema 4 as scanner.FCStd is -- is made at
    once, and making it built its visual while its own Visibility, not
    read back yet, still said shown: parked, then built by the drain.

Scene: an App::Part "Asm" holding a PartDesign Body (an additive box,
hidden, and a subtractive cylinder, the tip) and a hidden Part::Box
"Spare"; a Link "L" to Asm; a hidden Part::Cylinder "Rod" outside and a
Link "LR" to it, which shows it whatever its own Visibility says. Saved
twice, at the current schema and at schema 4 (the scene claim: only the
second keeps Box's colour lists in files), and each reopened with the
progressive load on and off. Claims, each of the four reopens:
  - the hidden features under the linked Part are not forced and have no
    mesh (Box, Spare);
  - the tip has one, and Rod, shown by its link, has one and is forced;
  - the link draws the Part (a non-empty box through L);
  - Box shown afterwards is built then.

Scored 2026-11-17: the tree before the fix fails 16 of 33 (Box and Spare
forced and meshed on all four reopens); with the link's force narrowed
alone it fails 2 (the schema 4 file, progressive: both meshed by the
drain); with both, none.
"""
import os
import traceback
import zipfile

import FreeCAD
import FreeCADGui
from PySide import QtCore
from pivy import coin

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
RENDER = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render")
DOCPARAMS = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")
DOCPARAMS.SetBool("AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name
         + (" | " + str(detail) if detail else ""))
    return cond


def settle(n=10):
    for _ in range(n):
        QtCore.QCoreApplication.processEvents()


def points(vobj):
    """Points under the view's own root. An unbuilt visual holds Coin's
    default single point, a built one its mesh"""
    sa = coin.SoSearchAction()
    sa.setType(coin.SoCoordinate3.getClassTypeId())
    sa.setInterest(coin.SoSearchAction.ALL)
    sa.apply(vobj.RootNode)
    return sum(p.getTail().point.getNum() for p in sa.getPaths())


def build(path, schema=None):
    doc = FreeCAD.newDocument("LinkHidden")
    if schema:
        doc.SaveSchemaVersion = schema
    asm = doc.addObject("App::Part", "Asm")
    body = doc.addObject("PartDesign::Body", "Body")
    asm.addObject(body)
    box = body.newObject("PartDesign::AdditiveBox", "Box")
    box.Length = box.Width = box.Height = 10
    hole = body.newObject("PartDesign::SubtractiveCylinder", "Hole")
    hole.Radius = 2
    hole.Height = 30
    spare = doc.addObject("Part::Box", "Spare")
    asm.addObject(spare)
    link = doc.addObject("App::Link", "L")
    link.LinkedObject = asm
    link.Placement.Base = FreeCAD.Vector(0, 40, 0)
    rod = doc.addObject("Part::Cylinder", "Rod")
    rod.Placement.Base = FreeCAD.Vector(40, 0, 0)
    lr = doc.addObject("App::Link", "LR")
    lr.LinkedObject = rod
    lr.Placement.Base = FreeCAD.Vector(40, 40, 0)
    doc.recompute()
    settle()
    box.ViewObject.Visibility = False
    spare.ViewObject.Visibility = False
    rod.ViewObject.Visibility = False
    settle()
    note("saved: visible %s" % [o.Name for o in doc.Objects
                                if o.ViewObject and o.ViewObject.Visibility])
    doc.saveAs(path)
    FreeCAD.closeDocument(doc.Name)
    settle()


def reopen(path, progressive):
    RENDER.SetBool("ProgressiveLoad", progressive)
    doc = FreeCAD.openDocument(path)
    while FreeCADGui.isBuildingVisuals():
        QtCore.QCoreApplication.processEvents()
    view = FreeCADGui.getDocument(doc.Name).mdiViewsOfType("Gui::View3DInventor")[0]
    view.fitAll()
    settle(20)
    return doc


def claims(doc, tag):
    g = FreeCADGui.getDocument(doc.Name)
    v = {n: g.getObject(n) for n in ("Box", "Spare", "Hole", "Rod")}
    note("%s: points %s forced %s visible %s" % (
        tag, {n: points(o) for n, o in v.items()},
        {n: o.ForceUpdate for n, o in v.items()},
        {n: o.Visibility for n, o in v.items()}))
    for n in ("Box", "Spare"):
        check("%s: hidden %s under the linked Part is not forced" % (tag, n),
              not v[n].Visibility and not v[n].ForceUpdate)
        check("%s: hidden %s under the linked Part has no mesh" % (tag, n),
              points(v[n]) <= 1, points(v[n]))
    check("%s: the tip has its mesh" % tag, points(v["Hole"]) > 1, points(v["Hole"]))
    check("%s: Rod, shown by its link, is forced and has its mesh" % tag,
          v["Rod"].ForceUpdate and points(v["Rod"]) > 1, points(v["Rod"]))
    action = coin.SoGetBoundingBoxAction(coin.SbViewportRegion())
    action.apply(g.getObject("L").RootNode)
    check("%s: the link draws the Part" % tag, not action.getBoundingBox().isEmpty())
    v["Box"].Visibility = True
    settle()
    check("%s: Box shown afterwards is built then" % tag, points(v["Box"]) > 1,
          points(v["Box"]))


def run():
    try:
        path = os.path.join(OUT, "link-hidden.FCStd")
        build(path)
        # Saved at schema 4, the format upstream reads, as scanner.FCStd
        # (2025) is: each Part view keeps its colour lists in files of the
        # archive. The load restores such a view provider at once, not in
        # its deferred drain, and creating it there built its visual
        # before its own Visibility was read back: still shown, it was
        # parked for the progressive drain, which built it after it was
        # hidden again.
        old = os.path.join(OUT, "link-hidden-files.FCStd")
        build(old, schema=4)
        with zipfile.ZipFile(old) as z:
            held = sorted(n for n in z.namelist() if n.startswith("Box.ViewObject."))
        with zipfile.ZipFile(path) as z:
            inline = not any(".ViewObject." in n for n in z.namelist())
        if not check("the scene: Box's view keeps lists in files only in the second file",
                     held and inline, held):
            return
        for file, kind in ((path, ""), (old, " colours in files")):
            for progressive in (True, False):
                tag = ("progressive" if progressive else "eager") + kind
                doc = reopen(file, progressive)
                claims(doc, tag)
                FreeCAD.closeDocument(doc.Name)
                settle()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(0, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1000, run)
