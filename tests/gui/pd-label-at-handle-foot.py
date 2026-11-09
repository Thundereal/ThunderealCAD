"""A PartDesign value's label stands at its active handle's foot (entry 241).

The reporter 2026-11-02: "I want the same for all partdesign on view edit,
include dressup feature common radius, anchor the label to the active handle
base point. it can have some offset to not obscure the handle itself. also
note, dragging can happen when the handle is not activated yet, I want it
become active, hence label sync once dragging starts." And (d): "on
highlight handle, show tooltip with lable text and value (in the first line
if there's already tooltips)".

A 10 x 10 sketch, a pad of 5 mm up, in edit, the view isometric. Claims:
  - no value active: no Length label (it was always at the top middle);
  - Tab: "Length 5 mm", its middle near the arrow's foot (the pad's top,
    (5, 5, 5)) and clear of the arrow's line on the screen;
  - 8, Enter: Length 8, the label at the new foot (5, 5, 8);
  - Escape: no label; a drag of the arrow, no value active: in the drag the
    label is shown and stands at the foot of the value it reads; after it,
    still shown;
  - Escape, a drag cancelled with Escape: no label after it, Length as before;
  - the pointer resting on the arrow: its tool tip reads the label's text
    first, "Drag to change the value" after it; a move hides it, a rest
    brings it back; in a drag, none while moving, and resting it reads the
    value as dragged (entry 241 (d), the reporter's ruling);
  - a revolution: Tab: "Angle ..." by its handle, not at the top middle.
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
DOC = "LabelAtHandleFoot"

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


def real_key(widget, k):
    QtTest.QTest.keyPress(widget, k, Qt.NoModifier)
    pump(5)
    target = QtWidgets.QApplication.focusWidget() or widget
    QtTest.QTest.keyRelease(target, k, Qt.NoModifier)
    pump(10)


def key(k):
    real_key(viewer_widget(), k)


def field_text(node, name):
    value = node.getField(name).get()
    return value.getString() if hasattr(value, "getString") else str(value)


def text_labels():
    """(text, viewport pixel) of each text-only datum label shown"""
    from pivy import coin

    search = coin.SoSearchAction()
    search.setName(coin.SbName("OnViewRoot"))
    search.setInterest(coin.SoSearchAction.FIRST)
    search.apply(view().getAuxSceneGraph())
    path = search.getPath()
    if not path:
        return []
    root = path.getTail()
    search = coin.SoSearchAction()
    search.setType(coin.SoType.fromName("SoDatumLabel"))
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    found = []
    for p in search.getPaths():
        label = p.getTail()
        if field_text(label, "datumtype") != "TEXT":
            continue
        text = field_text(label, "string").strip().strip('"')
        transform = p.getNodeFromTail(1).getChild(0)
        pixel = None
        if transform.isOfType(coin.SoTransform.getClassTypeId()):
            pixel = vp(FreeCAD.Vector(*transform.translation.getValue().getValue()))
        found.append((text, pixel))
    return found


def label(start):
    for text, pixel in text_labels():
        if text.startswith(start):
            return text, pixel
    return None, None


def vp(v):
    x, y = view().getPointOnViewport(v)
    return (x, y)


def beside(pixel, foot, toward):
    """(distance from the foot, distance from the arrow's line) on the screen,
    the arrow from `foot` toward `toward`"""
    dx, dy = toward[0] - foot[0], toward[1] - foot[1]
    length = math.hypot(dx, dy) or 1.0
    px, py = pixel[0] - foot[0], pixel[1] - foot[1]
    return round(math.hypot(px, py), 1), round(abs(px * dy - py * dx) / length, 1)


def at_top_middle(pixel):
    width, height = view().getSize()
    return abs(pixel[0] - width / 2) <= 2 and height - 50 <= pixel[1] <= height - 10


def pick_arrow(center, reach=80, step=2):
    """A viewport pixel well inside an arrow near `center`"""
    from pivy import coin

    viewer = view().getViewer()
    region = viewer.getSoRenderManager().getViewportRegion()

    def kind(x, y):
        action = coin.SoRayPickAction(region)
        action.setPoint(coin.SbVec2s(int(x), int(y)))
        action.setRadius(1)
        action.apply(viewer.getSoRenderManager().getSceneGraph())
        picked = action.getPickedPoint()
        if picked is None:
            return False
        path = picked.getPath()
        return any("LinearDragger" in path.getNode(i).getTypeId().getName().getString()
                   for i in range(path.getLength()))

    cx, cy = center
    best, found = -1, None
    for r in range(0, reach, step):
        for dx in range(-r, r + 1, step):
            for dy in (-r, r) if abs(dx) != r else range(-r, r + 1, step):
                if not kind(cx + dx, cy + dy):
                    continue
                score = sum(kind(cx + dx + i, cy + dy + j) for i in (-2, 0, 2) for j in (-2, 0, 2))
                if score > best:
                    best, found = score, (cx + dx, cy + dy)
        if found is not None and r > 6:
            break
    return found


def to_window(x, y):
    width, height = view().getSize()
    w = viewer_widget()
    local = QtCore.QPoint(int(x), int(height - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def drag(start, end, escape=False, steps=20, midway=None):
    """From viewport pixel `start` to `end`; `midway()` called halfway, the
    button still down; Escape before letting go"""
    win = mw().windowHandle()
    p = to_window(*start)
    q = to_window(*end)
    QtTest.QTest.mouseMove(win, p)
    pump(5)
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(5)
    for i in range(1, steps + 1):
        m = QtCore.QPoint(p.x() + (q.x() - p.x()) * i // steps, p.y() + (q.y() - p.y()) * i // steps)
        QtTest.QTest.mouseMove(win, m)
        pump(8)
        if midway and i == steps // 2:
            midway()
    if escape:
        real_key(viewer_widget(), Qt.Key_Escape)
        pump(20)
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, q)
    pump(100)


def type_value(digits):
    for d in digits:
        key(getattr(Qt, "Key_" + {".": "Period"}.get(d, d)))
    box = QtWidgets.QApplication.focusWidget()
    if box is not None:
        real_key(box, Qt.Key_Return)
    pump(60)


def tip_widget():
    """The handle's tip: Gui::ToolTip's label (entry 246), shown"""
    for w in QtWidgets.QApplication.allWidgets():
        if w.objectName() == "fc_overlay_tiplabel" and w.isVisible():
            return w
    return None


def tip_lines():
    """The tool tip's lines, [""] when none shows"""
    w = tip_widget()
    if w is None:
        return [""]
    labels = w.findChildren(QtWidgets.QLabel)
    text = labels[0].text() if labels else w.text()
    return text.split("\n")


def length_of(text):
    try:
        return float(text.split()[1])
    except (IndexError, ValueError, AttributeError):
        return None


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        mw().activateWindow()
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        sk = body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(10, 0, 0),
               FreeCAD.Vector(10, 10, 0), FreeCAD.Vector(0, 10, 0)]
        for i in range(4):
            sk.addGeometry(Part.LineSegment(pts[i], pts[(i + 1) % 4]), False)
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sk
        pad.Length = 5
        doc.recompute()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        FreeCADGui.activeDocument().activeView().viewIsometric()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump()

        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(pad, 0)
        pump(100)
        check("the pad is in edit", gdoc.getInEdit() is not None)
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump(40)

        text, pixel = label("Length")
        check("no value active: no Length label", text is None, text_labels())

        viewer_widget().setFocus()
        pump(5)
        key(Qt.Key_Tab)
        text, pixel = label("Length")
        foot = vp(FreeCAD.Vector(5, 5, 5))
        up = vp(FreeCAD.Vector(5, 5, 6))
        note("INFO Tab: label %s at %s, the foot at %s" % (text, pixel, foot))
        check("Tab: 'Length 5 mm' shown", text == "Length 5 mm", text_labels())
        if pixel is not None:
            near, off = beside(pixel, foot, up)
            check("its middle near the arrow's foot", 8 <= near <= 150, (near, pixel, foot))
            check("and clear of the arrow's line", off >= 10, (off, pixel, foot, up))
            check("not at the top middle", not at_top_middle(pixel), pixel)
        if os.environ.get("GT_PICTURE"):
            view().saveRenderDump(os.path.join(OUT, "pad-length-at-foot.png"), metadata=False)

        type_value("8")
        check("8, Enter: Length is 8", abs(pad.Length.Value - 8) < 1e-9, pad.Length)
        # the focus back in the view closes the label, the value still active
        viewer_widget().setFocus()
        pump(40)
        text, pixel = label("Length")
        foot8 = vp(FreeCAD.Vector(5, 5, 8))
        note("INFO after 8: label %s at %s, the foot at %s" % (text, pixel, foot8))
        if check("the label reads 'Length 8 mm'", text == "Length 8 mm", text_labels()):
            near, off = beside(pixel, foot8, vp(FreeCAD.Vector(5, 5, 9)))
            check("it stands at the new foot", 8 <= near <= 150 and off >= 10,
                  (near, off, pixel, foot8))

        key(Qt.Key_Escape)
        text, _ = label("Length")
        check("Escape: no label", text is None, text_labels())
        arrow = pick_arrow(foot8)
        note("INFO the arrow at %s" % (arrow,))
        if check("the length arrow is found", arrow is not None):
            seen = {}

            def midway():
                t, p = label("Length")
                seen["text"], seen["pixel"] = t, p
                value = length_of(t)
                if p is not None and value is not None:
                    f = vp(FreeCAD.Vector(5, 5, value))
                    seen["beside"] = beside(p, f, vp(FreeCAD.Vector(5, 5, value + 1)))
                    seen["foot"] = f

            a, b = vp(FreeCAD.Vector(5, 5, 8)), vp(FreeCAD.Vector(5, 5, 11))
            end = (arrow[0] + b[0] - a[0], arrow[1] + b[1] - a[1])
            drag(arrow, end, midway=midway)
            note("INFO in the drag: %s; after: Length %s" % (seen, pad.Length))
            check("a drag of a handle not active: its label shown in the drag",
                  seen.get("text") is not None and seen["text"].startswith("Length"), seen)
            near, off = seen.get("beside", (None, None))
            check("in the drag the label stands at the foot of the value it reads",
                  near is not None and 8 <= near <= 150 and off >= 10, seen)
            check("the drag changed the length", pad.Length.Value > 8.5, pad.Length)
            text, _ = label("Length")
            check("after the drag: the label still shown (the handle active)",
                  text is not None, text_labels())

            # a cancelled drag makes nothing active
            key(Qt.Key_Escape)
            length = pad.Length.Value
            arrow = pick_arrow(vp(FreeCAD.Vector(5, 5, length)))
            if check("the arrow is found again", arrow is not None):
                a = vp(FreeCAD.Vector(5, 5, length))
                b = vp(FreeCAD.Vector(5, 5, length + 3))
                end = (arrow[0] + b[0] - a[0], arrow[1] + b[1] - a[1])
                drag(arrow, end, escape=True)
                text, _ = label("Length")
                check("a drag cancelled with Escape: no label after it, Length as before",
                      text is None and abs(pad.Length.Value - length) < 1e-9,
                      (text_labels(), pad.Length))

            # the tool tip: the value first (entry 241 (d)), shown once the
            # pointer rests, hidden while it moves -- in a drag too
            arrow = pick_arrow(vp(FreeCAD.Vector(5, 5, pad.Length.Value)))
            if check("the arrow is found for its tool tip", arrow is not None):
                win = mw().windowHandle()
                p = to_window(*arrow)
                QtTest.QTest.mouseMove(win, p)
                pump(10)
                QtTest.QTest.mouseMove(win, QtCore.QPoint(p.x() + 1, p.y()))
                pump(120)
                lines = tip_lines()
                want = "Length %g mm" % pad.Length.Value
                note("INFO the tool tip at rest: %r, the value %s" % (lines, want))
                check("resting on the arrow: its tool tip reads the label's text first",
                      lines[0] == want, (lines, want))
                check("and what the handle does after it",
                      len(lines) > 1 and lines[1].startswith("Drag to change the value"), lines)
                # entry 246: the rounded label, not Qt's square one, beside
                # the pointer
                tip = tip_widget()
                check("the tip is the rounded tip label, not Qt's",
                      tip is not None and not QtWidgets.QToolTip.isVisible(),
                      (tip, QtWidgets.QToolTip.isVisible()))
                if tip is not None:
                    at = tip.mapToGlobal(QtCore.QPoint(0, 0))
                    pointer = mw().windowHandle().mapToGlobal(QtCore.QPoint(p.x() + 1, p.y()))
                    off = (at.x() - pointer.x(), at.y() - pointer.y())
                    check("and stands beside the pointer",
                          -40 <= off[0] <= 40 and -60 <= off[1] <= 40, off)
                QtTest.QTest.mouseMove(win, p)
                # hideText() lets the tip go after a short fade (Qt: 300 ms)
                pump(45)
                check("the pointer moved: the tip hidden", not tip_lines()[0], tip_lines())
                pump(120)
                check("resting again: the tip back", tip_lines()[0] == want, tip_lines())

                seen = {}

                def rest_in_drag():
                    seen["moving"] = tip_lines()[0]
                    pump(120)
                    seen["resting"] = tip_lines()[0]
                    seen["label"] = label("Length")[0]

                length = pad.Length.Value
                a = vp(FreeCAD.Vector(5, 5, length))
                b = vp(FreeCAD.Vector(5, 5, length + 4))
                end = (arrow[0] + b[0] - a[0], arrow[1] + b[1] - a[1])
                drag(arrow, end, midway=rest_in_drag)
                note("INFO the tool tip in a drag: %s; after it Length %s" % (seen, pad.Length))
                check("in a drag, while moving: no tip", not seen.get("moving"), seen)
                check("in a drag, resting: the tip reads the value as dragged, as the label does",
                      seen.get("resting") and seen.get("resting") == seen.get("label")
                      and seen["resting"] != want, seen)
                if tip_widget() is not None:
                    tip_widget().hide()
        gdoc.resetEdit()
        pump(60)

        # a rotation handle's label, by the handle
        doc.removeObject("Pad")
        doc.recompute()
        sk2 = body.newObject("Sketcher::SketchObject", "Sketch001")
        pts = [FreeCAD.Vector(2, 0, 0), FreeCAD.Vector(6, 0, 0),
               FreeCAD.Vector(6, 4, 0), FreeCAD.Vector(2, 4, 0)]
        for i in range(4):
            sk2.addGeometry(Part.LineSegment(pts[i], pts[(i + 1) % 4]), False)
        rev = body.newObject("PartDesign::Revolution", "Revolution")
        rev.Profile = sk2
        rev.ReferenceAxis = (sk2, ["V_Axis"])
        rev.Angle = 180
        doc.recompute()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump(40)
        gdoc.setEdit(rev, 0)
        pump(100)
        viewer_widget().setFocus()
        pump(5)
        key(Qt.Key_Tab)
        text, pixel = label("Angle")
        note("INFO revolution: Tab: %s at %s" % (text, pixel))
        check("revolution: Tab: 'Angle ...' shown, not at the top middle",
              text is not None and pixel is not None and not at_top_middle(pixel), text_labels())
        if os.environ.get("GT_PICTURE"):
            view().saveRenderDump(os.path.join(OUT, "revolution-angle-at-handle.png"),
                                  metadata=False)
        gdoc.resetEdit()
        pump(60)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        if FreeCADGui.getDocument(DOC).getInEdit():
            FreeCADGui.getDocument(DOC).resetEdit()
            pump(20)
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
