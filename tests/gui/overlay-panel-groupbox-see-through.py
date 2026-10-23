"""A group box in an overlaid task panel is as see-through as the panel.

docs/HandsOnQueue.md entry 220 (the reporter, 2026-10-23): "new defect
dimension edit panel groupbox has opaque background in overlay transparent
mode". Not TechDraw's own: the Dark theme grounds every QWidget (#7F7F7F),
and what paints a styled ground of its own deeper in a panel -- a group box,
a bare frame -- painted it over the overlay's see-through panel ground.

The Dark theme, every dock overlaid and transparent. A Pad in edit (three
group boxes), then a Hole (its DrillFrame). Each panel's content frame is
rendered with its children onto a clear image. Claims:
  - inside each group box, below its title, where no child widget is, the
    commonest alpha is the panel's own ground's (View/DockOverlayAlpha), not
    opaque (the title and the border stay as drawn);
  - the same inside Hole's bare frame;
  - a spin box's field keeps its own ground (it is not made see-through).

Runs offscreen as well.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")

state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()


def mw():
    return FreeCADGui.getMainWindow()


def cls(w):
    return w.metaObject().className()


def content_frame(w):
    while w is not None:
        if isinstance(w, QtWidgets.QFrame) and w.property("class") == "content":
            return w
        w = w.parentWidget()
    return None


def free_points(w, top):
    """Points of w covered by none of its visible children, below `top` (a
    group box's title and border are drawn opaque, as they should be)"""
    kids = [c.geometry() for c in w.children()
            if isinstance(c, QtWidgets.QWidget) and c.isVisible() and not c.isWindow()]
    out = []
    for y in range(top, max(top + 1, w.height() - 4), 3):
        for x in range(6, max(7, w.width() - 6), 3):
            p = QtCore.QPoint(x, y)
            if not any(k.adjusted(-2, -2, 2, 2).contains(p) for k in kids):
                out.append(p)
    return out


def alpha_in(w, top=4):
    """The commonest alpha of w's own uncovered pixels, as its panel's
    content frame renders it, and the frame's own ground's"""
    frame = content_frame(w)
    if frame is None:
        return None, None
    img = QtGui.QImage(frame.size(), QtGui.QImage.Format_ARGB32)
    img.fill(QtCore.Qt.transparent)
    frame.render(img, QtCore.QPoint(), QtGui.QRegion(), QtWidgets.QWidget.RenderFlag.DrawChildren)
    seen = {}
    for p in free_points(w, top):
        a = QtGui.QColor.fromRgba(img.pixel(w.mapTo(frame, p))).alpha()
        seen[a] = seen.get(a, 0) + 1
    if not seen:
        return None, None
    return max(seen, key=seen.get), QtGui.QColor.fromRgba(img.pixel(1, 1)).alpha()


def run():
    try:
        check("the Dark theme applies", FreeCADGui.applyTheme("Dark"))
        settle(2000)
        doc = FreeCAD.newDocument("GroupBoxSeeThrough")
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        doc.recompute()
        FreeCADGui.runCommand("Std_DockOverlayAll")
        settle(2500)
        for tab in mw().findChildren(QtWidgets.QWidget):
            if cls(tab) == "Gui::OverlayTabWidget" and tab.count():
                for b in tab.findChildren(QtWidgets.QAbstractButton):
                    if b.objectName() == "OBTN Transparent" and not b.isChecked():
                        b.click()
                        settle(800)
        alpha = VIEW.GetInt("DockOverlayAlpha", 180)
        note("INFO overlay alpha %d" % alpha)
        import Part
        sk = body.newObject("Sketcher::SketchObject", "Sketch")
        sk.Support = (box, ["Face6"])
        sk.MapMode = "FlatFace"
        sk.addGeometry(Part.Circle(FreeCAD.Vector(5, 5, 0), FreeCAD.Vector(0, 0, 1), 2))
        doc.recompute()
        g = FreeCADGui.getDocument(doc.Name)

        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sk
        doc.recompute()
        g.setEdit(pad, 0)
        settle(3000)
        boxes = [b for b in mw().findChildren(QtWidgets.QGroupBox)
                 if b.isVisible() and content_frame(b) is not None]
        check("Pad's panel, overlaid: its group boxes", len(boxes) >= 2, len(boxes))
        for b in boxes:
            # below its title
            inside, ground = alpha_in(b, top=b.fontMetrics().height() + 6)
            check("Pad's group box %r: inside at the panel's alpha, not opaque" % b.title(),
                  inside is not None and abs(inside - ground) <= 6 and inside < 250,
                  (inside, ground))
        spin = None
        for w in mw().findChildren(QtWidgets.QAbstractSpinBox):
            if w.isVisible() and content_frame(w) is not None and w.objectName() == "lengthEdit":
                spin = w
        if spin is not None:
            edit = spin.findChild(QtWidgets.QLineEdit)
            inside, ground = alpha_in(edit) if edit else (None, None)
            check("Pad's length field keeps its own ground", inside is not None and inside > ground,
                  (inside, ground))
        g.resetEdit()
        settle(1500)

        hole = body.newObject("PartDesign::Hole", "Hole")
        hole.Profile = sk
        doc.recompute()
        g.setEdit(hole, 0)
        settle(3000)
        frame = None
        for w in mw().findChildren(QtWidgets.QFrame):
            if w.isVisible() and w.objectName() == "DrillFrame":
                frame = w
        if check("Hole's panel, overlaid: its DrillFrame", frame is not None):
            inside, ground = alpha_in(frame)
            check("Hole's bare frame: inside at the panel's alpha, not opaque",
                  inside is not None and abs(inside - ground) <= 6 and inside < 250,
                  (inside, ground))
        g.resetEdit()
        settle(1000)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.runCommand("Std_DockOverlayAll")
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        try:
            FreeCADGui.getDocument(name).resetEdit()
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
