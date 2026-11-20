"""PMI datum targets made as a user makes them (docs/GDandT.md sec 4.3).

A PartDesign Body -- a 40 x 20 x 10 box -- and its bottom face picked at
points. What is checked:

  - the datum target command makes a target where the face is picked,
    looking out of the part, and a datum with the next letter it is of;
  - picked with that datum, the next target is the datum's, numbered on;
  - the targets are drawn: their symbols and their X on the part.

  scripts/gui-test.sh tests/gui/pmi-targets.py /tmp/pmi-targets
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


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetBool("ShowNaviCube", False)
        doc = App.newDocument("PMITargets")
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObjectAt("PartDesign::AdditiveBox", "Box")
        box.Length, box.Width, box.Height = 40, 20, 10
        doc.recompute()
        Gui.activateWorkbench("PMIWorkbench")
        pump(30)
        view = Gui.ActiveDocument.ActiveView
        view.viewBottom()
        Gui.SendMsgToActiveView("ViewFit")
        pump(30)
        bottom = face_at(box.Shape, V(5, 5, 0))
        before = blue(view, "none.png")

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(doc.Name, body.Name, "%s.%s" % (box.Name, bottom), 5, 5, 0)
        pump(5)
        Gui.runCommand("PMI_DatumTarget")
        pump(20)
        targets = [o for o in doc.Objects if o.TypeId == "PMI::DatumTarget"]
        datums = [o for o in doc.Objects if o.TypeId == "PMI::Datum"]
        check("a target where the face is picked",
              len(targets) == 1 and targets[0].Target.Base.isEqual(V(5, 5, 0), 1e-6),
              [t.Target for t in targets])
        z = targets[0].Target.Rotation.multVec(V(0, 0, 1)) if targets else None
        check("looking out of the part", z is not None and z.isEqual(V(0, 0, -1), 1e-9), z)
        check("of a new datum, lettered A",
              len(datums) == 1 and datums[0].Letter == "A" and datums[0].Targets == targets,
              [(d.Letter, d.Targets) for d in datums])

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(datums[0])
        Gui.Selection.addSelection(doc.Name, body.Name, "%s.%s" % (box.Name, bottom), 35, 15, 0)
        pump(5)
        Gui.runCommand("PMI_DatumTarget")
        pump(20)
        targets = [o for o in doc.Objects if o.TypeId == "PMI::DatumTarget"]
        datums = [o for o in doc.Objects if o.TypeId == "PMI::Datum"]
        check("picked with the datum, the datum's next",
              len(datums) == 1 and len(datums[0].Targets) == 2
              and datums[0].Targets[1].Number == 2
              and datums[0].Targets[1].EquivalentText == "A2",
              [(d.Letter, [t.EquivalentText for t in d.Targets]) for d in datums])
        check("none invalid", all(o.Condition == "Ok" for o in targets + datums),
              [(o.Name, o.Condition, o.ConditionMessage) for o in targets + datums])
        # Two targets on one face: each at its own point, with its own
        # leader, not stacked as callouts of one feature are
        check("each with its own leader", all(t.AttachedTo is None for t in targets),
              [(t.Name, t.AttachedTo) for t in targets])
        Gui.Selection.clearSelection()
        Gui.SendMsgToActiveView("ViewFit")
        pump(30)
        after = blue(view, "targets.png")
        check("drawn", after > before + 100, "before %d, after %d" % (before, after))
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
