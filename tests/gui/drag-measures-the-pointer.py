"""A handle's drag ends at the value the pointer is at, whatever its
recomputes do and however long they take.

A linear handle's drag was measured by its Coin dragger
(SoLinearDragger::drag), from a starting point expressed in the dragger's
own frame -- a frame that moves with the dragger's translation. The gizmo
writes that translation itself as the drag goes (the value, reversed past
the start by entry 170's turn, clamped) and so does a value written back
into its box under it (entry 191's revert to the last working value when a
recompute fails). Each such write that differed from where the dragger
had put itself moved what the following moves were measured from; and the
value written back is the last that recomputed, so where a drag ended
turned on how long the recomputes took. A Pad dragged 10 mm down from a
length of 5, through its failing zero ("Length too small") and turned past
it, ended at 5.5 with a quick recompute and 6.0 with a slow one on the Mac
mini; at 5.0 or 6.0 on Windows from run to run. Now the drag is measured on
the arrow's line as it stood at the drag's start, in world coordinates.

A Pad, length 5, its profile a square (a quick recompute) and then a
600-sided polygon (a slow one, a tenth of a second or more); in edit, its
length arrow dragged 10 mm down in 20 moves. Each move aims at the point of
the arrow's axis it should be over, through the view as it is at that
move: the task panel grows when the recompute fails and the view with it,
and a pixel aimed at the start would be over another point of the model by
then. Claims, each speed:
  - a recompute of the failing value is observed: at the crossing the
    pointer rests until one fails, up to 4 s, and the claim FAILs by name if
    none comes. The throttle recomputes a drag once an interval
    (EditRecomputeWait), not at each value; a box whose moves pass the
    failing value inside one interval never recomputed it (Windows,
    2026-11-28), and the claims below would then test nothing;
  - the move after the crossing does not jump out past the pointer
    (before: it wrote 7.0 or 7.5, the pad jumping out the old way for a
    moment, where the pointer said 0.5 the new way) -- it may still be at
    the last working value entry 191 went back to, its turn a move later;
  - from the next move on every move writes the value the pointer is at;
  - it ends Reversed, at a length of 5.
"""
import math
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets
from PySide.QtCore import Qt

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "DragMeasuresThePointer"
START = 5.0
STEP = 0.5
MOVES = 20

state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        time.sleep(0.01)


def rest_for_failure(watch, seconds=4.0):
    """At the failing value, the pointer still: wait for a recompute of it
    to fail. The throttle recomputes a drag once an interval
    (EditRecomputeWait), not at each value, and a box whose moves pass the
    failing value inside one interval never recomputes it -- so the test
    rests there until one is seen, rather than assume it came"""
    end = time.monotonic() + seconds
    while watch.failed < 1 and time.monotonic() < end:
        pump(2)
    return watch.failed >= 1


def mw():
    return FreeCADGui.getMainWindow()


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def viewer_widget():
    return view().graphicsView()


def to_window(x, y):
    width, height = view().getSize()
    w = viewer_widget()
    local = QtCore.QPoint(int(round(x)), int(round(height - 1 - y)))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def dragger_at(x, y):
    from pivy import coin

    rm = view().getViewer().getSoRenderManager()
    action = coin.SoRayPickAction(rm.getViewportRegion())
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(1)
    action.apply(rm.getSceneGraph())
    picked = action.getPickedPoint()
    if picked is None:
        return False
    path = picked.getPath()
    return any(path.getNode(i).getTypeId().getName().getString().endswith("LinearDraggerContainer")
               for i in range(path.getLength()))


def axis_point_under(pixel):
    """The z of the point of the pad's axis (x 5, y 5) the view shows at
    viewport pixel `pixel`: the axis is the arrow's line"""
    best = None
    z = -20.0
    while z <= 30.0:
        x, y = view().getPointOnViewport(FreeCAD.Vector(5, 5, z))
        d = (x - pixel[0]) ** 2 + (y - pixel[1]) ** 2
        if best is None or d < best[0]:
            best = (d, z)
        z += 0.01
    return best[1]


def arrow_pixel():
    """A pixel of the length arrow on its axis, above the pad's top"""
    for z in [START + 0.5 + 0.25 * k for k in range(40)]:
        x, y = view().getPointOnViewport(FreeCAD.Vector(5, 5, z))
        for dx in (0, -1, 1, -2, 2):
            if dragger_at(x + dx, y):
                return (x + dx, y)
    return None


