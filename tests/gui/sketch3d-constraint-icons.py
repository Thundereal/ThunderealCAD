"""The 3D sketch's constraints without a value: icons and the panel's list (an S2
leftover, docs/Sketch3D.md section 10).

  - each constraint without a value shows its icon by what it binds: Along on its
    line, Parallel on both lines, Block on its curve; a coincidence and a
    dimension show none (a point, a label);
  - a press on an icon selects its constraint, and Delete removes it;
  - the panel lists every constraint, the view's selection shown in it; a row
    selected there selects the constraint, a coincidence too, and Delete in the
    list removes it;
  - turned, the view places the icons and the labels again for the new camera.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""
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
DOC = "Sketch3DConstraintIcons"
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


def label_color():
    """The dimension label's text colour in the edit scene, as (r, g, b) 0..255."""
    from pivy import coin

    view = FreeCADGui.getDocument(DOC).ActiveView
    sa = coin.SoSearchAction()
    sa.setType(coin.SoType.fromName("SoDatumLabel"))
    sa.setInterest(coin.SoSearchAction.FIRST)
    sa.apply(view.getViewer().getSoRenderManager().getSceneGraph())
    path = sa.getPath()
    if not path:
        return None
    text = path.getTail().getField("textColor").get().getString()
    return tuple(round(float(x) * 255) for x in text.split())


def types(sk):
    return [c.Type for c in sk.Constraints]


def run():
    try:
        import Part
        import Sketcher

        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        doc = FreeCAD.newDocument(DOC)
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        a = sk.addGeometry(Part.LineSegment(V(0, 0, 0), V(30, 0, 0)))
        b = sk.addGeometry(Part.LineSegment(V(30, 0, 0), V(30, 0, 20)))
        c = sk.addGeometry(Part.LineSegment(V(0, 12, 0), V(25, 14, 2)))
        d = sk.addGeometry(Part.LineSegment(V(-10, 30, 5), V(-10, 50, 5)))
        made = [
            Sketcher.Constraint("Coincident", a, 2, b, 1),
            Sketcher.Constraint("AlongX", a),
            Sketcher.Constraint("AlongZ", b),
            Sketcher.Constraint("Parallel", a, c),
            Sketcher.Constraint("Distance", a, 30.0),
            Sketcher.Constraint("Block", d),
        ]
        for k in made:
            sk.addConstraint(k)
        doc.recompute()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(sk)
        settle(1.0)
        view = gdoc.ActiveView
        view.viewIsometric()
        view.fitAll()
        settle(0.8)
        vp = sk.ViewObject
        coincident, along_x, along_z, parallel, distance, block = range(6)

        icons = vp.getIcons()
        shown = sorted(i for i, x, y in icons)
        check(
            "icons for the constraints without a value: Parallel on both lines",
            shown == [along_x, along_z, parallel, parallel, block],
            icons,
        )
        check(
            "none for a coincidence or a dimension",
            coincident not in shown and distance not in shown,
        )

        # -- teal, and the setting applies at once ---------------------------------
        check(
            "the dimension in the 3D sketch's teal", label_color() == (0, 131, 143), label_color()
        )
        grp = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
        grp.SetUnsigned("Sketch3DDimColor", 0x112233FF)
        settle(0.2)
        check("its setting applies at once", label_color() == (0x11, 0x22, 0x33), label_color())
        grp.RemUnsigned("Sketch3DDimColor")
        settle(0.2)
        check("and back to teal without it", label_color() == (0, 131, 143), label_color())

        # -- a press on an icon selects it, Delete removes it --------------------
        x, y = [(x, y) for i, x, y in icons if i == along_z][0]
        vp.clearSelection()
        vp.press(x, y)
        check(
            "a press on an icon selects its constraint",
            vp.getSelection()[1] == [along_z],
            vp.getSelection(),
        )
        viewport = view.graphicsView().viewport()
        viewport.setFocus()
        settle(0.2)
        QtTest.QTest.keyClick(viewport, QtCore.Qt.Key_Delete)
        settle(0.3)
        check("Delete removes it", "AlongZ" not in types(sk), types(sk))
        doc.undo()
        settle(0.3)
        check("(undone)", "AlongZ" in types(sk), types(sk))

        # -- the panel's list -----------------------------------------------------
        lists = [
            w
            for w in mw.findChildren(QtWidgets.QListWidget)
            if w.objectName() == "Sketch3DConstraintList"
        ]
        ok = check("the panel lists the constraints", len(lists) == 1)
        if ok:
            lst = lists[0]
            check(
                "every one", lst.count() == len(sk.Constraints), (lst.count(), len(sk.Constraints))
            )
            vp.clearSelection()
            vp.selectConstraint(parallel)
            settle(0.1)
            rows = [i for i in range(lst.count()) if lst.item(i).isSelected()]
            check("the view's selection shown in it", rows == [parallel], rows)
            lst.clearSelection()
            lst.item(coincident).setSelected(True)
            settle(0.1)
            check(
                "a row selected there selects the constraint, a coincidence too",
                vp.getSelection()[1] == [coincident],
                vp.getSelection(),
            )
            lst.setFocus()
            settle(0.2)
            QtTest.QTest.keyClick(lst, QtCore.Qt.Key_Delete)
            settle(0.3)
            check("Delete in the list removes it", "Coincident" not in types(sk), types(sk))
            check("and the list follows", lst.count() == len(sk.Constraints), lst.count())

        # -- the view turned: icons and labels placed again ----------------------
        before = sorted(vp.getIcons())
        label = vp.getLabelPlane(distance - 1)  # the coincidence went: one index down
        normal = label.Rotation.multVec(V(0, 0, 1)) if label else None
        view.viewFront()
        settle(0.8)
        after = sorted(vp.getIcons())
        check(
            "turned, the icons are placed for the new view",
            after and after != before,
            (before, after),
        )
        label = vp.getLabelPlane(distance - 1)
        turned = label.Rotation.multVec(V(0, 0, 1)) if label else None
        # its line runs along X: facing the front view, its plane's normal is along Y
        check(
            "the label turned to face the new view",
            turned is not None
            and abs(abs(turned.y) - 1) < 1e-6
            and normal is not None
            and abs(abs(normal.y) - 1) > 1e-3,
            (normal, turned),
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
