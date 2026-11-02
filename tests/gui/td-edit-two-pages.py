"""TechDraw edits beside other views' edits, with PerViewEdit on
(docs/HandsOnQueue.md entry 232, the TechDraw round).

An edit of an object with a view of its own -- a TechDraw page's -- runs in
that view, as a 3D view's runs in it: one per page. Its panel holds no
transaction: each change is a step of its own (Gui::EditSteps), and its
Cancel takes back those and no other view's.

A box and a sketch; page P1 with a projection group (Front, Right) and
dimension D1 on its Front; page P2 with a view of the box, dimension D2 and
balloon B2 on it. One 3D window.

Claims:
  - D1 in edit in P1 and D2 in P2 at once, each page answering its own,
    each with its dialog, the tree marking both; D2's edit, started with
    the 3D window in front, is its page's;
  - Cancel of D2, with a step of D1's after its own, takes back D2's
    format only; D1 stays in edit;
  - the sketch in edit in the 3D window beside D1: Cancel of the sketch
    takes back its step and keeps D1's later one;
  - OK of D1 keeps its format and ends its edit only;
  - the balloon in edit in P2 beside the group's in P1: Cancel of the
    balloon takes back its text and leaves the group's edit and spacing;
    Cancel of the group takes back its spacing.
"""
import glob
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

Qt = QtCore.Qt
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = FreeCAD.Vector
state = {"done": False}

VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
# Each page in a window of its own: a split's cell would take the second
# page in place of the first (docs/ViewPlacement.md sec 3.2 step 6)
OPEN_VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/OpenView")
FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name
         + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()
    for _ in range(5):
        QtCore.QCoreApplication.processEvents()


def activate(view):
    FreeCADGui.getMainWindow().setActiveWindow(view)
    settle(150)


def in_edit(gdoc, view):
    activate(view)
    vp = gdoc.getInEdit()
    return vp.Object.Name if vp else None


def marked(name):
    """Whether the tree marks the item `name` as in edit (its background)."""
    for tree in FreeCADGui.getMainWindow().findChildren(QtWidgets.QTreeWidget):
        it = QtWidgets.QTreeWidgetItemIterator(tree)
        while it.value():
            item = it.value()
            if item.text(0) == name and item.background(0).style() != QtCore.Qt.NoBrush:
                return True
            it += 1
    return False


def edit(view, obj):
    """A double click on `obj` in the tree, with `view` in front."""
    activate(view)
    obj.ViewObject.doubleClicked()
    settle(1500)


def box_of(view, name, kind):
    dlg = FreeCADGui.Control.activeTaskDialog(view=view)
    if dlg is None:
        return None
    for w in dlg.getDialogContent():
        for b in w.findChildren(kind):
            if b.objectName() == name:
                return b
    return None


def type_into(view, name, text):
    """Type `text` over the line edit or spin box `name` of the dialog of
    `view`, then Enter in a spin box; a line edit is left by its focus, as
    Enter there is the dialog's OK."""
    activate(view)
    box = box_of(view, name, QtWidgets.QAbstractSpinBox) \
        or box_of(view, name, QtWidgets.QLineEdit)
    if box is None:
        return False
    box.window().activateWindow()
    box.setFocus()
    settle(20)
    line = box if isinstance(box, QtWidgets.QLineEdit) else box.findChild(QtWidgets.QLineEdit)
    line.selectAll()
    QtTest.QTest.keyClicks(line, text)
    if not isinstance(box, QtWidgets.QLineEdit):
        QtTest.QTest.keyClick(box, Qt.Key_Return)
    settle(300)
    FreeCADGui.getMainWindow().setFocus()
    settle(500)
    return True


def click_later(name, text):
    """Click `text` in the modal box `name` (its text) once it is up."""
    def click():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QMessageBox) and w.isVisible() \
                    and (w.objectName() == name or w.text() == name):
                for b in w.buttons():
                    if b.text().replace("&", "") == text:
                        state["asked"] = True
                        b.click()
                        return
        QtCore.QTimer.singleShot(100, click)
    state["asked"] = False
    QtCore.QTimer.singleShot(200, click)


