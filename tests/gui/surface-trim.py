"""The Surface workbench's cutting commands and the Trim panel
(docs/Surface.md sec 5.2, 8, U2).

  - Extend edges, Split, Trim and Untrim sit on the Surface tool bar;
  - Trim, run on a face and a curve picked after it, makes a trim of the one
    by the other and opens its panel, which shows every piece of the cut and
    has the sides to keep picked in the view: a click keeps a piece, a
    second click lets it go, and the panel counts them;
  - OK keeps the side picked, as one undo step; Cancel puts it back;
  - Split keeps every piece; Untrim gives the trimmed face its natural
    boundary back; Extend edges carries a face on across an edge picked;
  - a selection a command cannot use is said, never in a dialog.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import math
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SurfaceTrim"
V = FreeCAD.Vector
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds=0.3):
    import time

    end = time.monotonic() + seconds
    while True:
        QtCore.QCoreApplication.processEvents()
        if time.monotonic() >= end:
            break
        time.sleep(0.01)


def panel():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget, "SurfaceGui_Trim"):
        if w.isVisible():
            return w
    return None


def child(cls, name):
    p = panel()
    return p.findChild(cls, name) if p is not None else None


def press(which):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            settle(0.4)
            return True
    return False


def items(name):
    w = child(QtWidgets.QListWidget, name)
    return [w.item(i).text() for i in range(w.count())] if w is not None else None


def wave(u, v):
    x, y = 20 * u, 12 * v
    return V(x, y, 2 * math.sin(x / 4) * math.cos(y / 5))


def pieceAt(shape, point):
    best = min(
        range(len(shape.Faces)), key=lambda i: shape.Faces[i].distToShape(Part.Vertex(point))[0]
    )
    return "Face%d" % (best + 1)


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        settle(0.5)
        bar = mw.findChild(QtWidgets.QToolBar, "Surface")
        names = [a.objectName() for a in bar.actions()] if bar else []
        check(
            "Extend edges, Split, Trim and Untrim are on the Surface tool bar",
            all(
                n in names
                for n in ("Surface_ExtendEdges", "Surface_Split", "Surface_Trim", "Surface_Untrim")
            ),
            names,
        )

        doc = FreeCAD.newDocument(DOC)
        gdoc = FreeCADGui.getDocument(DOC)
        s = Part.BSplineSurface()
        s.interpolate([[wave(i / 8, j / 6) for j in range(7)] for i in range(9)])
        src = doc.addObject("Part::Feature", "Wave")
        src.Shape = s.toShape()
        ln = doc.addObject("Part::Line", "Cut")
        ln.X1, ln.Y1, ln.Z1 = 10, -2, 6
        ln.X2, ln.Y2, ln.Z2 = 10, 14, 6
        doc.recompute()
        undos = len(doc.UndoNames)

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(src, "Face1")
        FreeCADGui.runCommand("Surface_Split")
        settle(0.3)
        check(
            "Split with one thing picked makes nothing, and opens no dialog",
            not [o for o in doc.Objects if o.TypeId == "Surface::Split"]
            and QtWidgets.QApplication.activeModalWidget() is None,
        )
        FreeCADGui.Selection.addSelection(ln, "Edge1")
        FreeCADGui.runCommand("Surface_Split")
        settle(0.5)
        splits = [o for o in doc.Objects if o.TypeId == "Surface::Split"]
        check(
            "Split on a face and a curve keeps both pieces",
            len(splits) == 1 and splits[0].isValid() and len(splits[0].Shape.Faces) == 2,
            [(o.Name, o.getStatusString()) for o in splits],
        )

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(src, "Face1")
        FreeCADGui.Selection.addSelection(ln, "Edge1")
        FreeCADGui.runCommand("Surface_Trim")
        settle(0.6)
        trims = [o for o in doc.Objects if o.TypeId == "Surface::Trim"]
        if not check("Trim made from a face and a curve", len(trims) == 1, len(trims)):
            return finish()
        t = trims[0]
        check(
            "the face is its base and the curve its tool",
            t.Base[0][0] == src and t.Tools[0][0] == ln,
            (t.Base, t.Tools),
        )
        if not check("its panel opens", panel() is not None, gdoc.getInEdit()):
            return finish()
        keep = child(QtWidgets.QToolButton, "buttonKeep")
        label = child(QtWidgets.QLabel, "labelKept")
        check(
            "with nothing kept, it is picking, and shows every piece",
            keep.isChecked() and len(t.Shape.Faces) == 2,
            (keep.isChecked(), len(t.Shape.Faces)),
        )
        check("and counts them", label.text() == "0 of 2 pieces kept", label.text())

        p = V(3, 6, 0)
        FreeCADGui.Selection.addSelection(t, pieceAt(t.Shape, p), p.x, p.y, p.z)
        settle(0.4)
        check(
            "a click keeps the piece",
            len(t.KeepPoints) == 1 and label.text() == "1 of 2 pieces kept",
            (t.KeepPoints, label.text()),
        )
        FreeCADGui.Selection.addSelection(t, pieceAt(t.Shape, p), p.x, p.y + 1, p.z)
        settle(0.4)
        check(
            "a second click lets it go",
            len(t.KeepPoints) == 0 and label.text() == "0 of 2 pieces kept",
            (t.KeepPoints, label.text()),
        )
        FreeCADGui.Selection.addSelection(t, pieceAt(t.Shape, p), p.x, p.y, p.z)
        settle(0.4)
        child(QtWidgets.QDoubleSpinBox, "directionZ").setValue(-1)
        settle(0.4)
        check("a direction set is the feature's", t.Direction == V(0, 0, -1), t.Direction)
        check(
            "OK closes the panel",
            press(QtWidgets.QDialogButtonBox.Ok) and gdoc.getInEdit() is None,
            gdoc.getInEdit(),
        )
        bb = t.Shape.optimalBoundingBox(False, False)
        check(
            "and keeps the side picked",
            t.isValid() and len(t.Shape.Faces) == 1 and abs(bb.XMax - 10) < 1e-4 and bb.XMin < 1e-6,
            (t.getStatusString(), bb),
        )
        check("as one undo step", len(doc.UndoNames) == undos + 2, doc.UndoNames[:4])

        gdoc.setEdit(t, 0)
        settle(0.5)
        check(
            "editing it again does not start picking",
            panel() is not None and not child(QtWidgets.QToolButton, "buttonKeep").isChecked(),
        )
        child(QtWidgets.QToolButton, "buttonClear").click()
        settle(0.3)
        check(
            "Clear lets every side go, which is said in the panel",
            len(t.KeepPoints) == 0 and child(QtWidgets.QLabel, "labelStatus").text() != "",
            child(QtWidgets.QLabel, "labelStatus").text(),
        )
        check(
            "Cancel puts it back",
            press(QtWidgets.QDialogButtonBox.Cancel)
            and gdoc.getInEdit() is None
            and len(t.KeepPoints) == 1,
            t.KeepPoints,
        )
        doc.recompute()

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(t, "Face1")
        FreeCADGui.runCommand("Surface_Untrim")
        settle(0.5)
        un = [o for o in doc.Objects if o.TypeId == "Surface::Untrim"]
        check(
            "Untrim gives the face its natural boundary back",
            len(un) == 1 and un[0].isValid() and abs(un[0].Shape.Area - src.Shape.Area) < 1e-4,
            [(o.getStatusString(), o.Shape.Area) for o in un],
        )

        FreeCADGui.Selection.clearSelection()
        edge = [
            "Edge%d" % (i + 1)
            for i, e in enumerate(src.Shape.Edges)
            if abs(e.BoundBox.XMin - 20) < 1e-6 and abs(e.BoundBox.XMax - 20) < 1e-6
        ]
        FreeCADGui.Selection.addSelection(src, edge[0])
        FreeCADGui.runCommand("Surface_ExtendEdges")
        settle(0.5)
        ext = [o for o in doc.Objects if o.TypeId == "Surface::Extend"]
        check(
            "Extend edges carries the face on across the edge picked",
            len(ext) == 1
            and ext[0].isValid()
            and ext[0].Mode == "Edges"
            and ext[0].Shape.optimalBoundingBox(False, False).XMax > 24,
            [(o.getStatusString(), o.Shape.BoundBox) for o in ext],
        )
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(ln, "Edge1")
        FreeCADGui.runCommand("Surface_ExtendEdges")
        settle(0.3)
        check(
            "an edge of no face makes nothing, and opens no dialog",
            len([o for o in doc.Objects if o.TypeId == "Surface::Extend"]) == 1
            and QtWidgets.QApplication.activeModalWidget() is None,
        )
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
