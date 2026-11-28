"""A recompute failing under a handle's drag does not move the view.

What the user's words are, apart from our readings: docs/HandsOnQueue.md
entry 143, the reporter verbatim: "when dragging if recompute fails for
that dragging fillet, change cursor to stop sign" -- the stop sign is the
in-drag signal of a failing recompute. Not said by the user, and ours: that
the Report view is not popped while a handle is dragged.

Found on the Mac mini: a Pad's length arrow dragged through its failing zero
("Length too small") -- the error popped the Report view (its
ShowReportViewOnError, on by default), its dock took room, and the 3D view
shrank under a pointer that had not moved (866x311 -> 866x300), the model
with it.

Now while a handle is dragged the error is written but the pop held
(Gui::DragInProgress); once the drag has ended the pop comes if the edited
object is still in error.

Claims:
  - a Pad dragged through its failing zero, the Report view tabbed behind
    the Python console, and again with it hidden: the view's size and
    place the same at every move; the error written in the Report view;
    the length arrow shown and the stop-sign cursor up at the failing move
    (both so before this change too); after the release (the pad working
    again, the error no longer standing) the Report view not popped;
  - a fillet beside a missing edge (in error throughout) with one of its
    working edges dragged: no pop during the drag, the view the same; after
    the release, the error standing, the Report view popped.
"""
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
DOC = "DragKeepsTheView"

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


def view():
    return FreeCADGui.getDocument(DOC).ActiveView


def viewer_widget():
    return view().graphicsView()


def to_window(x, y):
    width, height = view().getSize()
    w = viewer_widget()
    local = QtCore.QPoint(int(round(x)), int(round(height - 1 - y)))
    g = w.viewport().mapToGlobal(local) if hasattr(w, "viewport") else w.mapToGlobal(local)
    return mw().mapFromGlobal(g)


def view_place():
    w = viewer_widget()
    vp = w.viewport() if hasattr(w, "viewport") else w
    g = vp.mapToGlobal(QtCore.QPoint(0, 0))
    return tuple(view().getSize()) + (g.x(), g.y())


def dragger_at(x, y):
    from pivy import coin

    rm = view().getViewer().getSoRenderManager()
    action = coin.SoRayPickAction(rm.getViewportRegion())
    action.setPoint(coin.SbVec2s(int(x), int(y)))
    action.setRadius(1)
    action.apply(rm.getSceneGraph())
    picked = action.getPickedPoint()
    if picked is None:
        return False
    path = picked.getPath()
    return any(path.getNode(i).getTypeId().getName().getString().endswith("LinearDraggerContainer")
               for i in range(path.getLength()))


def arrow_on_axis(z_from, z_to):
    """A pixel of a length arrow on the pad's axis between z_from and z_to"""
    z = z_from
    while z <= z_to:
        x, y = view().getPointOnViewport(FreeCAD.Vector(5, 5, z))
        for dx in (0, -1, 1, -2, 2):
            if dragger_at(x + dx, y):
                return (x + dx, y)
        z += 0.25
    return None


def axis_point_under(pixel):
    best = None
    z = -20.0
    while z <= 30.0:
        x, y = view().getPointOnViewport(FreeCAD.Vector(5, 5, z))
        d = (x - pixel[0]) ** 2 + (y - pixel[1]) ** 2
        if best is None or d < best[0]:
            best = (d, z)
        z += 0.01
    return best[1]


def docks():
    return {d.objectName(): d for d in mw().findChildren(QtWidgets.QDockWidget)}


def report_up():
    """The Report view's dock shown and, tabbed with others, its tab the one
    up"""
    d = docks().get("Report view")
    if not (d and d.isVisible()):
        return False
    title = d.windowTitle()
    for bar in mw().findChildren(QtWidgets.QTabBar):
        titles = [bar.tabText(i) for i in range(bar.count())]
        if title in titles and bar.isVisible():
            return bar.tabText(bar.currentIndex()) == title
    return True


def report_text():
    d = docks().get("Report view")
    if not d:
        return ""
    for w in d.findChildren(QtWidgets.QTextEdit):
        return w.toPlainText()
    return ""


