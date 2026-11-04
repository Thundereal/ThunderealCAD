"""The 3D sketch's constraints from the selection, phase S2d (docs/Sketch3D.md section 7).

  - a press on a sketch point selects it (and moves the manipulator there);
  - the Sketcher's constraint commands make their 3D constraint of the
    selection: Coincident of two points, Along X / Z of lines (the 3D
    toolbar's), Distance of a line with what it measures, Lock of a point (its
    three coordinates); Horizontal says it needs a plane and makes nothing;
  - a new dimension opens the in-view value editor; Enter takes what is typed,
    a value or an expression, and the label shows it; it is one undo step;
  - the panel's status follows what changes it: a conflict added from Python,
    and Undo taking it away; a value that would conflict is refused, and the
    editor says why;
  - a selected constraint goes with the Delete key and with deleteSelection().

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "Sketch3DConstraints"
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


def types(sk):
    return [c.Type for c in sk.Constraints]


def status_text():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QLabel, "Sketch3DStatus"):
        return w.text()
    return ""


def editor_says():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QLabel, "DatumValueEditorResult"):
        if w.isVisible():
            return w.text()
    return ""


def type_in_editor(text):
    """What is typed into the in-view editor (DatumValueEditor's line), then Enter."""
    boxes = [
        w
        for w in FreeCADGui.getMainWindow().findChildren(
            QtWidgets.QLineEdit, "DatumValueEditorLine"
        )
        if w.isVisible()
    ]
    if not boxes:
        return False
    w = boxes[0]
    w.setText(text)
    w.textEdited.emit(text)  # what typing does, which the editor listens to
    settle(0.3)
    # Enter closes the editor and its line with it: no release after
    QtWidgets.QApplication.sendEvent(
        w, QtGui.QKeyEvent(QtCore.QEvent.KeyPress, QtCore.Qt.Key_Return, QtCore.Qt.NoModifier)
    )
    settle(0.5)
    return True


def run():
    try:
        import Part

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(20, 2, 1)))
        sk.addGeometry(Part.LineSegment(V(22, 4, 0), V(22.5, 5, 18)))
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.viewIsometric()
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject
        cmd = FreeCADGui.Command.get

        # -- select by press, Coincident --------------------------------------
        for p in (sk.Geometry[0].EndPoint, sk.Geometry[1].StartPoint):
            vp.press(*vp.toView(p))
        sel = vp.getSelection()[0]
        check("presses select the two points", sel == [(0, 2), (1, 1)], sel)
        check(
            "Coincident is active in a 3D sketch's edit",
            cmd("Sketcher_ConstrainCoincident").isActive(),
        )
        cmd("Sketcher_ConstrainCoincident").run()
        check("it makes their coincidence", types(sk) == ["Coincident"], types(sk))
        check(
            "which the solve holds",
            (sk.Geometry[0].EndPoint - sk.Geometry[1].StartPoint).Length < 1e-9,
        )
        check("and the selection goes", vp.getSelection() == ([], []), vp.getSelection())

        # -- Along X, Along Z -------------------------------------------------
        vp.select(0)
        cmd("Sketcher_Constrain3DAlongX").run()
        vp.select(1)
        cmd("Sketcher_Constrain3DAlongZ").run()
        check("Along X and Along Z of lines", types(sk)[-2:] == ["AlongX", "AlongZ"], types(sk))
        d0 = sk.Geometry[0].EndPoint - sk.Geometry[0].StartPoint
        d1 = sk.Geometry[1].EndPoint - sk.Geometry[1].StartPoint
        check(
            "the lines now run along them",
            abs(d0.y) + abs(d0.z) < 1e-9 and abs(d1.x) + abs(d1.y) < 1e-9,
            (d0, d1),
        )

        # -- Horizontal refused -----------------------------------------------
        n = len(sk.Constraints)
        vp.select(0)
        cmd("Sketcher_ConstrainHorizontal").run()
        check("Horizontal makes nothing in a 3D sketch", len(sk.Constraints) == n, types(sk))
        vp.clearSelection()

        # -- Distance, its value typed ----------------------------------------
        vp.select(0)
        length = (sk.Geometry[0].EndPoint - sk.Geometry[0].StartPoint).Length
        cmd("Sketcher_ConstrainDistance").run()
        settle(0.5)
        i = len(sk.Constraints) - 1
        check(
            "Distance of a line, with what it measures",
            sk.Constraints[i].Type == "Distance" and abs(sk.Constraints[i].Value - length) < 1e-9,
            (sk.Constraints[i].Type, sk.Constraints[i].Value, length),
        )
        labels = vp.getLabels()
        check("its label is drawn", i in labels and "mm" in labels[i], labels)
        check("and the value editor opens", vp.isEditingDatum())
        typed = type_in_editor("25")
        check(
            "Enter takes what is typed",
            typed and not vp.isEditingDatum() and abs(sk.Constraints[i].Value - 25) < 1e-9,
            (typed, vp.isEditingDatum(), sk.Constraints[i].Value),
        )
        g = sk.Geometry[0]
        check("the line is 25 long", abs((g.EndPoint - g.StartPoint).Length - 25) < 1e-9)
        check("and its label says so", "25" in vp.getLabels().get(i, ""), vp.getLabels())
        doc.undo()
        settle(0.3)
        check(
            "one undo takes the dimension and its value back together, as in 2D",
            len(sk.Constraints) == i,
            types(sk),
        )
        doc.redo()
        settle(0.3)
        check(
            "and redo brings both",
            len(sk.Constraints) == i + 1 and abs(sk.Constraints[i].Value - 25) < 1e-9,
            types(sk),
        )

        vp.editDatum(i)
        settle(0.3)
        typed = type_in_editor("=2 * 15 mm")
        check(
            "an expression typed binds it",
            typed and abs(sk.Constraints[i].Value - 30) < 1e-9 and sk.ExpressionEngine,
            (sk.Constraints[i].Value, sk.ExpressionEngine),
        )

        # -- Lock -------------------------------------------------------------
        n = len(sk.Constraints)
        vp.select(1, 2)
        cmd("Sketcher_ConstrainLock").run()
        check(
            "Lock gives a point its three coordinates",
            types(sk)[n:] == ["DistanceX", "DistanceY", "DistanceZ"],
            types(sk)[n:],
        )
        # what is left free: the second line's length (its start slides along Z)
        check("leaving one degree of freedom", sk.DoF == 1, sk.DoF)

        # -- the status follows ------------------------------------------------
        import Sketcher

        check("the panel says what is left", "1 degrees of freedom" in status_text(), status_text())
        doc.openTransaction("conflict")
        sk.addConstraint(Sketcher.Constraint("DistanceX", 0, 40.0))
        doc.commitTransaction()
        settle(0.3)
        check(
            "a conflict added from Python is said in the panel",
            "Conflicting" in status_text() and sk.Conflicting,
            (status_text(), sk.Conflicting),
        )
        doc.undo()
        settle(0.3)
        check(
            "and Undo takes it out of the panel and the sketch",
            "Conflicting" not in status_text() and sk.Conflicting == [] and sk.DoF == 1,
            (status_text(), sk.Conflicting, sk.DoF),
        )
        doc.openTransaction("redundant")
        sk.addConstraint(Sketcher.Constraint("DistanceX", 0, 30.0))
        doc.commitTransaction()
        settle(0.3)
        j = len(sk.Constraints) - 1
        check("a redundant one is said", "Redundant" in status_text(), status_text())
        vp.editDatum(j)
        settle(0.3)
        type_in_editor("40")
        check(
            "a value that would conflict is refused, and the editor says why",
            abs(sk.Constraints[j].Value - 30) < 1e-9 and vp.isEditingDatum() and editor_says(),
            (sk.Constraints[j].Value, vp.isEditingDatum(), editor_says()),
        )
        vp.finishDatumEdit(False)
        settle(0.3)
        check("given up, the entry closes", not vp.isEditingDatum())
        doc.undo()
        settle(0.3)
        check("Undo takes it away again", "Redundant" not in status_text(), status_text())

        # -- Delete -----------------------------------------------------------
        n = len(sk.Constraints)
        vp.selectConstraint(n - 1)
        viewport = view.graphicsView().viewport()
        viewport.setFocus()
        settle(0.2)
        QtTest.QTest.keyClick(viewport, QtCore.Qt.Key_Delete)
        settle(0.3)
        check("Delete removes the selected constraint", len(sk.Constraints) == n - 1, types(sk))
        vp.selectConstraint(n - 2)
        vp.deleteSelection()
        check("and so does deleteSelection()", len(sk.Constraints) == n - 2, types(sk))
        check(
            "no constraint is malformed or conflicting",
            sk.Malformed == [] and sk.Conflicting == [],
            (sk.Malformed, sk.Conflicting),
        )
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    gdoc = FreeCADGui.getDocument(DOC) if FreeCAD.listDocuments().get(DOC) else None
    if gdoc and gdoc.getInEdit():
        gdoc.resetEdit()
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
