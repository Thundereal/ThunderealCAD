"""An addon's module that calls FreeCADGui at import loads in a worker, and
what a worker cannot make faithfully is made here, said in the Report view.

docs/WorkerProcess.md sec 72; the reporter, 2026-11-26: "implement a partial
FreeCADGui module to allow as much as possible external modules to load
without error ... as a last resort, warn user and take the recompute back to
main process". The case: scanner.FCStd's OpticsWorkbench objects, whose
module calls Gui.addCommand at import, failed to restore in the worker's
copy, and the worker recomputed the document without their code.

Mod/Test/WorkerGuiProbe.py is such a module (addCommand and addIconPath at
import, unguarded), with four features: Plain, a box of Size; Colored,
whose execute() writes its ViewObject.LineColor as Ray.execute does; Wide,
whose execute() writes its ViewObject.LineWidth; and Scener, which reaches
its view's scene graph. Saved, closed and
opened again (the restore that failed), an edit is recomputed by Std_Refresh,
which hands it to a worker. Claims:
  - the worker had the module: Plain is made there, to its new size;
  - Colored is made there too: its LineColor is the object's look, which
    the worker's view object writes through to the object, and it comes
    back with the result -- the view's colour by the new size (the
    reporter, 2026-11-26: "most of the view object appearance property is
    actually a stand in for app namespace property");
  - Wide, whose execute wrote what is the view's alone, is made there too:
    a worker has its view's properties as the copy saved them, and the
    write is done again to the view provider here (sec 73) -- its line
    width by the new size;
  - Scener, which reached the scene graph no worker has, is made here,
    the one object so, an addon's, and the Report view names it and what
    it touched;
  - the result is whole: nothing touched, each size the edited one.
With OpticsWorkbench installed (GT_ADDONS), a ray on a mirror, its length
and wavelength edited: made by the worker, its colour that of the new
wavelength, nothing touched.
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


def touched(doc):
    return sorted(o.Name for o in doc.Objects if "Touched" in o.State)


def report():
    view = FreeCADGui.getMainWindow().findChild(QtWidgets.QTextEdit, "Report view")
    return view.toPlainText() if view else ""


def byWorker(doc):
    """Std_Refresh, which hands the recompute to a worker; waited for"""
    pool = FreeCAD.workerPool(doc)
    FreeCADGui.runCommand("Std_Refresh")
    handed = pool.recomputing()
    end = time.time() + 120
    while pool.recomputing() and time.time() < end:
        pump(0.05)
    pump(0.5)
    return handed, pool.recomputeInfo()


def reopened(doc):
    path = os.path.join(OUT, doc.Name + ".FCStd")
    doc.saveAs(path)
    name = doc.Name
    FreeCAD.closeDocument(name)
    pump(0.5)
    doc = FreeCAD.openDocument(path)
    pump(1.0)
    return doc


def probe():
    import WorkerGuiProbe

    doc = FreeCAD.newDocument("AddonByWorker")
    base = doc.addObject("Part::Box", "Base")
    plain = doc.addObject("Part::FeaturePython", "Plain")
    WorkerGuiProbe.Plain(plain)
    plain.setExpression("Size", "Base.Length")
    colored = doc.addObject("Part::FeaturePython", "Colored")
    WorkerGuiProbe.Colored(colored)
    colored.setExpression("Size", "Base.Width")
    wide = doc.addObject("Part::FeaturePython", "Wide")
    WorkerGuiProbe.Wide(wide)
    wide.setExpression("Size", "Base.Height")
    scener = doc.addObject("Part::FeaturePython", "Scener")
    WorkerGuiProbe.Scener(scener)
    scener.setExpression("Size", "Base.Length")
    doc.recompute()
    doc = reopened(doc)
    base, plain, colored, wide, scener = doc.Base, doc.Plain, doc.Colored, doc.Wide, doc.Scener
    check("reopened here with the module's classes",
          None not in (plain.Proxy, colored.Proxy, wide.Proxy),
          (plain.Proxy, colored.Proxy, wide.Proxy))

    before = len(report())
    base.Length = 12
    base.Width = 11
    base.Height = 13
    handed, info = byWorker(doc)
    said = report()[before:]
    here = os.getpid()
    check("Std_Refresh hands it to a worker", handed, info)
    check("the worker had the module: Plain made there",
          plain.MadeBy not in (0, here) and abs(plain.Shape.Volume - 12 ** 3) < 1e-6,
          (plain.MadeBy, here, plain.Shape.Volume, info))
    check("Colored made there too, its view's colour the object's look, come back",
          colored.MadeBy not in (0, here) and abs(colored.Shape.Volume - 11 ** 3) < 1e-6
          and abs(colored.ViewObject.LineColor[0] - 0.11) < 0.01
          and abs(colored.LineColor[0] - 0.11) < 0.01,
          (colored.MadeBy, colored.Shape.Volume, colored.ViewObject.LineColor,
           colored.LineColor, info))
    check("Wide, which writes the view's own line width, made there, the width done here",
          wide.MadeBy not in (0, here) and abs(wide.Shape.Volume - 13 ** 3) < 1e-6
          and abs(wide.ViewObject.LineWidth - 1.3) < 1e-6,
          (wide.MadeBy, wide.Shape.Volume, wide.ViewObject.LineWidth, info))
    check("Scener, which reaches the scene graph, made here",
          scener.MadeBy == here and abs(scener.Shape.Volume - 12 ** 3) < 1e-6,
          (scener.MadeBy, scener.Shape.Volume, info))
    check("... and only Scener, an addon's", info["ExecutedHereNames"] == ["Scener"]
          and [(f["Object"], f["BuiltIn"]) for f in info["Faked"]] == [("Scener", False)], info)
    check("... the Report view names it and what it touched",
          "Scener" in said and "ViewObject.RootNode" in said and "Wide" not in said
          and "Colored" not in said, said[-600:])
    check("the result is whole: nothing touched", touched(doc) == [], touched(doc))
    FreeCAD.closeDocument(doc.Name)


def optics():
    try:
        import OpticsWorkbench
        import Ray
    except ImportError:
        note("INFO OpticsWorkbench not installed (GT_ADDONS): its part skipped")
        return
    doc = FreeCAD.newDocument("OpticsByWorker")
    box = doc.addObject("Part::Box", "MirrorBase")
    box.Placement.Base = FreeCAD.Vector(50, -5, -5)
    doc.recompute()
    OpticsWorkbench.makeMirror([box])
    OpticsWorkbench.makeRay()
    doc.recompute()
    doc = reopened(doc)
    ray = [o for o in doc.Objects if o.Name.startswith("Ray")][0]
    was = (ray.Shape.Length, tuple(ray.ViewObject.LineColor))
    # Short of the mirror at x = 50: one segment of 30 (a ray finds the
    # optics by looking at the document, not by a link to them); blue
    ray.MaxRayLength = 30
    ray.Wavelength = 450
    rgb = [c / 255.0 for c in Ray.wavelen2rgb(450)]
    handed, info = byWorker(doc)
    color = tuple(ray.ViewObject.LineColor)
    check("Optics: handed to a worker", handed, info)
    check("Optics: the ray made by the worker, to its new length",
          ray.Name not in info.get("ExecutedHereNames", []) and info["Supplied"] >= 1
          and abs(ray.Shape.Length - 30) < 1e-6, (was, ray.Shape.Length, info))
    check("Optics: its colour that of the new wavelength",
          all(abs(color[i] - rgb[i]) < 0.01 for i in range(3)), (was, color, rgb))
    check("Optics: nothing touched", touched(doc) == [], touched(doc))
    FreeCAD.closeDocument(doc.Name)


def run():
    try:
        WORKERS.RemBool("RecomputeCommand")
        probe()
        optics()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
