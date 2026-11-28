"""A drag of a fillet's handle into a radius that fails holds at the last
that worked: not back to where the drag began, one failure, and it follows
again once the pointer comes back.

docs/HandsOnQueue.md entry 191. The user's words (2026-10-19): "how about
instead of zero refuse, just auto revert to the last working value. Do not
go through the normal undo stack, as the whole drag may continue multiple
working step, but the whole drag is one transaction" -- "if failed, I
mean". The note-taker's reading, marked there as not said: as the mouse
keeps moving, the value and the handle stay at the last working one until
the mouse reaches a working value again.

What the Mac mini found: a drag's recompute waited for the pointer to stop
(each move started the wait again), so a quick drag recomputed nothing
until its end, and a value failing there went back to the last that had
recomputed -- the drag's start. A slow one held, but failed again on every
move past the limit. Now a drag recomputes at least once a wait, and past
a value that failed it writes nothing until the pointer comes back.

A 10 mm PartDesign box, a fillet of radius 1 on one edge, in edit; its
arrow dragged away from the edge, toward a radius the box cannot take,
quickly (a move every few hundredths of a second) and slowly (a move every
half second). Claims, each way:
  - the drag went past what works: a recompute in it failed;
  - released, the radius is not the drag's start: it is one that worked,
    and the fillet is valid;
  - at most one recompute failed;
  - the whole drag is one undo step;
and slowly, once held, the pointer brought back toward the start: the
radius follows it again, below the held one.

Escape with the button still down, once held (the Windows hang reported
with the fillet drag was on an Escape): the drag is cancelled -- the radius
back to the drag's start, no undo step, the fillet still in edit and valid
-- and the moves and the release after it end the test in good time rather
than hang it.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "FilletDragHolds"
START = 1.0
# how far the drag goes, in radius: well past what a 10 mm box takes
REACH = 30.0

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


def mw():
    return FreeCADGui.getMainWindow()


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def viewer_widget():
    return view().graphicsView()


def dragger_at(x, y):
    """The linear dragger picked at viewport pixel (x, y), or None"""
    from pivy import coin

    rm = view().getViewer().getSoRenderManager()
    action = coin.SoRayPickAction(rm.getViewportRegion())
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(1)
    action.apply(rm.getSceneGraph())
    picked = action.getPickedPoint()
    if picked is None:
        return None
    path = picked.getPath()
    for i in range(path.getLength()):
        node = path.getNode(i)
        if node.getTypeId().getName().getString().endswith("LinearDraggerContainer"):
            return node.getPart("dragger", False)
    return None


def pick_arrow(center, reach=70, step=2):
    """A viewport pixel well inside an arrow near `center`"""
    cx, cy = center
    best, found = -1, None
    for r in range(0, reach, step):
        for dx in range(-r, r + 1, step):
            for dy in (-r, r) if abs(dx) != r else range(-r, r + 1, step):
                if dragger_at(cx + dx, cy + dy) is None:
                    continue
                score = sum(dragger_at(cx + dx + i, cy + dy + j) is not None
                            for i in (-2, 0, 2) for j in (-2, 0, 2))
                if score > best:
                    best, found = score, (cx + dx, cy + dy)
        if found is not None and best >= 9:
            break
    return found


def to_window(x, y):
    width, height = view().getSize()
    w = viewer_widget()
    local = QtCore.QPoint(int(x), int(height - 1 - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


class Failures:
    """The fillet's recomputes that failed, as they happen"""

    def __init__(self, name):
        self.name = name
        self.count = 0

    def slotRecomputedObject(self, obj):
        if obj.Name == self.name and not obj.isValid():
            self.count += 1


