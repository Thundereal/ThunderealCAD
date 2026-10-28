"""A linked set's import leaves its files to be saved (docs/TransactionLogJournal.md sec 35.18).

An import puts each file's branch in that file's own history, which a save
of that file writes; closed unsaved, the branch is gone. The import marks
the files it gave a branch modified, so that a close asks.

Our `Asm` links the box of our `Part`; a copy of both elsewhere, its box
made longer and its `Asm` link moved. Their set imported into ours.

Claims:
  - before, neither of ours is modified;
  - after, both are: each holds a branch to be saved;
  - a branch's own document, opened, is not: it cannot be saved;
  - saved, neither is, and the branches are there when opened again.
"""
import os
import shutil
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
ROOT = os.path.join(OUT, "setroot")
STATE = {}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def modified(doc):
    return FreeCADGui.getDocument(doc.Name).Modified


def ofFile(path):
    for d in FreeCAD.listDocuments().values():
        if d.FileName == path:
            return d
    return None


def run():
    try:
        shutil.rmtree(ROOT, True)
        ours = os.path.join(ROOT, "ours")
        theirs = os.path.join(ROOT, "theirs")
        os.makedirs(ours)
        part = FreeCAD.newDocument("ModPart")
        part.UndoMode = 1
        part.openTransaction("box")
        box = part.addObject("Part::Box", "Box")
        part.recompute()
        part.commitTransaction()
        part.saveAs(os.path.join(ours, "Part.FCStd"))
        asm = FreeCAD.newDocument("ModAsm")
        asm.UndoMode = 1
        asm.openTransaction("link")
        asm.addObject("App::Link", "L").LinkedObject = box
        asm.recompute()
        asm.commitTransaction()
        asm.saveAs(os.path.join(ours, "Asm.FCStd"))
        for d in (asm, part):
            FreeCAD.closeDocument(d.Name)
        shutil.copytree(ours, theirs)
        p = FreeCAD.openDocument(os.path.join(theirs, "Part.FCStd"))
        p.Box.Length = 25
        p.recompute()
        p.save()
        a = FreeCAD.openDocument(os.path.join(theirs, "Asm.FCStd"))
        a.L.Placement = FreeCAD.Placement(FreeCAD.Vector(5, 0, 0), FreeCAD.Rotation())
        a.recompute()
        a.save()
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)

        asmPath = os.path.join(ours, "Asm.FCStd")
        partPath = os.path.join(ours, "Part.FCStd")
        doc = FreeCAD.openDocument(asmPath)
        FreeCAD.setActiveDocument(doc.Name)
        QtCore.QCoreApplication.processEvents()
        ourPart = ofFile(partPath)
        check("before, neither of ours is modified",
              ourPart is not None and not modified(doc) and not modified(ourPart),
              (modified(doc), ourPart and modified(ourPart)))
        res = doc.importLinkedSet(os.path.join(theirs, "Asm.FCStd"))
        QtCore.QCoreApplication.processEvents()
        got = {os.path.basename(f["theirs"]): f for f in res["files"]}
        check("after, both hold a branch to be saved, and are modified",
              got["Part.FCStd"]["branch"] and got["Asm.FCStd"]["branch"]
              and modified(doc) and modified(ourPart),
              (got["Part.FCStd"]["branch"], got["Asm.FCStd"]["branch"], modified(doc),
               modified(ourPart)))
        tip = FreeCAD.openDocument(asmPath + "@" + got["Asm.FCStd"]["branch"] + "@")
        QtCore.QCoreApplication.processEvents()
        check("a branch's own document is not", not modified(tip), modified(tip))
        FreeCAD.closeDocument(tip.Name)
        for d in list(FreeCAD.listDocuments().values()):
            if "@" in d.FileName:
                FreeCAD.closeDocument(d.Name)
        # The part first: a save of it is a change of what links it (the
        # link's stamp)
        FreeCADGui.getDocument(ourPart.Name).save()
        FreeCADGui.getDocument(doc.Name).save()
        QtCore.QCoreApplication.processEvents()
        check("saved, neither is", not modified(doc) and not modified(ourPart),
              (modified(doc), modified(ourPart)))
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        doc = FreeCAD.openDocument(asmPath)
        ourPart = ofFile(partPath)
        branches = [b["name"] for b in ourPart.getTransactionBranches()]
        check("the branches are there when opened again",
              got["Part.FCStd"]["branch"] in branches, branches)
    except Exception:
        note("ABORT run:\n" + traceback.format_exc())
    finish()


def finish():
    try:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
    except Exception:
        note("ABORT finish:\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, run)
