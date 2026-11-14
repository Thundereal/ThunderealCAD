"""A dress-up's reference whose name has become another element's is taken back in its panel, and shown.

docs/TopoNamingEnhance.md sec 9.12, the guard. A reference goes by its
element's name, and a name that holds a place can be another element's after
a recompute: the reference moves and the dress-up is made on the other
element, with no error. In the recompute the element the reference was on is
looked for by its geometry; where it is still there, unchanged, the
reference is known to have been moved off it. Nothing is changed then (a
shape that changed is no ground for moving a reference unseen, ruled
2026-11-11). The dress-up's panel, opened to edit, takes the element the
reference was on, and not silently: the row is marked and says what it was,
the element is selected in the view, and the panel says so in a line; OK
keeps it, Cancel leaves the reference as it was.

A body on a box whose edges are named by a history given by hand; a fillet
on its top front edge. The box is made again with the names of that edge and
the top back edge the other way round.

Claims:
  - the names changed round, the fillet is made on the other edge, with no
    error, and the fillet says where the edge it was on is;
  - opened, its panel lists the edge the reference was on, the fillet made
    on it;
  - the row is marked, its tip saying what it was;
  - the panel's line says what was taken;
  - the edge is the selection;
  - Cancel leaves the reference on the other edge;
  - opened again and OK: the reference kept on the edge it was on, and
    nothing said of it any more.
"""

import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "DressUpMoved"
V = FreeCAD.Vector

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


def selection():
    return [
        (s.ObjectName, list(s.SubElementNames)) for s in FreeCADGui.Selection.getSelectionEx("", 0)
    ]


def tree():
    for w in mw().findChildren(QtWidgets.QTreeWidget):
        if w.objectName() == "treeWidgetReferences" and w.isVisible():
            return w
    return None


def message():
    for w in mw().findChildren(QtWidgets.QLabel):
        if w.objectName() == "message" and w.isVisible():
            return w.text()
    return ""


def rows(widget):
    return [widget.topLevelItem(i) for i in range(widget.topLevelItemCount())] if widget else []


def press(which):
    for w in mw().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            pump(60)
            return True
    return False


def edge_on(shape, a, b):
    """The edge of shape from a to b"""
    pa, pb = V(*a), V(*b)
    for i, e in enumerate(shape.Edges):
        if len(e.Vertexes) != 2:
            continue
        ends = [v.Point for v in e.Vertexes]
        if all(min((p - q).Length for q in (pa, pb)) < 1e-7 for p in ends):
            return "Edge%d" % (i + 1)
    return None


def edge_index(shape, a, b):
    return int(edge_on(shape, a, b)[4:]) - 1


def filleted_at(shape, y):
    """Whether the top edge at y is rounded off: no sharp edge left there"""
    return edge_on(shape, (0, y, 10), (40, y, 10)) is None


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        mw().showNormal()
        mw().setGeometry(QtGui.QGuiApplication.primaryScreen().availableGeometry())
        pump(30)
        doc = FreeCAD.newDocument(DOC)
        doc.UndoMode = 1
        gdoc = FreeCADGui.getDocument(DOC)

        box = Part.makeBox(40, 20, 10)
        sources = []
        for i, e in enumerate(box.Edges):
            e.Tag = 100 + i
            sources.append(e)
        front = edge_index(box, (0, 0, 10), (40, 0, 10))
        back = edge_index(box, (0, 20, 10), (40, 20, 10))

        def named(swap):
            """A copy of the box, its edges named after the box's; with swap,
            the names of the top front and the top back edge changed round"""
            shape = box.copy()
            order = list(range(len(sources)))
            if swap:
                order[front], order[back] = order[back], order[front]
            shape.mapShapes([], [(sources[i], [shape.Edges[j]]) for i, j in enumerate(order)])
            return shape

        solid = doc.addObject("Part::Feature", "Solid")
        solid.Shape = named(False)
        body = doc.addObject("PartDesign::Body", "Body")
        body.BaseFeature = solid
        doc.recompute()
        base = body.Group[0]
        frontEdge = edge_on(base.Shape, (0, 0, 10), (40, 0, 10))
        backEdge = edge_on(base.Shape, (0, 20, 10), (40, 20, 10))
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (base, [frontEdge])
        fillet.Radius = 2
        doc.recompute()
        made = (
            "Invalid" not in fillet.State
            and filleted_at(fillet.Shape, 0)
            and not filleted_at(fillet.Shape, 20)
        )

        solid.Shape = named(True)
        doc.recompute()
        pump(20)
        told = [(p, o.Name, e, w) for p, o, e, w in fillet.getReferencesElsewhere()]
        if not check(
            "the names changed round, the fillet is made on the other edge with no error, "
            "and says where the edge it was on is",
            made
            and "Invalid" not in fillet.State
            and fillet.Base[1] == [backEdge]
            and filleted_at(fillet.Shape, 20)
            and not filleted_at(fillet.Shape, 0)
            and told == [("Base", base.Name, backEdge, frontEdge)],
            (made, fillet.State, fillet.Base[1], frontEdge, backEdge, told),
        ):
            return finish()

        FreeCADGui.Selection.clearSelection()
        gdoc.setEdit(fillet.Name)
        pump(120)
        items = rows(tree())
        row = items[0] if len(items) == 1 else None
        check(
            "its panel lists the edge the reference was on, the fillet made on it",
            row is not None
            and row.text(0) == frontEdge
            and fillet.Base[1] == [frontEdge]
            and "Invalid" not in fillet.State
            and filleted_at(fillet.Shape, 0)
            and not filleted_at(fillet.Shape, 20),
            ([i.text(0) for i in items], frontEdge, fillet.Base[1], fillet.State),
        )
        check(
            "the row is marked, its tip saying what it was",
            row is not None and backEdge in row.toolTip(0) and "found where" in row.toolTip(0),
            row and row.toolTip(0),
        )
        said = message()
        check(
            "the panel says what it took",
            "another element" in said and frontEdge in said and backEdge in said,
            said,
        )
        picked = selection()
        subs = picked[0][1] if len(picked) == 1 else []
        check(
            "the edge is the selection, on the feature the fillet builds on",
            len(picked) == 1 and len(subs) == 1 and subs[0].endswith("." + frontEdge),
            picked,
        )
        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(60)
        doc.recompute()
        check(
            "Cancel leaves the reference as it was, on the other edge",
            fillet.Base[1] == [backEdge] and filleted_at(fillet.Shape, 20),
            (fillet.Base[1], fillet.State),
        )
        gdoc.setEdit(fillet.Name)
        pump(120)
        press(QtWidgets.QDialogButtonBox.Ok)
        pump(60)
        doc.recompute()
        check(
            "opened again and OK: the reference kept on the edge it was on, and nothing "
            "said of it any more",
            fillet.Base[1] == [frontEdge]
            and "Invalid" not in fillet.State
            and filleted_at(fillet.Shape, 0)
            and fillet.getReferencesElsewhere() == [],
            (fillet.Base[1], fillet.State, fillet.getReferencesElsewhere()),
        )
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
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