def one_speed(sides):
    doc = FreeCAD.newDocument(DOC)
    body = doc.addObject("PartDesign::Body", "Body")
    sk = body.newObject("Sketcher::SketchObject", "Sketch")
    if sides == 4:
        pts = [FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(10, 0, 0),
               FreeCAD.Vector(10, 10, 0), FreeCAD.Vector(0, 10, 0)]
    else:
        pts = [FreeCAD.Vector(5 + 7 * math.cos(2 * math.pi * k / sides),
                              5 + 7 * math.sin(2 * math.pi * k / sides), 0)
               for k in range(sides)]
    sk.addGeometry([Part.LineSegment(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))],
                   False)
    pad = body.newObject("PartDesign::Pad", "Pad")
    pad.Profile = sk
    pad.Length = START
    doc.recompute()
    began = time.time()
    pad.touch()
    doc.recompute()
    took = time.time() - began
    FreeCADGui.SendMsgToActiveView("ViewFit")
    FreeCADGui.activeDocument().activeView().viewIsometric()
    FreeCADGui.SendMsgToActiveView("ViewFit")
    pump()
    gdoc = FreeCADGui.getDocument(DOC)
    gdoc.setEdit(pad, 0)
    pump(100)
    FreeCADGui.SendMsgToActiveView("ViewFit")
    pump(40)
    what = "a %d-sided profile (a recompute %.3f s)" % (sides, took)
    arrow = arrow_pixel()
    if not check("%s: the length arrow is found" % what, arrow is not None):
        return

    class Watch:
        failed = 0
        writes = []

        def slotRecomputedObject(self, obj):
            if obj.Name == "Pad" and not obj.isValid():
                Watch.failed += 1

    watch = Watch()
    FreeCAD.addDocumentObserver(watch)
    z0 = axis_point_under(arrow)
    win = mw().windowHandle()
    off = []
    jumped = []
    try:
        p = to_window(*arrow)
        QtTest.QTest.mouseMove(win, p)
        pump(5)
        QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
        pump(5)
        q = p
        crossed = None
        rested = None
        for k in range(1, MOVES + 1):
            # aimed through the view as it is now
            x, y = view().getPointOnViewport(FreeCAD.Vector(5, 5, z0 - STEP * k))
            q = to_window(x, y)
            QtTest.QTest.mouseMove(win, q)
            pump(8)
            want = START - STEP * k
            if crossed is None and want <= 0.0:
                crossed = k
                rested = rest_for_failure(watch)
            if crossed is not None and k == crossed + 1:
                # The move after the failing one: the pad may still be at the
                # last working value it went back to (entry 191), its turn
                # taken a move later -- but never out past the pointer
                # (before: 7.0 or 7.5, the pad jumping out the old way)
                got = -pad.Length.Value if pad.Reversed else pad.Length.Value
                if abs(got) > abs(want) + 0.051:
                    jumped.append((k, round(got, 3), want))
            elif crossed is not None and k > crossed + 1:
                # what the move writes, once the turn and the recompute it
                # schedules have gone through: up to a second
                for _ in range(50):
                    got = -pad.Length.Value if pad.Reversed else pad.Length.Value
                    if abs(got - want) <= 0.051:
                        break
                    pump(2)
                if abs(got - want) > 0.051:
                    off.append((k, round(got, 3), want))
        pump(60)
        QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, q)
        pump(100)
    finally:
        FreeCAD.removeDocumentObserver(watch)
    note("INFO %s: ended Reversed %s, Length %.3f; %d failed recompute(s)"
         % (what, pad.Reversed, pad.Length.Value, Watch.failed))
    check("%s: a failing recompute observed at the crossing (rested for it up to 4 s)" % what,
          bool(rested), (rested, Watch.failed))
    check("%s: the move after the crossing did not jump out past the pointer" % what,
          not jumped, jumped)
    check("%s: from the next move on every move wrote the value the pointer is at" % what,
          not off, off[:6])
    check("%s: it ended Reversed, at the pointer's length 5" % what,
          bool(pad.Reversed) and abs(pad.Length.Value - 5.0) < 0.051,
          (pad.Reversed, pad.Length.Value))
    gdoc.resetEdit()
    pump(40)
    FreeCAD.closeDocument(DOC)
    pump(20)


