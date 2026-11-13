"""PartDesign panels show what they find again by name; they do not take it silently.

Ruled 2026-11-11 (docs/SheetMetal.md sec 12), after the dress-ups' and sheet
metal's panels: a reference whose element the feature it is of has no more is
looked up by its name's history when the feature's panel opens. What answers
is taken in the panel's transaction -- by a list every element that answers,
an edge cut in two as its two halves; by a reference that holds one element
only where one answers -- and shown: marked where the panel lists it, its
tip saying what it was; said in a line; selected in the view. OK keeps it,
Cancel leaves the reference missing.

Each case its own body: a box 40 x 20 x 10 and a pocket taking its corner at
x 0, y 0, which trims the top face and the top front edge. The pocket
suppressed, the face and the edge are the box's whole ones.

Claims:
  - a pad up to the top face: the face found, its cell marked, said and
    selected; Cancel leaves it missing; OK keeps it and the pad is made;
  - a draft whose neutral plane is the top face: its label marked, said in
    the panel's line, selected;
  - a pipe along the top front edge: the edge found, marked, said, selected;
  - a pipe along the top back edge of a second pocket, which is then moved
    to notch that edge in two: both halves taken, marked and said;
  - a pad up to the front face of a second pocket, which is then moved to
    slot that face through in two: the pad runs up to a list of faces, and
    takes both parts;
  - a draft whose neutral plane is that face: two faces answer for the one
    it holds, none is taken, nothing is said.
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
DOC = "PdRefound"
V = FreeCAD.Vector
AMBER = "#b07000"

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


def selected():
    """The elements selected, each as (the feature it is of, its name)"""
    res = []
    for s in FreeCADGui.Selection.getSelectionEx("", 0):
        for sub in s.SubElementNames:
            parts = sub.split(".")
            res.append((parts[-3] if len(parts) > 2 else parts[0], parts[-1]))
    return sorted(res)


def said():
    """What the panel's line says was found again"""
    for w in mw().findChildren(QtWidgets.QLabel):
        if w.isVisible() and "Found again by name" in w.text():
            return w.text()
    return ""


def cells():
    """The marked cells of the panel's tables: (text, tip)"""
    res = []
    for w in mw().findChildren(QtWidgets.QTableWidget):
        if not w.isVisible():
            continue
        for r in range(w.rowCount()):
            for c in range(w.columnCount()):
                item = w.item(r, c)
                if item and item.text() and item.foreground().color().name() == AMBER:
                    res.append((item.text(), item.toolTip()))
    return sorted(res)


def label(name):
    for w in mw().findChildren(QtWidgets.QLabel):
        if w.objectName() == name and w.isVisible():
            return w
    return None


def press(which):
    for w in mw().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            pump(60)
            return True
    return False


def rect(body, name, x0, y0, x1, y1, z):
    sketch = body.newObject("Sketcher::SketchObject", name)
    sketch.MapMode = "Deactivated"
    sketch.Placement = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation())
    pts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    for i in range(4):
        a, b = pts[i], pts[(i + 1) % 4]
        sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
    return sketch


def face_at(shape, p, n):
    for i, f in enumerate(shape.Faces):
        if (
            f.Surface.TypeId == "Part::GeomPlane"
            and f.distToShape(Part.Vertex(V(*p)))[0] < 1e-7
            and f.normalAt(0, 0).isEqual(V(*n), 1e-7)
        ):
            return "Face%d" % (i + 1)
    return None


def edge_on(shape, a, b):
    best, length = None, 0
    pa, pb = V(*a), V(*b)
    for i, e in enumerate(shape.Edges):
        if len(e.Vertexes) != 2:
            continue
        on = all((v.Point - pa).cross(pb - pa).Length < 1e-7 for v in e.Vertexes)
        if on and e.Length > length:
            best, length = "Edge%d" % (i + 1), e.Length
    return best


def edges_on(shape, y, z):
    return sorted(
        "Edge%d" % (i + 1)
        for i, e in enumerate(shape.Edges)
        if len(e.Vertexes) == 2
        and all(abs(v.Point.y - y) < 1e-7 and abs(v.Point.z - z) < 1e-7 for v in e.Vertexes)
    )


