"""The 3D sketch's typed values, the last S2 leftovers (docs/Sketch3D.md section 10).

  - while a line is drawn, a number typed opens an entry: Enter places the end
    that far toward the pointer, the line given that length as a Distance, one
    undo step with the line;
  - three values place the next point at that offset in the manipulator's
    frame, with X and Z distances for the non-zero ones;
  - text that is neither is refused and the entry stays; Escape closes it;
  - a new distance's entry takes "z 7": the dimension becomes Distance Z, 7;
  - the entry says what three offsets and a kind will do, not "Not a value";
  - after a ring turned the manipulator, its entry takes the angle exactly.

The entries are typed into as a user types (the focused line of the value
editor), opened through the view provider's entry points.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
import math
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "Sketch3DTypedValues"
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


def cons(sk):
    return [(c.Type, c.First, c.FirstPos, c.Second, c.SecondPos) for c in sk.Constraints]


def click(vp, p):
    x, y = vp.toView(p)
    vp.move(x, y)
    return vp.press(x, y)


def hover(vp, p):
    vp.move(*vp.toView(p))


def editor_says():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QLabel, "DatumValueEditorResult"):
        if w.isVisible():
            return w.text()
    return ""


def enter():
    w = QtWidgets.QApplication.focusWidget()
    QtTest.QTest.keyClick(w, QtCore.Qt.Key_Return)
    settle(0.2)


def type_in(text, enter=True):
    """Typed into the focused entry, as keys."""
    w = QtWidgets.QApplication.focusWidget()
    if w is None:
        return False
    QtTest.QTest.keyClicks(w, text)
    if enter:
        QtTest.QTest.keyClick(w, QtCore.Qt.Key_Return)
    settle(0.2)
    return True


def run():
    try:
        import Part
        import Sketcher

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        sk.addGeometry(Part.Point(V(-20, -20, -5)), True)
        sk.addGeometry(Part.Point(V(50, 50, 30)), True)
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.viewIsometric()
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject
        vp.setRotationHandles(False)

        # -- a line's length typed ---------------------------------------------
        FreeCADGui.Command.get("Sketcher_CreateLine").run()
        vp.setManipulator(V(0, 0, 0), FreeCAD.Rotation())
        click(vp, V(0, 0, 0))
        hover(vp, V(12, 9, 0))  # toward (4, 3, 0)
        n = len(sk.Geometry)
        undo = doc.UndoCount
        check("a number typed while drawing opens the entry", vp.startTypedPlacement("2"))
        check("which is open", vp.isTypingPlacement())
        type_in("5")
        check("Enter closes it", not vp.isTypingPlacement())
        g = sk.Geometry[n] if len(sk.Geometry) > n else None
        ok = check("the line is made", g is not None and g.TypeId == "Part::GeomLineSegment", g)
        if ok:
            check("25 long", abs(g.length() - 25) < 1e-6, g.length())
            d = (g.EndPoint - g.StartPoint).normalize()
            check("toward the pointer", (d - V(0.8, 0.6, 0)).Length < 0.05, d)
            c = cons(sk)
            check("with its length as a Distance", ("Distance", n, 0, -2000, 0) in c, c)
            check(
                "the value 25",
                abs([k for k in sk.Constraints if k.Type == "Distance"][-1].Value - 25) < 1e-9,
            )
            check("one undo step with the line", doc.UndoCount == undo + 1, doc.UndoCount - undo)
        vp.finishTool()

        # -- offsets ----------------------------------------------------------------
        FreeCADGui.Command.get("Sketcher_CreatePolyline").run()
        vp.setManipulator(V(0, 20, 0), FreeCAD.Rotation())
        click(vp, V(0, 20, 0))
        n = len(sk.Geometry)
        vp.startTypedPlacement("1")
        type_in("0, 0, 5", enter=False)
        says = editor_says()
        check(
            "the entry says what three values will do, not 'Not a value'",
            says and "Not a value" not in says and "10" in says,
            says,
        )
        enter()
        g = sk.Geometry[n] if len(sk.Geometry) > n else None
        ok = check("three values: a point at that offset", g is not None, g)
        if ok:
            d = g.EndPoint - g.StartPoint
            check("10, 0, 5 from the last point", (d - V(10, 0, 5)).Length < 1e-9, d)
            kinds = [k[0] for k in cons(sk) if k[1] == n and k[3] == n]
            check("with X and Z distances", sorted(kinds) == ["DistanceX", "DistanceZ"], kinds)

        # -- a key typed into the view itself ------------------------------------
        viewport = view.graphicsView().viewport()
        viewport.setFocus()
        settle(0.2)
        QtTest.QTest.keyClick(viewport, QtCore.Qt.Key_3)
        settle(0.2)
        check("a digit typed into the view opens the entry", vp.isTypingPlacement())
        w = QtWidgets.QApplication.focusWidget()
        check(
            "with that digit in it",
            isinstance(w, QtWidgets.QLineEdit) and w.text() == "3",
            w.text() if isinstance(w, QtWidgets.QLineEdit) else w,
        )
        if w is not None:
            QtTest.QTest.keyClick(w, QtCore.Qt.Key_Escape)
            settle(0.2)

        # -- refused, Escape -------------------------------------------------------
        vp.startTypedPlacement("x")
        type_in("yz")
        check("text that is no placement leaves the entry open", vp.isTypingPlacement())
        type_in("", enter=False)
        QtTest.QTest.keyClick(QtWidgets.QApplication.focusWidget(), QtCore.Qt.Key_Escape)
        settle(0.2)
        check("Escape closes it", not vp.isTypingPlacement())
        vp.finishTool()
        vp.finishTool()

        # -- a dimension's kind ----------------------------------------------------
        p = sk.addGeometry(Part.Point(V(-10, 0, 0)))
        q = sk.addGeometry(Part.Point(V(-4, 3, 2)))
        settle(0.2)
        vp.clearSelection()
        vp.select(p, 1)
        vp.select(q, 1)
        FreeCADGui.Command.get("Sketcher_ConstrainDistance").run()
        settle(0.3)
        check("a new distance's entry is open", vp.isEditingDatum())
        w = QtWidgets.QApplication.focusWidget()
        if isinstance(w, QtWidgets.QLineEdit):
            w.selectAll()
        type_in("z 7", enter=False)
        says = editor_says()
        check(
            "and says what 'z 7' will make",
            says and "Not a value" not in says and "Z" in says,
            says,
        )
        enter()
        last = sk.Constraints[-1]
        check("'z 7' makes it Distance Z", last.Type == "DistanceZ", last.Type)
        gq, gp = sk.Geometry[q], sk.Geometry[p]
        check("of 7", abs((gq.Z - gp.Z) - 7) < 1e-7, gq.Z - gp.Z)
        if vp.isEditingDatum():
            vp.finishDatumEdit(True)

        # -- the manipulator's angle -------------------------------------------------
        vp.resetManipulator()
        vp.turnByRing(2, 30)
        check("a ring turn opens the angle's entry", vp.isTypingTurn())
        w = QtWidgets.QApplication.focusWidget()
        if isinstance(w, QtWidgets.QLineEdit):
            w.selectAll()
        type_in("45")
        rot = vp.getManipulator()["Rotation"]
        check(
            "the angle typed, exactly",
            abs(math.degrees(rot.Angle) - 45) < 1e-9,
            math.degrees(rot.Angle),
        )
        check("about Z", abs(abs(rot.Axis.z) - 1) < 1e-9, rot.Axis)
        check("and the entry closed", not vp.isTypingTurn())

        # -- the working plane seen edge on: no point, no exception ----------------
        vp.resetManipulator()
        view.viewFront()  # the XY plane edge on
        settle(0.5)
        # off the manipulator's axes on screen (they lock a pick to them)
        ox, oy = vp.toView(vp.getManipulator()["Origin"])
        try:
            got = vp.pointAt(ox + 137, oy + 93)
            check("a pick on a plane seen edge on gives no point", got is None, got)
        except Exception as e:
            check("a pick on a plane seen edge on gives no point", False, e)
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
