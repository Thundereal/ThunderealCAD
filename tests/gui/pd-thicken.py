"""PartDesign Thicken's command and panel (docs/Surface.md).

A body with a B-spline face bound in. With it selected:
  - PartDesign_Thicken makes a Thicken of the face in the active body and
    opens its panel, the face listed;
  - the panel's thickness, Reversed and Midplane are the feature's,
    recomputed;
  - a face of a box outside the body, picked with Add, is linked through a
    binder in the body; two faces that do not touch are said to be not one
    piece, in the panel; the list's Remove takes it out again;
  - OK keeps it, the body's Tip;
  - a second one, cancelled, is gone.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import math
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "PDThicken"
V = FreeCAD.Vector
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
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        time.sleep(0.01)


def mw():
    return FreeCADGui.getMainWindow()


def panel():
    for w in mw().findChildren(QtWidgets.QWidget, "TaskThickenParameters"):
        if w.isVisible():
            return w
    return None


def child(cls, name):
    p = panel()
    return p.findChild(cls, name) if p is not None else None


def press(which):
    for w in mw().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            pump(60)
            return True
    return False


def bumpFace():
    pts = [
        [V(40 * i / 6, 30 * j / 6, 20 + 6 * math.sin(math.pi * i / 6) * math.sin(math.pi * j / 6)) for j in range(7)]
        for i in range(7)
    ]
    s = Part.BSplineSurface()
    s.interpolate(pts)
    f = Part.Face(s)
    if f.normalAt(0.5, 0.5).z < 0:
        f.reverse()
    return f


def run():
    try:
        mw().showMaximized()
        FreeCADGui.activateWorkbench("PartDesignWorkbench")
        pump(50)
        doc = FreeCAD.newDocument(DOC)
        gdoc = FreeCADGui.getDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        src = doc.addObject("Part::Feature", "Bump")
        src.Shape = bumpFace()
        binder = body.newObject("PartDesign::SubShapeBinder", "Binder")
        binder.Support = [(src, "Face1")]
        box = doc.addObject("Part::Box", "Box")
        box.Placement.Base = V(100, 0, 0)
        doc.recompute()
        src.Visibility = False
        gdoc.ActiveView.setActiveObject("pdbody", body)
        pump(20)

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(binder, "Face1")
        FreeCADGui.runCommand("PartDesign_Thicken")
        pump(80)
        th = doc.getObject("Thicken")
        lst = child(QtWidgets.QListWidget, "listFaces")
        check(
            "the command makes a Thicken of the face in the body, its panel open",
            th is not None
            and th in body.Group
            and [(o.Name, list(s)) for o, s in th.Faces] == [("Binder", ["Face1"])]
            and panel() is not None
            and lst is not None
            and lst.count() == 1,
            (th and th.Faces, panel(), lst and lst.count()),
        )
        if th is None or panel() is None:
            return finish()
        check("it is built", th.isValid(), th.getStatusString())

        spin = child(QtWidgets.QAbstractSpinBox, "thickness")
        rev = child(QtWidgets.QCheckBox, "checkReversed")
        mid = child(QtWidgets.QCheckBox, "checkMidplane")
        v1 = th.Shape.Volume
        spin.setProperty("rawValue", 3.0)
        pump(80)
        check(
            "the panel's thickness is the feature's, recomputed",
            abs(th.Value.Value - 3) < 1e-9 and th.Shape.Volume > v1 * 1.3,
            (th.Value, v1, th.Shape.Volume),
        )
        rev.setChecked(True)
        pump(80)
        z = th.Shape.BoundBox.ZMin
        check("Reversed is the feature's: below the face", th.Reversed and z < 19, (th.Reversed, z))
        rev.setChecked(False)
        mid.setChecked(True)
        pump(80)
        z = th.Shape.BoundBox.ZMin
        check(
            "Midplane is the feature's: half below",
            th.Midplane and 18 < z < 19.5,
            (th.Midplane, z),
        )
        mid.setChecked(False)
        pump(40)

        add = child(QtWidgets.QToolButton, "buttonAdd")
        add.setChecked(True)
        pump(10)
        FreeCADGui.Selection.addSelection(box, "Face1")
        pump(80)
        add.setChecked(False)
        pump(20)
        links = [o for o, _ in th.Faces]
        status = child(QtWidgets.QLabel, "labelStatus")
        check(
            "a face outside the body is linked through a binder in it",
            len(links) == 2 and links[1].isDerivedFrom("Part::SubShapeBinder") and links[1] in body.Group,
            [(o.Name, o.TypeId) for o in links],
        )
        check(
            "two faces apart: not one piece, said in the panel",
            not th.isValid() and status is not None and "one piece" in status.text(),
            (th.getStatusString(), status and status.text()),
        )
        lst.setCurrentRow(1)
        remove = [a for a in lst.actions() if a.objectName() == "actionRemove"]
        if remove:
            remove[0].trigger()
        pump(80)
        check(
            "the list's Remove takes it out, built again",
            len(th.Faces) == 1 and th.isValid() and lst.count() == 1,
            (th.Faces, th.getStatusString(), lst.count()),
        )

        press(QtWidgets.QDialogButtonBox.Ok)
        check(
            "OK keeps it, the body's Tip",
            gdoc.getInEdit() is None and th.isValid() and body.Tip == th,
            (gdoc.getInEdit(), th.getStatusString(), body.Tip and body.Tip.Name),
        )

        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(binder, "Face1")
        FreeCADGui.runCommand("PartDesign_Thicken")
        pump(80)
        made = doc.getObject("Thicken001") is not None
        press(QtWidgets.QDialogButtonBox.Cancel)
        check(
            "a second one, cancelled, is gone",
            made and doc.getObject("Thicken001") is None and body.Tip == th,
            (made, body.Tip and body.Tip.Name),
        )
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        if FreeCADGui.Control.activeDialog():
            FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
