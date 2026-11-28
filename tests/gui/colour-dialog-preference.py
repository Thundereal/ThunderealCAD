"""The colour dialogs of Draft's and BIM's layer managers and of a BIM
material's task panel take the user's choice of dialog.

1eaf70304a: every colour dialog of the program honours
Dialog/DontUseNativeColorDialog (Gui::DialogOptions in C++,
draftutils.gui_utils.get_color() in Python); these three called a plain
QColorDialog.getColor and always showed the platform's own -- on macOS
the native NSColorPanel, which no test can find or answer.

As a user would: a document with a Draft layer;
  - Draft_LayerManager, a double click on the layer's line colour cell,
    the colour chosen, the cell committed, OK;
  - BIM_Layers, the same on its shape colour cell;
  - a BIM material edited (its task panel), its colour button pressed,
    the colour chosen, the panel's OK.
Each twice:
  - the preference true (the test profile's): the dialog that comes up
    is a QColorDialog the test sees and answers, getColor was asked for
    Qt's own dialog, and the colour chosen lands in the model;
  - the preference false: getColor is asked for the platform's dialog
    (no DontUseNativeDialog option). getColor is only recorded then,
    answering a colour itself -- a native panel is never opened in a
    test -- and that colour lands in the model too.
getColor is wrapped where the program finds it (the QColorDialog class),
recording what it was asked for; with the preference true it goes on to
the real dialog.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets
from PySide.QtCore import Qt

try:
    from PySide6 import QtTest
except ImportError:
    from PySide2 import QtTest

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ColourDialogPreference"
DIALOG = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Dialog")
QCD = QtWidgets.QColorDialog
STEPS = []
S = {"calls": []}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(ms=200):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()


def close(a, b):
    return all(abs(x - y) < 1.5 / 255 for x, y in zip(a, b))


def rgb(qcolor):
    return (qcolor.redF(), qcolor.greenF(), qcolor.blueF())


# -- getColor, recorded ------------------------------------------------------

def spy(answer):
    """getColor recorded: with `answer` None the real dialog follows (and is
    answered by a timer); otherwise that colour is returned, no dialog"""
    real = S["real"]

    def getColor(*args, **kwargs):
        options = args[3] if len(args) > 3 else kwargs.get("options")
        S["calls"].append(options)
        if answer is not None:
            return QtGui.QColor(answer)
        return real(*args, **kwargs)

    return getColor


def asked_native(options):
    """Whether a getColor call asked for the platform's own dialog"""
    if options is None:
        return True
    try:
        return not bool(options & QCD.DontUseNativeDialog)
    except TypeError:
        return options != QCD.DontUseNativeDialog


def answer_dialog(colour):
    """A timer that finds the QColorDialog up now, notes it, and answers it"""
    S["seen"] = None

    def answer(tries=[0]):
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QCD) and w.isVisible():
                S["seen"] = w.metaObject().className()
                w.setCurrentColor(QtGui.QColor(colour))
                w.accept()
                return
        tries[0] += 1
        if tries[0] < 50:
            QtCore.QTimer.singleShot(100, answer)

    QtCore.QTimer.singleShot(200, answer)


def leg_begin(native):
    """The preference for this leg; getColor wrapped. Returns the colour
    the leg should end with"""
    DIALOG.SetBool("DontUseNativeColorDialog", not native)
    S["calls"] = []
    S["seen"] = None
    colour = "#c80a1e" if native else "#0ac81e"
    QCD.getColor = staticmethod(spy(colour if native else None))
    if not native:
        answer_dialog(colour)
    return colour


def leg_end(what, native, colour, model):
    QCD.getColor = S["real"]
    tag = "%s, preference %s" % (what, "false" if native else "true")
    check("%s: getColor was called once" % tag, len(S["calls"]) == 1, S["calls"])
    if native:
        check("%s: it asked for the platform's own dialog" % tag,
              S["calls"] and asked_native(S["calls"][0]), S["calls"])
    else:
        check("%s: it asked for Qt's own dialog" % tag,
              S["calls"] and not asked_native(S["calls"][0]), S["calls"])
        check("%s: the dialog up was a QColorDialog, seen and answered" % tag,
              S["seen"] == "QColorDialog", S["seen"])
    check("%s: the colour chosen is in the model" % tag,
          model is not None and close(model, rgb(QtGui.QColor(colour))), (model, colour))


# -- the layer managers -----------------------------------------------------

def layer_dialog():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(w, QtWidgets.QDialog) and w.isVisible() and hasattr(w, "tree") \
                and hasattr(w, "buttonOK"):
            return w
    return None


