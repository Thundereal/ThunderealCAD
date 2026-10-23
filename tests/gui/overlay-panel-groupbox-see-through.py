"""A group box in an overlaid task panel: a faint ground of its own, not the theme's opaque one.

docs/HandsOnQueue.md entry 220 (the reporter, 2026-10-23): "new defect
dimension edit panel groupbox has opaque background in overlay transparent
mode". Not TechDraw's own: the Dark theme grounds every QWidget (#7F7F7F),
and what paints a styled ground of its own deeper in a panel -- a group box,
a bare frame -- painted it over the overlay's see-through panel ground.
Entry 223: "do not make the group box background completely transparent ...
Maybe like the alternative item background, do that for all group box in
overlay panel"; "yes, include plain frames".

The Dark theme, every dock overlaid and transparent. A Pad in edit (three
group boxes), then a Hole (its DrillFrame). Each panel's content frame is
rendered with its children onto a clear image. Claims:
  - inside each group box, below its title, where no child widget is, the
    commonest alpha is a list's second row's over the panel's ground --
    the band at View/DockOverlayAlpha on the panel's ground at it -- not
    the panel's alone (entry 220's), not opaque (the theme's);
  - inside Hole's bare frame, which is in a group box, the box's ground --
    not a second one on it; a bare frame in no box (TechDraw's rich
    annotation panel has one) a ground of its own;
  - a spin box's field keeps its own ground.

Runs offscreen as well.
"""
import glob
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


def parents(w):
    p = w.parentWidget()
    while p is not None and p.property("class") != "content":
        yield p
        p = p.parentWidget()


def band_over(ground):
    """The alpha of a band at the overlay's alpha laid on the panel's ground"""
    alpha = VIEW.GetInt("DockOverlayAlpha", 180)
    return 255 - (255 - alpha) * (255 - ground) // 255


def banded(inside, ground):
    # the band's ground on the panel's: more than the panel's alone (entry
    # 220's see-through box), less than the theme's opaque one
    return (inside is not None and abs(inside - band_over(ground)) <= 8
            and inside > ground + 10 and inside < 250)


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
            check("Pad's group box %r: inside a band's ground on the panel's, not opaque"
                  % b.title(), banded(inside, ground), (inside, ground, band_over(ground)))
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
            # in a group box: the box's ground, not a second one on it
            check("Hole's bare frame, in its group box: the box's ground, not a second one",
                  banded(inside, ground), (inside, ground, band_over(ground)))
        frames = [w for w in mw().findChildren(QtWidgets.QFrame)
                  if w.isVisible() and type(w) is QtWidgets.QFrame
                  and w.metaObject().className() == "QFrame" and content_frame(w) is not None
                  and w.property("class") is None
                  and w.frameShape() not in (QtWidgets.QFrame.HLine, QtWidgets.QFrame.VLine)
                  and not any(isinstance(p, QtWidgets.QGroupBox) for p in parents(w))]
        note("INFO bare frames in no group box: %s" % [f.objectName() for f in frames])
        for f in frames[:3]:
            inside, ground = alpha_in(f)
            if inside is not None:
                check("a bare frame %r in no box: a band's ground on the panel's" % f.objectName(),
                      banded(inside, ground), (inside, ground, band_over(ground)))
        g.resetEdit()
        settle(1000)

        # A bare frame in no group box: TechDraw's rich annotation panel holds
        # its form in one (TaskRichAnno.ui, "frame"; entry 229)
        page = doc.addObject("TechDraw::DrawPage", "Page")
        tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                           "Templates", "**", "*A4*Landscape*.svg"), recursive=True)
        if templates:
            tmpl.Template = templates[0]
        page.Template = tmpl
        doc.recompute()
        page.ViewObject.doubleClicked()
        settle(2000)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.runCommand("TechDraw_RichTextAnnotation")
        settle(2500)
        anno = None
        for w in mw().findChildren(QtWidgets.QFrame):
            if (w.isVisible() and w.objectName() == "frame" and content_frame(w) is not None
                    and w.findChild(QtWidgets.QLineEdit, "leBaseView") is not None):
                anno = w
        if check("the rich annotation panel, overlaid: its bare frame", anno is not None):
            in_box = any(isinstance(p, QtWidgets.QGroupBox) for p in parents(anno))
            check("... which is in no group box", not in_box)
            inside, ground = alpha_in(anno)
            check("a bare frame in no box: a band's ground on the panel's",
                  banded(inside, ground), (inside, ground, band_over(ground)))
        FreeCADGui.Control.closeDialog()
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