def stock(doc, name):
    """A body: the box, and the pocket taking its corner"""
    body = doc.addObject("PartDesign::Body", name)
    box = body.newObject("PartDesign::AdditiveBox", name + "Box")
    box.Length, box.Width, box.Height = 40, 20, 10
    doc.recompute()
    pocket = body.newObject("PartDesign::Pocket", name + "Pocket")
    pocket.Profile = rect(body, name + "Corner", -5, -5, 5, 5, 10)
    pocket.Length = 3
    doc.recompute()
    return body, pocket


def missing(feature, prop):
    return [s for s in getattr(feature, prop)[1] if s.startswith("?")]


def open_panel(gdoc, feature):
    FreeCADGui.Selection.clearSelection()
    gdoc.setEdit(feature.Name)
    pump(150)


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

        # -- a pad up to the top face ------------------------------------
        body, pocket = stock(doc, "A")
        top = face_at(pocket.Shape, (20, 10, 10), (0, 0, 1))
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = rect(body, "Above", 20, 8, 30, 14, 20)
        pad.Type = "UpToFace"
        pad.UpToFace = (pocket, [top])
        pad.Reversed = True
        doc.recompute()
        made = "Invalid" not in pad.State
        pocket.Suppress = True
        doc.recompute()
        pump(20)
        whole = face_at(pocket.Shape, (20, 10, 10), (0, 0, 1))
        if check(
            "the pocket suppressed, the pad up to the face it trimmed has lost the face",
            made and missing(pad, "UpToFace") == ["?" + top],
            (made, pad.State, pad.UpToFace),
        ):
            open_panel(gdoc, pad)
            check(
                "the pad's panel finds the face again by name",
                pad.UpToFace[1] == [whole],
                (pad.UpToFace, whole),
            )
            marked = cells()
            check(
                "its cell is marked, its tip saying what it was",
                len(marked) == 1 and marked[0][0] == whole and top in marked[0][1],
                marked,
            )
            check(
                "a line under the table says what was found",
                top in said() and whole in said(),
                said(),
            )
            check(
                "the face is the selection, on the pocket",
                selected() == [(pocket.Name, whole)],
                selected(),
            )
            press(QtWidgets.QDialogButtonBox.Cancel)
            pump(60)
            doc.recompute()
            check(
                "Cancel leaves the face missing",
                missing(pad, "UpToFace") == ["?" + top],
                pad.UpToFace,
            )
            open_panel(gdoc, pad)
            press(QtWidgets.QDialogButtonBox.Ok)
            pump(60)
            doc.recompute()
            check(
                "OK keeps it, and the pad is made",
                pad.UpToFace[1] == [whole] and "Invalid" not in pad.State,
                (pad.UpToFace, pad.State, pad.getStatusString()),
            )

        # -- a draft, its neutral plane the top face -----------------------
        body, pocket = stock(doc, "B")
        top = face_at(pocket.Shape, (20, 10, 10), (0, 0, 1))
        draft = body.newObject("PartDesign::Draft", "Draft")
        draft.Base = (pocket, [face_at(pocket.Shape, (40, 10, 5), (1, 0, 0))])
        draft.NeutralPlane = (pocket, [top])
        draft.Angle = 5
        doc.recompute()
        made = "Invalid" not in draft.State
        pocket.Suppress = True
        doc.recompute()
        pump(20)
        whole = face_at(pocket.Shape, (20, 10, 10), (0, 0, 1))
        if check(
            "a draft's neutral plane, the face the suppressed pocket trimmed, is lost",
            made and missing(draft, "NeutralPlane") == ["?" + top],
            (made, draft.State, draft.NeutralPlane),
        ):
            open_panel(gdoc, draft)
            plane = label("linePlane")
            check(
                "the draft's panel finds the plane again: its label marked, its tip saying "
                "what it was",
                draft.NeutralPlane[1] == [whole]
                and plane is not None
                and AMBER in plane.text()
                and whole in plane.text()
                and top in plane.toolTip(),
                (draft.NeutralPlane, plane and (plane.text(), plane.toolTip())),
            )
            check(
                "the panel's line says it, and the plane is selected",
                top in said() and whole in said() and (pocket.Name, whole) in selected(),
                (said(), selected()),
            )
            press(QtWidgets.QDialogButtonBox.Cancel)
            pump(60)
            doc.recompute()
            check(
                "Cancel leaves the plane missing",
                missing(draft, "NeutralPlane") == ["?" + top],
                draft.NeutralPlane,
            )

        # -- a pipe along the top front edge -------------------------------
        body, pocket = stock(doc, "C")
        spine = edge_on(pocket.Shape, (5, 0, 10), (40, 0, 10))
        circle = body.newObject("Sketcher::SketchObject", "Circle")
        circle.MapMode = "Deactivated"
        circle.Placement = FreeCAD.Placement(V(5, 0, 10), FreeCAD.Rotation(V(0, 1, 0), 90))
        circle.addGeometry(Part.Circle(V(0, 0, 0), V(0, 0, 1), 1))
        pipe = body.newObject("PartDesign::AdditivePipe", "Pipe")
        pipe.Profile = circle
        pipe.Spine = (pocket, [spine])
        doc.recompute()
        made = "Invalid" not in pipe.State
        pocket.Suppress = True
        doc.recompute()
        pump(20)
        whole = edge_on(pocket.Shape, (0, 0, 10), (40, 0, 10))
        if check(
            "a pipe's spine, the edge the suppressed pocket trimmed, is lost",
            made and missing(pipe, "Spine") == ["?" + spine],
            (made, pipe.State, pipe.Spine),
        ):
            open_panel(gdoc, pipe)
            marked = cells()
            check(
                "the pipe's panel finds the edge again: marked, said and selected",
                pipe.Spine[1] == [whole]
                and len(marked) == 1
                and marked[0][0] == whole
                and spine in marked[0][1]
                and spine in said()
                and selected() == [(pocket.Name, whole)],
                (pipe.Spine, marked, said(), selected()),
            )
            press(QtWidgets.QDialogButtonBox.Cancel)
            pump(60)
            doc.recompute()

        # -- a spine cut in two: both halves --------------------------------
        # a second pocket, at the far front corner, clear of the top back
        # edge; then moved to notch that edge's middle
        body, pocket = stock(doc, "D")
        second = body.newObject("PartDesign::Pocket", "DSecond")
        moved = rect(body, "DMoved", -5, -5, 5, 5, 10)
        moved.Placement = FreeCAD.Placement(V(40, 0, 10), FreeCAD.Rotation())
        second.Profile = moved
        second.Length = 3
        doc.recompute()
        back = edge_on(second.Shape, (0, 20, 10), (40, 20, 10))
        circle = body.newObject("Sketcher::SketchObject", "Circle2")
        circle.MapMode = "Deactivated"
        circle.Placement = FreeCAD.Placement(V(0, 20, 10), FreeCAD.Rotation(V(0, 1, 0), 90))
        circle.addGeometry(Part.Circle(V(0, 0, 0), V(0, 0, 1), 1))
        pipe = body.newObject("PartDesign::AdditivePipe", "Pipe2")
        pipe.Profile = circle
        pipe.Spine = (second, [back])
        doc.recompute()
        made = "Invalid" not in pipe.State
        moved.Placement = FreeCAD.Placement(V(20, 20, 10), FreeCAD.Rotation())
        doc.recompute()
        pump(20)
        halves = edges_on(second.Shape, 20, 10)
        if check(
            "the pocket a pipe's spine is on moved to notch the spine in two: the spine is lost",
            made and len(halves) == 2 and len(missing(pipe, "Spine")) == 1,
            (made, halves, pipe.State, pipe.Spine),
        ):
            open_panel(gdoc, pipe)
            marked = cells()
            check(
                "a list takes both halves: marked, and said as the one now the two",
                sorted(pipe.Spine[1]) == halves
                and sorted(m[0] for m in marked) == halves
                and all(h in said() for h in halves),
                (pipe.Spine, marked, said()),
            )
            press(QtWidgets.QDialogButtonBox.Cancel)
            pump(60)
            doc.recompute()

        # -- a face cut in two, of a list: both parts -----------------------
        # a pad from in front of the box up to its front face; the second
        # pocket, clear of that face, then moved to slot it through in two
        body, pocket = stock(doc, "E")
        second = body.newObject("PartDesign::Pocket", "ESecond")
        moved = rect(body, "EMoved", -2, -5, 2, 5, 10)
        moved.Placement = FreeCAD.Placement(V(38, 20, 10), FreeCAD.Rotation())
        second.Profile = moved
        second.Length = 12
        doc.recompute()
        front = face_at(second.Shape, (25, 0, 5), (0, -1, 0))
        before = body.newObject("Sketcher::SketchObject", "InFront")
        before.MapMode = "Deactivated"
        before.Placement = FreeCAD.Placement(V(0, -10, 0), FreeCAD.Rotation(V(1, 0, 0), 90))
        pts = [(8, 2), (14, 2), (14, 8), (8, 8)]
        for k in range(4):
            a, b = pts[k], pts[(k + 1) % 4]
            before.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        pad = body.newObject("PartDesign::Pad", "Pad2")
        pad.Profile = before
        pad.Type = "UpToFace"
        pad.UpToFace = (second, [front])
        doc.recompute()
        if "Invalid" in pad.State:
            pad.Reversed = True
            doc.recompute()
        made = "Invalid" not in pad.State and front is not None
        moved.Placement = FreeCAD.Placement(V(20, 0, 10), FreeCAD.Rotation())
        doc.recompute()
        pump(20)
        fronts = sorted(
            "Face%d" % (k + 1)
            for k, f in enumerate(second.Shape.Faces)
            if f.Surface.TypeId == "Part::GeomPlane"
            and abs(f.BoundBox.YMax) < 1e-7
            and abs(f.BoundBox.YMin) < 1e-7
        )
        if check(
            "the pocket moved to slot the face a pad runs up to in two: the face is lost",
            made and len(fronts) == 2 and len(missing(pad, "UpToFace")) == 1,
            (made, fronts, pad.State, pad.getStatusString(), pad.UpToFace),
        ):
            open_panel(gdoc, pad)
            marked = cells()
            upto = sorted(sub for obj, subs in pad.UpToShape for sub in subs)
            check(
                "the pad runs up to a list: both parts of the face taken, marked and said",
                upto == fronts
                and sorted(m[0] for m in marked) == fronts
                and all(f in said() for f in fronts),
                (pad.UpToShape, marked, said()),
            )
            press(QtWidgets.QDialogButtonBox.Cancel)
            pump(60)
            doc.recompute()

        # -- one face it holds, two that answer: none taken ----------------
        # a draft's neutral plane, the front face, slotted in two the same way
        body, pocket = stock(doc, "F")
        second = body.newObject("PartDesign::Pocket", "FSecond")
        moved = rect(body, "FMoved", -2, -5, 2, 5, 10)
        moved.Placement = FreeCAD.Placement(V(38, 20, 10), FreeCAD.Rotation())
        second.Profile = moved
        second.Length = 12
        doc.recompute()
        front = face_at(second.Shape, (25, 0, 5), (0, -1, 0))
        draft = body.newObject("PartDesign::Draft", "Draft2")
        draft.Base = (second, [face_at(second.Shape, (0, 10, 5), (-1, 0, 0))])
        draft.NeutralPlane = (second, [front])
        draft.Angle = 5
        doc.recompute()
        made = "Invalid" not in draft.State and front is not None
        moved.Placement = FreeCAD.Placement(V(20, 0, 10), FreeCAD.Rotation())
        doc.recompute()
        pump(20)
        if check(
            "the pocket moved to slot a draft's neutral plane in two: the plane is lost",
            made and len(missing(draft, "NeutralPlane")) == 1,
            (made, draft.State, draft.getStatusString(), draft.NeutralPlane),
        ):
            held = draft.NeutralPlane[1]
            open_panel(gdoc, draft)
            plane = label("linePlane")
            check(
                "two faces answer for the one plane it holds: none is taken, its label "
                "unmarked, nothing said or selected",
                draft.NeutralPlane[1] == held
                and plane is not None
                and AMBER not in plane.text()
                and said() == ""
                and selected() == [],
                (draft.NeutralPlane, plane and plane.text(), said(), selected()),
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