def cancel(view):
    activate(view)
    FreeCADGui.Control.activeTaskDialog(view=view).reject()
    settle(1500)


def accept(view):
    activate(view)
    FreeCADGui.Control.activeTaskDialog(view=view).accept()
    settle(1500)


def value(q):
    return q.Value if hasattr(q, "Value") else q


def run():
    try:
        VIEW.SetBool("PerViewEdit", True)
        OPEN_VIEW.SetString("DocViewTarget", "Tab")
        doc = FreeCAD.newDocument("TdTwoPages")
        box = doc.addObject("Part::Box", "Box")
        box.Length, box.Width, box.Height = 40, 20, 10
        sketch = doc.addObject("Sketcher::SketchObject", "S")
        sketch.addGeometry(Part.LineSegment(V(0, 0, 0), V(10, 0, 0)))
        doc.recompute()
        templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                           "Templates", "**", "*A4*Landscape*.svg"),
                              recursive=True)

        def page(name):
            p = doc.addObject("TechDraw::DrawPage", name)
            t = doc.addObject("TechDraw::DrawSVGTemplate", name + "Template")
            if templates:
                t.Template = templates[0]
            p.Template = t
            return p

        p1 = page("P1")
        grp = doc.addObject("TechDraw::DrawProjGroup", "Group")
        p1.addView(grp)
        grp.Source = [box]
        front = grp.addProjection("Front")
        grp.addProjection("Right")
        grp.ScaleType = "Custom"
        grp.Scale = 1.0
        grp.AutoDistribute = True
        grp.X, grp.Y = 120, 120
        p2 = page("P2")
        v2 = doc.addObject("TechDraw::DrawViewPart", "V2")
        p2.addView(v2)
        v2.Source = [box]
        v2.ScaleType = "Custom"
        v2.Scale = 1.0
        v2.X, v2.Y = 120, 120
        doc.recompute()
        settle(4000)
        d1 = doc.addObject("TechDraw::DrawViewDimension", "D1")
        d1.Type = "Distance"
        d1.References2D = [(front, "Edge0")]
        p1.addView(d1)
        d2 = doc.addObject("TechDraw::DrawViewDimension", "D2")
        d2.Type = "Distance"
        d2.References2D = [(v2, "Edge0")]
        p2.addView(d2)
        b2 = doc.addObject("TechDraw::DrawViewBalloon", "B2")
        b2.SourceView = v2
        b2.OriginX, b2.OriginY = 0, 0
        b2.Text = "1"
        p2.addView(b2)
        doc.recompute()
        settle(2000)
        doc.UndoMode = 1
        doc.clearUndos()
        gdoc = FreeCADGui.getDocument(doc.Name)
        format0 = (d1.FormatSpec, d2.FormatSpec)
        spacing0 = value(grp.spacingX)

        v3d = gdoc.mdiViewsOfType("Gui::View3DInventor")[0]
        # Each page's view: the active one once it is opened
        p1.ViewObject.doubleClicked()
        settle(1500)
        pv1 = gdoc.activeView()
        p2.ViewObject.doubleClicked()
        settle(1500)
        pv2 = gdoc.activeView()
        if len(gdoc.mdiViewsOfType("TechDrawGui::MDIViewPage")) != 2 or pv1 is None or pv2 is None or pv1 == pv2:
            note("ABORT the page views are not both open")
            return

        # Two dimensions, one per page; D2's started with the 3D window in
        # front
        edit(pv1, d1)
        edit(v3d, d2)
        check("each page answers its own dimension",
              (in_edit(gdoc, pv1), in_edit(gdoc, pv2)) == ("D1", "D2"),
              (in_edit(gdoc, pv1), in_edit(gdoc, pv2)))
        check("the 3D window has none", in_edit(gdoc, v3d) is None, in_edit(gdoc, v3d))
        check("each page has its dialog",
              FreeCADGui.Control.activeTaskDialog(view=pv1) is not None
              and FreeCADGui.Control.activeTaskDialog(view=pv2) is not None)
        check("the tree marks both as in edit", (marked("D1"), marked("D2")) == (True, True),
              (marked("D1"), marked("D2")))

        # D2's step, then D1's
        check("D2's format typed in its panel", type_into(pv2, "leFormatSpecifier", "%.2f B"))
        check("D1's format typed in its panel", type_into(pv1, "leFormatSpecifier", "%.1f A"))
        check("each has what was typed", (d1.FormatSpec, d2.FormatSpec) == ("%.1f A", "%.2f B"),
              (d1.FormatSpec, d2.FormatSpec))
        check("and both are still in edit",
              (in_edit(gdoc, pv1), in_edit(gdoc, pv2)) == ("D1", "D2"),
              (in_edit(gdoc, pv1), in_edit(gdoc, pv2)))
        note("INFO undo steps %s" % list(doc.UndoNames))
        cancel(pv2)
        check("Cancel of D2 takes back its format", d2.FormatSpec == format0[1], d2.FormatSpec)
        check("and keeps D1's, though it came after", d1.FormatSpec == "%.1f A", d1.FormatSpec)
        check("D1 stays in edit, D2 does not",
              (in_edit(gdoc, pv1), in_edit(gdoc, pv2)) == ("D1", None),
              (in_edit(gdoc, pv1), in_edit(gdoc, pv2)))
        check("and the tree marks D1 only", (marked("D1"), marked("D2")) == (True, False),
              (marked("D1"), marked("D2")))

        # The sketch in the 3D window beside D1
        activate(v3d)
        gdoc.setEdit(sketch, 0)
        settle(1500)
        check("the sketch goes in edit beside D1",
              (in_edit(gdoc, v3d), in_edit(gdoc, pv1)) == ("S", "D1"),
              (in_edit(gdoc, v3d), in_edit(gdoc, pv1)))
        activate(v3d)
        click_later("Delete all geometry and constraints?", "Yes")
        FreeCADGui.runCommand("Sketcher_DeleteAllGeometry")
        settle(800)
        check("the sketch's step is made", len(sketch.Geometry) == 0, len(sketch.Geometry))
        check("D1's next format typed", type_into(pv1, "leFormatSpecifier", "%.3f C"))
        cancel(v3d)
        check("Cancel of the sketch takes back its step", len(sketch.Geometry) == 1,
              len(sketch.Geometry))
        check("and keeps D1's", d1.FormatSpec == "%.3f C", d1.FormatSpec)
        check("D1 stays in edit", in_edit(gdoc, pv1) == "D1", in_edit(gdoc, pv1))

        accept(pv1)
        check("OK of D1 keeps its format", d1.FormatSpec == "%.3f C", d1.FormatSpec)
        check("and ends its edit", in_edit(gdoc, pv1) is None, in_edit(gdoc, pv1))
        check("its tree mark with it", not marked("D1"))

        # The balloon in P2 beside the group in P1
        edit(pv1, grp)
        edit(pv2, b2)
        check("the group and the balloon in edit, one per page",
              (in_edit(gdoc, pv1), in_edit(gdoc, pv2)) == ("Group", "B2"),
              (in_edit(gdoc, pv1), in_edit(gdoc, pv2)))
        check("the group's spacing typed", type_into(pv1, "sbXSpacing", "37 mm"))
        check("the balloon's text typed", type_into(pv2, "leText", "Z9"))
        check("each has what was typed", (value(grp.spacingX), b2.Text) == (37.0, "Z9"),
              (value(grp.spacingX), b2.Text))
        note("INFO undo steps %s" % list(doc.UndoNames))
        cancel(pv2)
        check("Cancel of the balloon takes back its text", b2.Text == "1", b2.Text)
        check("and leaves the group's spacing", value(grp.spacingX) == 37.0, grp.spacingX)
        check("the group stays in edit", in_edit(gdoc, pv1) == "Group", in_edit(gdoc, pv1))
        cancel(pv1)
        check("Cancel of the group takes back its spacing", value(grp.spacingX) == spacing0,
              grp.spacingX)
        check("and ends its edit", in_edit(gdoc, pv1) is None, in_edit(gdoc, pv1))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    VIEW.SetBool("PerViewEdit", False)
    OPEN_VIEW.RemString("DocViewTarget")
    try:
        FreeCADGui.resetEdit()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
