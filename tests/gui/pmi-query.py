"""What applies to a face, and cross-highlighting (docs/GDandT.md sec 6.5).

A PartDesign Body -- a 40 x 20 x 10 box with a through hole -- with a
flatness on its top face, datums A (bottom) and B (left) and a position of
the hole to |A|B. Seen from the top, what is checked:

  - a callout selected outlines its faces in the selection colour: the box's
    rim, far from the callout, turns green;
  - a face picked lights what applies to it: the top face its flatness, the
    bottom face datum A and the position citing it; a face with nothing on
    it lights nothing; a face preselected lights as a pick does;
  - the query panel lists what applies to the face picked, keeps the list
    when a callout in it is clicked, and the click selects that callout.

  scripts/gui-test.sh tests/gui/pmi-query.py /tmp/pmi-query
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
    with open(RESULT, "a") as f:
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


def capture(view, name):
    path = os.path.join(OUT, name)
    view.saveImage(path, 800, 600, "#FFFFFF")
    return QtGui.QImage(path)


def changed(a, b, left):
    """Pixels that differ between two captures, in their left fraction"""
    n = 0
    for y in range(0, a.height(), 2):
        for x in range(0, int(a.width() * left), 2):
            if a.pixel(x, y) != b.pixel(x, y):
                n += 1
    return n


def colours(view, name, left=1.0):
    """(green, blue) pixels of a capture, in its left fraction"""
    img = capture(view, name)
    green = blue = 0
    for y in range(0, img.height(), 2):
        for x in range(0, int(img.width() * left), 2):
            c = QtGui.QColor(img.pixel(x, y))
            if c.green() > 120 and c.green() - c.red() > 60 and c.green() - c.blue() > 60:
                green += 1
            if c.blue() > 110 and c.blue() - c.red() > 60 and c.blue() - c.green() > 30:
                blue += 1
    return green, blue


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetBool("ShowNaviCube", False)
        doc = App.newDocument("PMIQuery")
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

        def feature(point, kind=None):
            faces = [face_at(solid.Shape, point)]
            return PMI.feature(pmi, solid, faces, kind) if kind else PMI.feature(pmi, solid, faces)

        def add(kind, name, **props):
            obj = doc.addObject("PMI::" + kind, name)
            pmi.addObject(obj)
            for k, v in props.items():
                setattr(obj, k, v)
            return obj

        flat = add("GeometricTolerance", "Flatness", Characteristic="Flatness",
                   Feature=feature(V(5, 5, 10)), Value=0.05)
        add("Datum", "DatumA", Letter="A", Feature=feature(V(5, 5, 0)))
        add("Datum", "DatumB", Letter="B", Feature=feature(V(0, 5, 5)))
        drf = PMI.frame(pmi, ["A", "B"])
        pos = add("GeometricTolerance", "Position", Characteristic="Position",
                  Feature=feature(V(23, 10, 5), "Axis"), Value=0.1, ZoneShape="Diameter",
                  DatumSystem=drf)
        doc.recompute()
        Gui.activateWorkbench("PMIWorkbench")
        pump(30)
        view = Gui.ActiveDocument.ActiveView
        view.viewTop()
        Gui.SendMsgToActiveView("ViewFit")
        pump(30)

        def pick(point):
            return "%s.%s" % (hole.Name, face_at(hole.Shape, point))

        # A callout selected outlines its faces
        Gui.Selection.clearSelection()
        pump(10)
        none = capture(view, "none.png")
        Gui.Selection.addSelection(flat)
        pump(20)
        n = changed(none, capture(view, "flat-selected.png"), 0.3)
        check("a callout selected outlines its faces", n > 50,
              "%d pixels changed left of the callouts" % n)
        Gui.Selection.clearSelection()
        pump(20)
        n = changed(none, capture(view, "cleared.png"), 1.0)
        check("and the outline goes with the selection", n < 5, n)

        # A face picked lights what applies
        _, blue0 = colours(view, "plain.png")
        Gui.Selection.addSelection(doc.Name, body.Name, pick(V(5, 5, 10)))
        pump(20)
        g, blue = colours(view, "top-picked.png")
        check("the top face picked lights its flatness", g > 100 and blue < blue0,
              "green %d, blue %d of %d" % (g, blue, blue0))
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(doc.Name, body.Name, pick(V(5, 0, 5)))
        pump(20)
        g, blue = colours(view, "front-picked.png")
        # Its own highlight is the model's (green, seen edge on): the
        # callouts stay as they were
        check("a face with nothing on it lights nothing", blue >= blue0 - 20,
              "blue %d of %d" % (blue, blue0))
        Gui.Selection.clearSelection()
        pump(10)
        Gui.Selection.setPreselection(body, pick(V(5, 5, 10)))
        pump(20)
        g, _ = colours(view, "top-preselected.png")
        check("a face preselected lights as a pick does", g > 100, g)
        Gui.Selection.clearPreselection()
        pump(20)
        g, _ = colours(view, "unpreselected.png")
        check("and stops when it is not", g < 5, g)
        Gui.Selection.addSelection(doc.Name, body.Name, pick(V(5, 5, 0)))
        pump(20)
        g, blue = colours(view, "bottom-picked.png")
        # The flatness and datum B stay blue
        check("the bottom face lights datum A and the position citing it",
              g > 300 and blue0 / 5 < blue < blue0 / 2,
              "green %d, blue %d of %d" % (g, blue, blue0))
        Gui.Selection.clearSelection()
        pump(10)

        # The query panel
        Gui.runCommand("PMI_Query")
        pump(20)
        panel = [w for w in Gui.getMainWindow().findChildren(QtWidgets.QWidget, "PMITaskQuery")
                 if w.isVisible()]
        check("the query opens its panel", len(panel) == 1)
        listing = panel[0].findChild(QtWidgets.QListWidget, "applies")
        Gui.Selection.addSelection(doc.Name, body.Name, pick(V(5, 5, 0)))
        pump(20)
        items = [listing.item(i).text() for i in range(listing.count())]
        check("the bottom face: datum A on it, the position citing it",
              len(items) == 2 and items[0].startswith("DatumA")
              and items[1].startswith("Position") and "datum A" in items[1], items)
        listing.itemClicked.emit(listing.item(1))
        pump(20)
        sel = Gui.Selection.getSelection()
        check("a callout clicked in the list is selected, and the list stays",
              sel == [pos] and listing.count() == 2, ([o.Name for o in sel], listing.count()))
        for box_ in Gui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
            b = box_.button(QtWidgets.QDialogButtonBox.Close)
            if b is not None and b.isVisible():
                b.click()
        pump(20)
        check("Close closes it", not [w for w in Gui.getMainWindow().findChildren(
            QtWidgets.QWidget, "PMITaskQuery") if w.isVisible()])
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    Gui.Selection.clearSelection()
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
