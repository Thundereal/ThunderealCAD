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
    40 percent, the next edit's label has one that opaque.
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
            out.append((text, float(alpha.getValue()) if alpha else None))
    return out


def key(k):
    w = viewer_widget()
    QtTest.QTest.keyPress(w, k, Qt.NoModifier)
    pump(5)
    target = QtWidgets.QApplication.focusWidget() or w
    QtTest.QTest.keyRelease(target, k, Qt.NoModifier)
    pump(10)


GIZMOS = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Gui/Gizmos")


def try_feature(obj, name, first, opacity=0):
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
        try_feature(box, "box", "Length")
        try_feature(cyl, "cylinder", "Radius")
        GIZMOS.SetInt("OnViewLabelBackgroundOpacity", 40)
        try_feature(box, "box, the ground at 40%", "Length", 40)
        GIZMOS.RemInt("OnViewLabelBackgroundOpacity")
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        try:
            FreeCADGui.Control.closeDialog()
        except Exception:
            pass
        for n in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(n)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
