"""The GD&T of a STEP file, imported the way the GUI imports it (docs/GDandT.md
sec 5.1, P2).

NIST's CTC-01 (src/Mod/PMI/PMITests/data) through ImportGui, both ways it
reads a file -- at once, and progressively -- each giving what the headless
import gives (PMITests/TestStep.py): the datums, the tolerances and the
dimensions, none invalid, drawn.

  scripts/gui-test.sh tests/gui/pmi-step.py /tmp/pmi-step
"""

import os
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
HERE = os.path.dirname(os.path.abspath(__file__))
CTC01 = os.path.join(HERE, "..", "..", "src", "Mod", "PMI", "PMITests", "data",
                     "nist_ctc_01_asme1_ap242-e1.stp")
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


def blue(path):
    img = QtGui.QImage(path)
    n = 0
    for y in range(0, img.height(), 2):
        for x in range(0, img.width(), 2):
            c = QtGui.QColor(img.pixel(x, y))
            if c.blue() > 110 and c.blue() - c.red() > 60 and c.blue() - c.green() > 30:
                n += 1
    return n


def counts(doc):
    kinds = ("Datum", "GeometricTolerance", "Dimension")
    return tuple(len([o for o in doc.Objects if o.TypeId == "PMI::" + k]) for k in kinds)


def unsound(doc):
    return [o.Name for o in doc.Objects
            if getattr(o, "Condition", None) in ("Invalid", "Broken")]


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetBool("ShowNaviCube", False)
        import Import
        import ImportGui

        ref = App.newDocument("Headless")
        Import.insert(CTC01, ref.Name)
        ref.recompute()
        want = counts(ref)
        App.closeDocument(ref.Name)
        check("the headless import reads GD&T", all(n > 0 for n in want), want)

        for name, progressive in (("AtOnce", False), ("Progressive", True)):
            doc = App.newDocument(name)
            pump(10)
            ImportGui.insert(CTC01, doc.Name, progressive=progressive)
            pump(60)
            doc.recompute()
            pump(20)
            got = counts(doc)
            check("%s: the same GD&T" % name, got == want, (got, want))
            check("%s: none of it invalid" % name, not unsound(doc), unsound(doc))
            view = Gui.ActiveDocument.ActiveView
            view.viewIsometric()
            Gui.SendMsgToActiveView("ViewFit")
            pump(30)
            shot = os.path.join(OUT, "%s.png" % name)
            view.saveImage(shot, 1000, 750, "#FFFFFF")
            n = blue(shot)
            check("%s: drawn" % name, n > 300, n)
            App.closeDocument(doc.Name)
            pump(10)
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
