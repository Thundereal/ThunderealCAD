"""A dress-up's reference found again by name when its panel opens is shown, not taken silently.

Ruled 2026-11-11 (docs/SheetMetal.md sec 12): a reference whose element the
base has no more is searched for by geometry in a recompute, alone, which is
safe; an edge a feature before the dress-up has trimmed is another shape, so
the reference is marked missing and the dress-up is in error. Its panel,
opened to edit, looks the name up by its history and takes what answers,
all of it -- an edge a feature cut in two is its two halves -- and not
silently: each row is marked and says what it was, the elements are selected
in the view, and the panel says so in a line; OK keeps it, Cancel leaves the
reference as it was.

A box 40 x 20 x 10; a pocket taking its corner at x 0, so the top front edge
runs from x 5; a fillet on that edge. The pocket suppressed, the edge is the
box's whole one: the fillet's reference is missing. Then a fillet on the top
back edge, and a pocket put in before it that notches the edge's middle: two
edges answer for the one, and both are taken, and shown.

Claims:
  - the pocket suppressed, the fillet is in error, its reference missing;
  - opened, its panel finds the edge: no "?" row, the fillet made;
  - the row is marked, its tip saying what it was;
  - the panel's line says what was found;
  - the edge is the selection, on the feature the fillet builds on;
  - Cancel leaves the reference missing;
  - opened again and OK: the reference kept, the fillet made;
  - where two edges answer, both are rows, marked, said and selected.
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
DOC = "DressUpRefound"
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
    """The longest edge of shape on the line from a to b"""
    best, length = None, 0
    pa, pb = V(*a), V(*b)
    for i, e in enumerate(shape.Edges):
        if len(e.Vertexes) != 2:
            continue
        if all((v.Point - pa).cross(pb - pa).Length < 1e-7 for v in e.Vertexes) and e.Length > length:
            best, length = "Edge%d" % (i + 1), e.Length
    return best


def rect(body, name, x0, y0, x1, y1, z):
    sketch = body.newObject("Sketcher::SketchObject", name)
    sketch.MapMode = "Deactivated"
    sketch.Placement = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation())
    pts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    for i in range(4):
        a, b = pts[i], pts[(i + 1) % 4]
        sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
    return sketch


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0
        )
        mw().showNormal()
        mw().setGeometry(QtGui.QGuiApplication.primaryScreen().availableGeometry())
        pump(30)
        doc = FreeCAD.newDocument(DOC)
        doc.UndoMode = 1
        gdoc = FreeCADGui.getDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length, box.Width, box.Height = 40, 20, 10
        doc.recompute()
        pocket = body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = rect(body, "Corner", -5, -5, 5, 5, 10)
        pocket.Length = 3
        doc.recompute()
        edge = edge_on(pocket.Shape, (5, 0, 10), (40, 0, 10))
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (pocket, [edge])
        fillet.Radius = 1
        doc.recompute()
        made = "Invalid" not in fillet.State and edge is not None
        pocket.Suppress = True
        doc.recompute()
        pump(20)
        whole = edge_on(pocket.Shape, (0, 0, 10), (40, 0, 10))
        if not check(
            "the pocket suppressed, the fillet on the edge it trimmed is in error, its "
            "reference missing",
            made and "Invalid" in fillet.State and fillet.Base[1][0].startswith("?"),
            (made, fillet.State, fillet.Base[1]),
        ):
            return finish()

        FreeCADGui.Selection.clearSelection()
        gdoc.setEdit(fillet.Name)
        pump(120)
        items = rows(tree())
        row = items[0] if len(items) == 1 else None
        check(
            "its panel finds the edge again by name: no missing row, the fillet made",
            row is not None
            and row.text(0) == whole
            and fillet.Base[1] == [whole]
            and "Invalid" not in fillet.State,
            ([i.text(0) for i in items], whole, fillet.Base[1], fillet.State),
        )
        check(
            "the row is marked, its tip saying what it was",
            row is not None and edge in row.toolTip(0) and "found again" in row.toolTip(0),
            row and row.toolTip(0),
        )
        said = message()
        check(
            "the panel says what it found",
            "Found again by name" in said and edge in said and (whole or "?") in said,
            said,
        )
        picked = selection()
        subs = picked[0][1] if len(picked) == 1 else []
        check(
            "the edge is the selection, on the feature the fillet builds on",
            len(picked) == 1
            and picked[0][0] == "Body"
            and len(subs) == 1
            and subs[0].startswith("Pocket.")
            and subs[0].endswith("." + whole),
            picked,
        )
        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(60)
        doc.recompute()
        check(
            "Cancel leaves the reference as it was, missing",
            fillet.Base[1][0].startswith("?") and "Invalid" in fillet.State,
            (fillet.Base[1], fillet.State),
        )
        gdoc.setEdit(fillet.Name)
        pump(120)
        press(QtWidgets.QDialogButtonBox.Ok)
        pump(60)
        doc.recompute()
        check(
            "opened again and OK: the reference kept, the fillet made",
            fillet.Base[1] == [whole] and "Invalid" not in fillet.State,
            (fillet.Base[1], fillet.State),
        )

        # two edges for one: a pocket put in before a fillet, notching the
        # middle of its edge
        back = edge_on(fillet.Shape, (0, 20, 10), (40, 20, 10))
        second = body.newObject("PartDesign::Fillet", "Second")
        second.Base = (fillet, [back])
        second.Radius = 1
        doc.recompute()
        made = "Invalid" not in second.State and back is not None
        notch = doc.addObject("PartDesign::Pocket", "Notch")
        middle = rect(body, "Middle", 15, 15, 25, 25, 10)
        body.insertObject(notch, fillet, True)
        notch.Profile = middle
        notch.Length = 3
        doc.recompute()
        pump(20)
        if not check(
            "a pocket put in before a fillet, notching its edge in two: the fillet in error",
            made and "Invalid" in second.State and second.Base[1][0].startswith("?"),
            (made, second.State, second.Base[1]),
        ):
            return finish()
        FreeCADGui.Selection.clearSelection()
        gdoc.setEdit(second.Name)
        pump(120)
        items = rows(tree())
        names = [i.text(0) for i in items]
        halves = sorted(
            "Edge%d" % (i + 1)
            for i, e in enumerate(notch.Shape.Edges)
            if len(e.Vertexes) == 2
            and all(
                abs(v.Point.y - 20) < 1e-7 and abs(v.Point.z - 10) < 1e-7 for v in e.Vertexes
            )
        )
        was = back
        said = message()
        picked = selection()
        subs = sorted(sub.rsplit(".", 1)[-1] for sub in (picked[0][1] if len(picked) == 1 else []))
        check(
            "two edges answer for the one, the edge's two halves: both taken, the fillet made",
            len(halves) == 2 and sorted(names) == halves and "Invalid" not in second.State,
            (names, halves, second.State),
        )
        check(
            "both rows are marked, saying what they were",
            len(items) == 2 and all("found again" in i.toolTip(0) for i in items),
            [i.toolTip(0) for i in items],
        )
        check(
            "the panel says the one is now the two, and both are the selection",
            "Found again by name" in said
            and all(h in said for h in halves)
            and subs == halves,
            (said, picked, was),
        )
        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(60)
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
