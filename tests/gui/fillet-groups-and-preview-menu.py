"""A fillet's groups in its panel and its handles, and the handle's menu from the preview.

docs/HandsOnQueue.md entry 202, the user's hands-on of 2026-10-21: "right
click with preview fillet face or its corresonding edge/face hovering shall
bring up the same context menu as its handle ... move the group column to the
second. let's bring back the multiple group colored handle, but add limit to
auto assign color that's distinct from the (pre)selection highlight, and
dragging state color. Also allow user the select color, which is persistent to
fillet property. Also add a conscise tooltips explain each mode ... Double
click the group column shall allow editting the group."

A PartDesign box, a fillet on three edges of its top: A on the default
radius, B and C with radii of their own (groups of their own). In edit.
Claims:
  - the Group column is shown second;
  - the list's header tool tip names the modes; a row's names its own;
  - the arrows: A's plain, B's and C's each a colour of its own, apart from
    each other and from the selected, the pointer's and the dragged colours;
  - "Group colour..." from the list's menu on B's row: the colour dialog's
    blue is B's arrows' colour and the fillet's GroupColors; "Automatic group
    colour" puts the automatic one back and empties it;
  - a double click on A's Group cell opens an editor; C's group typed there:
    A joins it (C's radius, the same group);
  - the pointer on B's base edge, then on B's fillet face: a right click
    opens B's arrow's menu ("One radius" checked), not the view's.

Runs on the real screen; the clicks go through the window.
"""
import colorsys
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
DOC = "FilletGroupsPreviewMenu"
SIZE = 10.0
RADIUS = 1.0
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


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def viewer_widget():
    return view().graphicsView()


def to_window(x, y):
    width, height = view().getSize()
    w = viewer_widget()
    local = QtCore.QPoint(int(x), int(height - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def move(x, y):
    win = mw().windowHandle()
    QtTest.QTest.mouseMove(win, to_window(x, y))
    pump(10)
    QtTest.QTest.mouseMove(win, to_window(x + 1, y))
    pump(10)
    QtTest.QTest.mouseMove(win, to_window(x, y))
    pump(30)


def field_text(node, name):
    value = node.getField(name).get()
    return value.getString() if hasattr(value, "getString") else str(value)


def arrow_colours():
    """The colour of every shown arrow"""
    from pivy import coin

    root = view().getViewer().getSoRenderManager().getSceneGraph()
    search = coin.SoSearchAction()
    search.setType(coin.SoType.fromName("SoLinearDraggerContainer"))
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    colours = []
    for p in search.getPaths():
        container = p.getTail()
        if field_text(container, "visible") not in ("TRUE", "1"):
            continue
        dragger = container.getPart("dragger", False)
        if dragger is not None:
            c = dragger.getField("color").getValue()
            colours.append(tuple(round(v, 3) for v in (c[0], c[1], c[2])))
    return colours


def packed(name, default):
    value = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").GetUnsigned(name, default)
    return ((value >> 24) & 0xff) / 255.0, ((value >> 16) & 0xff) / 255.0, ((value >> 8) & 0xff) / 255.0


def hue(c):
    return colorsys.rgb_to_hsv(*c)


def hue_gap(a, b):
    d = abs(hue(a)[0] - hue(b)[0])
    return min(d, 1.0 - d)


def rows(tree):
    return {tree.topLevelItem(i).text(0): tree.topLevelItem(i) for i in range(tree.topLevelItemCount())}


def group_of(tree, name):
    item = rows(tree).get(name)
    return item.data(GROUP_COLUMN, Qt.UserRole) if item else None


def catch_menu(seen):
    """Note the items of the menu open now, and which are checked, then close it"""
    def answer():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QMenu) and w.isVisible():
                seen["items"] = [a.text().replace("&", "") for a in w.actions()
                                 if a.text()]
                seen["checked"] = [a.text().replace("&", "") for a in w.actions()
                                   if a.isCheckable() and a.isChecked()]
                w.close()
                return
        if seen.get("tries", 0) < 20:
            seen["tries"] = seen.get("tries", 0) + 1
            QtCore.QTimer.singleShot(100, answer)
    QtCore.QTimer.singleShot(100, answer)


def right_click(x, y):
    win = mw().windowHandle()
    move(x, y)
    p = to_window(x, y)
    QtTest.QTest.mousePress(win, Qt.RightButton, Qt.NoModifier, p)
    pump(10)
    QtTest.QTest.mouseRelease(win, Qt.RightButton, Qt.NoModifier, p)
    pump(80)


def preselection():
    sel = FreeCADGui.Selection.getPreselection()
    try:
        return "%s %s" % (sel.ObjectName, list(sel.SubElementNames))
    except Exception:
        return str(sel)


def answer_colour_dialog(colour, seen):
    def answer():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QColorDialog) and w.isVisible():
                seen["dialog"] = True
                w.setCurrentColor(QtGui.QColor(colour))
                w.accept()
                return
        if seen.get("tries", 0) < 30:
            seen["tries"] = seen.get("tries", 0) + 1
            QtCore.QTimer.singleShot(100, answer)
    QtCore.QTimer.singleShot(200, answer)


