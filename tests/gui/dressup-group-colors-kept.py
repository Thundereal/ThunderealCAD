"""An edge joining an existing group takes that group's colour; no other group's colour moves.

docs/HandsOnQueue.md entry 226 (the reporter, 2026-10-23): "when set fillet
edge group to be the same as some existing group, I am expecting the
changing group to take the color from existing group. But it is the other
way round". A group's automatic colour was its place among the groups there
are: group 1 emptied, every later group took the hue of the one before it.

A PartDesign box, a fillet of three edges, each typed into a group of its
own (1, 2, 3); the colours read from the rows' Group swatches. Then Edge1
typed into group 3. Claims:
  - the three groups' colours are three;
  - after the join, group 2 keeps its colour, group 3 keeps its colour;
  - Edge1 wears group 3's colour.

Runs offscreen as well.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
GROUP_COLUMN = 5

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


def tree():
    for w in mw().findChildren(QtWidgets.QTreeWidget):
        if w.objectName() == "treeWidgetReferences" and w.isVisible():
            return w
    return None


def row(text):
    w = tree()
    for i in range(w.topLevelItemCount()):
        if w.topLevelItem(i).text(0) == text:
            return w.topLevelItem(i)
    return None


def swatch(edge):
    """The colour of the row's Group swatch, (r, g, b)"""
    item = row(edge)
    icon = item.icon(GROUP_COLUMN) if item else None
    if icon is None or icon.isNull():
        return None
    img = icon.pixmap(16, 16).toImage()
    c = img.pixelColor(img.width() // 2, img.height() // 2)
    return (c.red(), c.green(), c.blue())


def type_group(edge, group):
    w = tree()
    item = row(edge)
    editor = None
    for _ in range(3):
        w.setCurrentItem(item, GROUP_COLUMN)
        w.editItem(item, GROUP_COLUMN)
        pump(20)
        focus = QtWidgets.QApplication.focusWidget()
        editor = focus if isinstance(focus, QtWidgets.QSpinBox) else None
        if editor is None:
            for s in w.viewport().findChildren(QtWidgets.QSpinBox):
                if s.isVisible():
                    editor = s
        if editor is not None:
            break
        pump(50)
    if editor is None:
        return False
    editor.setValue(group)
    QtTest.QTest.keyClick(editor, Qt.Key_Return)
    pump(250)
    return True


def run():
    try:
        doc = FreeCAD.newDocument("GroupColorsKept")
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1", "Edge2", "Edge3"])
        fillet.Radius = 1.0
        doc.recompute()
        gdoc = FreeCADGui.getDocument(doc.Name)
        gdoc.setEdit(fillet, 0)
        pump(80)
        if not check("the fillet is in edit, its list shown", tree() is not None):
            return
        for edge, group in (("Edge1", 1), ("Edge2", 2), ("Edge3", 3)):
            type_group(edge, group)
        groups = {e: int(row(e).data(GROUP_COLUMN, Qt.UserRole) or 0)
                  for e in ("Edge1", "Edge2", "Edge3")}
        before = {e: swatch(e) for e in ("Edge1", "Edge2", "Edge3")}
        note("INFO groups %s, colours %s" % (groups, before))
        if not check("three edges in groups 1, 2, 3",
                     groups == {"Edge1": 1, "Edge2": 2, "Edge3": 3}, groups):
            return
        check("three colours", None not in before.values() and len(set(before.values())) == 3,
              before)

        type_group("Edge1", 3)
        after = {e: swatch(e) for e in ("Edge1", "Edge2", "Edge3")}
        note("INFO after Edge1 joins group 3: groups %s, colours %s"
             % ({e: int(row(e).data(GROUP_COLUMN, Qt.UserRole) or 0)
                 for e in ("Edge1", "Edge2", "Edge3")}, after))
        check("group 2 keeps its colour", after["Edge2"] == before["Edge2"],
              (before["Edge2"], after["Edge2"]))
        check("group 3 keeps its colour", after["Edge3"] == before["Edge3"],
              (before["Edge3"], after["Edge3"]))
        check("Edge1 wears group 3's colour", after["Edge1"] == before["Edge3"],
              (after["Edge1"], before["Edge3"]))
        gdoc.resetEdit()
        pump(40)
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
