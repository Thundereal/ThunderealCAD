"""A hatch's face and a centre line's face found again by name alone are taken on their edit, said, and can be reattached.

The reporter, 2026-11-26, on dimensions first and then: "yes, cover hatches
and centre lines too" -- in a recompute a reference moves only where the
geometry agrees; one found by its name alone is taken in the edit's panel,
with the amber warning, Cancel restoring it, and the panel can reattach.

Two boxes seen from above: two faces, of 100 and of 200. A hatch and a face
centre line on Face0; each moved to Face1 with Face0's name and a saved
geometry that is nothing in the view. Claims, for each:
  - the recompute leaves it on Face1;
  - its edit's panel takes Face0 by its name and says so in amber, the row
    marked;
  - Cancel puts it back on Face1;
  - edited again, Reattach with the row selected and Face0 picked in the
    view puts it on Face0, and OK keeps it.

Runs on the hidden desktop (scripts/gui-test.cmd).
"""
import glob
import os
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "HatchClReattach"
NOWHERE = "F 1 0 0 999"

state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(seconds):
    end = time.time() + seconds
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        time.sleep(0.02)


def mw():
    return FreeCADGui.getMainWindow()


def panel(cls):
    for w in mw().findChildren(QtWidgets.QWidget):
        if w.isVisible() and w.metaObject().className() == cls:
            return w
    return None


def press(role):
    for box in mw().findChildren(QtWidgets.QDialogButtonBox):
        if not box.isVisible():
            continue
        button = box.button(role)
        if button is not None and button.isVisible():
            button.click()
            settle(2)
            return True
    return False


def again(doc, view):
    view.touch()
    doc.recompute()
    settle(3)


def setup():
    doc = FreeCAD.newDocument(DOC)
    doc.UndoMode = 1
    src = doc.addObject("Part::Feature", "Shape")
    src.Shape = Part.Compound([Part.makeBox(10, 10, 10),
                               Part.makeBox(20, 10, 10, FreeCAD.Vector(30, 0, 0))])
    templates = glob.glob(os.path.join(FreeCAD.getResourceDir(), "Mod", "TechDraw",
                                       "Templates", "**", "*A4*Landscape*.svg"), recursive=True)
    page = doc.addObject("TechDraw::DrawPage", "Page")
    tmpl = doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    if templates:
        tmpl.Template = templates[0]
    page.Template = tmpl
    view = doc.addObject("TechDraw::DrawViewPart", "View")
    page.addView(view)
    view.Source = [src]
    view.Direction = (0.0, 0.0, 1.0)
    view.X, view.Y = 120, 120
    doc.recompute()
    settle(4)
    page.ViewObject.doubleClicked()
    settle(3)
    return doc, page, view


def reattach(p, doc, view, sub, how):
    lw = p.findChild(QtWidgets.QListWidget, "lwReferences")
    button = p.findChild(QtWidgets.QPushButton, "pbReattach")
    if not check("%s: the reference list and Reattach there" % how,
                 lw is not None and button is not None):
        return False
    lw.setCurrentRow(0)
    button.click()
    settle(0.5)
    check("%s: Reattach down" % how, button.isChecked())
    FreeCADGui.Selection.addSelection(doc.Name, view.Name, sub)
    settle(2)
    check("%s: Reattach up again" % how, not button.isChecked())
    return True


def said(p, how):
    label = p.findChild(QtWidgets.QLabel, "lMessage")
    text = label.text() if label is not None else ""
    check("%s: the panel says it, in amber" % how,
          label is not None and label.isVisible() and "Found again by name" in text
          and "#b07000" in text, text)
    lw = p.findChild(QtWidgets.QListWidget, "lwReferences")
    row = lw.item(0) if lw is not None and lw.count() else None
    check("%s: the row marked" % how,
          row is not None and row.text() == "Face0"
          and row.foreground().color().name() == "#b07000",
          (row.text(), row.foreground().color().name()) if row is not None else None)


