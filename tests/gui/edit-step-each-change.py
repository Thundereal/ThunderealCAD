"""Each change in an edit's task panel an undo step of its own, with
View/EditStepEachChange (on by default) and PerViewEdit off, its default.

Claims, for a pad (PartDesign's panels), a linear pattern (Gui::EditSteps)
and a TechDraw dimension:
  - two values typed in the panel are two undo steps;
  - Std_Undo with the dialog still open takes back the last of them only,
    and the dialog stays open;
  - Cancel takes back the rest, and the edit ends;
  - OK keeps what was typed and ends the edit.
And with EditStepEachChange off, the control: the pad's two values are one
undo step, made at OK, as before.

The two values are typed further apart than Document/
TransactionLogCoalesceSeconds (3 s): two writes of one property by hand
closer than that are folded into one step, as ruled
(docs/TransactionLogJournal.md sec 37.4 P4).
"""
import glob
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets
from PySide6 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = FreeCAD.Vector
Qt = QtCore.Qt
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
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


def dialog(view):
    return FreeCADGui.Control.activeTaskDialog(view=view)


def type_into(view, name=None, cls=None, text=""):
    """Type `text` into the visible box `name` (or of class `cls`) of the
    dialog of `view`: a spin box then Enter, a line edit then its focus
    leaving it, as Enter there is the dialog's OK."""
    activate(view)
    dlg = dialog(view)
    if dlg is None:
        return False
    box = None
    for w in dlg.getDialogContent():
        for b in w.findChildren(QtWidgets.QWidget):
            if not isinstance(b, (QtWidgets.QAbstractSpinBox, QtWidgets.QLineEdit)):
                continue
            if isinstance(b, QtWidgets.QLineEdit) and isinstance(
                    b.parent(), QtWidgets.QAbstractSpinBox):
                continue
            hit = (name and b.objectName() == name) or (
                cls and b.metaObject().className() == cls)
            if hit and (box is None or not box.isVisible()):
                box = b
    if box is None:
        return False
    box.window().activateWindow()
    box.setFocus()
    settle(20)
    spin = isinstance(box, QtWidgets.QAbstractSpinBox)
    line = box.findChild(QtWidgets.QLineEdit) if spin else box
    line.selectAll()
    QtTest.QTest.keyClicks(line, text)
    if spin:
        QtTest.QTest.keyClick(box, Qt.Key_Return)
    settle(300)
    FreeCADGui.getMainWindow().setFocus()
    settle(500)
    return True


def pace():
    """Past the coalescing window (sec 37.4 P4), as a person types two values"""
    settle(3500)


def undo(view):
    activate(view)
    FreeCADGui.runCommand("Std_Undo")
    settle(800)