def set_report(how):
    """'behind': tabbed behind the Python console; 'hidden': not shown"""
    ds = docks()
    report, console = ds["Report view"], ds["Python console"]
    if how == "behind":
        console.show()
        report.show()
        mw().tabifyDockWidget(console, report)
        pump(10)
        # the console's tab the one up: through the tab bar the docks share
        for bar in mw().findChildren(QtWidgets.QTabBar):
            for i in range(bar.count()):
                if bar.tabText(i) == console.windowTitle():
                    bar.setCurrentIndex(i)
        console.raise_()
    else:
        report.hide()
    pump(30)
    note("INFO the Report view set %s: up %s" % (how, report_up()))


class Failures:
    def __init__(self, name):
        self.name = name
        self.count = 0

    def slotRecomputedObject(self, obj):
        if obj.Name == self.name and not obj.isValid():
            self.count += 1


def pad_leg(how):
    doc = FreeCAD.newDocument(DOC)
    body = doc.addObject("PartDesign::Body", "Body")
    sk = body.newObject("Sketcher::SketchObject", "Sketch")
    pts = [FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(10, 0, 0),
           FreeCAD.Vector(10, 10, 0), FreeCAD.Vector(0, 10, 0)]
    sk.addGeometry([Part.LineSegment(pts[i], pts[(i + 1) % 4]) for i in range(4)], False)
    pad = body.newObject("PartDesign::Pad", "Pad")
    pad.Profile = sk
    pad.Length = 5
    doc.recompute()
    FreeCADGui.SendMsgToActiveView("ViewFit")
    FreeCADGui.activeDocument().activeView().viewIsometric()
    FreeCADGui.SendMsgToActiveView("ViewFit")
    pump()
    gdoc = FreeCADGui.getDocument(DOC)
    gdoc.setEdit(pad, 0)
    pump(100)
    FreeCADGui.SendMsgToActiveView("ViewFit")
    pump(40)
    set_report(how)
    what = "a Pad, the Report view %s" % ("behind the console" if how == "behind" else "hidden")
    arrow = arrow_on_axis(5.5, 15.0)
    if not check("%s: the length arrow is found" % what, arrow is not None):
        return
    z0 = axis_point_under(arrow)
    failures = Failures("Pad")
    FreeCAD.addDocumentObserver(failures)
    win = mw().windowHandle()
    places = []
    at_failure = None
    try:
        p = to_window(*arrow)
        QtTest.QTest.mouseMove(win, p)
        pump(5)
        QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
        pump(5)
        q = p
        places.append(view_place())
        for k in range(1, 21):
            x, y = view().getPointOnViewport(FreeCAD.Vector(5, 5, z0 - 0.5 * k))
            q = to_window(x, y)
            before = failures.count
            QtTest.QTest.mouseMove(win, q)
            pump(8)
            places.append(view_place())
            if at_failure is None and failures.count > before:
                cursor = QtWidgets.QApplication.overrideCursor()
                at_failure = {
                    "move": k,
                    "arrow": arrow_on_axis(-1.0, 8.0) is not None,
                    "cursor": cursor.shape() if cursor is not None else None,
                    "report up": report_up(),
                }
        pump(30)
        QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, q)
        pump(250)   # past the pop's wait for the release's recompute
    finally:
        FreeCAD.removeDocumentObserver(failures)
    note("INFO %s: the view's size and place %s; at the failing move %s; ended Reversed %s %s"
         % (what, sorted(set(places)), at_failure, pad.Reversed, pad.Length))
    check("%s: a recompute failed in the drag" % what, failures.count >= 1, failures.count)
    check("%s: the view's size and place the same at every move" % what,
          len(set(places)) == 1, sorted(set(places)))
    check("%s: the error written in the Report view" % what,
          "Length too small" in report_text(), report_text()[-200:])
    if at_failure is not None:
        check("%s: the length arrow shown at the failing move" % what, at_failure["arrow"],
              at_failure)
        check("%s: the stop-sign cursor up at the failing move" % what,
              at_failure["cursor"] == Qt.ForbiddenCursor, at_failure)
        check("%s: the Report view not popped in the drag" % what, not at_failure["report up"],
              at_failure)
    check("%s: after the release, the pad working, the Report view not popped" % what,
          pad.isValid() and not report_up(), (pad.State, report_up()))
    gdoc.resetEdit()
    pump(40)
    FreeCAD.closeDocument(DOC)
    pump(20)