def hatch_round(doc, view):
    hatch = doc.addObject("TechDraw::DrawHatch", "Hatch")
    hatch.Source = (view, ["Face0"])
    doc.recompute()
    settle(3)
    names = list(hatch.SavedNames)
    check("hatch: Face0 named", names and names[0], names)
    doc.openTransaction("misplace")
    hatch.Source = (view, ["Face1"])
    hatch.SavedNames = names
    hatch.SavedGeometry = [NOWHERE]
    doc.commitTransaction()
    again(doc, view)
    check("hatch: the recompute leaves it on Face1", list(hatch.Source[1]) == ["Face1"],
          hatch.Source[1])

    hatch.ViewObject.doubleClicked()
    settle(2)
    p = panel("TechDrawGui::TaskHatch")
    if not check("hatch: its panel open", p is not None):
        return
    check("hatch: the edit takes Face0 by its name", list(hatch.Source[1]) == ["Face0"],
          hatch.Source[1])
    said(p, "hatch")
    check("hatch: Cancel pressed", press(QtWidgets.QDialogButtonBox.Cancel))
    check("hatch: Cancel puts it back on Face1", list(hatch.Source[1]) == ["Face1"], hatch.Source[1])
    check("hatch: and what was saved of it", list(hatch.SavedGeometry) == [NOWHERE],
          list(hatch.SavedGeometry))

    hatch.ViewObject.doubleClicked()
    settle(2)
    p = panel("TechDrawGui::TaskHatch")
    if not check("hatch: its panel open again", p is not None):
        return
    # taken by name again on opening; Reattach to Face1 and back proves the pick
    if reattach(p, doc, view, "Face1", "hatch"):
        check("hatch: Reattach puts it on the face picked", list(hatch.Source[1]) == ["Face1"],
              hatch.Source[1])
    check("hatch: OK pressed", press(QtWidgets.QDialogButtonBox.Ok))
    check("hatch: OK keeps it", list(hatch.Source[1]) == ["Face1"], hatch.Source[1])
    check("hatch: nothing left to take by name", hatch.findByName() == [], hatch.findByName())


def centreline_edge(view, tag):
    n = 0
    while n < 200:
        try:
            cl = view.getCenterLineBySelection("Edge%d" % n)
            if cl is not None and cl.Tag == tag:
                return "Edge%d" % n
        except Exception:
            pass
        n += 1
    return None


def centreline_round(doc, view):
    tag = view.makeCenterLine(["Face0"], 0)
    again(doc, view)
    cl = view.getCenterLine(tag)
    names = list(cl.FaceNames)
    check("centre line: Face0 named", names and names[0], names)
    cl.Faces = ["Face1"]
    cl.FaceGeometry = [NOWHERE]
    again(doc, view)
    cl = view.getCenterLine(tag)
    check("centre line: the recompute leaves it on Face1", list(cl.Faces) == ["Face1"],
          list(cl.Faces))

    def edit():
        edge = centreline_edge(view, tag)
        if not check("centre line: its edge in the view", edge is not None):
            return None
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(doc.Name, view.Name, edge)
        FreeCADGui.runCommand("TechDraw_FaceCenterLine")
        settle(2)
        return panel("TechDrawGui::TaskCenterLine")

    p = edit()
    if not check("centre line: its panel open", p is not None):
        return
    cl = view.getCenterLine(tag)
    check("centre line: the edit takes Face0 by its name", list(cl.Faces) == ["Face0"],
          list(cl.Faces))
    said(p, "centre line")
    check("centre line: Cancel pressed", press(QtWidgets.QDialogButtonBox.Cancel))
    cl = view.getCenterLine(tag)
    check("centre line: Cancel puts it back on Face1", list(cl.Faces) == ["Face1"],
          list(cl.Faces))

    p = edit()
    if not check("centre line: its panel open again", p is not None):
        return
    if reattach(p, doc, view, "Face1", "centre line"):
        cl = view.getCenterLine(tag)
        check("centre line: Reattach puts it on the face picked", list(cl.Faces) == ["Face1"],
              list(cl.Faces))
    check("centre line: OK pressed", press(QtWidgets.QDialogButtonBox.Ok))
    cl = view.getCenterLine(tag)
    check("centre line: OK keeps it", list(cl.Faces) == ["Face1"], list(cl.Faces))
    check("centre line: nothing left to take by name", cl.findByName(view) == [],
          cl.findByName(view))


def run():
    try:
        doc, page, view = setup()
        check("two faces in the view", len(view.getFaceNames()) == 2, len(view.getFaceNames()))
        hatch_round(doc, view)
        centreline_round(doc, view)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        try:
            FreeCADGui.getDocument(name).resetEdit()
        except Exception:
            pass
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, mw().close)


QtCore.QTimer.singleShot(3000, run)
