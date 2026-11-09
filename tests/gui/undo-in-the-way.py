"""An undo refused for a step made since that depends on it says the step,
and offers the undo with it or with what it changed kept
(docs/MultiViewEdit.md sec 12.11).

A TechDraw dimension edited on its page -- two formats typed, then OK: one
group, the page's. Then, with the 3D view the input's, a command sets the
format again: a step of the 3D view's, which the page does not list.

Claims:
  - the page's Std_Undo takes the edit from under that step, is refused for
    it, and says it in a dialog;
  - "Keep theirs" undoes the edit and leaves the format as the 3D view's
    step set it;
  - "Undo with them" undoes the edit and the 3D view's step with it.
"""
import glob
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets
from PySide6 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
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
    activate(view)
    dlg = dialog(view)
    box = None
    for w in dlg.getDialogContent():
        for b in w.findChildren(QtWidgets.QLineEdit):
            if b.objectName() == name and (box is None or not box.isVisible()):
                box = b
    if box is None:
        return False
    box.window().activateWindow()
    box.setFocus()
    settle(20)
    box.selectAll()
    QtTest.QTest.keyClicks(box, text)
    settle(300)
    FreeCADGui.getMainWindow().setFocus()
    settle(500)
    return True


def pace():
    """Past the coalescing window (TransactionLogJournal.md sec 37.4 P4)"""
    settle(3500)


SEEN = []


def answer(button):
    """Click `button` of the in-the-way dialog once it is up"""
    def find():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QMessageBox) and w.objectName() == "undoInTheWay":
                SEEN.append((w.text(), w.informativeText(), [b.text() for b in w.buttons()]))
                for b in w.buttons():
                    if b.text() == button:
                        b.click()
                        return
                w.reject()
                return
        QtCore.QTimer.singleShot(200, find)
    QtCore.QTimer.singleShot(300, find)


FORMAT = ["%.3f C"]


class SetFormat:
    """A command: the dimension's format set, a step of the input's view"""

    def GetResources(self):
        return {"MenuText": "Set format"}

    def IsActive(self):
        return True

    def Activated(self):
        doc = FreeCAD.ActiveDocument
        doc.openTransaction("Set format")
        doc.Dim.FormatSpec = FORMAT[0]
        doc.commitTransaction()


def run():
    try:
        VIEW.SetBool("PerViewEdit", False)
        VIEW.SetBool("EditStepEachChange", True)
        doc = FreeCAD.newDocument("InTheWay")
        part = doc.addObject("Part::Box", "Block")
        part.Length, part.Width, part.Height = 40, 20, 10
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
        page.ViewObject.doubleClicked()
        settle(2000)
        pv = gdoc.activeView()
        spec0 = dim.FormatSpec

        def edit():
            activate(pv)
            dim.ViewObject.doubleClicked()
            settle(1500)
            type_into(pv, "leFormatSpecifier", "%.1f A")
            pace()
            type_into(pv, "leFormatSpecifier", "%.2f B")
            dialog(pv).accept()
            settle(1500)

        # the command on a toolbar of its own: a toolbar's command is the
        # active view's step
        bar = FreeCADGui.getMainWindow().addToolBar("TestInTheWay")
        action = bar.addAction("Set format")
        action.triggered.connect(lambda checked=False: SetFormat().Activated())
        bar.setVisible(True)
        button = bar.widgetForAction(action)
        settle(500)

        def set_format():
            activate(v3d)
            QtTest.QTest.mouseClick(button, Qt.LeftButton)
            settle(800)

        edit()
        check("the edit is one group on the page",
              dim.FormatSpec == "%.2f B" and len({g for _, _, g in doc.getUndoSteps() if g}) == 1,
              doc.getUndoSteps())
        pace()
        set_format()
        note("  page lists %s, 3D view lists %s" % (pv.undoActions(), v3d.undoActions()))
        check("the 3D view's step is not the page's",
              dim.FormatSpec == "%.3f C" and "Set format" not in pv.undoActions(),
              (dim.FormatSpec, pv.undoActions()))

        answer("Keep theirs")
        activate(pv)
        FreeCADGui.runCommand("Std_Undo")
        settle(2500)
        note("  dialog %s" % (SEEN[-1:],))
        check("the page's undo is refused for the 3D view's step, and says it",
              SEEN and "Set format" in SEEN[-1][1], SEEN)
        check("keep theirs: the edit undone, the format the 3D view's",
              dim.FormatSpec == "%.3f C" and not [s for s in doc.getUndoSteps() if s[2]],
              (dim.FormatSpec, doc.getUndoSteps()))

        # again, the other way: a new edit, a new step of the 3D view's
        pace()
        before = dim.FormatSpec
        edit()
        pace()
        FORMAT[0] = "%.4f D"
        set_format()
        sets = doc.UndoNames.count("Set format")
        del SEEN[:]
        answer("Undo with them")
        activate(pv)
        FreeCADGui.runCommand("Std_Undo")
        settle(2500)
        note("  dialog %s, steps %s" % (SEEN[-1:], doc.getUndoSteps()))
        check("undo with them: the edit and the 3D view's step both undone",
              SEEN and dim.FormatSpec == before
              and doc.UndoNames.count("Set format") == sets - 1, (dim.FormatSpec, doc.UndoNames))
    except Exception:
        note("FAIL exception\n" + traceback.format_exc())


def finish():
    try:
        FreeCADGui.resetEdit()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


def main():
    run()
    finish()


QtCore.QTimer.singleShot(1500, main)
