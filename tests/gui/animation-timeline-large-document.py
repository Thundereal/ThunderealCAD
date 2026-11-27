"""The animation timeline costs nothing per change in a large document.

dell, 2026-11-27: MiSTer_binary.FCStd (18199 objects) took 521 s to open in
the GUI; 29 of 35 samples were in AnimationTimeline::onSlotChangedObject,
which looked for the document's active Animation -- a scan of every object
-- on every change of every object before it asked whether the object was
an Animation at all.

A document of SMALL objects and one of LARGE, each with an Animation (not
chosen, so the timeline has to look for it), each active in turn. One
object's property is set CHANGES times in each. Claims:
  - a change in the large document costs at most twice one in the small
    (before the fix: about LARGE/SMALL times as much);
  - the timeline still follows its Animation: Time set to 1.5 s, the
    timeline's time box shows 1.5.
"""
import os
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))

SMALL = 50
LARGE = 20000
CHANGES = 4000


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds=0.2):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def make(name, count):
    doc = App.newDocument(name)
    for i in range(count):
        doc.addObject("App::DocumentObjectGroup", "F%d" % i)
    doc.addObject("App::Animation", "Animation")
    probe = doc.addObject("App::FeaturePython", "Probe")
    probe.addProperty("App::PropertyInteger", "N")
    pump(0.5)
    return doc, probe


def per_change(doc, probe):
    App.setActiveDocument(doc.Name)
    Gui.ActiveDocument = Gui.getDocument(doc.Name)
    pump(0.3)
    best = None
    for _ in range(3):
        start = time.perf_counter()
        for i in range(CHANGES):
            probe.N = i
        took = (time.perf_counter() - start) / CHANGES
        best = took if best is None else min(best, took)
    return best


def run():
    docs = []
    try:
        small, sprobe = make("TimelineSmall", SMALL)
        docs.append(small.Name)
        large, lprobe = make("TimelineLarge", LARGE)
        docs.append(large.Name)
        ts = per_change(small, sprobe)
        tl = per_change(large, lprobe)
        check("a change in %d objects costs at most twice one in %d" % (LARGE, SMALL),
              tl <= 2 * ts + 5e-6,
              "%.1f us against %.1f us" % (tl * 1e6, ts * 1e6))

        anim = large.getObject("Animation")
        anim.Time = 1.5
        pump(0.5)
        mw = Gui.getMainWindow()
        dock = next((d for d in mw.findChildren(QtWidgets.QDockWidget)
                     if d.objectName() == "Animation"), None)
        values = [b.value() for b in dock.findChildren(QtWidgets.QDoubleSpinBox)] if dock else []
        check("the timeline still follows its Animation: Time 1.5 s shown",
              any(abs(v - 1.5) < 1e-9 for v in values), values)
    except Exception:
        note("ABORT\n" + traceback.format_exc())
    for name in docs:
        try:
            App.closeDocument(name)
        except Exception:
            pass
    note("DONE")
    QtCore.QTimer.singleShot(200, QtWidgets.QApplication.quit)


QtCore.QTimer.singleShot(1000, run)
