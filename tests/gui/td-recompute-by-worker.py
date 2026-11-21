"""A TechDraw view in a recompute made by a worker (docs/WorkerProcess.md
sec 39).

A plate with holes (Part::Box cut by a compound of cylinders), a page and
a view of it. Here, the GUI makes the view's projection in threads that end
after its recompute. Claims:
  - here: what the view took, its threads included, is what the next
    recompute that reaches it is expected to take -- not only the start
    its recompute returned after;
  - the Recompute command after an edit of the plate goes to a worker,
    which makes the view (inline, a console process), and its result is
    put in: nothing is made here by its type, the view has the edges of
    the new plate, no thread here projects it again;
  - those are the edges this process makes of it.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.path.realpath(os.environ["GT_OUT"])
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))


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


class Writes:
    """When the view's stored projection was written, from a start"""

    def __init__(self, name):
        self.name = name
        self.times = []
        self.start = time.perf_counter()
        self.recomputed = None

    def slotChangedObject(self, obj, prop):
        if obj.Name == self.name and prop == "ProjectedGeometry":
            self.times.append(time.perf_counter() - self.start)

    def slotRecomputedObject(self, obj):
        if obj.Name == self.name:
            self.recomputed = time.perf_counter() - self.start

    def settle(self, quiet=3.0, most=120):
        """Until no write for `quiet` seconds"""
        end = time.time() + most
        while time.time() < end:
            last = self.times[-1] if self.times else 0
            if time.perf_counter() - self.start - last > quiet:
                return
            pump(0.05)


def edges(view):
    return sorted(round(e.Length, 5) for e in view.getVisibleEdges())


def run():
    try:
        import Part
        import TechDraw  # noqa: F401

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        doc = FreeCAD.newDocument("TDByWorker")
        plate = doc.addObject("Part::Box", "Plate")
        plate.Length = 200
        plate.Width = 200
        plate.Height = 10
        holes = doc.addObject("Part::Feature", "Holes")
        holes.Shape = Part.makeCompound([
            Part.makeCylinder(3, 30, FreeCAD.Vector(10 + 15 * i, 10 + 15 * j, -10))
            for i in range(12) for j in range(12)])
        cut = doc.addObject("Part::Cut", "Cut")
        cut.Base = plate
        cut.Tool = holes
        page = doc.addObject("TechDraw::DrawPage", "Page")
        template = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        template.Template = os.path.join(
            FreeCAD.getHomePath(), "Mod", "TechDraw", "TDTest", "TestTemplate.svg")
        page.Template = template
        view = doc.addObject("TechDraw::DrawViewPart", "View")
        page.addView(view)
        view.Source = [cut]
        view.Direction = FreeCAD.Vector(1, 1, 1)
        pool = FreeCAD.workerPool(doc)
        pool.expectedRecompute()  # starts noting the times

        writes = Writes("View")
        FreeCAD.addDocumentObserver(writes)
        doc.recompute()
        writes.settle()
        FreeCAD.removeDocumentObserver(writes)
        whole = writes.times[-1] if writes.times else -1
        start = writes.recomputed if writes.recomputed is not None else -1
        here = edges(view)
        note("here: recompute returned %.3f s, view complete at %.3f s, %d edges" % (
            start, whole, len(here)))
        view.touch()
        expected, unknown = pool.expectedRecompute()
        # The view's time from its recompute's end to its last thread's is
        # in what it is expected to take; without it, only the part before
        check("here, the view's threads count in what its recompute is expected to take",
              whole > start > 0 and expected >= 0.8 * (whole - start), (expected, whole, start))
        doc.recompute()
        pump(0.2)
        writes = Writes("View")
        FreeCAD.addDocumentObserver(writes)
        writes.settle(quiet=1.0)
        FreeCAD.removeDocumentObserver(writes)

        plate.Height = 14
        writes = Writes("View")
        FreeCAD.addDocumentObserver(writes)
        before = pool.recomputeInfo()
        FreeCADGui.runCommand("Std_Refresh")
        pump(0.3)
        end = time.time() + 120
        while pool.recomputing() and time.time() < end:
            pump(0.05)
        pump(3.0)  # a thread here would write after this
        FreeCAD.removeDocumentObserver(writes)
        info = pool.recomputeInfo()
        check("the Recompute command goes to a worker, whose result is put in",
              info["Applied"] == before["Applied"] + 1, info)
        check("... nothing made here by its type, the view put in from the worker",
              info["MadeHere"] == 0 and info["ExecutedHere"] == 0, info)
        # The put-in reads the value back in its parts (Restore, RestoreDocFile):
        # writes at one moment. A thread here would write a projection's
        # time after
        check("... its projection written by the put-in only: no thread here",
              writes.times and writes.times[-1] - writes.times[0] < 0.05, writes.times)
        byWorker = edges(view)
        check("... and the view has the new plate's edges", byWorker and byWorker != here,
              (len(byWorker), len(here)))

        view.touch()
        doc.recompute()
        writes = Writes("View")
        FreeCAD.addDocumentObserver(writes)
        writes.settle(quiet=2.0)
        FreeCAD.removeDocumentObserver(writes)
        check("... the edges this process makes of it", edges(view) == byWorker,
              (len(edges(view)), len(byWorker)))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