def edit_colour_cell(dialog, column):
    """A double click on the layer's cell in `column`; the editor the
    delegate opens (the colour dialog comes up in it) committed by Return"""
    tree = dialog.tree
    model = tree.model()
    row = None
    for r in range(model.rowCount()):
        if model.item(r, 1).toolTip() == S["layer"]:
            row = r
    if row is None:
        note("INFO no row for %s: %s" % (S["layer"], [model.item(r, 1).text()
                                                      for r in range(model.rowCount())]))
        return False
    index = model.index(row, column)
    tree.scrollTo(index)
    pump(100)
    rect = tree.visualRect(index)
    QtTest.QTest.mouseClick(tree.viewport(), Qt.LeftButton, Qt.NoModifier, rect.center())
    pump(100)
    QtTest.QTest.mouseDClick(tree.viewport(), Qt.LeftButton, Qt.NoModifier, rect.center())
    pump(200)
    editor = QtWidgets.QApplication.focusWidget()
    note("INFO the cell's editor: %s, text %r" % (
        editor.metaObject().className() if editor else None,
        editor.text() if isinstance(editor, QtWidgets.QLineEdit) else None))
    if isinstance(editor, QtWidgets.QLineEdit):
        QtTest.QTest.keyClick(editor, Qt.Key_Return)
        pump(100)
    return True


def draft_leg(native):
    colour = leg_begin(native)

    def drive(tries=[0]):
        dialog = layer_dialog()
        if dialog is None:
            tries[0] += 1
            if tries[0] < 50:
                QtCore.QTimer.singleShot(100, drive)
            return
        try:
            edit_colour_cell(dialog, 4)  # line colour
        finally:
            dialog.buttonOK.click()

    QtCore.QTimer.singleShot(500, drive)
    FreeCADGui.runCommand("Draft_LayerManager")  # modal: returns when it closes
    pump(300)
    vobj = FreeCAD.getDocument(DOC).getObject(S["layer"]).ViewObject
    leg_end("Draft layer manager, line colour", native, colour, vobj.LineColor[:3])


def bim_leg(native):
    colour = leg_begin(native)
    FreeCADGui.runCommand("BIM_Layers")  # not modal
    pump(500)
    dialog = layer_dialog()
    if check("BIM layer manager is up", dialog is not None):
        try:
            edit_colour_cell(dialog, 5)  # shape colour
        finally:
            dialog.buttonOK.click()
    pump(300)
    vobj = FreeCAD.getDocument(DOC).getObject(S["layer"]).ViewObject
    leg_end("BIM layer manager, shape colour", native, colour, vobj.ShapeColor[:3])


# -- a BIM material's task panel --------------------------------------------

def material_leg(native):
    import Arch

    doc = FreeCAD.getDocument(DOC)
    mat = Arch.makeMaterial(name="Mat%s" % ("Native" if native else "Qt"))
    doc.recompute()
    colour = leg_begin(native)
    FreeCADGui.getDocument(DOC).setEdit(mat.Name)
    pump(500)
    panel = getattr(mat.ViewObject.Proxy, "taskd", None)
    if check("material %s: its task panel is up" % mat.Name, panel is not None):
        button = panel.form.ButtonColor
        QtTest.QTest.mouseClick(button, Qt.LeftButton)  # the dialog comes up on press
        pump(200)
        ok = None
        for box in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
            b = box.button(QtWidgets.QDialogButtonBox.Ok)
            if b is not None and b.isVisible():
                ok = b
        if ok is not None:
            ok.click()
        else:
            note("INFO no OK button in sight: the panel accepted directly")
            panel.accept()
        pump(500)
    leg_end("BIM material, colour button", native, colour, tuple(mat.Color[:3]))


# -- run --------------------------------------------------------------------

def start():
    import Draft
    import bimcommands.BimLayers  # noqa: F401 -- registers BIM_Layers
    import draftguitools.gui_layers  # noqa: F401 -- registers Draft_LayerManager

    S["real"] = QCD.getColor
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
    note("INFO the profile's preference: DontUseNativeColorDialog %s"
         % DIALOG.GetBool("DontUseNativeColorDialog", True))
    doc = FreeCAD.newDocument(DOC)
    layer = Draft.make_layer("Paint")
    doc.recompute()
    S["layer"] = layer.Name
    pump(500)


def legs():
    for leg in (draft_leg, bim_leg, material_leg):
        for native in (False, True):
            try:
                leg(native)
            except Exception:
                note("ABORT in %s(native=%s): %s" % (
                    leg.__name__, native, traceback.format_exc().replace("\n", " | ")))
            finally:
                QCD.getColor = S["real"]
                DIALOG.SetBool("DontUseNativeColorDialog", True)
            pump(300)


def finish():
    try:
        FreeCADGui.getDocument(DOC).resetEdit()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


def advance():
    if not STEPS:
        finish()
        return
    step = STEPS.pop(0)
    try:
        step()
    except Exception:
        note("ABORT in %s: %s" % (step.__name__, traceback.format_exc().replace("\n", " | ")))
        STEPS[:] = []
    QtCore.QTimer.singleShot(500, advance)


STEPS[:] = [start, legs]
QtCore.QTimer.singleShot(2500, advance)
