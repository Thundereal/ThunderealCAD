"""PMI views driven as a user drives them (docs/GDandT.md sec 6.3).

A PartDesign Body -- a 40 x 20 x 10 box with a through hole -- with a
flatness on its top face and a front view of the annotations. What is
checked:

  - activated, the view sets the camera to its own, and only the callouts
    it shows are drawn: none at first, the flatness once it is a member;
  - a datum made while the view is active joins it and lies in its plane,
    not in its face's;
  - its clipping is applied while it is active;
  - let go, the camera and the clipping come back, and every callout shows;
  - a view listing its Objects shows only them while it is active, in that
    3D view only (the others' Visibility untouched), and they all come back
    when it is let go;
  - the PMI view command makes a view from the camera as it is, and makes
    it active;
  - Show in new 3D view opens one with the PMI view active in it, showing
    only that view's annotations and objects while the first 3D view is
    untouched; another PMI view made active in the first, each keeps its
    own; the new one closed, the first keeps its PMI view;
  - with the DoubleClickNewView preference a double click opens a new 3D
    view, and again brings that one up rather than opening another.

  scripts/gui-test.sh tests/gui/pmi-views.py /tmp/pmi-views
"""

import os
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = App.Vector
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
        time.sleep(0.01)


def face_at(shape, point):
    for i, face in enumerate(shape.Faces):
        if face.isInside(point, 1e-6, True):
            return "Face%d" % (i + 1)
    raise AssertionError("no face at %s" % point)


def blue(view, name):
    path = os.path.join(OUT, name)
    view.saveImage(path, 800, 600, "#FFFFFF")
    img = QtGui.QImage(path)
    n = 0
    for y in range(0, img.height(), 2):
        for x in range(0, img.width(), 2):
            c = QtGui.QColor(img.pixel(x, y))
            if c.blue() > 110 and c.blue() - c.red() > 60 and c.blue() - c.green() > 30:
                n += 1
    return n


def red(view, name):
    path = os.path.join(OUT, name)
    view.saveImage(path, 800, 600, "#FFFFFF")
    img = QtGui.QImage(path)
    n = 0
    for y in range(0, img.height(), 2):
        for x in range(0, img.width(), 2):
            c = QtGui.QColor(img.pixel(x, y))
            if c.red() > 120 and c.red() - c.green() > 60 and c.red() - c.blue() > 60:
                n += 1
    return n