def wedge_edges(wedge):
    shape = wedge.Shape
    top = bottom = None
    for i, e in enumerate(shape.Edges):
        ends = e.Vertexes
        if len(ends) != 2:
            continue
        if (all(abs(v.Point.y - shape.BoundBox.YMax) < 1e-7 for v in ends)
                and all(abs(v.Point.z - wedge.Z2min.Value) < 1e-7 for v in ends)):
            top = "Edge%d" % (i + 1)
        if (all(abs(v.Point.y - shape.BoundBox.YMin) < 1e-7 for v in ends)
                and all(abs(v.Point.z - wedge.Zmin.Value) < 1e-7 for v in ends)):
            bottom = "Edge%d" % (i + 1)
    return top, bottom


def fillet_leg():
    """A fillet in error throughout (its other edge missing): the error
    stands after the release, and the pop comes then"""
    doc = FreeCAD.newDocument(DOC)
    body = doc.addObject("PartDesign::Body", "Body")
    wedge = body.newObject("PartDesign::AdditiveWedge", "Wedge")
    doc.recompute()
    top, bottom = wedge_edges(wedge)
    bottom_edge = wedge.Shape.getElement(bottom)
    fillet = body.newObject("PartDesign::Fillet", "Fillet")
    fillet.Base = (wedge, [top, bottom])
    fillet.Radius = 2.0
    fillet.Segments = [(top, [(0.0, 1.0), (1.0, 1.5)])]
    doc.recompute()
    wedge.X2min = 5
    wedge.X2max = 5
    doc.recompute()
    FreeCADGui.SendMsgToActiveView("ViewFit")
    FreeCADGui.activeDocument().activeView().viewIsometric()
    pump()
    gdoc = FreeCADGui.getDocument(DOC)
    gdoc.setEdit(fillet, 0)
    pump(80)
    set_report("hidden")
    what = "a fillet beside a missing edge, the Report view hidden"
    middle = bottom_edge.valueAt((bottom_edge.FirstParameter + bottom_edge.LastParameter) / 2)
    cx, cy = view().getPointOnViewport(middle)
    arrow = None
    for r in range(0, 70, 2):
        for dx in range(-r, r + 1, 2):
            for dy in ((-r, r) if abs(dx) != r else range(-r, r + 1, 2)):
                if arrow is None and dragger_at(cx + dx, cy + dy):
                    arrow = (cx + dx, cy + dy)
        if arrow is not None:
            break
    if not check("%s: the bottom edge's arrow is found" % what, arrow is not None):
        return
    check("%s: the fillet in error before the drag" % what, not fillet.isValid(), fillet.State)
    win = mw().windowHandle()
    places = []
    up_in_drag = False
    dx, dy = cx - arrow[0], cy - arrow[1]
    length = max(1.0, (dx * dx + dy * dy) ** 0.5)
    p = to_window(*arrow)
    QtTest.QTest.mouseMove(win, p)
    pump(5)
    QtTest.QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
    pump(5)
    q = p
    places.append(view_place())
    for k in range(1, 7):
        q = to_window(arrow[0] + dx / length * 2 * k, arrow[1] + dy / length * 2 * k)
        QtTest.QTest.mouseMove(win, q)
        pump(20)
        places.append(view_place())
        up_in_drag = up_in_drag or report_up()
    pump(60)
    QtTest.QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, q)
    pump(250)
    note("INFO %s: radius after %s; the view %s; the Report view up in the drag %s, after %s"
         % (what, fillet.Radius, sorted(set(places)), up_in_drag, report_up()))
    check("%s: the Report view not popped in the drag" % what, not up_in_drag)
    check("%s: the view's size and place the same in the drag" % what, len(set(places)) == 1,
          sorted(set(places)))
    check("%s: after the release, the error standing, the Report view popped" % what,
          not fillet.isValid() and report_up(), (fillet.State, report_up()))
    gdoc.resetEdit()
    pump(40)
    FreeCAD.closeDocument(DOC)
    pump(20)


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/OutputWindow").SetBool(
            "checkShowReportViewOnError", True)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Gui/Gizmos").SetBool(
            "EnableCoarseSnap", False)
        mw().activateWindow()
        # hidden first, on the rig's layout as it comes: the Report view
        # shown by a pop then takes room of its own
        pad_leg("hidden")
        pad_leg("behind")
        fillet_leg()
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
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
