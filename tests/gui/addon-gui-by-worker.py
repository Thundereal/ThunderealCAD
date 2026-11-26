"""An addon's module that calls FreeCADGui at import loads in a worker, and
what a worker cannot make faithfully is made here, said in the Report view.

docs/WorkerProcess.md sec 72; the reporter, 2026-11-26: "implement a partial
FreeCADGui module to allow as much as possible external modules to load
without error ... as a last resort, warn user and take the recompute back to
main process". The case: scanner.FCStd's OpticsWorkbench objects, whose
module calls Gui.addCommand at import, failed to restore in the worker's
copy, and the worker recomputed the document without their code.

Mod/Test/WorkerGuiProbe.py is such a module (addCommand and addIconPath at
import, unguarded), with two features: Plain, a box of Size, and Colored,
whose execute() writes its ViewObject as Ray.execute does. Saved, closed and
opened again (the restore that failed), an edit is recomputed by Std_Refresh,
which hands it to a worker. Claims:
  - the worker had the module: Plain is made there, to its new size;
  - Colored, whose execute wrote its view, is made here, its view written
    (the colour by its new size), and the Report view names it and what it
    touched;
  - the result is whole: nothing touched, each size the edited one.
With OpticsWorkbench installed (GT_ADDONS), the same of a ray on a mirror:
the ray made here, named in the Report view, nothing touched.
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
    doc.recompute()
    doc = reopened(doc)
    base, plain, colored = doc.Base, doc.Plain, doc.Colored
    check("reopened here with the module's classes",
          plain.Proxy is not None and colored.Proxy is not None,
          (plain.Proxy, colored.Proxy))

    before = len(report())
    base.Length = 12
    base.Width = 11
    handed, info = byWorker(doc)
    said = report()[before:]
    check("Std_Refresh hands it to a worker", handed, info)
    check("the worker had the module: Plain made there",
          plain.MadeBy not in (0, os.getpid()) and abs(plain.Shape.Volume - 12 ** 3) < 1e-6,
          (plain.MadeBy, os.getpid(), plain.Shape.Volume, info))
    check("Colored, which writes its view, made here, its view written",
          colored.MadeBy == os.getpid() and abs(colored.Shape.Volume - 11 ** 3) < 1e-6
          and abs(colored.ViewObject.LineColor[0] - 0.11) < 0.01,
          (colored.MadeBy, colored.Shape.Volume, colored.ViewObject.LineColor, info))
    check("... the Report view names it and what it touched",
          "Colored" in said and "ViewObject" in said, said[-600:])
    check("the result is whole: nothing touched", touched(doc) == [], touched(doc))
    FreeCAD.closeDocument(doc.Name)


def optics():
    try:
        import OpticsWorkbench
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
    was = ray.Shape.Length
    before = len(report())
    # Short of the mirror at x = 50: one segment of 30 (a ray finds the
    # optics by looking at the document, not by a link to them)
    ray.MaxRayLength = 30
    handed, info = byWorker(doc)
    said = report()[before:]
    check("Optics: handed to a worker", handed, info)
    check("Optics: the ray made here, to its new length",
          ray.Name in info.get("ExecutedHereNames", []) and abs(ray.Shape.Length - 30) < 1e-6,
          (was, ray.Shape.Length, info))
    check("Optics: the Report view names the ray", ray.Name in said, said[-600:])
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
