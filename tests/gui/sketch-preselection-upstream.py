"""Upstream's Sketcher preselection tests, run on the fork's hover pick.

src/Mod/Sketcher/SketcherTests/TestConstraintPreselectionGui.py is
upstream's file, unchanged: which element a hover at a viewport position
preselects when a vertex, a curve, an axis and a constraint's icon or
number overlap. Upstream wrote it for its screen-space preselection rework
(2f3161f312 and the commits after it); the fork keeps its own pick, and
this runs the same checks against it through the same probe,
SketcherGui.getActiveSketchPreselection().

Under the default renderer (render cache mode 3) the number of a dimension
could not be picked at all, whatever lay under it: the two datum text
checks failed. Mode 0 passed all five.

Each unittest case is written as a PASS/FAIL line; GT_RENDER_CACHE sets
the render cache mode for the run (default: the view's own, 3).
"""
import os
import traceback
import unittest

import FreeCAD
from PySide import QtCore

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


class Result(unittest.TextTestResult):
    def addSuccess(self, test):
        super().addSuccess(test)
        note("PASS " + test.id())

    def addFailure(self, test, err):
        super().addFailure(test, err)
        note("FAIL " + test.id() + " | " + str(err[1])[:2000])

    def addError(self, test, err):
        super().addError(test, err)
        note("FAIL " + test.id() + " | " + "".join(traceback.format_exception(*err))[-2000:])

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        note("FAIL skipped " + test.id() + " | " + reason)


# px per mm the curve-over-dimension case is viewed at
CURVE_CASE_SCALE = 6.0


def pin_curve_case_scale(cases):
    """The curve-over-dimension case is of a dimension's LINE: its probes,
    16 px either side of the line's midpoint, are meant to stay off the
    dimension's number, 12 mm along the line. Where that lies in pixels was
    left to fitAll, so to the window: on the Mac mini's 866x311 view it is
    1.88 px/mm, the number 23 px from the midpoint and picked from 2 to 42 px
    -- the probes' right half on it, and the number rightly wins over the
    curve under it (upstream's own testDistanceDatumTextWinsOverOverlapping-
    Curve). That case alone is viewed at CURVE_CASE_SCALE, centred on the
    sketch, whatever the window's height; upstream's file stays unchanged,
    and the font is still set below (010ab58c92) for the same reason."""
    original = cases.configure_view_state.__func__
    test = cases.testCurveWinsOverOverlappingDistanceDimensionLine
    pinned = {"on": False}

    def configure_view_state(cls, view, tilt=None):
        original(cls, view, tilt)
        if not pinned["on"]:
            return
        sketch = FreeCAD.ActiveDocument.getObject("Sketch")
        box = sketch.Shape.BoundBox if sketch is not None and not sketch.Shape.isNull() else None
        cam = view.getCameraNode()
        if box is not None and box.isValid():
            pos = cam.position.getValue()
            cam.position.setValue(box.Center.x, box.Center.y, pos[2])
        # twice: the second corrects the first by what it measures
        for _ in range(2):
            a = view.getPointOnViewport(FreeCAD.Vector(0, 0, 0))
            b = view.getPointOnViewport(FreeCAD.Vector(10, 0, 0))
            scale = abs(b[0] - a[0]) / 10.0
            if scale > 0:
                cam.height.setValue(cam.height.getValue() * scale / CURVE_CASE_SCALE)
            cls.pump_gui_events()

    def curve_case(self):
        pinned["on"] = True
        try:
            test(self)
        finally:
            pinned["on"] = False

    cases.configure_view_state = classmethod(configure_view_state)
    cases.testCurveWinsOverOverlappingDistanceDimensionLine = curve_case


def run():
    try:
        mode = os.environ.get("GT_RENDER_CACHE")
        if mode is not None:
            FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt(
                "RenderCache", int(mode))
        # The curve-over-dimension case probes 16 px either side of a line whose
        # dimension's number stands 12 mm along it. Its left end is the label's
        # width from there: at the application font's height the last column of
        # probes is on the number. The test is of a dimension's LINE.
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt(
            "EditSketcherFontSize", 13)
        from SketcherTests import TestConstraintPreselectionGui

        pin_curve_case_scale(TestConstraintPreselectionGui.SketcherGuiTestCases)
        suite = unittest.defaultTestLoader.loadTestsFromModule(TestConstraintPreselectionGui)
        with open(os.path.join(OUT, "unittest.txt"), "w") as stream:
            result = unittest.TextTestRunner(
                stream=stream, resultclass=Result, verbosity=2).run(suite)
        if result.testsRun == 0:
            note("FAIL no test ran")
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
