"""Part_CheckGeometry checks in a worker, by default, and here when the
setting says no; the panel lists the same either way.

docs/WorkerProcess.md sec 25. CheckCommand of Preferences/Workers ("Check
geometry in a worker process" in the Document preferences) is on unless
set off. Claims:
  - with the setting as it comes, the panel opens with the check still
    with a worker, and says so;
  - the results come: the bow tie face invalid, its wire self-intersecting,
    the overlapping boxes faulty to the BOP check;
  - set off, the panel lists the same rows, made here at once;
  - on again, a check that runs long -- a grid of overlapping spheres --
    and the panel closed while a worker makes it: the worker is ended
    (sec 26), not left making what no one will see.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.path.realpath(os.environ["GT_OUT"])
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
WORKERS = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Workers")
CHECK = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Part/CheckGeometry")


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


def panel():
    """The results' tree and message of the open panel"""
    mw = FreeCADGui.getMainWindow()
    for tree in mw.findChildren(QtWidgets.QTreeView):
        model = tree.model()
        if model and model.headerData(2, QtCore.Qt.Horizontal) == "Error":
            texts = []
            for label in mw.findChildren(QtWidgets.QLabel):
                try:
                    texts.append(label.text())
                except RuntimeError:
                    pass  # gone
            return tree, " / ".join(t for t in texts
                                    if t and ("processed" in t or "worker" in t))
    return None, ""


def rows(tree):
    model = tree.model()
    out = []

    def walk(parent, depth):
        for r in range(model.rowCount(parent)):
            index = model.index(r, 0, parent)
            out.append((depth,) + tuple(model.index(r, c, parent).data() for c in range(3)))
            walk(index, depth + 1)

    walk(QtCore.QModelIndex(), 0)
    return out


def run():
    try:
        WORKERS.RemBool("CheckCommand")
        CHECK.SetBool("AutoRun", True)
        CHECK.SetBool("RunBOPCheck", True)
        doc = FreeCAD.newDocument("CheckByWorker")
        V = FreeCAD.Vector
        bow = doc.addObject("Part::Feature", "Bow")
        bow.Shape = Part.Face(Part.makePolygon([V(0, 0, 0), V(10, 10, 0), V(10, 0, 0),
                                                V(0, 10, 0), V(0, 0, 0)]))
        overlap = doc.addObject("Part::Feature", "Overlap")
        overlap.Shape = Part.makeCompound([Part.makeBox(10, 10, 10),
                                           Part.makeBox(10, 10, 10, V(5, 5, 5))])
        doc.recompute()
        pump(1.0)
        pool = FreeCAD.workerPool(doc)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(bow)
        FreeCADGui.Selection.addSelection(overlap)

        FreeCADGui.runCommand("Part_CheckGeometry")
        end = time.time() + 20
        while panel()[0] is None and time.time() < end:
            pump(0.05)
        tree, text = panel()
        info = pool.info()
        check("by default, the check is with a worker",
              tree is not None and info["Busy"] + info["Queued"] == 1 and not rows(tree), info)
        check("... and the panel says so", "worker process" in text, text)
        end = time.time() + 180
        while (pool.info()["Busy"] or pool.info()["Queued"]) and time.time() < end:
            pump(0.05)
        pump(0.3)
        tree, text = panel()
        there = rows(tree)
        check("the results come", there and "2 processed" in text, (text, there[:3]))
        top = [r for r in there if r[0] == 0]
        check("... both shapes invalid", [r[1] for r in top] == ["CheckByWorker.Bow",
                                                                  "CheckByWorker.Overlap"], top)
        check("... the bow tie's wire self-intersecting",
              any(r[2] == "Wire" and "ntersect" in (r[3] or "") for r in there), there)
        check("... the boxes faulty to the BOP check",
              any("Self-intersect" in (r[3] or "") or "Self" in (r[3] or "") for r in there
                  if r[0] == 1), there)
        FreeCADGui.Control.closeDialog()
        pump(0.5)

        WORKERS.SetBool("CheckCommand", False)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(bow)
        FreeCADGui.Selection.addSelection(overlap)
        FreeCADGui.runCommand("Part_CheckGeometry")
        end = time.time() + 60
        while panel()[0] is None and time.time() < end:
            pump(0.05)
        pump(0.3)
        tree, text = panel()
        here = rows(tree)
        check("set off, made here at once", here and pool.info()["Busy"] == 0, text)
        check("... the same rows", here == there, (here, there))
        FreeCADGui.Control.closeDialog()
        pump(0.5)

        WORKERS.RemBool("CheckCommand")
        grid = doc.addObject("Part::Feature", "Grid")
        grid.Shape = Part.makeCompound([Part.makeSphere(3, V(i * 4, j * 4, 0))
                                        for i in range(25) for j in range(25)])
        doc.recompute()
        pump(0.5)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(grid)
        FreeCADGui.runCommand("Part_CheckGeometry")
        end = time.time() + 60
        while not pool.info()["Busy"] and time.time() < end:
            pump(0.05)
        info = pool.info()
        pids = info["Pids"]
        pump(info["EndAfter"] + 0.5)
        info = pool.info()
        check("a long check is with a worker still", info["Busy"] == 1, info)
        FreeCADGui.Control.closeDialog()
        pump(0.3)
        info = pool.info()
        check("... closing the panel ends its worker",
              info["Ended"] == 1 and info["Busy"] == 0 and not set(pids) & set(info["Pids"]),
              (pids, info))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        WORKERS.RemBool("CheckCommand")
        CHECK.RemBool("AutoRun")
        CHECK.RemBool("RunBOPCheck")
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
