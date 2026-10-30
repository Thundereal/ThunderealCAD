"""The edge conditions in the view (docs/Surface.md sec 8): a marker at the
middle of each outer boundary of a network or a blend, pointing into the
surface.

A network of three profiles and three guides over z = 2 sin(pi u) sin(pi v)
+ u, the first profile given a support face. In edit:
  - one arrow on each of the four boundaries, standing at its middle and
    pointing into the surface;
  - a click on the first profile's arrow steps its continuity G0 -> G1 ->
    G2 -> G0, the panel's box with it;
  - the clicked value active, a weight typed in the view is the feature's
    and the panel's;
  - the panel's weight moves the arrow: its length is the weight times a
    quarter of the boundary;
  - dragged out along itself the arrow raises the weight;
  - the handle's menu (Space) sets the continuity;
  - Cancel takes the arrows away and puts the conditions back.
A blend between two planes: an arrow on each of its two edges; a click on
the first steps Side1Continuity from G1 to G2; a weight at its edge's end
given in the panel puts an arrow at each end of the edge in place of the
middle's, each as long as the weight there, and the end's dragged raises
the end weight.

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
DOC = "SurfaceEdgeMarkers"
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


def marker_of(middle):
    """The container standing at `middle`, and its frame"""
    for node in containers():
        f = frame(node)
        if (f[0] - middle).Length < 1e-3:
            return node, f
    return None, None


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


def drag_arrow(node, pixels):
    """Pressed on the arrow and pulled `pixels` further along it on the screen"""
    at = arrow_pixel(node)
    if at is None:
        return False
    pos, way, out = frame(node)
    a = view().getPointOnViewport(pos)
    b = view().getPointOnViewport(pos + way * max(out, 1.0))
    dx, dy = b[0] - a[0], b[1] - a[1]
    norm = math.hypot(dx, dy) or 1.0
    win = mw().windowHandle()
    p = to_window(*at)
    QtTest.QTest.mouseMove(win, p)
    pump(5)
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(5)
    for i in range(1, 11):
        q = to_window(at[0] + dx / norm * pixels * i / 10, at[1] + dy / norm * pixels * i / 10)
        QtTest.QTest.mouseMove(win, q)
        pump(5)
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, q)
    pump(80)
    QtTest.QTest.mouseMove(win, to_window(5, 5))
    pump(40)
    return True


def real_key(widget, k):
    QtTest.QTest.keyPress(widget, k, Qt.NoModifier)
    pump(5)
    target = QtWidgets.QApplication.focusWidget() or widget
    QtTest.QTest.keyRelease(target, k, Qt.NoModifier)
    pump(10)


def key(k):
    real_key(viewer_widget(), k)


def open_box():
    for w in mw().findChildren(QtWidgets.QAbstractSpinBox):
        if not w.isVisible() or w.metaObject().className() != "Gui::QuantitySpinBox":
            continue
        parent = w
        while parent is not None and parent.metaObject().className() != "Gui::TaskView::TaskView":
            parent = parent.parentWidget()
        if parent is None:
            return w
    return None


def type_value(digits):
    """Typed at the active handle: the first digit opens its label"""
    key(getattr(Qt, "Key_" + digits[0]))
    box = open_box()
    if box is None:
        return False
    for ch in digits[1:]:
        QtTest.QTest.keyClicks(box, ch)
    pump(5)
    real_key(box, Qt.Key_Return)
    pump(80)
    # the label closed: the view's again
    real_key(box, Qt.Key_Escape)
    pump(20)
    return True


def menu_by_space(choice):
    """Space in the view, the menu read and `choice` triggered from a timer"""
    seen = {"items": None}

    def answer():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QMenu) and w.isVisible():
                seen["items"] = [
                    (a.text().replace("&", ""), a.isChecked())
                    for a in w.actions()
                    if not a.isSeparator() and a.text()
                ]
                for a in w.actions():
                    if a.text().replace("&", "") == choice:
                        a.trigger()
                        break
                w.close()
                return
        QtCore.QTimer.singleShot(100, answer)

    QtCore.QTimer.singleShot(300, answer)
    key(Qt.Key_Space)
    pump(80)
    return seen["items"]


def f(u, v):
    return V(10 * u, 10 * v, 2 * math.sin(math.pi * u) * math.sin(math.pi * v) + u)


def curve(alongU, fixed):
    c = Part.BSplineCurve()
    c.interpolate([f(t / 4, fixed) if alongU else f(fixed, t / 4) for t in range(5)])
    return c


def middles(face):
    """The middles of the four boundaries of the face's surface, its centre,
    and the boundaries' lengths"""
    s = face.Surface
    u0, u1, v0, v1 = s.bounds()
    um, vm = (u0 + u1) / 2, (v0 + v1) / 2
    ends = {
        "UMin": (s.value(u0, vm), s.uIso(u0).length()),
        "UMax": (s.value(u1, vm), s.uIso(u1).length()),
        "VMin": (s.value(um, v0), s.vIso(v0).length()),
        "VMax": (s.value(um, v1), s.vIso(v1).length()),
    }
    return ends, s.value(um, vm)


