"""A red cross where a failed fillet edge's handle would be (entry 180).

The reporter 2026-10-19 13:22 and 13:27: "current failed fillet has no
handle. I want to show a red cross ... at the root where the handle would
have been ... fixed-size like handle ... not pickable". A 10 mm box, one of
its edges filleted with a radius it cannot take (20 mm):
  - the fillet fails on that edge (FailedEdges names it);
  - in edit, the gizmo container's marks hold one cross (two red bars as
    thick as a handle's shaft, 15:08), under an unpickable pick style;
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
    """The crosses in the edit's on-top branch (a gizmo container's
    annotation hangs in the editing root by itself, its marks under it):
    each two bars (cubes) under a separator with its transform and colour,
    the marks' pick style first. [([position], colour, pick style, bar
    thickness)]. Coin's own types are named without the "So"."""
    from pivy import coin

    view = FreeCADGui.getDocument(DOC).ActiveView
    root = view.getViewer().getSoRenderManager().getSceneGraph()
    search = coin.SoSearchAction()
    search.setType(coin.SoType.fromName("SoCube"))
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    seen = {}
    for p in search.getPaths():
        kinds = [p.getNode(i).getTypeId().getName().getString() for i in range(p.getLength())]
        if "SoFCEditingRoot" not in kinds or p.getLength() < 4:
            continue
        cross = p.getNodeFromTail(2)
        marks = p.getNodeFromTail(3)
        key = int(cross.this) if hasattr(cross, "this") else id(cross)
        if key in seen:
            continue
        cube = coin.cast(p.getTail(), "SoCube")
        style = None
        if marks.getNumChildren() and marks.getChild(0).getTypeId().getName().getString() == "PickStyle":
            style = field_text(marks.getChild(0), "style")
        position = None
        colour = None
        for i in range(cross.getNumChildren()):
            child = cross.getChild(i)
            kind = child.getTypeId().getName().getString()
            if kind == "Transform":
                position = tuple(coin.cast(child, "SoTransform").translation.getValue().getValue())
            elif kind == "BaseColor":
                colour = coin.cast(child, "SoBaseColor").rgb[0].getValue()
        seen[key] = ([position] if position else [], colour, style, cube.height.getValue())
    return list(seen.values())


def selection_pixels(name):
    """How many pixels of a picture of the view are near the selection
    colour (View/SelectionColor)"""
    from PySide import QtGui

    path = os.path.join(OUT, name)
    FreeCADGui.getDocument(DOC).ActiveView.saveRenderDump(path, metadata=False)
    packed = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").GetUnsigned(
        "SelectionColor", 0x1CAD1C00)
    r, g, b = (packed >> 24) & 0xFF, (packed >> 16) & 0xFF, (packed >> 8) & 0xFF
    img = QtGui.QImage(path)
    count = 0
    for y in range(0, img.height(), 2):
        for x in range(0, img.width(), 2):
            c = QtGui.QColor(img.pixel(x, y))
            if abs(c.red() - r) < 40 and abs(c.green() - g) < 40 and abs(c.blue() - b) < 40:
                count += 1
    return count


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
        # made once, so that the failure keeps the face it last made (the
        # preview's red face, which entry 182 selects), then failed
        fillet.Radius = 2
        doc.recompute()
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
            # as thick as a linear handle's shaft (SoArrowGeometry radius 0.2)
            check("... its bars as thick as a handle's shaft (0.4)",
                  abs(found[0][3] - 0.4) < 1e-6, found[0][3])
            # a picture: the cross beside the edge's own arrow
            try:
                FreeCADGui.getDocument(DOC).ActiveView.saveRenderDump(
                    os.path.join(OUT, "cross.png"), metadata=False)
            except Exception as e:
                note("INFO no picture: %r" % e)

        # Entry 182: a selection that names the failed edge's preview face
        # itself -- as a click on that face makes it (the reporter 13:49,
        # 15:16; the row of a present edge selects the base's edge, which is
        # fine) -- shows the face in the selection colour. Failed faces come
        # after the made ones in the preview's shape.
        faces = len(fillet.DressUpShape.Faces)
        note("INFO preview faces %d" % faces)
        if check("the preview has the failed edge's face", faces > 0):
            FreeCADGui.Selection.clearSelection()
            pump(20)
            before = selection_pixels("before.png")
            FreeCADGui.Selection.addSelection(DOC, "Body", "Fillet.;AddSub;Face%d" % faces)
            pump(80)
            sel = [(s.ObjectName, list(s.SubElementNames))
                   for s in FreeCADGui.Selection.getSelectionEx("", 0)]
            after = selection_pixels("selected.png")
            note("INFO selection %s; selection-coloured pixels %d before, %d after"
                 % (sel, before, after))
            check("... selected (as the base's edge it stands for), the preview face shows "
                  "the selection colour", after > before + 50, (before, after))
            FreeCADGui.Selection.clearSelection()
            pump(40)
            gone = selection_pixels("cleared.png")
            check("... and not once the selection is cleared", gone <= before + 10, (before, gone))

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
        # 182's other half (the reporter 13:49): a WORKING edge selected --
        # its face's click selects the base's edge -- leaves the preview face
        # unlit; only the edge's own line shows selected (a few pixels, far
        # fewer than a lit face's 270 or so)
        FreeCADGui.Selection.clearSelection()
        pump(20)
        base_ = selection_pixels("working-before.png")
        FreeCADGui.Selection.addSelection(DOC, "Body", "Box.Edge1")
        pump(80)
        lit = selection_pixels("working-selected.png")
        note("INFO a working edge selected: selection-coloured pixels %d before, %d after"
             % (base_, lit))
        check("a working edge selected: its preview face is not lit", lit - base_ < 120,
              (base_, lit))
        FreeCADGui.Selection.clearSelection()
        pump(20)
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
