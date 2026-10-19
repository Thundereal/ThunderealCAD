"""A red cross where a failed fillet edge's handle would be (entry 180).

The reporter 2026-10-19 13:22 and 13:27: "current failed fillet has no
handle. I want to show a red cross ... at the root where the handle would
have been ... fixed-size like handle ... not pickable". A 10 mm box, one of
its edges filleted with a radius it cannot take (20 mm):
  - the fillet fails on that edge (FailedEdges names it);
  - in edit, the gizmo container's marks hold one cross (an SoMarkerSet of
    one point, red), under an unpickable pick style;
  - the cross stands on the failed edge (within 1e-3 of it);
  - the radius made good (2 mm): no cross.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "FilletFailedCross"

state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def field_text(node, name):
    value = node.getField(name).get()
    return value.getString() if hasattr(value, "getString") else str(value)


def crosses():
    """The marker sets in the edit's on-top branch (a gizmo container's
    annotation hangs in the editing root by itself, its marks under it):
    [(points, colour, pick style)]. Coin's own types are named without the
    "So" ("MarkerSet", "Coordinate3")."""
    from pivy import coin

    view = FreeCADGui.getDocument(DOC).ActiveView
    root = view.getViewer().getSoRenderManager().getSceneGraph()
    search = coin.SoSearchAction()
    search.setType(coin.SoType.fromName("SoMarkerSet"))
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    found = []
    for p in search.getPaths():
        kinds = [p.getNode(i).getTypeId().getName().getString() for i in range(p.getLength())]
        if "SoFCEditingRoot" not in kinds:
            continue
        marks = p.getNodeFromTail(1)
        style = None
        points = []
        colour = None
        for i in range(marks.getNumChildren()):
            child = marks.getChild(i)
            kind = child.getTypeId().getName().getString()
            if kind == "PickStyle":
                style = field_text(child, "style")
            elif kind == "BaseColor":
                colour = coin.cast(child, "SoBaseColor").rgb[0].getValue()
            elif kind == "Coordinate3":
                c = coin.cast(child, "SoCoordinate3")
                points = [tuple(c.point[j].getValue()) for j in range(c.point.getNum())]
            elif kind == "MarkerSet":
                found.append((points, colour, style))
    return found


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1"])
        fillet.Radius = 20
        doc.recompute()
        note("INFO fillet State %s, FailedEdges %s" % (fillet.State, fillet.FailedEdges))
        check("the fillet fails on the edge", "Edge1" in fillet.FailedEdges, fillet.FailedEdges)

        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(fillet, 0)
        pump(120)
        check("the fillet is in edit", gdoc.getInEdit() is not None)
        found = crosses()
        note("INFO marks: %s" % found)
        check("one cross in the gizmos' marks", len(found) == 1 and len(found[0][0]) == 1, found)
        if found and found[0][0]:
            at = FreeCAD.Vector(*found[0][0][0])
            edge = box.Shape.getElement("Edge1")
            dist = edge.distToShape(Part.Vertex(at))[0]
            check("... red", found[0][1] is not None and found[0][1][0] > 0.9
                  and found[0][1][1] < 0.1, found[0][1])
            check("... not picked", found[0][2] in ("UNPICKABLE", "2"), found[0][2])
            check("... on the failed edge", dist < 1e-3, dist)

        # made good in the panel, as a user does: the panel's recompute is
        # what refreshes its handles and marks (finishedRecomputeFeature)
        box_ = None
        for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QAbstractSpinBox):
            if w.isVisible() and w.metaObject().className() == "Gui::QuantitySpinBox" \
                    and abs(w.property("rawValue") - 20.0) < 1e-6:
                box_ = w
                break
        if not check("the panel's radius box is found (20 mm)", box_ is not None):
            return
        box_.setProperty("rawValue", 2.0)
        pump(200)
        note("INFO after a good radius: FailedEdges %s, marks %s" % (fillet.FailedEdges, crosses()))
        check("the radius made good: no cross", crosses() == [], crosses())
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
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
