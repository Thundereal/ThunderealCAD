"""A drawing and the model it draws, each in edit in a view of its own,
their changes interleaved with recomputes between them (docs/MultiViewEdit.md
sec 12.9; asked 2026-10-30: "any surprises").

With PartDesign's edit preview on (its default) the pad's shape, and the
drawing of it, wait for OK; the rest runs with it off, the model's shape
following each change.

The model: a pad in a body, in edit in a 3D view. The drawing: a page with
a front view of the body and a dimension of the pad's height, the dimension
in edit on the page. PerViewEdit on, each change a step of its own.

Claims:
  - each view's undo lists its own edit's steps only, whatever the other
    did and whatever recomputed between;
  - a recompute -- a script's, or Std_Refresh in either view -- makes no
    step of its own, and the drawing follows the model through it;
  - the page's undo of its step from under the model's later one takes
    back the format only: the model and the drawing of it stay;
  - the model's undo and redo, with a recompute after, move the drawing
    with it; the page's step is not touched;
  - OK on the page keeps its change; Cancel on the pad takes back the
    pad's own steps only, and the drawing follows;
  - from the 3D view, out of edit, everything can be undone back to where
    it started, and nothing is left touched or invalid.
Every stage also writes the whole state, for a reader to look over.
"""
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
PART = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Part")
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


def press(view, label):
    """Click the OK or Cancel of the dialog of `view`, as a user does: an
    input to that view's panel, so the step it makes is that view's"""
    activate(view)
    dlg = dialog(view)
    w = dlg.getDialogContent()[0] if dlg is not None else None
    while w is not None and w.metaObject().className() != "Gui::TaskView::TaskView":
        w = w.parentWidget()
    buttons = [] if w is None else [
        b for b in w.findChildren(QtWidgets.QAbstractButton)
        if b.isVisible() and b.text().replace("&", "") == label]
    if not check("the %s button of the view's panel" % label, len(buttons) == 1,
                 [b.text() for b in buttons]):
        (dlg.accept if label == "OK" else dlg.reject)()
    else:
        QtTest.QTest.mouseClick(buttons[0], Qt.LeftButton)
    settle(1500)


def run_in(view, command):
    activate(view)
    FreeCADGui.runCommand(command)
    settle(800)


class Rig:
    pass


R = Rig()


def raw():
    try:
        return round(R.dim.getRawValue(), 3)
    except Exception as e:
        return "err:%s" % e


def drawn():
    """The drawing settled: the height the dimension measures, waited for
    while the view's hidden-line work runs in its thread"""
    last = None
    for _ in range(40):
        settle(150)
        now = raw()
        if now == last and "Touched" not in R.tview.State:
            return now
        last = now
    return last


def height(obj):
    try:
        return round(obj.Shape.BoundBox.ZLength, 3)
    except Exception as e:
        return "err:%s" % e


def tallest():
    best = 0
    for i in range(64):
        try:
            e = R.tview.getEdgeByIndex(i)
        except Exception:
            break
        a, b = e.Vertexes[0].Point, e.Vertexes[-1].Point
        if abs(a.x - b.x) < 1e-6:
            best = max(best, abs(a.y - b.y))
    return round(best, 3)


def touched():
    return sorted(o.Name for o in R.doc.Objects if "Touched" in o.State)


def invalid():
    return sorted(o.Name for o in R.doc.Objects if "Invalid" in o.State)


def state(stage):
    note("STATE %s | pad %.3f | measured %s | format %r | touched %s | invalid %s"
         % (stage, R.pad.Length.Value, raw(), R.dim.FormatSpec, touched(), invalid()))
    note("      undo %s | redo %s" % (R.doc.UndoNames, R.doc.RedoNames))
    note("      pad shape %s | body shape %s | front view tallest edge %s | front %s"
         % (height(R.pad), height(R.body), tallest(), R.tview.State))
    note("      3D lists %s | page lists %s" % (R.va.undoActions(), R.pv.undoActions()))