def rotation_handle_pixel():
    """A pixel of the revolution's angle handle, and how many were found"""
    from pivy import coin

    rm = view().getViewer().getSoRenderManager()
    w, h = view().getSize()
    hits = []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            a = coin.SoRayPickAction(rm.getViewportRegion())
            a.setPoint(coin.SbVec2s(x, y))
            a.setRadius(1)
            a.apply(rm.getSceneGraph())
            picked = a.getPickedPoint()
            if picked is None:
                continue
            path = picked.getPath()
            if any(path.getNode(i).getTypeId().getName().getString()
                   .endswith("RotationDraggerContainer") for i in range(path.getLength())):
                hits.append((x, y))
    return (hits[len(hits) // 2] if hits else None), len(hits)


def one_rotation():
    """A Revolution's angle handle dragged from 60 deg through 0 -- where the
    revolution fails, "Angle of revolution too small" -- on to -30: turned,
    30. Seen along the axis, so the handle's plane faces the view and an
    angle on the screen is the angle about the axis."""
    doc = FreeCAD.newDocument(DOC)
    body = doc.addObject("PartDesign::Body", "Body")
    sk = body.newObject("Sketcher::SketchObject", "Sketch")
    pts = [FreeCAD.Vector(5, 0, 0), FreeCAD.Vector(10, 0, 0), FreeCAD.Vector(10, 10, 0),
           FreeCAD.Vector(5, 10, 0)]
    sk.addGeometry([Part.LineSegment(pts[i], pts[(i + 1) % 4]) for i in range(4)], False)
    rev = body.newObject("PartDesign::Revolution", "Revolution")
    rev.Profile = sk
    rev.ReferenceAxis = (sk, ["V_Axis"])
    rev.Angle = 60
    doc.recompute()
    v = FreeCADGui.activeDocument().activeView()
    v.setCameraType("Orthographic")
    v.viewFront()
    gdoc = FreeCADGui.getDocument(DOC)
    gdoc.setEdit(rev, 0)
    pump(100)
    cam = view().getCameraNode()
    cam.position.setValue(0, -100, 0)
    cam.height.setValue(25.0)
    pump(40)
    what = "a revolution's angle"
    handle, found = rotation_handle_pixel()
    if not check("%s: its handle is found" % what, handle is not None, found):
        return
    cx, cy = view().getPointOnViewport(FreeCAD.Vector(0, 0, 0))
    radius = math.hypot(handle[0] - cx, handle[1] - cy)
    start = math.atan2(handle[1] - cy, handle[0] - cx)
    # the profile lies at 0 on the screen, the revolution's 60 below it: a
    # smaller angle is a larger one on the screen
    profile = view().getPointOnViewport(FreeCAD.Vector(10, 5, 0))
    toward = 1.0 if math.atan2(profile[1] - cy, profile[0] - cx) > start else -1.0
    note("INFO %s: the handle %s, %.1f px from the axis at %s" % (
        what, handle, radius, (round(cx), round(cy))))

    class Watch:
        failed = 0

        def slotRecomputedObject(self, obj):
            if obj.Name == "Revolution" and not obj.isValid():
                Watch.failed += 1

    watch = Watch()
    FreeCAD.addDocumentObserver(watch)
    win = mw().windowHandle()
    off, jumped = [], []
    try:
        p = to_window(*handle)
        QtTest.QTest.mouseMove(win, p)
        pump(5)
        QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
        pump(5)
        q = p
        crossed = None
        rested = None
        for k in range(1, 19):
            a = start + toward * math.radians(5 * k)
            cx, cy = view().getPointOnViewport(FreeCAD.Vector(0, 0, 0))
            q = to_window(cx + radius * math.cos(a), cy + radius * math.sin(a))
            QtTest.QTest.mouseMove(win, q)
            pump(8)
            want = 60.0 - 5 * k
            if crossed is None and want <= 0.0:
                crossed = k
                rested = rest_for_failure(watch)
            if crossed is not None and k == crossed + 1:
                got = -rev.Angle.Value if rev.Reversed else rev.Angle.Value
                if abs(got) > abs(want) + 0.51:
                    jumped.append((k, round(got, 2), want))
            elif crossed is not None and k > crossed + 1:
                for _ in range(50):
                    got = -rev.Angle.Value if rev.Reversed else rev.Angle.Value
                    if abs(got - want) <= 0.51:
                        break
                    pump(2)
                if abs(got - want) > 0.51:
                    off.append((k, round(got, 2), want))
        pump(60)
        QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, q)
        pump(100)
    finally:
        FreeCAD.removeDocumentObserver(watch)
    note("INFO %s: ended Reversed %s, Angle %.2f; %d failed recompute(s)"
         % (what, rev.Reversed, rev.Angle.Value, Watch.failed))
    check("%s: a failing recompute observed at the crossing (rested for it up to 4 s)" % what,
          bool(rested), (rested, Watch.failed))
    check("%s: the move after the crossing did not jump out past the pointer" % what,
          not jumped, jumped)
    check("%s: from the next move on every move wrote the angle the pointer is at" % what,
          not off, off[:6])
    check("%s: it ended turned, at the pointer's 30" % what,
          bool(rev.Reversed) and abs(rev.Angle.Value - 30.0) < 0.51,
          (rev.Reversed, rev.Angle.Value))
    gdoc.resetEdit()
    pump(40)
    FreeCAD.closeDocument(DOC)
    pump(20)


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        mw().activateWindow()
        for sides in (4, 600):
            one_speed(sides)
        one_rotation()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        try:
            FreeCADGui.getDocument(name).resetEdit()
            pump()
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
