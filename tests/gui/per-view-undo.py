"""Each view's undo (docs/MultiViewEdit.md sec 12.9, docs/HandsOnQueue.md
entry 227, ruled 2026-10-30: "for 3D views it is 3. other views 1").

Claims, the log on (its default):
  - a 3D view in edit lists the edit's own steps only, and its Std_Undo
    takes back the newest of them; with PerViewEdit off another 3D view of
    the document is in the edit too, and lists the same;
  - with PerViewEdit on, a 3D view in no edit lists every step but those of
    the edit another view has open, and its Std_Undo leaves those alone;
  - a TechDraw page lists the steps made in it and those made in no view (a
    script's), not the 3D view's edit, and its Std_Undo takes back its own
    step from under the edit's, the edit still open;
  - a toolbar command is the active view's: a group made from the toolbar
    with a 3D view active is listed there, not on the page;
  - with the edit ended, the 3D view lists every step again.
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


def type_into(view, name, text):
    """Type `text` into the visible box `name` of the dialog of `view`: a
    spin box then Enter, a line edit then its focus leaving it"""
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
            if b.objectName() == name and (box is None or not box.isVisible()):
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
    """Past the coalescing window (TransactionLogJournal sec 37.4 P4)"""
    settle(3500)


def undo(view):
    activate(view)
    FreeCADGui.runCommand("Std_Undo")
    settle(800)


def tool_button(command):
    for b in FreeCADGui.getMainWindow().findChildren(QtWidgets.QToolButton):
        a = b.defaultAction()
        if a is not None and a.objectName() == command and b.isVisible():
            return b
    return None


def run():
    try:
        VIEW.SetBool("PerViewEdit", False)
        VIEW.SetBool("EditStepEachChange", True)
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        doc = FreeCAD.newDocument("PerViewUndo")
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
        block = doc.addObject("Part::Box", "Block")
        block.Length, block.Width, block.Height = 40, 20, 10
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
        tview.Source = [block]
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
        va = gdoc.mdiViewsOfType("Gui::View3DInventor")[0]
        activate(va)
        FreeCADGui.runCommand("Std_ViewCreate")
        settle(1000)
        vb = [v for v in gdoc.mdiViewsOfType("Gui::View3DInventor") if v != va][0]
        page.ViewObject.doubleClicked()
        settle(1500)
        pv = gdoc.activeView()

        # A script's step: in no view, so every view's
        doc.openTransaction("Script step")
        block.Length = 41
        doc.commitTransaction()
        settle(300)

        # A step of the page's: its dimension's panel
        spec0 = dim.FormatSpec
        activate(pv)
        dim.ViewObject.doubleClicked()
        settle(1500)
        type_into(pv, "leFormatSpecifier", "%.1f A")
        dialog(pv).accept()
        settle(1500)
        pace()
        page_list = pv.undoActions()
        check("page: lists its own step and the script's",
              "Script step" in page_list and len(page_list) >= 2, page_list)

        # A toolbar command with the 3D view active: the 3D view's
        activate(va)
        button = tool_button("Std_Group")
        if check("the group tool's button is there", button is not None):
            groups = len(doc.findObjects("App::DocumentObjectGroup"))
            QtTest.QTest.mouseClick(button, Qt.LeftButton)
            settle(1000)
            made = len(doc.findObjects("App::DocumentObjectGroup")) == groups + 1
            group_step = doc.UndoNames[0] if made else None
            check("a group made from the toolbar", made, doc.UndoNames)
            check("toolbar: listed in the 3D view it was made in",
                  va.undoActions()[:1] == [group_step], va.undoActions())
            check("toolbar: not listed on the page",
                  pv.undoActions() == page_list, (pv.undoActions(), page_list))
        pace()

        # The pad in edit in the first 3D view
        activate(va)
        FreeCAD.setActiveTransaction("Edit Pad")
        gdoc.setEdit(pad, 0)
        settle(1500)
        type_into(va, "lengthEdit", "7")
        whole = doc.UndoNames
        edit_list = va.undoActions()
        check("3D view in edit: lists the edit's step only",
              len(edit_list) == 1 and edit_list[0] == whole[0], (edit_list, whole))
        check("other 3D view, in the edit too (PerViewEdit off): the same",
              vb.undoActions() == edit_list, (vb.undoActions(), edit_list))
        check("page: not the 3D view's edit",
              pv.undoActions() == page_list, (pv.undoActions(), page_list))

        # The page's undo takes its own step, from under the edit's
        undo(pv)
        check("page: Std_Undo takes back its step from under the edit's",
              dim.FormatSpec == spec0 and round(pad.Length.Value, 3) == 7.0,
              (dim.FormatSpec, pad.Length))
        check("page: the edit is still open", dialog(va) is not None, dialog(va))

        # The edit's own undo
        undo(va)
        check("3D view in edit: Std_Undo takes back the edit's step",
              round(pad.Length.Value, 3) == 15.0 and dialog(va) is not None,
              (pad.Length, dialog(va)))
        dialog(va).reject()
        settle(1500)
        check("edit ended: the 3D view lists every step again",
              va.undoActions() == doc.UndoNames, (va.undoActions(), doc.UndoNames))
        pace()

        # PerViewEdit on: the other 3D view is in no edit
        VIEW.SetBool("PerViewEdit", True)
        activate(va)
        FreeCAD.setActiveTransaction("Edit Pad")
        gdoc.setEdit(pad, 0)
        settle(1500)
        type_into(va, "lengthEdit", "8")
        whole = doc.UndoNames
        check("per view: the edit's view lists its step only",
              va.undoActions() == whole[:1], (va.undoActions(), whole))
        check("per view: a 3D view in no edit lists every step but the edit's",
              vb.undoActions() == whole[1:], (vb.undoActions(), whole))
        undo(vb)
        check("per view: its Std_Undo leaves the edit's step",
              round(pad.Length.Value, 3) == 8.0 and doc.UndoNames[:1] == whole[:1]
              and len(doc.UndoNames) == len(whole) - 1, (pad.Length, doc.UndoNames))
        dialog(va).reject()
        settle(1500)
        check("per view: Cancel takes the edit back",
              round(pad.Length.Value, 3) == 15.0 and dialog(va) is None,
              (pad.Length, dialog(va)))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    VIEW.SetBool("PerViewEdit", False)
    try:
        FreeCADGui.resetEdit()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