def run():
    try:
        VIEW.SetBool("PerViewEdit", False)
        VIEW.SetBool("EditStepEachChange", True)
        doc = FreeCAD.newDocument("EditStepEach")
        body = doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        sketch.Support = (doc.getObject("XY_Plane"), [""])
        sketch.MapMode = "FlatFace"
        corners = [V(0, 0, 0), V(10, 0, 0), V(10, 10, 0), V(0, 10, 0)]
        for i in range(4):
            sketch.addGeometry(Part.LineSegment(corners[i], corners[(i + 1) % 4]))
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 15
        pattern = body.newObject("PartDesign::LinearPattern", "Pattern")
        pattern.Originals = [pad]
        pattern.Direction = (doc.getObject("X_Axis"), [""])
        pattern.Length = 40
        pattern.Occurrences = 2
        part = doc.addObject("Part::Box", "Block")
        part.Length, part.Width, part.Height = 40, 20, 10
        doc.recompute()
        templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                           "Templates", "**", "*A4*Landscape*.svg"),
                              recursive=True)
        page = doc.addObject("TechDraw::DrawPage", "Page")
        tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        if templates:
            tmpl.Template = templates[0]
        page.Template = tmpl
        tview = doc.addObject("TechDraw::DrawViewPart", "TView")
        page.addView(tview)
        tview.Source = [part]
        tview.ScaleType = "Custom"
        tview.Scale = 1.0
        tview.X, tview.Y = 120, 120
        doc.recompute()
        settle(3000)
        dim = doc.addObject("TechDraw::DrawViewDimension", "Dim")
        dim.Type = "Distance"
        dim.References2D = [(tview, "Edge0")]
        page.addView(dim)
        doc.recompute()
        settle(1500)
        doc.UndoMode = 1
        doc.clearUndos()
        gdoc = FreeCADGui.getDocument(doc.Name)
        v3d = gdoc.mdiViewsOfType("Gui::View3DInventor")[0]

        def edit(obj, name, view=v3d):
            activate(view)
            FreeCAD.setActiveTransaction(name)
            gdoc.setEdit(obj, 0)
            settle(1500)

        # A pad: PartDesign's panel
        edit(pad, "Edit Pad")
        before = len(doc.UndoNames)
        type_into(v3d, "lengthEdit", text="7")
        pace()
        type_into(v3d, "lengthEdit", text="8")
        check("pad: two values typed are two undo steps",
              len(doc.UndoNames) == before + 2 and round(pad.Length.Value, 3) == 8.0,
              (doc.UndoNames, pad.Length))
        undo(v3d)
        check("pad: Std_Undo in the edit takes back the last only, the dialog open",
              round(pad.Length.Value, 3) == 7.0 and dialog(v3d) is not None,
              (pad.Length, dialog(v3d)))
        dialog(v3d).reject()
        settle(1500)
        check("pad: Cancel takes back the rest and ends the edit",
              round(pad.Length.Value, 3) == 15.0 and gdoc.getInEdit() is None,
              (pad.Length, gdoc.getInEdit()))
        check("pad: and leaves the undo list as before the edit, no step of its own",
              len(doc.UndoNames) == before, doc.UndoNames)
        edit(pad, "Edit Pad")
        type_into(v3d, "lengthEdit", text="9")
        dialog(v3d).accept()
        settle(1500)
        check("pad: OK keeps the value and ends the edit",
              round(pad.Length.Value, 3) == 9.0 and gdoc.getInEdit() is None,
              (pad.Length, gdoc.getInEdit()))

        # A pattern: Gui::EditSteps
        edit(pattern, "Edit Pattern")
        before = len(doc.UndoNames)
        type_into(v3d, cls="Gui::UIntSpinBox", text="3")
        pace()
        type_into(v3d, cls="Gui::UIntSpinBox", text="4")
        check("pattern: two values typed are two undo steps",
              len(doc.UndoNames) == before + 2 and pattern.Occurrences == 4,
              (doc.UndoNames, pattern.Occurrences))
        undo(v3d)
        check("pattern: Std_Undo in the edit takes back the last only, the dialog open",
              pattern.Occurrences == 3 and dialog(v3d) is not None,
              (pattern.Occurrences, dialog(v3d)))
        dialog(v3d).reject()
        settle(1500)
        check("pattern: Cancel takes back the rest and ends the edit",
              pattern.Occurrences == 2 and gdoc.getInEdit() is None,
              (pattern.Occurrences, gdoc.getInEdit()))
        check("pattern: and leaves the undo list as before the edit, no step of its own",
              len(doc.UndoNames) == before, doc.UndoNames)

        # A TechDraw dimension, in its page's view
        page.ViewObject.doubleClicked()
        settle(1500)
        pv = gdoc.activeView()
        spec0 = dim.FormatSpec
        activate(pv)
        dim.ViewObject.doubleClicked()
        settle(1500)
        before = len(doc.UndoNames)
        type_into(pv, "leFormatSpecifier", text="%.1f A")
        pace()
        type_into(pv, "leFormatSpecifier", text="%.2f B")
        check("dimension: two values typed are two undo steps",
              len(doc.UndoNames) == before + 2 and dim.FormatSpec == "%.2f B",
              (doc.UndoNames, dim.FormatSpec))
        undo(pv)
        check("dimension: Std_Undo in the edit takes back the last only, the dialog open",
              dim.FormatSpec == "%.1f A" and dialog(pv) is not None,
              (dim.FormatSpec, dialog(pv)))
        dialog(pv).reject()
        settle(1500)
        check("dimension: Cancel takes back the rest and ends the edit",
              dim.FormatSpec == spec0 and dialog(pv) is None, (dim.FormatSpec, dialog(pv)))
        check("dimension: and leaves the undo list as before the edit, no step of its own",
              len(doc.UndoNames) == before, doc.UndoNames)

        # The control: EditStepEachChange off
        VIEW.SetBool("EditStepEachChange", False)
        edit(pad, "Edit Pad")
        before = len(doc.UndoNames)
        type_into(v3d, "lengthEdit", text="7")
        pace()
        type_into(v3d, "lengthEdit", text="8")
        dialog(v3d).accept()
        settle(1500)
        check("off: the pad's two values are one undo step, made at OK",
              len(doc.UndoNames) == before + 1 and round(pad.Length.Value, 3) == 8.0,
              (doc.UndoNames, pad.Length))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    VIEW.SetBool("EditStepEachChange", True)
    try:
        FreeCADGui.resetEdit()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
