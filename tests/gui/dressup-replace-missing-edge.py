"""With a missing reference's row selected, a pick of an unused base edge takes its place.

docs/HandsOnQueue.md entry 188 (the reporter, 2026-10-19 18:03): "when one
missing (not failed) preview shape is selected, clicking another edge (not
already filleted) on base feature shall change the edge reference inside the
failed one. apply this to all other dressup features"

A PartDesign wedge, a fillet of radius 2 on a top edge and a bottom edge, the
top edge with points of its own (Segments); the top then collapses to a line,
so the top edge's reference is missing ("?Edge" row). In edit. Claims:
  - a click on the missing edge's kept face selects its row; the click a pick
    of an unused base edge makes (the selection cleared, the edge added)
    then renames that row in its place: no "?" row, the new edge where the
    missing one was, in the feature's Base, the fillet made without failure;
  - the new row is the selected one, and the base's edge the selection;
  - the points the missing edge had are the new edge's (its row's children);
  - one undo step brings the missing reference back;
  - a pick of an edge already in the list, with the missing row selected,
    replaces nothing: the list and Base as they were;
  - the same for a chamfer, its missing row selected from the list.

Moves the mouse pointer and clicks in the 3D view: run on a real screen.
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
DOC = "DressUpReplaceMissing"
RADIUS = 2.0

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


def face_on(face):
    u0, u1, v0, v1 = face.ParameterRange
    n = face.normalAt((u0 + u1) / 2, (v0 + v1) / 2)
    view().setViewDirection((-n.x, -n.y, -n.z))
    view().fitAll()
    pump(40)


def on_handle(x, y):
    from pivy import coin

    viewer = view().getViewer()
    rm = viewer.getSoRenderManager()
    action = coin.SoRayPickAction(rm.getViewportRegion())
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(4)
    action.apply(rm.getSceneGraph())
    picked = action.getPickedPoint()
    if picked is None:
        return False
    path = picked.getPath()
    for i in range(path.getLength()):
        if "Dragger" in path.getNode(i).getTypeId().getName().getString():
            return True
    return False


def spot(face):
    u0, u1, v0, v1 = face.ParameterRange
    for fu, fv in ((0.5, 0.2), (0.5, 0.8), (0.2, 0.5), (0.8, 0.5), (0.3, 0.3), (0.5, 0.5)):
        point = face.valueAt(u0 + (u1 - u0) * fu, v0 + (v1 - v0) * fv)
        x, y = view().getPointOnScreen(point)
        if not on_handle(x, y):
            return int(x), int(y)
    return int(x), int(y)


def to_window(x, y):
    width, height = view().getSize()
    w = view().graphicsView()
    local = QtCore.QPoint(int(x), int(height - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def click(x, y):
    win = mw().windowHandle()
    p = to_window(x, y)
    QtTest.QTest.mouseMove(win, p)
    pump(10)
    QtTest.QTest.mouseMove(win, to_window(x + 1, y))
    pump(10)
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, to_window(x + 1, y))
    pump(5)
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, to_window(x + 1, y))
    pump(60)


def pick_base(edge):
    """What a plain click on the base's edge does to the selection: cleared,
    then the edge added"""
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(DOC, "Body", "Wedge." + edge)
    pump(80)


def selection():
    return [(s.ObjectName, list(s.SubElementNames))
            for s in FreeCADGui.Selection.getSelectionEx("", 0)]


def tree():
    for w in mw().findChildren(QtWidgets.QTreeWidget):
        if w.objectName() == "treeWidgetReferences" and w.isVisible():
            return w
    return None


def rows(widget):
    return [widget.topLevelItem(i).text(0) for i in range(widget.topLevelItemCount())]


def row(widget, text):
    for i in range(widget.topLevelItemCount()):
        if widget.topLevelItem(i).text(0) == text:
            return widget.topLevelItem(i)
    return None


def selected_rows(widget):
    out = []
    for it in widget.selectedItems():
        it = it.parent() or it
        if it.text(0) not in out:
            out.append(it.text(0))
    return out


def wedge_edges(wedge):
    shape = wedge.Shape
    top = bottom = None
    used = set()
    for i, e in enumerate(shape.Edges):
        ends = e.Vertexes
        if len(ends) != 2:
            continue
        if (all(abs(v.Point.y - shape.BoundBox.YMax) < 1e-7 for v in ends)
                and all(abs(v.Point.z - wedge.Z2min.Value) < 1e-7 for v in ends)):
            top = "Edge%d" % (i + 1)
        if (all(abs(v.Point.y - shape.BoundBox.YMin) < 1e-7 for v in ends)
                and all(abs(v.Point.z - wedge.Zmin.Value) < 1e-7 for v in ends)):
            bottom = "Edge%d" % (i + 1)
    return top, bottom


def free_edge(wedge, taken):
    """An edge of the collapsed wedge on its bottom face, not in \a taken"""
    shape = wedge.Shape
    for i, e in enumerate(shape.Edges):
        name = "Edge%d" % (i + 1)
        if name in taken or len(e.Vertexes) != 2:
            continue
        if all(abs(v.Point.y - shape.BoundBox.YMin) < 1e-7 for v in e.Vertexes):
            return name
    return None


def base_subs(feature):
    return list(feature.Base[1]) if feature.Base else []


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetBool("PerViewEdit", True)
        mw().showNormal()
        mw().setGeometry(QtGui.QGuiApplication.primaryScreen().availableGeometry())
        pump(30)
        mw().activateWindow()
        doc = FreeCAD.newDocument(DOC)
        doc.UndoMode = 1
        body = doc.addObject("PartDesign::Body", "Body")
        wedge = body.newObject("PartDesign::AdditiveWedge", "Wedge")
        doc.recompute()
        top, bottom = wedge_edges(wedge)
        note("INFO the top edge %s, the bottom edge %s" % (top, bottom))
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (wedge, [top, bottom])
        fillet.Radius = RADIUS
        fillet.Segments = [(top, [(0.0, 1.0), (1.0, 1.5)])]
        doc.recompute()
        wedge.X2min = 5
        wedge.X2max = 5
        doc.recompute()
        kept = fillet.FailedShape
        check("the top edge is missing, its face kept",
              len(fillet.FailedEdges) == 1 and not kept.isNull() and len(kept.Faces) >= 1,
              "FailedEdges %s" % list(fillet.FailedEdges))

        gdoc = FreeCADGui.getDocument(DOC)
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump()
        gdoc.setEdit(fillet, 0)
        pump(80)
        widget = tree()
        if not check("the fillet is in edit, its edge list shown",
                     gdoc.getInEdit() is not None and widget is not None):
            return
        before = rows(widget)
        missing = [t for t in before if t.startswith("?")]
        present = [t for t in before if not t.startswith("?")]
        note("INFO rows %s, Base %s" % (before, base_subs(fillet)))
        if not check("a missing row and a present one", missing and present, before):
            return
        missing_children = row(widget, missing[0]).childCount()
        note("INFO the missing row's points (children): %d" % missing_children)
        new = free_edge(wedge, set(present) | {m.lstrip("?") for m in missing})
        note("INFO the unused edge picked: %s" % new)

        # the missing edge's kept face clicked: its row selected
        FreeCADGui.Selection.clearSelection()
        pump(20)
        face_on(kept.Faces[0])
        x, y = spot(kept.Faces[0])
        click(x, y)
        if not check("a click on the missing edge's kept face selects its row",
                     selected_rows(widget) == [missing[0]], selected_rows(widget)):
            return
        undo_before = doc.UndoCount
        pick_base(new)
        after = rows(widget)
        index = before.index(missing[0])
        check("a pick of an unused base edge: no missing row left",
              not any(t.startswith("?") for t in after), after)
        check("... the new edge in the missing one's place in the list",
              len(after) == len(before) and after[index] == new, "%s -> %s" % (before, after))
        subs = base_subs(fillet)
        check("... and in the feature's Base, the missing one not",
              new in subs and not any(s.lstrip("?") == missing[0].lstrip("?") for s in subs),
              subs)
        # (Touched while the panel is open: preview-on-edit pauses the
        # feature's own recompute, tests/gui/pd-leave-edit-recompute.py)
        check("... the fillet made without a failed edge", not list(fillet.FailedEdges)
              and "Invalid" not in fillet.State
              and "Invalid" not in fillet.State, "FailedEdges %s, state %s"
              % (list(fillet.FailedEdges), fillet.State))
        check("... the new row the selected one", selected_rows(widget) == [new],
              selected_rows(widget))
        sel = selection()
        names = [n for _, ss in sel for n in ss]
        check("... the base's edge the selection",
              any(n.startswith("Wedge.") and n.endswith("." + new) for n in names), sel)
        if missing_children:
            check("... the missing edge's points are the new edge's",
                  row(widget, new) is not None
                  and row(widget, new).childCount() == missing_children,
                  row(widget, new).childCount() if row(widget, new) else None)
        else:
            note("INFO the missing row showed no points to carry over")
        pump(40)
        check("one undo step", doc.UndoCount - undo_before == 1,
              "%d -> %d" % (undo_before, doc.UndoCount))
        doc.undo()
        pump(80)
        widget = tree()
        check("... which brings the missing reference back",
              widget is not None and rows(widget) == before,
              rows(widget) if widget else None)

        # an edge already in the list: nothing replaced
        if widget is not None:
            FreeCADGui.Selection.clearSelection()
            pump(20)
            r = row(widget, missing[0])
            if r is not None:
                widget.clearSelection()
                r.setSelected(True)
                pump(30)
            subs0 = base_subs(fillet)
            pick_base(present[0])
            check("a pick of an edge in the list, the missing row selected: the list as it was",
                  rows(widget) == before, rows(widget))
            check("... and Base", base_subs(fillet) == subs0, base_subs(fillet))
            # as before 188: the picked edge's row joins the selection
            check("... the picked edge's row selected", present[0] in selected_rows(widget),
                  selected_rows(widget))

        gdoc.resetEdit()
        pump(60)

        # a chamfer: its missing row selected from the list
        chamfer_doc()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def chamfer_doc():
    name = DOC + "C"
    doc = FreeCAD.newDocument(name)
    body = doc.addObject("PartDesign::Body", "Body")
    wedge = body.newObject("PartDesign::AdditiveWedge", "Wedge")
    doc.recompute()
    top, bottom = wedge_edges(wedge)
    chamfer = body.newObject("PartDesign::Chamfer", "Chamfer")
    chamfer.Base = (wedge, [top, bottom])
    chamfer.Size = 1.0
    doc.recompute()
    wedge.X2min = 5
    wedge.X2max = 5
    doc.recompute()
    gdoc = FreeCADGui.getDocument(name)
    FreeCADGui.SendMsgToActiveView("ViewFit")
    pump()
    gdoc.setEdit(chamfer, 0)
    pump(80)
    widget = tree()
    if not check("the chamfer is in edit, its edge list shown",
                 gdoc.getInEdit() is not None and widget is not None):
        return
    before = rows(widget)
    missing = [t for t in before if t.startswith("?")]
    present = [t for t in before if not t.startswith("?")]
    note("INFO chamfer rows %s" % before)
    if not check("a chamfer's missing row", missing, before):
        return
    new = free_edge(wedge, set(present) | {m.lstrip("?") for m in missing})
    FreeCADGui.Selection.clearSelection()
    pump(20)
    r = row(widget, missing[0])
    widget.clearSelection()
    r.setSelected(True)
    pump(30)
    check("the chamfer's missing row selected from the list",
          selected_rows(widget) == [missing[0]], selected_rows(widget))
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(name, "Body", "Wedge." + new)
    pump(80)
    after = rows(widget)
    index = before.index(missing[0])
    check("a chamfer: the pick of an unused edge takes the missing one's place",
          len(after) == len(before) and after[index] == new
          and not any(t.startswith("?") for t in after), "%s -> %s" % (before, after))
    check("... and in its Base", new in base_subs(chamfer), base_subs(chamfer))
    gdoc.resetEdit()
    pump(40)


def finish():
    if state["done"]:
        return
    state["done"] = True
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").RemBool("PerViewEdit")
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