def same(a, b):
    return a.isSame(b, 1e-6)


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetBool("ShowNaviCube", False)
        doc = App.newDocument("PMIViews")
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObjectAt("PartDesign::AdditiveBox", "Box")
        box.Length, box.Width, box.Height = 40, 20, 10
        hole = body.newObjectAt("PartDesign::SubtractiveCylinder", "Hole")
        hole.Radius, hole.Height = 3, 10
        hole.Placement = App.Placement(V(20, 10, 0), App.Rotation())
        doc.recompute()
        import PMI

        pmi = PMI.getSet(body)
        doc.recompute()
        solid = body.SolidBodies[0]
        top = PMI.feature(pmi, solid, [face_at(solid.Shape, V(5, 5, 10))])
        flat = doc.addObject("PMI::GeometricTolerance", "Flatness")
        pmi.addObject(flat)
        flat.Characteristic, flat.Feature, flat.Value = "Flatness", top, 0.05
        front = doc.addObject("PMI::View", "Front")
        pmi.addObject(front)
        front.Standard = "Front"
        doc.recompute()
        Gui.activateWorkbench("PMIWorkbench")
        pump(30)
        view = Gui.ActiveDocument.ActiveView
        view.viewIsometric()
        Gui.SendMsgToActiveView("ViewFit")
        pump(30)
        iso = view.getCameraOrientation()
        shown = blue(view, "iso-all.png")

        # Activated
        view.setActiveObject("pmiview", front)
        pump(30)
        check("activated, the camera is the view's",
              same(view.getCameraOrientation(), front.Camera.Rotation),
              (view.getCameraOrientation(), front.Camera.Rotation))
        none = blue(view, "front-none.png")
        flat.Views = [front]
        doc.recompute()
        pump(20)
        member = blue(view, "front-member.png")
        check("only its members are drawn: none, then the flatness",
              none < 20 and member > 100, "none %d, member %d" % (none, member))
        z = flat.Plane.Rotation.multVec(V(0, 0, 1))
        check("a member lies in the view's plane, face on", z.isEqual(V(0, -1, 0), 1e-9), z)

        # A datum made in it
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(doc.Name, body.Name,
                                   "%s.%s" % (hole.Name, face_at(hole.Shape, V(5, 5, 10))))
        pump(5)
        Gui.runCommand("PMI_Datum")
        pump(20)
        datum = [o for o in doc.Objects if o.TypeId == "PMI::Datum"][-1]
        z = datum.Plane.Rotation.multVec(V(0, 0, 1))
        check("a datum made in it joins it, in its plane",
              datum.Views == [front] and z.isEqual(V(0, -1, 0), 1e-9), (datum.Views, z))

        # Clipping
        front.ClipPlane = App.Placement(V(0, 10, 0), front.Camera.Rotation)
        front.Clip = True
        doc.recompute()
        pump(20)
        check("its clipping is applied", view.hasClippingPlane())

        # Let go
        view.setActiveObject("pmiview", None)
        pump(30)
        check("let go, the camera comes back", same(view.getCameraOrientation(), iso),
              (view.getCameraOrientation(), iso))
        check("and the clipping goes", not view.hasClippingPlane())
        again = blue(view, "iso-again.png")
        check("and every callout shows", again >= shown, "before %d, after %d" % (shown, again))

        # Its objects: what it does not list hidden while it is active, in
        # this view only, and back when it is let go
        other = doc.addObject("Part::Box", "Other")
        other.Placement = App.Placement(V(60, 0, 0), App.Rotation())
        other.ViewObject.ShapeColor = (1.0, 0.0, 0.0)
        bodies = doc.addObject("PMI::View", "Bodies")
        pmi.addObject(bodies)
        bodies.Standard = "Isometric"
        doc.recompute()
        pump(20)
        view.setActiveObject("pmiview", bodies)
        pump(30)
        both = red(view, "objects-all.png")
        bodies.Objects = [body]
        doc.recompute()
        pump(30)
        only = red(view, "objects-body.png")
        check("listing its objects, only they show: the other box hidden",
              both > 100 and only < 5, "all %d, listed %d" % (both, only))
        check("in the view only: the box's own Visibility untouched", other.Visibility)
        view.setActiveObject("pmiview", None)
        pump(30)
        back = red(view, "objects-back.png")
        check("let go, the box shows again", back > 100, back)
        view.setActiveObject("pmiview", bodies)
        pump(30)
        again = red(view, "objects-again.png")
        view.setActiveObject("pmiview", None)
        pump(30)
        check("made active again, its objects apply at once", again < 5, again)

        # The command
        view.viewTop()
        # The turn is animated: the camera is read once it is there, not
        # partway (0.2 s was partway on Windows, 7 degrees off)
        for _ in range(300):
            pump(1)
            if view.getViewDirection().isEqual(V(0, 0, -1), 1e-9):
                break
        pump(5)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(pmi)
        Gui.runCommand("PMI_View")
        pump(30)
        made = [o for o in doc.Objects if o.TypeId == "PMI::View"][-1]
        z = made.Camera.Rotation.multVec(V(0, 0, 1))
        check("the command makes a view from the camera, active",
              made != front and z.isEqual(V(0, 0, 1), 1e-6)
              and view.getActiveObject("pmiview") == made, (made.Name, z))
        view.setActiveObject("pmiview", None)
        pump(10)

        # From a perspective camera: kept as it is, the height it shows at
        # its focal distance -- none was kept, and shown again it was fitted
        # to the whole model
        import math

        def seen(cam):
            return 2 * cam.focalDistance.getValue() * math.tan(cam.heightAngle.getValue() / 2)

        view.setCameraType("Perspective")
        pump(20)
        Gui.SendMsgToActiveView("ViewFit")
        pump(10)
        view.getCameraNode().heightAngle.setValue(0.3)
        pump(10)
        want = seen(view.getCameraNode())
        Gui.runCommand("PMI_View")
        pump(30)
        persp = [o for o in doc.Objects if o.TypeId == "PMI::View"][-1]
        check("from a perspective camera the view keeps its height",
              persp.Perspective and abs(persp.Height.Value - want) < 1e-3 * want,
              (persp.Perspective, persp.Height.Value, want))
        view.setActiveObject("pmiview", None)
        view.getCameraNode().heightAngle.setValue(0.9)
        pump(10)
        view.setActiveObject("pmiview", persp)
        pump(30)
        got = seen(view.getCameraNode())
        check("shown again, it shows that height", abs(got - want) < 1e-3 * want, (got, want))
        view.setActiveObject("pmiview", None)
        view.setCameraType("Orthographic")
        pump(20)

        # Side by side: a PMI view in a new 3D view, the first untouched,
        # each 3D view showing a PMI view of its own
        gdoc = Gui.getDocument(doc.Name)

        def views3d():
            return gdoc.mdiViewsOfType("Gui::View3DInventor")

        # From the isometric, where the front view's callouts are not edge
        # on, nothing selected (a selected set draws them green)
        Gui.Selection.clearSelection()
        view.viewIsometric()
        Gui.SendMsgToActiveView("ViewFit")
        pump(30)
        before = len(views3d())
        all_callouts = blue(view, "side-first-all.png")
        Gui.Selection.addSelection(bodies)
        Gui.runCommand("PMI_ViewNewWindow")
        Gui.Selection.clearSelection()
        pump(40)
        fresh = [v for v in views3d() if v != view]
        nv = fresh[-1] if fresh else None
        check("Show in new 3D view opens one, the PMI view active in it",
              len(views3d()) == before + 1 and nv is not None
              and nv.getActiveObject("pmiview") == bodies
              and view.getActiveObject("pmiview") is None,
              (before, len(views3d())))
        if nv is not None:
            nv_blue, nv_red = blue(nv, "side-new-blue.png"), red(nv, "side-new-red.png")
            first_blue, first_red = (blue(view, "side-first-blue.png"),
                                     red(view, "side-first-red.png"))
            check("in the new one only its objects and annotations: none of its own, "
                  "the other box hidden",
                  nv_blue < 20 and nv_red < 5, "blue %d, red %d" % (nv_blue, nv_red))
            check("the first 3D view untouched: every callout, the other box",
                  all_callouts > 100 and first_blue >= all_callouts - 20 and first_red > 100,
                  "blue %d of %d, red %d" % (first_blue, all_callouts, first_red))
            view.setActiveObject("pmiview", front)
            pump(30)
            first_front = blue(view, "side-first-front.png")
            nv_still = blue(nv, "side-new-still.png"), red(nv, "side-new-still-red.png")
            check("another PMI view active in the first: each keeps its own",
                  first_front > 100 and nv_still[0] < 20 and nv_still[1] < 5
                  and nv.getActiveObject("pmiview") == bodies,
                  "first %d of %d, new %s" % (first_front, all_callouts, nv_still))
            nv.close()
            pump(40)
            check("the new one closed, the first keeps its PMI view",
                  len(views3d()) == before and view.getActiveObject("pmiview") == front
                  and same(view.getCameraOrientation(), front.Camera.Rotation))
            view.setActiveObject("pmiview", None)
            pump(30)
            check("and lets it go as before", blue(view, "side-first-back.png")
                  >= all_callouts - 20)

        # The preference: a double click opens a new 3D view, or brings up
        # the one the PMI view is active in
        grp = App.ParamGet("User parameter:BaseApp/Preferences/Mod/PMI")
        grp.SetBool("DoubleClickNewView", True)
        Gui.getMainWindow().setActiveWindow(view)
        pump(10)
        bodies.ViewObject.doubleClicked()
        pump(40)
        opened = [v for v in views3d() if v != view]
        check("with the preference on, a double click opens a new 3D view",
              len(views3d()) == before + 1 and opened
              and opened[-1].getActiveObject("pmiview") == bodies
              and view.getActiveObject("pmiview") is None, len(views3d()))
        Gui.getMainWindow().setActiveWindow(view)
        pump(10)
        bodies.ViewObject.doubleClicked()
        pump(30)
        check("double-clicked again, the one it is active in comes up, no other opened",
              len(views3d()) == before + 1
              and gdoc.ActiveView.getActiveObject("pmiview") == bodies,
              len(views3d()))
        grp.SetBool("DoubleClickNewView", False)
        for v in views3d():
            if v != view:
                v.close()
        pump(30)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


def close_modals():
    w = QtWidgets.QApplication.activeModalWidget()
    if w is not None:
        text = getattr(w, "text", lambda: "")()
        note("FAIL a modal dialog came up | " + str(text))
        w.close()


modal_timer = QtCore.QTimer()
modal_timer.timeout.connect(close_modals)
modal_timer.start(500)
QtCore.QTimer.singleShot(1500, run)
