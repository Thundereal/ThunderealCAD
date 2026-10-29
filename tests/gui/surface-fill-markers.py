"""The edge conditions of Sections and GeomFillSurface in their dialogs and in
the view (docs/Surface.md sec 8): their own panels do not show them, so each
dialog has a box of them, and each condition its arrow, as a network's.

Sections through three curves, the first an edge of a neighbour face. In
edit:
  - a box "Edge conditions" with the first and the last section's rows;
  - an arrow on each of the two end sections, none in between;
  - a click on the first's arrow steps FirstSectionContinuity G0 -> G1, its
    row with it, the surface built (the neighbour is its support);
  - OK keeps it.
A GeomFillSurface of four curves round a loop, Curved:
  - its box with four rows, and an arrow on each curve;
  - a click on the second curve's steps Boundary2Continuity, its row too;
  - Cancel puts it back and takes the arrows away.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import math
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets
from PySide.QtCore import Qt

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SurfaceFillMarkers"
V = FreeCAD.Vector
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
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


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def viewer_widget():
    return view().graphicsView()


def panel(name):
    for w in mw().findChildren(QtWidgets.QWidget, name):
        if w.isVisible():
            return w
    return None


def press(which):
    for w in mw().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            pump(40)
            return True
    return False


def field_text(node, name):
    value = node.getField(name).get()
    return value.getString() if hasattr(value, "getString") else str(value)


def containers():
    """Every shown linear dragger container of the view"""
    from pivy import coin

    root = view().getViewer().getSoRenderManager().getSceneGraph()
    search = coin.SoSearchAction()
    search.setType(coin.SoType.fromName("SoLinearDraggerContainer"))
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    return [
        p.getTail()
        for p in search.getPaths()
        if field_text(p.getTail(), "visible") in ("TRUE", "1")
    ]


def frame(node):
    """(where it stands, which way it points, how far its arrow is out)"""
    pos = V(*[float(v) for v in field_text(node, "translation").split()[:3]])
    x, y, z, angle = [float(v) for v in field_text(node, "rotation").split()[:4]]
    way = FreeCAD.Rotation(V(x, y, z), math.degrees(angle)).multVec(V(0, 1, 0))
    dragger = node.getPart("dragger", False)
    out = float(field_text(dragger, "translation").split()[1]) if dragger else 0.0
    return pos, way, out


def dragger_at(x, y):
    from pivy import coin

    rm = view().getViewer().getSoRenderManager()
    action = coin.SoRayPickAction(rm.getViewportRegion())
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(1)
    action.apply(rm.getSceneGraph())
    picked = action.getPickedPoint()
    if picked is None:
        return None
    path = picked.getPath()
    for i in range(path.getLength()):
        node = path.getNode(i)
        if node.getTypeId().getName().getString().endswith("LinearDraggerContainer"):
            return node
    return None


def pick_arrow(center, want, reach=70, step=2):
    """A pixel well inside the arrow of container `want`, near `center`"""
    cx, cy = center
    best, found = -1, None
    for r in range(0, reach, step):
        for dx in range(-r, r + 1, step):
            for dy in (-r, r) if abs(dx) != r else range(-r, r + 1, step):
                if dragger_at(cx + dx, cy + dy) != want:
                    continue
                score = sum(
                    dragger_at(cx + dx + i, cy + dy + j) == want
                    for i in (-2, 0, 2)
                    for j in (-2, 0, 2)
                )
                if score > best:
                    best, found = score, (cx + dx, cy + dy)
        if found is not None and best >= 9:
            break
    return found


def to_window(x, y):
    width, height = view().getSize()
    w = viewer_widget()
    local = QtCore.QPoint(int(x), int(height - y))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def arrow_pixel(node):
    pos, way, out = frame(node)
    p = view().getPointOnViewport(pos + way * out)
    return pick_arrow((p[0], p[1]), node)


def click_arrow(node):
    at = arrow_pixel(node)
    if at is None:
        return False
    win = mw().windowHandle()
    p = to_window(*at)
    QtTest.QTest.mouseMove(win, p)
    pump(5)
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(5)
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(60)
    # off the handle, so that the next click is not a double click on it
    QtTest.QTest.mouseMove(win, to_window(5, 5))
    pump(40)
    return True


def spline(doc, name, *pts):
    c = Part.BSplineCurve()
    c.interpolate([V(*p) for p in pts])
    o = doc.addObject("Part::Feature", name)
    o.Shape = c.toShape()
    return o


def edgeOn(shape, edge, tol=1e-6):
    for i, e in enumerate(shape.Edges):
        mid = Part.Vertex(e.valueAt((e.FirstParameter + e.LastParameter) / 2))
        if edge.distToShape(mid)[0] < tol and edge.distToShape(e.Vertexes[0])[0] < tol:
            return "Edge%d" % (i + 1)
    return None


def arrows_on(curves):
    """For each curve, how many shown arrows stand on it"""
    where = [frame(c)[0] for c in containers()]
    return [sum(1 for p in where if c.Shape.distToShape(Part.Vertex(p))[0] < 1e-3) for c in curves]


def arrow_on(curve):
    for c in containers():
        if curve.Shape.distToShape(Part.Vertex(frame(c)[0]))[0] < 1e-3:
            return c
    return None


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0
        )
        mw().showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        pump(50)
        doc = FreeCAD.newDocument(DOC)
        gdoc = FreeCADGui.getDocument(DOC)

        # Sections
        s1 = spline(doc, "s1", (0, 0, 0), (0, 5, 1), (0, 10, 0))
        s2 = spline(doc, "s2", (5, 0, 2), (5, 5, 3), (5, 10, 2))
        s3 = spline(doc, "s3", (10, 0, 0), (10, 5, 1), (10, 10, 0))
        ext = doc.addObject("Part::Extrusion", "Neighbour")
        ext.Base = s1
        ext.DirMode = "Custom"
        ext.Dir = V(-1, 0, 1)
        ext.LengthFwd = 5
        doc.recompute()
        s = doc.addObject("Surface::Sections", "Sec")
        s.NSections = [(ext, edgeOn(ext.Shape, s1.Shape.Edges[0]))] + [
            (o, "Edge1") for o in (s2, s3)
        ]
        doc.recompute()
        if not check("the sections build", s.isValid(), s.getStatusString()):
            return finish()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        view().viewIsometric()
        pump(30)
        gdoc.setEdit(s, 0)
        pump(80)
        p = panel("SurfaceGui_SectionsConditions")
        combo = p.findChild(QtWidgets.QComboBox, "firstSectionContinuity") if p else None
        last = p.findChild(QtWidgets.QComboBox, "lastSectionContinuity") if p else None
        if not check(
            "the dialog has a box of the end conditions",
            combo is not None and last is not None,
            gdoc.getInEdit(),
        ):
            return finish()
        on = arrows_on((s1, s2, s3))
        check("an arrow on each end section, none between", on == [1, 0, 1], on)
        first = arrow_on(s1)
        if first is not None:
            click_arrow(first)
        check(
            "a click on the first's arrow steps it to G1, its row too, built",
            s.FirstSectionContinuity == "G1" and combo.currentIndex() == 1 and s.isValid(),
            (s.FirstSectionContinuity, combo.currentIndex(), s.getStatusString()),
        )
        check(
            "OK keeps it",
            press(QtWidgets.QDialogButtonBox.Ok)
            and gdoc.getInEdit() is None
            and s.FirstSectionContinuity == "G1"
            and len(containers()) == 0,
            (gdoc.getInEdit(), s.FirstSectionContinuity, len(containers())),
        )

        # GeomFillSurface
        for o in (s, s1, s2, s3, ext):
            o.Visibility = False
        pts = [(20, 0, 0), (30, 0, 0), (30, 10, 0), (20, 10, 0)]
        mids = [(25, 0, 2), (30, 5, -2), (25, 10, 2), (20, 5, -2)]
        loop = [spline(doc, "c%d" % i, pts[i], mids[i], pts[(i + 1) % 4]) for i in range(4)]
        g = doc.addObject("Surface::GeomFillSurface", "GFS")
        g.BoundaryList = [(c, "Edge1") for c in loop]
        g.FillType = "Curved"
        doc.recompute()
        if not check("the fill builds", g.isValid(), g.getStatusString()):
            return finish()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump(30)
        gdoc.setEdit(g, 0)
        pump(80)
        p = panel("SurfaceGui_GeomFillConditions")
        rows = [
            p.findChild(QtWidgets.QComboBox, "boundary%dContinuity" % i) if p else None
            for i in range(1, 5)
        ]
        check("the dialog has a box of four conditions", all(r is not None for r in rows), rows)
        on = arrows_on(loop)
        check("an arrow on each of the four curves", on == [1, 1, 1, 1], on)
        second = arrow_on(loop[1])
        if second is not None:
            click_arrow(second)
        check(
            "a click on the second's steps Boundary2Continuity, its row too",
            g.Boundary2Continuity == "G1" and rows[1] is not None and rows[1].currentIndex() == 1,
            (g.Boundary2Continuity, rows[1].currentIndex() if rows[1] else None),
        )
        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(40)
        check(
            "Cancel puts it back and takes the arrows away",
            gdoc.getInEdit() is None and g.Boundary2Continuity == "G0" and len(containers()) == 0,
            (gdoc.getInEdit(), g.Boundary2Continuity, len(containers())),
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