def run():
    try:
        VIEW.SetBool("PerViewEdit", True)
        VIEW.SetBool("EditStepEachChange", True)
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        doc = R.doc = FreeCAD.newDocument("TdModelInterleave")
        body = R.body = doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        sketch.Support = (doc.getObject("XY_Plane"), [""])
        sketch.MapMode = "FlatFace"
        corners = [V(0, 0, 0), V(10, 0, 0), V(10, 10, 0), V(0, 10, 0)]
        for i in range(4):
            sketch.addGeometry(Part.LineSegment(corners[i], corners[(i + 1) % 4]))
        pad = R.pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 15
        doc.recompute()
        import glob
        templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                           "Templates", "**", "*A4*Landscape*.svg"),
                              recursive=True)
        page = doc.addObject("TechDraw::DrawPage", "Page")
        tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        if templates:
            tmpl.Template = templates[0]
        page.Template = tmpl
        tview = R.tview = doc.addObject("TechDraw::DrawViewPart", "Front")
        page.addView(tview)
        tview.Source = [body]
        tview.Direction = V(0, -1, 0)
        tview.ScaleType = "Custom"
        tview.Scale = 1.0
        tview.X, tview.Y = 120, 120
        doc.recompute()
        settle(3000)
        # The pad's height: a vertical edge of the front view, 15 long
        edge = None
        for i in range(32):
            try:
                e = tview.getEdgeByIndex(i)
            except Exception:
                break
            a, b = e.Vertexes[0].Point, e.Vertexes[-1].Point
            if abs(a.x - b.x) < 1e-6 and abs(abs(a.y - b.y) - 15) < 1e-3:
                edge = i
                break
        if not check("the front view has the pad's height as an edge", edge is not None):
            raise RuntimeError("no edge")
        dim = R.dim = doc.addObject("TechDraw::DrawViewDimension", "Height")
        dim.Type = "DistanceY"
        dim.References2D = [(tview, "Edge%d" % edge)]
        page.addView(dim)
        doc.recompute()
        settle(1500)
        check("set up: the dimension measures the pad", drawn() == 15.0, raw())
        note("page KeepUpdated %s" % page.KeepUpdated)
        pad.Length = 16
        doc.recompute()
        h = drawn()
        check("set up: with no edit, the drawing follows the model", h == 16.0, h)
        pad.Length = 15
        doc.recompute()
        drawn()
        doc.UndoMode = 1
        doc.clearUndos()
        gdoc = FreeCADGui.getDocument(doc.Name)
        va = R.va = gdoc.mdiViewsOfType("Gui::View3DInventor")[0]
        page.ViewObject.doubleClicked()
        settle(1500)
        pv = R.pv = gdoc.activeView()
        spec0 = dim.FormatSpec
        state("start")

        # 0. PartDesign's edit preview (Mod/Part PreviewOnEdit, on by
        # default): the feature in edit shows what it adds, its own shape
        # waits for OK (Feature::setPauseRecompute) -- and so does what is
        # made from it, the drawing too
        PART.SetBool("PreviewOnEdit", True)
        activate(va)
        FreeCAD.setActiveTransaction("Edit Pad")
        gdoc.setEdit(pad, 0)
        settle(1500)
        type_into(va, "lengthEdit", "18")
        doc.recompute()
        h = drawn()
        state("0a preview: pad 18 + recompute")
        check("0: with the preview, the drawing waits for OK",
              round(pad.Length.Value, 3) == 18.0 and height(pad) == 15.0 and h == 15.0,
              (pad.Length, height(pad), h))
        press(va, "OK")
        h = drawn()
        state("0b preview: OK")
        check("0: OK, and the drawing follows", height(pad) == 18.0 and h == 18.0,
              (height(pad), h))
        pace()
        # Undone one step at a time, each with a recompute after, to 15
        for i in range(4):
            if round(pad.Length.Value, 3) == 15.0 or not doc.UndoNames:
                break
            run_in(va, "Std_Undo")
            h = drawn()
            state("0c preview: undo %d" % (i + 1))
            doc.recompute()
            h = drawn()
            state("0c preview: undo %d + recompute" % (i + 1))
        check("0: undone", round(pad.Length.Value, 3) == 15.0 and h == 15.0,
              (pad.Length, h))
        doc.clearUndos()
        pace()
        # The rest without it: the model's shape follows each change
        PART.SetBool("PreviewOnEdit", False)

        # 1. Both in edit: the pad in the 3D view, the dimension on the page
        activate(va)
        FreeCAD.setActiveTransaction("Edit Pad")
        gdoc.setEdit(pad, 0)
        settle(1500)
        activate(pv)
        dim.ViewObject.doubleClicked()
        settle(1500)
        check("both in edit, each in its view",
              dialog(va) is not None and dialog(pv) is not None
              and dialog(va) is not dialog(pv), (dialog(va), dialog(pv)))
        state("1 both in edit")

        # 2. The model: 20, then a script's recompute
        type_into(va, "lengthEdit", "20")
        state("2a pad 20")
        n = len(doc.UndoNames)
        doc.recompute()
        h = drawn()
        state("2b script recompute")
        check("2: a script's recompute makes no step", len(doc.UndoNames) == n,
              doc.UndoNames)
        check("2: the drawing follows the model", h == 20.0, h)
        step_a = doc.UndoNames[0]
        check("2: the 3D view lists the pad's step only", va.undoActions() == [step_a],
              va.undoActions())
        check("2: the page lists nothing of it", step_a not in pv.undoActions(),
              pv.undoActions())
        pace()

        # 3. The drawing: its format, then Std_Refresh on the page
        type_into(pv, "leFormatSpecifier", "%.1f X")
        state("3a format")
        step_b = doc.UndoNames[0]
        check("3: the format is a step of the page's",
              dim.FormatSpec == "%.1f X" and pv.undoActions()[:1] == [step_b]
              and step_b not in va.undoActions(), (pv.undoActions(), va.undoActions()))
        n = len(doc.UndoNames)
        run_in(pv, "Std_Refresh")
        h = drawn()
        state("3b refresh on the page")
        check("3: Std_Refresh on the page makes no step", len(doc.UndoNames) == n,
              doc.UndoNames)
        pace()

        # 4. The model again: 25, then Std_Refresh in the 3D view
        type_into(va, "lengthEdit", "25")
        step_c = doc.UndoNames[0]
        n = len(doc.UndoNames)
        run_in(va, "Std_Refresh")
        h = drawn()
        state("4 pad 25 + refresh in 3D")
        check("4: Std_Refresh in the 3D view makes no step", len(doc.UndoNames) == n,
              doc.UndoNames)
        check("4: the drawing follows", h == 25.0, h)
        check("4: the 3D view lists its two steps",
              va.undoActions() == [step_c, step_a], va.undoActions())
        check("4: the page lists its one", step_b in pv.undoActions()
              and step_a not in pv.undoActions() and step_c not in pv.undoActions(),
              pv.undoActions())
        pace()

        # 5. The page's undo, from under the model's later step
        run_in(pv, "Std_Undo")
        h = drawn()
        state("5 page undo")
        check("5: the page's undo takes back the format",
              dim.FormatSpec == spec0, dim.FormatSpec)
        check("5: the model and its drawing stay",
              round(pad.Length.Value, 3) == 25.0 and h == 25.0, (pad.Length, h))
        check("5: both edits still open", dialog(va) is not None and dialog(pv) is not None)

        # 6. The model's undo, then a recompute
        run_in(va, "Std_Undo")
        settle(300)
        state("6 3D undo")
        # The view's projection is not put back by an undo: marked, not
        # left stale and clean (docs/MultiViewEdit.md sec 12.10)
        check("6: before a recompute the drawing is right or marked",
              tallest() == 20.0 or "Touched" in R.tview.State, (tallest(), R.tview.State))
        doc.recompute()
        h = drawn()
        state("6 3D undo + recompute")
        check("6: the model's undo takes back 25", round(pad.Length.Value, 3) == 20.0,
              pad.Length)
        check("6: the drawing follows", h == 20.0, h)
        check("6: the page's format is not touched", dim.FormatSpec == spec0,
              dim.FormatSpec)

        # 7. The model's redo, then a recompute
        run_in(va, "Std_Redo")
        doc.recompute()
        h = drawn()
        state("7 3D redo + recompute")
        check("7: the model's redo brings back 25", round(pad.Length.Value, 3) == 25.0,
              pad.Length)
        check("7: the drawing follows", h == 25.0, h)

        # 8. The page's redo
        run_in(pv, "Std_Redo")
        h = drawn()
        state("8 page redo")
        check("8: the page's redo brings back its format", dim.FormatSpec == "%.1f X",
              dim.FormatSpec)
        check("8: the model stays", round(pad.Length.Value, 3) == 25.0 and h == 25.0,
              (pad.Length, h))
        pace()

        # 9. OK on the page, Cancel on the pad
        press(pv, "OK")
        state("9a page OK")
        check("9: OK keeps the format", dim.FormatSpec == "%.1f X", dim.FormatSpec)
        press(va, "Cancel")
        doc.recompute()
        h = drawn()
        state("9b pad Cancel + recompute")
        check("9: Cancel takes the pad back", round(pad.Length.Value, 3) == 15.0,
              pad.Length)
        check("9: the drawing follows", h == 15.0, h)
        check("9: the page's format stays", dim.FormatSpec == "%.1f X", dim.FormatSpec)
        check("9: no edit left", dialog(va) is None and dialog(pv) is None)
        cancel_step = doc.UndoNames[0]
        check("9: Cancel's step is the 3D view's, not the page's",
              va.undoActions()[:1] == [cancel_step] and cancel_step not in pv.undoActions(),
              (cancel_step, va.undoActions(), pv.undoActions()))

        # 10. From the 3D view, out of edit: undo all of it
        for i in range(12):
            if not doc.UndoNames:
                break
            run_in(va, "Std_Undo")
        doc.recompute()
        h = drawn()
        state("10 all undone")
        check("10: back where it started",
              round(pad.Length.Value, 3) == 15.0 and h == 15.0 and dim.FormatSpec == spec0,
              (pad.Length, h, dim.FormatSpec))
        check("10: the undo list is empty", doc.UndoNames == [], doc.UndoNames)
        check("10: nothing touched, nothing invalid",
              touched() == [] and invalid() == [], (touched(), invalid()))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    VIEW.SetBool("PerViewEdit", False)
    PART.RemBool("PreviewOnEdit")
    try:
        FreeCADGui.resetEdit()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
