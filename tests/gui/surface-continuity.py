"""The Surface workbench's Edge continuity command (docs/Surface.md sec 7,
U4): the readout of how two faces meet along an edge.

  - it sits on the Surface tool bar;
  - on an edge between two faces of a box it reports a crease (G0) at 90
    degrees; on a fillet's tangent edge G1; on two faces that share an edge,
    picked as faces, the same as on the edge;
  - a selection it cannot use makes nothing and opens no dialog.

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
DOC = "SurfaceContinuity"
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


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        settle(0.5)
        bar = mw.findChild(QtWidgets.QToolBar, "Surface")
        names = [a.objectName() for a in bar.actions()] if bar else []
        check(
            "Edge continuity is on the Surface tool bar", "Surface_EdgeContinuity" in names, names
        )

        # the report view holds back a line repeating a recent one, to show it
        # later with a count: here every readout is wanted as it is written
        out = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/OutputWindow")
        state["window"] = out.GetInt("DuplicateWindow", 3)
        out.SetInt("DuplicateWindow", 0)

        doc = FreeCAD.newDocument(DOC)
        box = doc.addObject("Part::Box", "Box")
        b2 = doc.addObject("Part::Box", "Box2")
        b2.Placement.Base = FreeCAD.Vector(20, 0, 0)
        doc.recompute()
        fillet = doc.addObject("Part::Fillet", "Fillet")
        fillet.Base = b2
        fillet.Edges = [(1, 2.0, 2.0)]
        b2.Visibility = False
        doc.recompute()

        def readout(picks):
            FreeCADGui.Selection.clearSelection()
            for obj, sub in picks:
                FreeCADGui.Selection.addSelection(obj, sub)
            report = next(
                (
                    w
                    for w in mw.findChildren(QtWidgets.QTextEdit)
                    if w.metaObject().className() == "Gui::DockWnd::ReportOutput"
                ),
                None,
            )
            marks = ("Edge continuity", "Select an edge")

            def count(t):
                return sum(t.count(m) for m in marks)

            before = count(report.toPlainText()) if report else 0
            FreeCADGui.runCommand("Surface_EdgeContinuity")
            settle(0.3)
            text = report.toPlainText() if report else ""
            if count(text) == before:
                return ""
            # the last line it wrote: the view trims old lines, so not by length
            return [x for x in text.splitlines() if any(m in x for m in marks)][-1]

        out = readout([(box, "Edge1")])
        check(
            "a box's edge reads as a crease at 90 deg", "G0" in out and "angle 90 deg" in out, out
        )
        rounds = [
            i for i, f in enumerate(fillet.Shape.Faces) if isinstance(f.Surface, Part.Cylinder)
        ]
        r = fillet.Shape.Faces[rounds[0]]
        edge = [e for e in r.Edges if isinstance(e.Curve, Part.Line)][0]
        name = ["Edge%d" % (i + 1) for i, e in enumerate(fillet.Shape.Edges) if e.isSame(edge)][0]
        out = readout([(fillet, name)])
        check(
            "a fillet's tangent edge reads as G1", "G1" in out and "curvature gap 0.5" in out, out
        )
        out = readout([(box, "Face1")])
        check(
            "one face alone makes no readout, and opens no dialog",
            "Edge continuity:" not in out and QtWidgets.QApplication.activeModalWidget() is None,
            out,
        )
        faces = [
            "Face%d" % (i + 1)
            for i, f in enumerate(fillet.Shape.Faces)
            if any(e.isSame(edge) for e in f.Edges)
        ]
        out2 = readout([(fillet, faces[0]), (fillet, faces[1])])
        check(
            "two faces sharing it read the same", "G1" in out2 and "curvature gap 0.5" in out2, out2
        )
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    if "window" in state:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/OutputWindow").SetInt(
            "DuplicateWindow", state["window"]
        )
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
