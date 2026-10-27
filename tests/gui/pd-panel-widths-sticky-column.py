"""PartDesign task panels narrow with their dock; a dress-up's element column stays put.

The reporter, 2026-10-23: "fillet edit task panel, the 'clear', '+' and '-'
button has fix width, set a sensible minimum size and allow it to auto
shrink. audit the reset of the widgets, like the checkboxes ... to have
sensible minimum size too. audit all partdesign task panels for this. also,
make the edge column sticky while the rest of the column scroll
horizontally. apply the same for the other dress up features."

Measured before (r204): the fillet's parameters needed 273 px, a quantity
spin box 144, a push button 75, "Transparent preview" 128; Revolution's axis
box 150, Hole's parameters 378. Claims:
  - the fillet's '+' and '-' and 'Clear': no fixed width, each able to go
    below Qt's minimum; '+' and '-' no wider than 30 px still;
  - the common check boxes, the default radius box: below Qt's minimum;
  - the fillet's, Pad's, Revolution's and Hole's panels narrower than before;
  - the fillet's list scrolled sideways as far as it goes: its Element
    column drawn where it was, rows and title alike; a click there selects
    the row and leaves the scroll as it was; a pointer there names the
    element's cell; a click on its title sorts by it;
  - a chamfer's flip boxes under the element column are cut or hidden.

Clicks the list only; runs offscreen as well.
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
DOC = "PdPanelWidths"

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


def visible(cls=QtWidgets.QWidget, name=None, class_name=None):
    for w in mw().findChildren(cls):
        if not w.isVisible():
            continue
        if name is not None and w.objectName() != name:
            continue
        if class_name is not None and w.metaObject().className() != class_name:
            continue
        return w
    return None


def by_text(cls, text):
    for w in mw().findChildren(cls):
        if w.isVisible() and w.text() == text:
            return w
    return None


def widths(w):
    return (w.minimumWidth(), w.minimumSizeHint().width()) if w else None


def panel_min(class_name):
    w = visible(class_name=class_name)
    return w.minimumSizeHint().width() if w else None


def strip(image, width):
    return image.copy(0, 0, width, image.height())


def same(a, b):
    if a.size() != b.size():
        return False
    a = a.convertToFormat(QtGui.QImage.Format_ARGB32)
    b = b.convertToFormat(QtGui.QImage.Format_ARGB32)
    diff = 0
    for y in range(a.height()):
        for x in range(a.width()):
            if a.pixel(x, y) != b.pixel(x, y):
                diff += 1
    return diff <= a.width() * a.height() // 100


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)

        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1", "Edge2", "Edge3", "Edge4"])
        fillet.Radius = 1.0
        fillet.Segments = [("Edge2", [(0.0, 0.5), (1.0, 0.8)])]
        doc.recompute()
        gdoc.setEdit(fillet, 0)
        pump(80)
        widths_part(gdoc)
        sticky_part(gdoc)
        gdoc.resetEdit()
        pump(40)
        doc.removeObject(fillet.Name)
        doc.recompute()

        chamfer_part(doc, gdoc, body, box)
        other_panels(doc, gdoc, body, box)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def widths_part(gdoc):
    tree = visible(QtWidgets.QTreeWidget, "treeWidgetReferences")
    if not check("the fillet is in edit, its list shown",
                 gdoc.getInEdit() is not None and tree is not None):
        return
    for name in ("btnAdd", "btnRemove"):
        b = visible(QtWidgets.QPushButton, name)
        check("the fillet's '%s': below Qt's minimum, not fixed, no wider than 30" % name,
              b is not None and b.minimumWidth() < b.minimumSizeHint().width()
              and b.sizePolicy().horizontalPolicy() != QtWidgets.QSizePolicy.Fixed
              and b.maximumWidth() <= 30,
              (widths(b), b.sizePolicy().horizontalPolicy() if b else None,
               b.maximumWidth() if b else None))
    b = visible(QtWidgets.QPushButton, "btnClear")
    check("the fillet's 'Clear': below Qt's minimum", b is not None
          and 0 < b.minimumWidth() < b.minimumSizeHint().width(), widths(b))
    for text in ("Show preview", "Transparent preview", "Refine shape", "Use All Edges",
                 "Toggle mode"):
        c = by_text(QtWidgets.QCheckBox, text)
        check("the check box '%s': below Qt's minimum" % text,
              c is not None and 0 < c.minimumWidth() < c.minimumSizeHint().width(), widths(c))
    r = visible(name="filletRadius")
    check("the default radius box: below its old 80 and Qt's minimum",
          r is not None and r.minimumWidth() < 80, widths(r))
    m = panel_min("PartDesignGui::TaskFilletParameters")
    check("the fillet's parameters narrower than the 273 px before", m is not None and m < 230, m)


def sticky_part(gdoc):
    tree = visible(QtWidgets.QTreeWidget, "treeWidgetReferences")
    if tree is None:
        return
    header = tree.header()
    for i in range(1, tree.columnCount()):
        if not header.isSectionHidden(i):
            header.resizeSection(i, 220)
    pump(20)
    QtGui.QCursor.setPos(mw().mapToGlobal(QtCore.QPoint(5, 5)))
    pump(10)
    bar = tree.horizontalScrollBar()
    note("INFO scroll range 0..%d, element width %d, viewport %d"
         % (bar.maximum(), header.sectionSize(0), tree.viewport().width()))
    if not check("the list scrolls sideways", bar.maximum() > header.sectionSize(0), bar.maximum()):
        return
    width = header.sectionSize(0)
    bar.setValue(0)
    pump(20)
    rows0 = strip(tree.viewport().grab().toImage(), width)
    title0 = strip(header.viewport().grab().toImage(), width)
    bar.setValue(bar.maximum())
    pump(20)
    rows1 = strip(tree.viewport().grab().toImage(), width)
    title1 = strip(header.viewport().grab().toImage(), width)
    rows0.save(os.path.join(OUT, "element-strip-unscrolled.png"))
    rows1.save(os.path.join(OUT, "element-strip-scrolled.png"))
    check("scrolled as far as it goes: the element cells drawn as unscrolled",
          same(rows0, rows1))
    check("... and the element title", same(title0, title1))

    item = tree.topLevelItem(1)
    rect = tree.visualItemRect(item)
    point = QtCore.QPoint(min(10, width // 2), rect.center().y())
    index = tree.indexAt(point)
    check("a point on the element strip names the element's cell",
          index.isValid() and index.column() == 0 and index.data() == item.text(0),
          (index.column(), index.data()))
    keep = bar.value()
    tree.clearSelection()
    QtTest.QTest.mouseClick(tree.viewport(), Qt.LeftButton, Qt.NoModifier, point)
    pump(20)
    check("a click there selects the row, the scroll as it was",
          [i.text(0) for i in tree.selectedItems()] == [item.text(0)] and bar.value() == keep,
          ([i.text(0) for i in tree.selectedItems()], keep, bar.value()))
    QtTest.QTest.mouseClick(header.viewport(), Qt.LeftButton, Qt.NoModifier,
                            QtCore.QPoint(min(10, width // 2), header.height() // 2))
    pump(20)
    check("a click on the element title sorts by it",
          header.sortIndicatorSection() == 0 and header.sortIndicatorOrder() == Qt.AscendingOrder,
          (header.sortIndicatorSection(), header.sortIndicatorOrder()))
    # back to the feature's order
    QtTest.QTest.mouseClick(header.viewport(), Qt.LeftButton, Qt.NoModifier,
                            QtCore.QPoint(min(10, width // 2), header.height() // 2))
    QtTest.QTest.mouseClick(header.viewport(), Qt.LeftButton, Qt.NoModifier,
                            QtCore.QPoint(min(10, width // 2), header.height() // 2))
    pump(20)
    check("... a third click, no sort", header.sortIndicatorSection() == -1,
          header.sortIndicatorSection())


def chamfer_part(doc, gdoc, body, box):
    chamfer = body.newObject("PartDesign::Chamfer", "Chamfer")
    chamfer.Base = (box, ["Edge1", "Edge2", "Edge3"])
    chamfer.Size = 1.0
    doc.recompute()
    gdoc.setEdit(chamfer, 0)
    pump(80)
    tree = visible(QtWidgets.QTreeWidget, "treeWidgetReferences")
    if not check("the chamfer is in edit, its list shown", tree is not None):
        return
    # an edge with sizes of its own: its row's flip box opens
    tree.clearSelection()
    item = tree.topLevelItem(0)
    tree.setCurrentItem(item)
    tree.editItem(item, 1)
    pump(20)
    editor = QtWidgets.QApplication.focusWidget()
    if isinstance(editor, QtWidgets.QAbstractSpinBox):
        QtTest.QTest.keyClicks(editor, "2")
        QtTest.QTest.keyClick(editor, Qt.Key_Return)
    pump(80)
    tree = visible(QtWidgets.QTreeWidget, "treeWidgetReferences")
    header = tree.header()
    for i in range(1, tree.columnCount()):
        if not header.isSectionHidden(i):
            header.resizeSection(i, 220)
    pump(20)
    width = header.sectionSize(0)
    editors = [w for w in tree.viewport().children()
               if isinstance(w, QtWidgets.QWidget) and w.isVisible()]
    note("INFO chamfer editors before the scroll: %s"
         % [(w.metaObject().className(), w.geometry().x()) for w in editors])
    # scrolled until the flip column is under the element one
    bar = tree.horizontalScrollBar()
    flip_x = header.sectionPosition(4)
    bar.setValue(min(bar.maximum(), max(0, flip_x - width // 2)))
    pump(30)
    bad = []
    for w in tree.viewport().children():
        if not isinstance(w, QtWidgets.QWidget) or not w.isVisible():
            continue
        g = w.geometry()
        if g.x() < width and w.mask().isEmpty():
            bad.append((w.metaObject().className(), g.x(), g.width()))
    check("a chamfer's flip boxes under the element column are cut or hidden", not bad,
          "scroll %d, element %d, uncut %s" % (bar.value(), width, bad))
    gdoc.resetEdit()
    pump(40)
    doc.removeObject(chamfer.Name)
    doc.recompute()


def other_panels(doc, gdoc, body, box):
    import Part

    sk = body.newObject("Sketcher::SketchObject", "Sketch")
    sk.Support = (box, ["Face6"])
    sk.MapMode = "FlatFace"
    sk.addGeometry(Part.Circle(FreeCAD.Vector(5, 5, 0), FreeCAD.Vector(0, 0, 1), 2))
    doc.recompute()
    before = {"Pad": ("PartDesignGui::TaskPadParameters", 240),
              "Revolution": ("PartDesignGui::TaskRevolutionParameters", 259),
              "Hole": ("PartDesignGui::TaskHoleParameters", 378)}
    for kind, (cls, old) in before.items():
        f = body.newObject("PartDesign::" + kind, kind)
        f.Profile = sk
        if kind == "Revolution":
            f.ReferenceAxis = (sk, ["V_Axis"])
        doc.recompute()
        gdoc.setEdit(f, 0)
        pump(80)
        m = panel_min(cls)
        check("%s's parameters narrower than the %d px before" % (kind, old),
              m is not None and m < old, m)
        if kind == "Revolution":
            axis = visible(QtWidgets.QComboBox, "axis")
            check("Revolution's axis box below its old 150", axis is not None
                  and axis.minimumWidth() < 150, widths(axis))
        if kind == "Pad":
            length = visible(name="lengthEdit")
            check("Pad's length box below Qt's 144", length is not None
                  and length.minimumWidth() < 144, widths(length))
        gdoc.resetEdit()
        pump(40)
        doc.removeObject(f.Name)
        doc.recompute()


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
