"""The PMI workbench driven as a user drives it (docs/GDandT.md sec 3, 5-8).

A PartDesign Body -- a 40 x 20 x 10 box with a through hole of radius 3 at
(20, 10) -- and faces picked on it the way a click picks them, on the Body's
tip feature: (Body, "Hole.FaceN"). What is checked:

  - a geometric tolerance on the top face: the Body gets an annotation set
    and a solid body, the frame is a flatness on (Solid, "Face..."), drawn by
    PMIGui's view provider; the command opens the frame builder;
  - datums on the bottom and the front face take A and B;
  - one on the hole: a position of the hole's axis, in a diameter zone; the
    builder offers what a hole admits, and datums and MMR picked in it make
    the frame; Cancel puts back what it changed;
  - a dimension on the hole is its diameter, 6; on the two end faces, the
    width, 40;
  - the callouts are drawn: a top view has their blue, and has none with
    them hidden;
  - in PartDesign the PMI tool bar is not offered until the set is made
    active (a double-click), and goes when it is let go;
  - undo takes the last annotation away.

  scripts/gui-test.sh tests/gui/pmi-workbench.py /tmp/pmi-workbench
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


def pick(doc, body, tip, *points):
    Gui.Selection.clearSelection()
    for p in points:
        Gui.Selection.addSelection(doc.Name, body.Name, "%s.%s" % (tip.Name, face_at(tip.Shape, p)))
    pump(5)


def newest(doc, type_id):
    objs = [o for o in doc.Objects if o.TypeId == type_id]
    return objs[-1] if objs else None


def blue(path):
    img = QtGui.QImage(path)
    n = 0
    for y in range(0, img.height(), 2):
        for x in range(0, img.width(), 2):
            c = QtGui.QColor(img.pixel(x, y))
            if c.blue() > 110 and c.blue() - c.red() > 60 and c.blue() - c.green() > 30:
                n += 1
    return n


def panel():
    for w in Gui.getMainWindow().findChildren(QtWidgets.QWidget, "PMITaskGeometricTolerance"):
        if w.isVisible():
            return w
    return None


def press(which):
    for box in Gui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = box.button(which)
        if b is not None and b.isVisible():
            b.click()
            pump(20)
            return True
    return False


def toolbar():
    for tb in Gui.getMainWindow().findChildren(QtWidgets.QToolBar, "PMI"):
        return tb
    return None


def offered():
    tb = toolbar()
    return tb is not None and tb.isVisible() and tb.toggleViewAction().isVisible()


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetBool("ShowNaviCube", False)
        doc = App.newDocument("PMIWorkbench")
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObjectAt("PartDesign::AdditiveBox", "Box")
        box.Length, box.Width, box.Height = 40, 20, 10
        hole = body.newObjectAt("PartDesign::SubtractiveCylinder", "Hole")
        hole.Radius, hole.Height = 3, 10
        hole.Placement = App.Placement(V(20, 10, 0), App.Rotation())
        doc.recompute()
        Gui.activateWorkbench("PMIWorkbench")
        pump(30)
        view = Gui.ActiveDocument.ActiveView

        # A tolerance on the top face
        pick(doc, body, hole, V(5, 5, 10))
        Gui.runCommand("PMI_GeometricTolerance")
        pump(20)
        flat = newest(doc, "PMI::GeometricTolerance")
        pmi = newest(doc, "PMI::AnnotationSet")
        solids = body.SolidBodies
        check("a tolerance on the top face is a flatness", flat is not None
              and flat.Characteristic == "Flatness", flat and flat.Characteristic)
        check("the Body has an annotation set and a solid body",
              pmi is not None and pmi in body.Group and len(solids) == 1,
              (pmi and pmi.Name, [s.Name for s in solids]))
        refs = flat.Feature.References if flat else None
        check("its feature refers to the solid body",
              refs and refs[0][0] == solids[0], refs)
        check("drawn by PMIGui's view provider",
              flat.ViewObject.TypeId == "PMIGui::ViewProviderCallout", flat.ViewObject.TypeId)

        check("the command opens the frame builder", panel() is not None)
        press(QtWidgets.QDialogButtonBox.Ok)
        check("and OK closes it", panel() is None)

        # Datums
        pick(doc, body, hole, V(5, 5, 0))
        Gui.runCommand("PMI_Datum")
        pump(20)
        a = newest(doc, "PMI::Datum")
        pick(doc, body, hole, V(5, 0, 5))
        Gui.runCommand("PMI_Datum")
        pump(20)
        b = newest(doc, "PMI::Datum")
        check("datums take A and B", (a.Letter, b.Letter) == ("A", "B"), (a.Letter, b.Letter))

        # A tolerance on the hole: a position of its axis, its frame built
        pick(doc, body, hole, V(23, 10, 5))
        Gui.runCommand("PMI_GeometricTolerance")
        pump(20)
        pos = newest(doc, "PMI::GeometricTolerance")
        check("one on the hole is a position of its axis in a diameter zone",
              pos != flat and pos.Characteristic == "Position" and pos.ZoneShape == "Diameter"
              and pos.Feature.Kind == "Axis", (pos.Characteristic, pos.Feature.Kind))
        p = panel()
        chars = p.findChild(QtWidgets.QComboBox, "characteristic")
        offered_chars = [chars.itemText(i) for i in range(chars.count())]
        check("the builder offers what a hole admits, position first",
              offered_chars[0] == "Position" and "Cylindricity" in offered_chars
              and "Flatness" not in offered_chars, offered_chars)
        p.findChild(QtWidgets.QComboBox, "primary").setCurrentText("A")
        p.findChild(QtWidgets.QComboBox, "secondary").setCurrentText("B")
        p.findChild(QtWidgets.QComboBox, "material").setCurrentIndex(1)
        pump(10)
        drf = pos.DatumSystem
        check("datums picked in it make the frame |A|B, MMR on the hole",
              drf is not None and drf.Compartments == ["A", "B"]
              and pos.MaterialCondition == "Maximum"
              and pos.EquivalentText.endswith("|A|B"),
              (drf and drf.Compartments, pos.MaterialCondition, pos.EquivalentText.encode("unicode_escape")))
        issues = p.findChild(QtWidgets.QLabel, "issues").text()
        check("and it says there is no problem", "No problems" in issues, issues)
        press(QtWidgets.QDialogButtonBox.Ok)

        # Cancel puts it back
        Gui.ActiveDocument.setEdit(pos)
        pump(20)
        panel().findChild(QtWidgets.QDoubleSpinBox, "value").setValue(0.5)
        pump(10)
        changed = abs(pos.Value.Value - 0.5) < 1e-9
        press(QtWidgets.QDialogButtonBox.Cancel)
        check("Cancel puts back what the builder changed",
              changed and abs(pos.Value.Value - 0.1) < 1e-9, pos.Value)

        # Dimensions
        pick(doc, body, hole, V(23, 10, 5))
        Gui.runCommand("PMI_Dimension")
        pump(20)
        dia = newest(doc, "PMI::Dimension")
        check("a dimension on the hole is its diameter, 6",
              dia.Type == "Diameter" and abs(dia.Nominal.Value - 6) < 1e-6,
              (dia.Type, dia.Nominal))
        pick(doc, body, hole, V(0, 5, 5), V(40, 5, 5))
        Gui.runCommand("PMI_Dimension")
        pump(20)
        width = newest(doc, "PMI::Dimension")
        check("on the two end faces, the width, 40",
              width.Type == "Width" and abs(width.Nominal.Value - 40) < 1e-6,
              (width.Type, width.Nominal))
        bad = [o.Name for o in doc.Objects if getattr(o, "Condition", "Ok") == "Invalid"]
        check("nothing invalid", not bad, bad)

        # Drawn
        Gui.Selection.clearSelection()
        view.viewTop()
        Gui.SendMsgToActiveView("ViewFit")
        pump(30)
        shot = os.path.join(OUT, "pmi-top.png")
        view.saveImage(shot, 800, 600, "#FFFFFF")
        n = blue(shot)
        callouts = [o for o in doc.Objects if hasattr(o, "Presentation")]
        for o in callouts:
            o.ViewObject.Visibility = False
        pump(20)
        bare = os.path.join(OUT, "pmi-top-hidden.png")
        view.saveImage(bare, 800, 600, "#FFFFFF")
        m = blue(bare)
        check("the callouts are drawn: blue in a top view, none with them hidden",
              n > 200 and m < n / 20, "with %d, without %d" % (n, m))
        for o in callouts:
            o.ViewObject.Visibility = True
        pump(10)

        # The tool bar in PartDesign
        Gui.activateWorkbench("PartDesignWorkbench")
        pump(40)
        check("in PartDesign the PMI tool bar is not offered", not offered(),
              toolbar() and toolbar().isVisible())
        pmi.ViewObject.doubleClicked()
        pump(30)
        check("the set made active offers it",
              view.getActiveObject("pmi") == pmi and offered())
        pmi.ViewObject.doubleClicked()
        pump(30)
        check("let go, it goes", view.getActiveObject("pmi") is None and not offered())

        # Undo
        before = len([o for o in doc.Objects if o.TypeId == "PMI::Dimension"])
        doc.undo()
        pump(20)
        after = len([o for o in doc.Objects if o.TypeId == "PMI::Dimension"])
        check("undo takes the last annotation away", after == before - 1, (before, after))
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
    # A command that refuses shows a message box, which would hang the run:
    # it is closed and said
    w = QtWidgets.QApplication.activeModalWidget()
    if w is not None:
        text = getattr(w, "text", lambda: "")()
        note("FAIL a modal dialog came up | " + str(text))
        w.close()


modal_timer = QtCore.QTimer()
modal_timer.timeout.connect(close_modals)
modal_timer.start(500)
QtCore.QTimer.singleShot(1500, run)
