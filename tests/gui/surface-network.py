"""The Surface workbench's Network command and task panel (docs/Surface.md
sec 5.1, 6, 8, U1).

  - Network sits on the Surface tool bar;
  - run on a whole 3D sketch, it makes a network of it, sorted into its two
    families, and opens its panel;
  - the panel lists the curves, and separates the families into profiles and
    guides edge by edge, which the user can correct: a curve moved to the
    wrong family is reported in the panel, not in a dialog, and OK is
    refused while it is so; moved back, it builds again;
  - Cancel takes the new network away again;
  - on a network of separate curves: a curve picked is added to the family
    asked for, the feature itself is not offered; a face picked is an edge
    condition's support; a continuity and a weight set in the panel are the
    feature's, with what they achieved shown; "apply to all" copies them;
  - Cancel puts every value back; OK keeps them as one undo step.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import math
import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SurfaceNetwork"
V = FreeCAD.Vector
state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds=0.3):
    import time

    end = time.monotonic() + seconds
    while True:
        QtCore.QCoreApplication.processEvents()
        if time.monotonic() >= end:
            break
        time.sleep(0.01)


def panel():
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QWidget, "SurfaceGui_Network"):
        if w.isVisible():
            return w
    return None


def child(cls, name):
    p = panel()
    return p.findChild(cls, name) if p is not None else None


def press(which):
    for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
        b = w.button(which)
        if b is not None and b.isVisible():
            b.click()
            settle(0.4)
            return True
    return False


def items(name):
    w = child(QtWidgets.QListWidget, name)
    return [w.item(i).text() for i in range(w.count())] if w is not None else None


def count(links):
    # a link list reads back grouped by object: (object, (sub, ...))
    return sum(len(subs) for _, subs in links)


def f(u, v):
    return V(10 * u, 10 * v, 2 * math.sin(math.pi * u) * math.sin(math.pi * v) + u)


def curve(alongU, fixed):
    c = Part.BSplineCurve()
    c.interpolate([f(t / 4, fixed) if alongU else f(fixed, t / 4) for t in range(5)])
    return c


PROFILES = [curve(True, v) for v in (0, 0.5, 1)]
GUIDES = [curve(False, u) for u in (0, 0.5, 1)]


def run():
    try:
        mw = FreeCADGui.getMainWindow()
        mw.showMaximized()
        FreeCADGui.activateWorkbench("SurfaceWorkbench")
        settle(0.5)
        bar = mw.findChild(QtWidgets.QToolBar, "Surface")
        names = [a.objectName() for a in bar.actions()] if bar else []
        check("Network is on the Surface tool bar", "Surface_Network" in names, names)

        doc = FreeCAD.newDocument(DOC)
        gdoc = FreeCADGui.getDocument(DOC)
        sketch(doc, gdoc)
        separate(doc, gdoc)
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def sketch(doc, gdoc):
    sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
    for c in (GUIDES[1], PROFILES[0], GUIDES[0], PROFILES[2], GUIDES[2], PROFILES[1]):
        sk.addGeometry(c)
    doc.recompute()
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(sk)
    FreeCADGui.runCommand("Surface_Network")
    settle(0.6)
    nets = [o for o in doc.Objects if o.TypeId == "Surface::Network"]
    if not check("Network made from a whole 3D sketch", len(nets) == 1, len(nets)):
        return
    n = nets[0]
    check(
        "the sketch is given whole, with no guides",
        [(o.Name, list(s)) for o, s in n.Profiles] == [("Sketch3D", [""])] and not n.Guides,
        (n.Profiles, n.Guides),
    )
    check("and it builds", n.isValid() and len(n.Shape.Faces) == 1, n.getStatusString())
    area = n.Shape.Area if n.isValid() else 0
    if not check("its panel opens", panel() is not None, gdoc.getInEdit()):
        return
    check(
        "the panel lists the sketch, and no guides",
        items("listProfiles") == ["Sketch3D"] and items("listGuides") == [],
        (items("listProfiles"), items("listGuides")),
    )
    sep = child(QtWidgets.QPushButton, "buttonSeparate")
    check("the families can be separated", sep is not None and sep.isEnabled())
    sep.click()
    settle(0.4)
    profiles, guides = items("listProfiles"), items("listGuides")
    check(
        "separated, they are three edges each",
        len(profiles) == 3 and len(guides) == 3 and count(n.Profiles) == 3 and count(n.Guides) == 3,
        (profiles, guides),
    )
    check(
        "the same surface",
        n.isValid() and abs(n.Shape.Area - area) < 1e-6,
        (area, n.Shape.Area if n.isValid() else n.getStatusString()),
    )
    check("and nothing left to separate", not sep.isEnabled())

    lp = child(QtWidgets.QListWidget, "listProfiles")
    move = child(QtWidgets.QToolButton, "buttonMove")
    lp.setCurrentRow(1)
    move.click()
    settle(0.4)
    status = child(QtWidgets.QLabel, "labelStatus")
    check(
        "a curve moved to the wrong family is reported in the panel",
        not n.isValid() and len(items("listGuides")) == 4 and status.text() != "",
        (items("listGuides"), status.text()),
    )
    check(
        "OK is refused while it is so, with no dialog",
        press(QtWidgets.QDialogButtonBox.Ok)
        and panel() is not None
        and QtWidgets.QApplication.activeModalWidget() is None,
        gdoc.getInEdit(),
    )
    lg = child(QtWidgets.QListWidget, "listGuides")
    lg.setCurrentRow(3)
    lp.clearSelection()
    move.click()
    settle(0.4)
    check(
        "moved back, it builds again",
        n.isValid() and len(items("listProfiles")) == 3 and status.text() == "",
        status.text(),
    )
    name = n.Name
    check(
        "Cancel closes the panel",
        press(QtWidgets.QDialogButtonBox.Cancel) and gdoc.getInEdit() is None,
        gdoc.getInEdit(),
    )
    check("and takes the new network away", doc.getObject(name) is None)


def separate(doc, gdoc):
    feats = []
    for prefix, curves in (("p", PROFILES), ("g", GUIDES)):
        for i, c in enumerate(curves):
            o = doc.addObject("Part::Feature", "%s%d" % (prefix, i))
            o.Shape = c.toShape()
            feats.append(o)
    p0 = feats[0]
    ext = doc.addObject("Part::Extrusion", "Neighbour")
    ext.Base = p0
    ext.DirMode = "Custom"
    ext.Dir = V(0, -1, 0.3)
    ext.LengthFwd = 5
    n = doc.addObject("Surface::Network", "Net")
    n.Profiles = [(o, "Edge1") for o in feats[:2]]
    n.Guides = [(o, "Edge1") for o in feats[3:]]
    doc.recompute()
    check("a network of two profiles builds", n.isValid(), n.getStatusString())
    undos = len(doc.UndoNames)
    FreeCADGui.Selection.clearSelection()
    gdoc.setEdit(n, 0)
    settle(0.5)
    if not check("editing it opens its panel", panel() is not None, gdoc.getInEdit()):
        return

    add = child(QtWidgets.QToolButton, "buttonAddProfile")
    add.setChecked(True)
    settle(0.2)
    FreeCADGui.Selection.addSelection(n, "Edge1")
    settle(0.2)
    check(
        "the network's own edge is not offered",
        not FreeCADGui.Selection.getSelectionEx() and count(n.Profiles) == 2,
        n.Profiles,
    )
    FreeCADGui.Selection.addSelection(feats[2], "Edge1")
    settle(0.4)
    check(
        "an edge picked is added to the profiles",
        count(n.Profiles) == 3 and n.Profiles[-1][0] == feats[2] and n.isValid(),
        (n.Profiles, n.getStatusString()),
    )
    check("and listed", items("listProfiles")[-1] == "p2.Edge1", items("listProfiles"))
    add.setChecked(False)

    pick = child(QtWidgets.QToolButton, "firstProfileSupport")
    pick.setChecked(True)
    settle(0.2)
    FreeCADGui.Selection.addSelection(ext, "Face1")
    settle(0.4)
    check(
        "a face picked is the first profile's support",
        n.FirstProfileSupport and n.FirstProfileSupport[0] == ext,
        n.FirstProfileSupport,
    )
    check(
        "and shown",
        child(QtWidgets.QLabel, "firstProfileSupportLabel").text() == "Neighbour.Face1",
        child(QtWidgets.QLabel, "firstProfileSupportLabel").text(),
    )
    check("the pick ends there", not pick.isChecked())
    child(QtWidgets.QComboBox, "firstProfileContinuity").setCurrentIndex(1)
    child(QtWidgets.QDoubleSpinBox, "firstProfileWeight").setValue(1.5)
    settle(0.4)
    check(
        "a continuity and a weight set in it are the feature's",
        n.FirstProfileContinuity == "G1" and abs(n.FirstProfileWeight - 1.5) < 1e-9,
        (n.FirstProfileContinuity, n.FirstProfileWeight),
    )
    achieved = child(QtWidgets.QLabel, "labelAchieved").text()
    check(
        "what it achieved is shown",
        n.isValid() and "first profile" in achieved,
        (achieved, n.getStatusString()),
    )
    child(QtWidgets.QPushButton, "buttonApplyAll").click()
    settle(0.4)
    check(
        "apply to all copies the continuity and the weight",
        all(
            getattr(n, side + "Continuity") == "G1"
            and abs(getattr(n, side + "Weight") - 1.5) < 1e-9
            for side in ("LastProfile", "FirstGuide", "LastGuide")
        ),
    )
    check(
        "the sides with no face to meet say so in the panel",
        not n.isValid() and child(QtWidgets.QLabel, "labelStatus").text() != "",
        child(QtWidgets.QLabel, "labelStatus").text(),
    )

    check(
        "Cancel closes the panel",
        press(QtWidgets.QDialogButtonBox.Cancel) and gdoc.getInEdit() is None,
        gdoc.getInEdit(),
    )
    doc.recompute()
    check(
        "and puts every value back",
        count(n.Profiles) == 2
        and not n.FirstProfileSupport
        and n.FirstProfileContinuity == "G0"
        and n.LastGuideContinuity == "G0"
        and n.isValid(),
        (n.Profiles, n.FirstProfileSupport, n.FirstProfileContinuity, n.getStatusString()),
    )
    check("with no undo step left", len(doc.UndoNames) == undos, doc.UndoNames[:3])

    gdoc.setEdit(n, 0)
    settle(0.5)
    child(QtWidgets.QDoubleSpinBox, "lastGuideWeight").setValue(2)
    tol = child(QtWidgets.QDoubleSpinBox, "fitTolerance")
    tol.setValue(1e-3)
    settle(0.3)
    check(
        "OK closes the panel",
        press(QtWidgets.QDialogButtonBox.Ok) and gdoc.getInEdit() is None,
        gdoc.getInEdit(),
    )
    check(
        "and keeps the values",
        abs(n.LastGuideWeight - 2) < 1e-9 and abs(n.FitTolerance.Value - 1e-3) < 1e-12,
        (n.LastGuideWeight, n.FitTolerance),
    )
    check("as one undo step", len(doc.UndoNames) == undos + 1, doc.UndoNames[:3])


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