def right_clicks(box, B):
    """(b) a right click on B's edge, then on its fillet face"""
    edge = box.Shape.getElement(B)
    mid = edge.valueAt((edge.FirstParameter + edge.LastParameter) / 2)
    # the fillet face's middle: off the edge toward the solid by the radius
    # along both faces, then out along the bisector by the radius
    inward = FreeCAD.Vector(-1, 0, -1)
    radius = 1.5
    face_point = mid + inward * radius + FreeCAD.Vector(1, 0, 1).normalize() * radius
    for label, point, wanted in (("the base edge", mid, B), ("the fillet face", face_point, "Face")):
        FreeCADGui.Selection.clearSelection()
        centre = view().getPointOnViewport(point)
        # the pixel near it whose pre-selection names what is wanted
        at = None
        for r in range(0, 9):
            for dx, dy in ((0, 0), (r, 0), (-r, 0), (0, r), (0, -r), (r, r), (-r, -r), (r, -r),
                           (-r, r)):
                x, y = centre[0] + dx, centre[1] + dy
                move(x, y)
                if wanted in preselection():
                    at = (x, y)
                    break
            if at:
                break
        info = view().getObjectInfo((int(centre[0]), int(centre[1])))
        note("INFO %s near %s: at %s, preselected %s; picked at the centre %s"
             % (label, tuple(centre), at, preselection(),
                None if not info else (info.get("Object"), info.get("Component"))))
        if at is None:
            at = centre
            move(*at)
        seen = {}
        catch_menu(seen)
        right_click(*at)
        pump(40)
        note("INFO the right click on %s: %s, checked %s"
             % (label, seen.get("items"), seen.get("checked")))
        check("a right click on B's %s: B's arrow's menu" % label,
              bool(seen.get("items")) and B in seen["items"]
              and "One radius" in (seen.get("checked") or []), seen)
        FreeCADGui.Selection.clearSelection()
        pump(10)


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        mw().showNormal()
        mw().setGeometry(QtGui.QGuiApplication.primaryScreen().availableGeometry())
        pump(30)
        mw().activateWindow()
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = SIZE
        doc.recompute()
        # the top's edges along x at y = 0 (A), along y at x = SIZE (B), along
        # x at y = SIZE (C)
        names = {}
        for i, e in enumerate(box.Shape.Edges):
            a, b = e.Vertexes[0].Point, e.Vertexes[1].Point
            if abs(a.z - SIZE) > 1e-7 or abs(b.z - SIZE) > 1e-7:
                continue
            if abs(a.y) < 1e-7 and abs(b.y) < 1e-7:
                names["A"] = "Edge%d" % (i + 1)
            elif abs(a.x - SIZE) < 1e-7 and abs(b.x - SIZE) < 1e-7:
                names["B"] = "Edge%d" % (i + 1)
            elif abs(a.y - SIZE) < 1e-7 and abs(b.y - SIZE) < 1e-7:
                names["C"] = "Edge%d" % (i + 1)
        note("INFO edges %s" % names)
        A, B, C = names["A"], names["B"], names["C"]
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, [A, B, C])
        fillet.Radius = RADIUS
        fillet.Segments = [(B, [(0.0, 1.5)]), (C, [(0.0, 2.0)])]
        doc.recompute()
        check("the fillet is valid", fillet.isValid() and fillet.Shape.isValid())

        FreeCADGui.SendMsgToActiveView("ViewFit")
        FreeCADGui.activeDocument().activeView().viewIsometric()
        pump()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(fillet, 0)
        pump(80)
        if not check("the fillet is in edit", gdoc.getInEdit() is not None):
            return
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump(30)
        tree = None
        for t in mw().findChildren(QtWidgets.QTreeWidget, "treeWidgetReferences"):
            if t.isVisible():
                tree = t
        if not check("the panel's list is found", tree is not None):
            return
        header = tree.header()

        # (c) the Group column second
        check("the Group column is shown second", header.visualIndex(GROUP_COLUMN) == 1,
              header.visualIndex(GROUP_COLUMN))

        # (e) tool tips
        tip = header.toolTip()
        check("the header's tool tip names the modes",
              all(m in tip for m in ("Default radius", "One radius", "Two radii", "Varying")),
              tip)
        row = rows(tree)
        tips = {k: row[names[k]].toolTip(0) for k in "ABC"}
        note("INFO row tool tips %s" % tips)
        check("A's row: its mode, the default radius", tips["A"].startswith("Default radius"),
              tips["A"])
        check("B's row: its mode and its group",
              tips["B"].startswith("One radius") and "Group" in tips["B"], tips["B"])

        # (b) first: once the list has had the pointer, the panel's hover is
        # the list's, and the real pointer decides when that ends
        right_clicks(box, B)
        # a click on nothing: no value active, nothing under the pointer --
        # the arrows in their own colours again
        width, height = view().getSize()
        for x, y in ((width // 2, 30), (30, height // 2), (width - 30, height // 2),
                     (width // 2, height - 30)):
            if view().getObjectInfo((x, y)) is None:
                win = mw().windowHandle()
                move(x, y)
                QtTest.QTest.mouseClick(win, Qt.LeftButton, Qt.NoModifier, to_window(x, y))
                pump(60)
                note("INFO a click on nothing at %s" % ((x, y),))
                break
        FreeCADGui.Selection.clearSelection()
        pump(20)

        # (d) the arrows' colours
        gA, gB, gC = (group_of(tree, names[k]) for k in "ABC")
        note("INFO groups A %s, B %s, C %s" % (gA, gB, gC))
        colours = arrow_colours()
        note("INFO arrow colours %s" % colours)
        plain = (1.0, 0.0, 0.0)
        own = sorted(set(c for c in colours if c != plain))
        check("the arrows: plain ones and two group colours", plain in colours and len(own) == 2,
              colours)
        taken = {"selected": packed("SelectionColor", 0x1cad1cff),
                 "pointer": packed("HighlightColor", 0xe1e114ff),
                 "dragged": (1.0, 1.0, 0.0), "plain": plain}
        gaps = {str(c): {k: round(hue_gap(c, t), 3) for k, t in taken.items()} for c in own}
        note("INFO hue gaps %s" % gaps)
        check("each group colour clear of the selected, pointer, dragged and plain hues",
              all(g >= 0.08 for d in gaps.values() for g in d.values()), gaps)

        # the list's menu: a colour picked for B's group, then automatic again
        tree.setCurrentItem(rows(tree)[B])
        pump(10)
        actions = {a.text().replace("&", ""): a for a in tree.actions()}
        pick = actions.get("Group colour...")
        automatic = actions.get("Automatic group colour")
        if check("the list's menu has the group colour actions",
                 pick is not None and automatic is not None, list(actions)):
            before = set(arrow_colours())
            seen = {}
            answer_colour_dialog("#0000ff", seen)
            pick.trigger()
            pump(80)
            colours = arrow_colours()
            stored = dict(fillet.GroupColors)
            note("INFO picked: dialog %s, GroupColors %s, arrows %s" % (seen.get("dialog"), stored, colours))
            check("a colour picked: in GroupColors by B's group",
                  stored.get(str(gB)) == "#0000ff", stored)
            check("... and B's arrows wear it", (0.0, 0.0, 1.0) in colours, colours)
            check("... and Automatic is offered", automatic.isEnabled())
            automatic.trigger()
            pump(60)
            after = set(arrow_colours())
            check("Automatic: GroupColors empty, the automatic colour back",
                  dict(fillet.GroupColors) == {} and after == before,
                  (dict(fillet.GroupColors), sorted(before), sorted(after)))

        # (f) a double click on A's Group cell: an editor; C's group typed
        item = rows(tree)[A]
        index = tree.indexFromItem(item, GROUP_COLUMN)
        tree.scrollTo(index)
        pump(10)
        rect = tree.visualRect(index)
        hit = tree.itemAt(rect.center())
        note("INFO the double click at %s: the row %s" % (rect.center(), hit.text(0) if hit else None))
        QtTest.QTest.mouseDClick(tree.viewport(), Qt.LeftButton, Qt.NoModifier, rect.center())
        pump(30)
        editor = QtWidgets.QApplication.focusWidget()
        item = rows(tree).get(A)
        note("INFO the double click's editor: %s; the cell's flags %s, the triggers %s, "
             "the state %s, current %s" % (
                 editor.metaObject().className() if editor else None,
                 int(item.flags().value) if item and hasattr(item.flags(), "value")
                 else (item.flags() if item else None),
                 tree.editTriggers(), tree.state(),
                 tree.currentItem().text(0) if tree.currentItem() else None))
        if not isinstance(editor, QtWidgets.QSpinBox):
            # asked for directly: the delegate's part, apart from the trigger's
            tree.editItem(rows(tree)[A], GROUP_COLUMN)
            pump(20)
            direct = QtWidgets.QApplication.focusWidget()
            note("INFO editItem's editor: %s" % (
                direct.metaObject().className() if direct else None))
            if isinstance(direct, QtWidgets.QSpinBox):
                QtTest.QTest.keyClick(direct, Qt.Key_Escape)
                pump(20)
        if check("a double click on A's Group cell opens a number editor",
                 isinstance(editor, QtWidgets.QSpinBox), editor):
            editor.setValue(int(gC))
            QtTest.QTest.keyClick(editor, Qt.Key_Return)
            pump(5)
            note("INFO just after Return: A's cell %r / %r, the state %s, focus %s" % (
                rows(tree)[A].text(GROUP_COLUMN), rows(tree)[A].data(GROUP_COLUMN, Qt.UserRole),
                tree.state(),
                QtWidgets.QApplication.focusWidget().metaObject().className()
                if QtWidgets.QApplication.focusWidget() else None))
            pump(80)
            note("INFO after: A's cell %r, A in Segments %s" % (
                rows(tree)[A].text(GROUP_COLUMN), A in str(fillet.Segments)))
            tipA = rows(tree)[A].toolTip(0)
            note("INFO groups after: %s" % {k: group_of(tree, names[k]) for k in "ABC"})
            check("A joins C's group, a radius of its own now",
                  group_of(tree, A) == gC and tipA.startswith("One radius"),
                  (group_of(tree, A), gC, tipA))

        check("still in edit", gdoc.getInEdit() is not None)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.getDocument(DOC).resetEdit()
        pump()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