def drag_away(fillet, wait_turns, back=False, escape=False):
    """The fillet's arrow dragged away from its edge in 20 moves, a move
    every `wait_turns` hundredths of a second; with `back`, then 10 moves
    back toward the start; with `escape`, then Escape pressed with the
    button down and five moves further out before the release. Returns
    (failures, radius held before coming back, radius after, undo steps,
    seconds from the Escape to the release's end)"""
    doc = fillet.Document
    box = doc.getObject("Box")
    edge = box.Shape.getElement("Edge1")
    middle = edge.valueAt((edge.FirstParameter + edge.LastParameter) / 2)
    edge_at = view().getPointOnViewport(middle)
    arrow = pick_arrow(edge_at)
    if not check("the arrow is found", arrow is not None):
        return None
    # radius per pixel along the arrow, from the dragger's own scale: its
    # translation is the radius times a factor, and a pixel is so much model
    a = view().getPoint(*arrow)
    b = view().getPoint(arrow[0] + 10, arrow[1])
    mm_per_px = ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5 / 10.0
    dx, dy = arrow[0] - edge_at[0], arrow[1] - edge_at[1]
    length = max(1.0, (dx * dx + dy * dy) ** 0.5)
    # generous: the arrow's slant makes a pixel more or less radius
    pixels = REACH * 0.6 / max(1e-9, mm_per_px)
    unit = (dx / length * pixels, dy / length * pixels)
    win = mw().windowHandle()
    failures = Failures(fillet.Name)
    FreeCAD.addDocumentObserver(failures)
    undo_before = len(doc.UndoNames)
    try:
        p = to_window(*arrow)
        QtTest.QTest.mouseMove(win, p)
        pump(5)
        QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
        pump(5)
        q = p
        for i in range(1, 21):
            q = QtCore.QPoint(int(p.x() + unit[0] * i / 20), int(p.y() - unit[1] * i / 20))
            QtTest.QTest.mouseMove(win, q)
            pump(wait_turns)
        pump(60)
        held = fillet.Radius.Value
        escaped = None
        if escape:
            t0 = time.time()
            QtTest.QTest.keyClick(win, Qt.Key_Escape)
            pump(30)
            for i in range(21, 26):
                q = QtCore.QPoint(int(p.x() + unit[0] * i / 20), int(p.y() - unit[1] * i / 20))
                QtTest.QTest.mouseMove(win, q)
                pump(wait_turns)
        if back:
            # toward the start again, to a third of the way out
            for i in range(19, 6, -1):
                q = QtCore.QPoint(int(p.x() + unit[0] * i / 60), int(p.y() - unit[1] * i / 60))
                QtTest.QTest.mouseMove(win, q)
                pump(wait_turns)
            pump(60)
        QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, q)
        pump(150)
        if escape:
            escaped = time.time() - t0
    finally:
        FreeCAD.removeDocumentObserver(failures)
    return (failures.count, held, fillet.Radius.Value, len(doc.UndoNames) - undo_before,
            escaped)


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Gui/Gizmos").SetBool(
            "EnableCoarseSnap", False)
        mw().activateWindow()
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        FreeCADGui.activeDocument().activeView().viewIsometric()
        pump()
        for what, wait_turns in (("quickly", 5), ("slowly", 50), ("Escape", 5)):
            fillet = body.newObject("PartDesign::Fillet", "Fillet")
            fillet.Base = (box, ["Edge1"])
            fillet.Radius = START
            doc.recompute()
            gdoc = FreeCADGui.getDocument(DOC)
            gdoc.setEdit(fillet, 0)
            pump(80)
            got = drag_away(fillet, wait_turns, back=(what == "slowly"),
                            escape=(what == "Escape"))
            if got is not None and what == "Escape":
                failed, held, after, steps, escaped = got
                note("INFO Escape: %d failed recompute(s), held %g, after %g, %d undo step(s),"
                     " %.2f s from the Escape to the release's end" % (failed, held, after,
                                                                      steps, escaped))
                check("Escape: held before it, past the drag's start", held > START + 1e-6, held)
                check("Escape: the radius back to the drag's start", abs(after - START) < 1e-6,
                      (held, after))
                check("Escape: no undo step", steps == 0, steps)
                check("Escape: still in edit", gdoc.getInEdit() is not None, gdoc.getInEdit())
                check("Escape: the fillet valid", fillet.isValid(), fillet.State)
                check("Escape: no hang, the release done in good time", escaped < 10, escaped)
            elif got is not None:
                failed, held, after, steps, _ = got
                note("INFO %s: %d failed recompute(s), held %g, after %g, %d undo step(s)"
                     % (what, failed, held, after, steps))
                check("%s: the drag went past what works, a recompute failed" % what,
                      failed >= 1, failed)
                check("%s: at most one recompute failed" % what, failed <= 1, failed)
                check("%s: held, not back to the drag's start" % what,
                      held > START + 1e-6, held)
                if what == "slowly":
                    check("slowly: the pointer brought back, the radius follows it below "
                          "the held one", START - 1e-6 < after < held - 1e-6, (held, after))
                else:
                    check("quickly: released, still the held radius", abs(after - held) < 1e-6,
                          (held, after))
                check("%s: released, the fillet valid" % what, fillet.isValid(), fillet.State)
                check("%s: the whole drag one undo step" % what, steps <= 1, steps)
            gdoc.resetEdit()
            pump(40)
            doc.removeObject(fillet.Name)
            doc.recompute()
            pump(20)
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
