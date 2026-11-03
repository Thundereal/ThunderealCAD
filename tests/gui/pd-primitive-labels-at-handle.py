"""A PartDesign primitive's values show beside their handles, one at a time,
on a see-through ground (docs/HandsOnQueue.md entry 240, the reporter
2026-10-28: "make the on view edit label for all primitve feature in
partdesign to show beside their corresponding handle. only show when the
handle is active. add some transparent background for each on view lable,
using the same background color from tree item").

Claims, a box and then a cylinder in their edits:
  - no handle active: no label shown (they were all at the top middle);
  - Tab, the first value's handle active: its label alone is shown;
  - that label has no ground under its text by default (the reporter: "Add
    label background transparency setting, default 0"); with the setting at
    40 percent, the next edit's label has one that opaque;
  - it has the outline a sketch's on-view parameters have (the reporter:
    "show the outline"), ConstraintOutlineWidth, 1 px by default;
  - its text is in the sketch's dimension colour, and on a light background
    a fifth darker (the reporter: "darken in light theme, only a bit");
  - Return opens its edit box where the label stood, and it stays there
    (entry 244, the reporter: "click or enter to edit will move the edit
    box instead of show in place of the label": the text, blank in edit,
    placed the label by its size).
Runs offscreen as well.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets
from PySide.QtCore import Qt
from PySide6 import QtTest
from pivy import coin

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "PrimitiveLabels"


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
        time.sleep(0.01)


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def viewer_widget():
    return view().graphicsView()


def shown(path):
    """Whether every switch on the path takes the next node of it"""
    for i in range(path.getLength() - 1):
        node = path.getNode(i)
        if node.isOfType(coin.SoSwitch.getClassTypeId()):
            which = node.whichChild.getValue()
            if which != coin.SO_SWITCH_ALL and which != path.getIndex(i + 1):
                return False
    return True


def labels():
    """The values' labels shown: (text, alpha of its ground)"""
    # from the aux root, so that every switch above a label is on its path
    found = coin.SoSearchAction()
    found.setType(coin.SoType.fromName("SoDatumLabel"))
    found.setInterest(coin.SoSearchAction.ALL)
    found.setSearchingAll(True)
    found.apply(view().getAuxSceneGraph())
    out = []
    for p in found.getPaths():
        label = p.getTail()
        text = label.getField("string")
        text = text.get().getString() if text else ""
        text = str(text).strip().strip('"')
        if shown(p):
            alpha = label.getField("backgroundAlpha")
            width = label.getField("outlineWidth")
            colour = label.getField("textColor")
            out.append((text, float(alpha.getValue()) if alpha else None,
                        float(width.getValue()) if width else None,
                        tuple(colour.getValue().getValue()) if colour else None))
    return out


def label_spot():
    """Where the one shown label stands, in global pixels: its transform's
    point on the screen"""
    found = coin.SoSearchAction()
    found.setType(coin.SoType.fromName("SoDatumLabel"))
    found.setInterest(coin.SoSearchAction.ALL)
    found.setSearchingAll(True)
    found.apply(view().getAuxSceneGraph())
    for p in found.getPaths():
        if not shown(p):
            continue
        parent = p.getNode(p.getLength() - 2)
        for i in range(p.getIndex(p.getLength() - 1) - 1, -1, -1):
            child = parent.getChild(i)
            if child.isOfType(coin.SoTransform.getClassTypeId()):
                t = child.translation.getValue().getValue()
                sx, sy = view().getPointOnScreen(FreeCAD.Vector(*t))[:2]
                vp = viewer_widget()
                vp = vp.viewport() if hasattr(vp, "viewport") else vp
                ratio = vp.devicePixelRatioF()
                g = vp.mapToGlobal(QtCore.QPoint(int(sx / ratio), int(vp.height() - sy / ratio)))
                return (g.x(), g.y())
    return None


def edit_box_centre():
    """The on-view edit box's middle, in global pixels: the one with the
    keys, else the first one shown outside the task panel"""
    w = QtWidgets.QApplication.focusWidget()
    while w is not None and w.metaObject().className() != "Gui::QuantitySpinBox":
        w = w.parentWidget()
    if w is not None and w.isVisible():
        c = w.mapToGlobal(w.rect().center())
        return (c.x(), c.y())
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QAbstractSpinBox):
        if not w.isVisible() or w.metaObject().className() != "Gui::QuantitySpinBox":
            continue
        parent = w
        while parent is not None and parent.metaObject().className() != "Gui::TaskView::TaskView":
            parent = parent.parentWidget()
        if parent is None:
            c = w.mapToGlobal(w.rect().center())
            return (c.x(), c.y())
    return None


VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")


def background(packed):
    """A plain background of that colour"""
    VIEW.SetBool("Gradient", False)
    VIEW.SetBool("RadialGradient", False)
    VIEW.SetUnsigned("BackgroundColor", packed)
    pump(20)


def dimension_colour(light):
    """The sketch's dimension colour, a fifth darker on a light background"""
    packed = VIEW.GetUnsigned("ConstrainedDimColor", 0xFF2600FF)
    rgb = [((packed >> s) & 0xFF) / 255 for s in (24, 16, 8)]
    return tuple(c * 0.8 for c in rgb) if light else tuple(rgb)


def key(k):
    w = viewer_widget()
    QtTest.QTest.keyPress(w, k, Qt.NoModifier)
    pump(5)
    target = QtWidgets.QApplication.focusWidget() or w
    QtTest.QTest.keyRelease(target, k, Qt.NoModifier)
    pump(10)


GIZMOS = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Gui/Gizmos")


def try_feature(obj, name, first, opacity=0, light=False):
    gdoc = FreeCADGui.getDocument(DOC)
    gdoc.setEdit(obj, 0)
    pump(100)
    now = labels()
    check("%s: no handle active, no label shown" % name, not now, now)
    viewer_widget().setFocus()
    pump(5)
    key(Qt.Key_Tab)
    pump(30)
    now = labels()
    check("%s: Tab, the first handle active: its label alone" % name,
          len(now) == 1 and now[0][0].startswith(first), now)
    if now:
        alpha = now[0][1]
        want = (opacity * 255 // 100) / 255
        check("%s: the label's ground at the setting's %d%%" % (name, opacity),
              alpha is not None and abs(alpha - want) < 1e-3, (alpha, want))
        width = now[0][2]
        check("%s: the label has the sketch's outline, 1 px by default" % name,
              width == 1.0, width)
        got, wanted = now[0][3], dimension_colour(light)
        check("%s: the text in the dimension colour%s" % (
                  name, ", a little darker" if light else ""),
              got is not None and max(abs(a - b) for a, b in zip(got, wanted)) < 0.01,
              (got, wanted))
        # Entry 244: Return opens the edit box where the label stood
        stood = label_spot()
        key(Qt.Key_Return)
        pump(10)
        box = edit_box_centre()
        pump(50)
        later = edit_box_centre()
        far = None if not (stood and box) else max(abs(stood[0] - box[0]), abs(stood[1] - box[1]))
        check("%s: Return, the edit box opens where the label stood" % name,
              far is not None and far <= 8, "label %s, box %s" % (stood, box))
        check("%s: ... and stays there" % name, box is not None and later == box,
              (box, later))
        # the edit given up at the box, which has the keys now
        editor = QtWidgets.QApplication.focusWidget()
        if editor is not None:
            QtTest.QTest.keyClick(editor, Qt.Key_Escape)
            pump(20)
    key(Qt.Key_Escape)
    gdoc.resetEdit()
    pump(30)


def run():
    try:
        doc = FreeCAD.newDocument(DOC)
        b1 = doc.addObject("PartDesign::Body", "BodyBox")
        box = b1.newObject("PartDesign::AdditiveBox", "Box")
        b2 = doc.addObject("PartDesign::Body", "BodyCyl")
        cyl = b2.newObject("PartDesign::AdditiveCylinder", "Cylinder")
        cyl.Placement.Base = FreeCAD.Vector(30, 0, 0)
        doc.recompute()
        view().viewIsometric()
        view().fitAll()
        pump(50)
        GIZMOS.RemInt("OnViewLabelBackgroundOpacity")
        background(0x333333FF)
        try_feature(box, "box", "Length")
        try_feature(cyl, "cylinder", "Radius")
        GIZMOS.SetInt("OnViewLabelBackgroundOpacity", 40)
        try_feature(box, "box, the ground at 40%", "Length", 40)
        GIZMOS.RemInt("OnViewLabelBackgroundOpacity")
        background(0xF0F0F0FF)
        try_feature(box, "box, a light background", "Length", light=True)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        try:
            FreeCADGui.Control.closeDialog()
        except Exception:
            pass
        for name in ("Gradient", "RadialGradient"):
            VIEW.RemBool(name)
        VIEW.RemUnsigned("BackgroundColor")
        for n in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(n)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
