"""FreeCADGui.open opens a FreeCAD document.

The reporter 2026-11-03, on a probe that called it: fix "FreeCADGui.open
refused the file ('File type 'fcstd' not supported')". FreeCADGui.open and
FreeCADGui.insert handled only the GUI's own types (Inventor, VRML, Python,
images); any other file was "not supported". Now a type the GUI does not
handle itself goes to the module registered to import it, as File > Open
and a drop do -- a .FCStd opens as a document.

Claims:
  - FreeCADGui.open on a saved .FCStd opens it: the document is there, with
    its object and a 3D view;
  - a type nobody imports is still refused, and nothing is opened.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
STEPS = []


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def by_file(path):
    want = os.path.normcase(os.path.abspath(path))
    for doc in FreeCAD.listDocuments().values():
        if doc.FileName and os.path.normcase(os.path.abspath(doc.FileName)) == want:
            return doc
    return None


def open_fcstd():
    path = os.path.join(OUT, "opened-by-gui.FCStd")
    doc = FreeCAD.newDocument("OpenedByGui")
    doc.addObject("Part::Box", "Box")
    doc.recompute()
    doc.saveAs(path)
    FreeCAD.closeDocument(doc.Name)
    check("the saved document is closed before the open", by_file(path) is None)

    FreeCADGui.open(path)
    doc = by_file(path)
    check("FreeCADGui.open on a .FCStd opens the document", doc is not None,
          sorted(FreeCAD.listDocuments()))
    if doc is not None:
        check("with its object", doc.getObject("Box") is not None)
        views = FreeCADGui.getDocument(doc.Name).mdiViewsOfType("Gui::View3DInventor")
        check("and a 3D view", len(views) > 0, len(views))


def unknown_type():
    path = os.path.join(OUT, "nobody-imports.zzq")
    with open(path, "w") as f:
        f.write("not a model\n")
    before = sorted(FreeCAD.listDocuments())
    FreeCADGui.open(path)
    check("a type nobody imports is refused, nothing opened",
          sorted(FreeCAD.listDocuments()) == before, sorted(FreeCAD.listDocuments()))


def finish():
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


def advance():
    if not STEPS:
        finish()
        return
    step = STEPS.pop(0)
    try:
        step()
    except Exception:
        note("ABORT in %s: %s" % (getattr(step, "__name__", step),
                                  traceback.format_exc().replace("\n", " | ")))
        STEPS[:] = []
    QtCore.QTimer.singleShot(1000, advance)


STEPS[:] = [open_fcstd, unknown_type]
QtCore.QTimer.singleShot(3000, advance)