def edgeAtX(shape, x):
    for i, e in enumerate(shape.Edges):
        bb = e.BoundBox
        if abs(bb.XMin - x) < 1e-6 and abs(bb.XMax - x) < 1e-6:
            return "Edge%d" % (i + 1)
    return None


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0
        )
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Gui/Gizmos").SetBool(
            "EnableCoarseSnap", False
        )
        mw().showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        pump(50)
        doc = FreeCAD.newDocument(DOC)
        gdoc = FreeCADGui.getDocument(DOC)

        feats = []
        for prefix, alongU in (("p", True), ("g", False)):
            for i, fixed in enumerate((0, 0.5, 1)):
                o = doc.addObject("Part::Feature", "%s%d" % (prefix, i))
                o.Shape = curve(alongU, fixed).toShape()
                feats.append(o)
        ext = doc.addObject("Part::Extrusion", "Neighbour")
        ext.Base = feats[0]
        ext.DirMode = "Custom"
        ext.Dir = V(0, -1, 0)
        ext.LengthFwd = 5
        n = doc.addObject("Surface::Network", "Net")
        n.Profiles = [(o, "Edge1") for o in feats[:3]]
        n.Guides = [(o, "Edge1") for o in feats[3:]]
        doc.recompute()
        n.FirstProfileSupport = (ext, "Face1")
        doc.recompute()
        if not check("the network builds", n.isValid(), n.getStatusString()):
            return finish()

        FreeCADGui.SendMsgToActiveView("ViewFit")
        view().viewIsometric()
        pump(30)
        gdoc.setEdit(n, 0)
        pump(80)
        p = panel("SurfaceGui_Network")
        if not check("the network's panel is open", p is not None, gdoc.getInEdit()):
            return finish()

        ends, centre = middles(n.Shape.Face1)
        found = {}
        for name, (middle, length) in ends.items():
            node, fr = marker_of(middle)
            found[name] = (node, fr, length)
        shown = len(containers())
        inward = all(
            fr is not None and fr[1].dot(centre - middle) > 0
            for (middle, _), (_, fr, _) in zip(ends.values(), found.values())
        )
        check(
            "an arrow on each of the four boundaries, at its middle, pointing in",
            shown == 4 and all(v[0] is not None for v in found.values()) and inward,
            (shown, {k: v[1] for k, v in found.items()}),
        )
        first, _, firstLength = found["VMin"]
        if first is None:
            return finish()

        combo = p.findChild(QtWidgets.QComboBox, "firstProfileContinuity")
        weightBox = p.findChild(QtWidgets.QDoubleSpinBox, "firstProfileWeight")
        steps = []
        for _ in range(3):
            click_arrow(first)
            steps.append((n.FirstProfileContinuity, combo.currentIndex()))
        check(
            "a click on the arrow steps the continuity G0 -> G1 -> G2 -> G0, the panel's too",
            steps == [("G1", 1), ("G2", 2), ("G0", 0)],
            steps,
        )

        click_arrow(first)  # G1, and the value active
        typed = type_value("2")
        check(
            "a weight typed at the active arrow is the feature's and the panel's",
            typed
            and abs(n.FirstProfileWeight - 2) < 1e-9
            and abs(weightBox.value() - 2) < 1e-9
            and n.FirstProfileContinuity == "G1",
            (typed, n.FirstProfileWeight, weightBox.value(), n.FirstProfileContinuity),
        )

        weightBox.setValue(1.5)
        pump(40)
        _, fr = marker_of(ends["VMin"][0])
        check(
            "the panel's weight moves the arrow: a quarter of the boundary per unit",
            fr is not None and abs(fr[2] - 1.5 * firstLength / 4) < 1e-3,
            (fr, firstLength),
        )

        before = n.FirstProfileWeight
        drag_arrow(first, 60)
        check(
            "dragged out along itself, the arrow raises the weight",
            n.FirstProfileWeight > before + 0.04
            and abs(weightBox.value() - n.FirstProfileWeight) < 1e-6,
            (before, n.FirstProfileWeight, weightBox.value()),
        )

        click_arrow(first)  # G2, and active
        click_arrow(first)  # G0
        items = menu_by_space("G2")
        check(
            "its menu offers G0, G1 and G2, G0 checked; G2 chosen is the feature's",
            items is not None
            and ("G0", True) in items
            and ("G1", False) in items
            and ("G2", False) in items
            and n.FirstProfileContinuity == "G2"
            and combo.currentIndex() == 2,
            (items, n.FirstProfileContinuity),
        )

        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(40)
        check(
            "Cancel takes the arrows away and puts the condition back",
            gdoc.getInEdit() is None
            and len(containers()) == 0
            and n.FirstProfileContinuity == "G0"
            and abs(n.FirstProfileWeight - 1) < 1e-9,
            (gdoc.getInEdit(), len(containers()), n.FirstProfileContinuity, n.FirstProfileWeight),
        )

        # a blend: an arrow on each of its two edges
        low = doc.addObject("Part::Plane", "Low")
        low.Length = low.Width = 10
        low.Placement.Base = V(0, 20, 0)
        high = doc.addObject("Part::Plane", "High")
        high.Length = high.Width = 10
        high.Placement = FreeCAD.Placement(V(15, 20, 3), FreeCAD.Rotation(V(0, 1, 0), -16.7))
        doc.recompute()
        b = doc.addObject("Surface::BlendSurface", "Blend")
        b.Edge1 = (low, edgeAtX(low.Shape, 10))
        b.Edge2 = (high, edgeAtX(high.Shape, 15))
        doc.recompute()
        for o in feats + [ext, n]:
            o.Visibility = False
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump(30)
        gdoc.setEdit(b, 0)
        pump(80)
        e1 = low.Shape.getElement(b.Edge1[1][0])
        mid1 = e1.valueAt((e1.FirstParameter + e1.LastParameter) / 2)
        node, fr = marker_of(mid1)
        check(
            "the blend has an arrow on each edge, the first at its middle",
            len(containers()) == 2 and node is not None,
            (len(containers()), [frame(c)[0] for c in containers()], mid1),
        )
        if node is not None:
            click_arrow(node)
            check(
                "a click on it steps the first side from G1 to G2",
                b.Side1Continuity == "G2" and b.isValid(),
                (b.Side1Continuity, b.getStatusString()),
            )
        endBox = panel("side1WeightEnd")
        check(
            "the first side has a box for its weight at the edge's end, 0 the weight all along",
            endBox is not None and endBox.value() == 0 and b.Side1WeightEnd == 0,
            (endBox, b.Side1WeightEnd),
        )
        if endBox is not None:
            endBox.setValue(3.0)
            pump(60)
            near = lambda p: next(
                ((c, frame(c)) for c in containers() if (frame(c)[0] - p).Length < 1e-2),
                (None, None),
            )
            start, fs = near(e1.valueAt(e1.FirstParameter))
            end, fe = near(e1.valueAt(e1.LastParameter))
            quarter = e1.Length / 4
            check(
                "given an end weight, the first side's arrows stand at its edge's two ends, "
                "as long as the weight at each, and none at its middle",
                abs(b.Side1WeightEnd - 3) < 1e-9
                and b.isValid()
                and len(containers()) == 3
                and near(mid1)[0] is None
                and fs is not None
                and fe is not None
                and abs(fs[2] - b.Side1Weight * quarter) < 1e-2
                and abs(fe[2] - 3 * quarter) < 1e-2,
                (b.Side1WeightEnd, b.getStatusString(), [frame(c) for c in containers()], quarter),
            )
            if end is not None:
                drag_arrow(end, 60)
                check(
                    "dragged out along itself, the end's arrow raises the end weight",
                    b.Side1WeightEnd > 3.04 and abs(endBox.value() - b.Side1WeightEnd) < 1e-6,
                    (b.Side1WeightEnd, endBox.value()),
                )
        press(QtWidgets.QDialogButtonBox.Cancel)
        pump(40)
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
