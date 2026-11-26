"""What built-in code does to views in its execute() is the same made by a
worker as made here (docs/WorkerProcess.md sec 73).

The reporter, 2026-11-26: "fix the code to make any built-in objects to
recompute in worker", and "be careful about view vs app space visibility
discrepency problem". Mod/Test/WorkerViewProbe.py's features, installed
under the home Mod and so built-in, do what the install's own do: Hider
shows or hides its base (ArchWall, Fixture), Sized takes its size from its
view's DisplaySize (ArchSectionPlane, wpproxy), Refresher redraws its view
and calls its view proxy (Dimension, BuildingPart).

Two documents are built the same: one is recomputed here, the other by a
worker, each after the same edit, and in each one object hidden in the
view only (setObjectVisibility, a per-view hide) and one hidden by
TempoVis. Claims:
  - the worker made every object: none made here, none faked, none taken
    back;
  - the census -- each object's App Visibility, its view provider's
    Visibility, whether it is shown (the mode switch), its display mode,
    the view's per-view entry -- is the same in both;
  - what each execute did through its view is the same: Sized's size from
    the view's DisplaySize, Refresher's proxy told once.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part  # noqa: F401
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
WORKERS = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Workers")


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


def loaded(doc, seconds=120):
    """Until the document's progressive load is over: a command that edits
    it -- Std_Refresh -- is refused while it fills (Command::invoke), which
    on a loaded box outlasted a fixed pump"""
    end = time.time() + seconds
    while (doc.Restoring or doc.LiveImport) and time.time() < end:
        pump(0.05)
    return not doc.Restoring and not doc.LiveImport


def touched(doc):
    return sorted(o.Name for o in doc.Objects if "Touched" in o.State)


def build(name):
    import WorkerViewProbe as P

    doc = FreeCAD.newDocument(name)
    shown = doc.addObject("Part::Box", "Shown")
    hidden = doc.addObject("Part::Box", "Hidden")
    hider = doc.addObject("Part::FeaturePython", "Hider")
    P.Hider(hider)
    hider.Base = hidden
    hider.Show = True
    sized = doc.addObject("Part::FeaturePython", "Sized")
    P.Sized(sized)
    P.SizedView(sized.ViewObject)
    plane = doc.addObject("Part::Plane", "Plane")
    facing = doc.addObject("Part::FeaturePython", "Facing")
    P.Facing(facing)
    facing.Base = plane
    refresher = doc.addObject("Part::FeaturePython", "Refresher")
    P.Refresher(refresher)
    P.RefresherView(refresher.ViewObject)
    perview = doc.addObject("Part::Box", "PerView")
    tempo = doc.addObject("Part::Box", "Tempo")
    doc.recompute()
    pump(0.3)
    # Saved and opened again: a worker's copy has the views as saved
    path = os.path.join(OUT, name + ".FCStd")
    doc.saveAs(path)
    FreeCAD.closeDocument(name)
    pump(0.3)
    doc = FreeCAD.openDocument(path)
    pump(0.5)
    check(name + " loaded before it is edited", loaded(doc))
    view = FreeCADGui.getDocument(doc.Name).activeView()
    view.setObjectVisibility(doc.PerView, False)
    from Show import TempoVis

    tv = TempoVis(doc)
    tv.hide(doc.Tempo)
    doc.Sized.ViewObject.DisplaySize = 7  # a view's own, after the save
    pump(0.3)
    return doc, tv


def edit(doc):
    doc.Hider.Show = False
    doc.Sized.Input = 3
    doc.Refresher.Input = 2
    doc.Plane.Length = 12
    # The camera below the plane: Draft turns its normal toward it. The
    # turn is animated, over frames: until it is there, not a fixed pump,
    # which a box with dear frames outlasted (the normal read 1.0 here)
    view = FreeCADGui.getDocument(doc.Name).activeView()
    view.viewBottom()
    end = time.time() + 60
    while view.getViewDirection().z < 0.99 and time.time() < end:
        pump(0.05)
    pump(0.3)
    check(doc.Name + "'s camera below the plane before the recompute",
          view.getViewDirection().z > 0.99, view.getViewDirection())


def census(doc):
    view = FreeCADGui.getDocument(doc.Name).activeView()
    rows = {}
    for o in doc.Objects:
        vp = o.ViewObject
        rows[o.Name] = (bool(o.Visibility), bool(vp.Visibility), bool(vp.isVisible()),
                        vp.DisplayMode, view.getObjectVisibility(o))
    return rows


def run():
    try:
        WORKERS.RemBool("RecomputeCommand")
        here, tv1 = build("ViewsHere")
        edit(here)
        WORKERS.SetBool("RecomputeCommand", False)
        FreeCAD.setActiveDocument(here.Name)
        FreeCADGui.runCommand("Std_Refresh")
        pump(0.5)
        WORKERS.RemBool("RecomputeCommand")
        check("made here when asked to be", touched(here) == [] and
              here.Sized.MadeBy == os.getpid(), (touched(here), here.Sized.MadeBy))

        there, tv2 = build("ViewsThere")
        edit(there)
        FreeCAD.setActiveDocument(there.Name)
        pool = FreeCAD.workerPool(there)
        FreeCADGui.runCommand("Std_Refresh")
        handed = pool.recomputing()
        end = time.time() + 120
        while pool.recomputing() and time.time() < end:
            pump(0.05)
        pump(0.5)
        info = pool.recomputeInfo()
        check("handed to a worker", handed, info)
        made = [n for n in ("Hider", "Sized", "Refresher", "Facing")
                if there.getObject(n).MadeBy not in (0, os.getpid())]
        check("the worker made every object", made == ["Hider", "Sized", "Refresher", "Facing"]
              and info["ExecutedHere"] == 0 and info["Unfit"] == 0, (made, info))
        check("... none faked", info["Faked"] == [], info["Faked"])
        check("nothing touched", touched(there) == [], touched(there))

        a, b = census(here), census(there)
        differ = {k: (a[k], b.get(k)) for k in a if a[k] != b.get(k)}
        check("the census is the same: Visibility, the view's, shown, display mode, "
              "per-view", differ == {}, differ or a)
        check("... the hider's base hidden in both, the view and the object",
              b["Hidden"][:3] == (False, False, False), b["Hidden"])
        check("... the per-view hide stays the view's alone",
              b["PerView"][0] and b["PerView"][1] and b["PerView"][4] in (False, 0), b["PerView"])
        check("... the TempoVis hide stays", not b["Tempo"][1], b["Tempo"])
        check("Sized's size the view's DisplaySize, after the save",
              abs(here.Sized.Shape.BoundBox.XLength - 7) < 1e-6
              and abs(there.Sized.Shape.BoundBox.XLength - 7) < 1e-6
              and abs(there.Sized.Shape.BoundBox.ZLength - 10) < 1e-6,
              (here.Sized.Shape.BoundBox, there.Sized.Shape.BoundBox))
        check("Facing's normal turned toward the camera the same, in both",
              here.Facing.NormalZ == there.Facing.NormalZ and abs(there.Facing.NormalZ) > 0.5
              and here.Facing.MadeBy == os.getpid(),
              (here.Facing.NormalZ, there.Facing.NormalZ))
        # "made 0" by the first recompute, before the save; "made 2" by the
        # edit's -- here directly, there done again from the worker's call
        check("Refresher's view proxy told the same, in both",
              list(here.Refresher.ViewObject.Marks) == ["made 0", "made 2"]
              and list(there.Refresher.ViewObject.Marks) == ["made 0", "made 2"],
              (here.Refresher.ViewObject.Marks, there.Refresher.ViewObject.Marks))
        tv1.restore()
        tv2.restore()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        WORKERS.RemBool("RecomputeCommand")
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
