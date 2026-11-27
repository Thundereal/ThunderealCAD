"""A dimension's reference found again by its name alone is taken on the dimension's edit, said, and can be reattached.

The reporter, 2026-11-26 (scanner.FCStd): a distance whose saved name a
B-spline carried after its view was made again was repointed onto it in a
recompute, and failed: "Edge21 is a BSpline, not a straight line". The
ruling: "the auto reference fix should only be done if geometry search found
the match instead of pure naming match. naming match auto fix should only be
done when double click editing with warning to user. also right now, there
is no reference edit in dimension task panel, expose that also for user to
reattach".

A line and a B-spline beside it; a page, a view of them, a distance on the
line. The line's saved geometry is moved off anything in the view and its
saved name made the spline's. Claims, with each change a step of its own
(View/EditStepEachChange on) and with one transaction held to OK (off):
  - the recompute does not repoint it: the dimension is in error, on the line;
  - the dimension's edit takes the spline by its name, and its panel says so
    in amber, the row marked;
  - Cancel puts the reference back on the line;
  - edited again, Reattach with the row selected and the line picked in the
    view puts the reference on the line; the dimension measures 10 and is not
    in error, and OK keeps it.

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
DOC = "DimReattach"

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


def panel():
    for w in mw().findChildren(QtWidgets.QWidget):
        if w.isVisible() and w.metaObject().className() == "TechDrawGui::TaskDimension":
            return w
    return None


def press(role):
    """The task dialog's OK or Cancel, as the user presses it"""
    for box in mw().findChildren(QtWidgets.QDialogButtonBox):
        if not box.isVisible():
            continue
        button = box.button(role)
        if button is not None and button.isVisible():
            button.click()
            settle(1.5)
            return True
    return False


def sub(dim):
    return dim.References2D[0][1][0]


def setup():
    doc = FreeCAD.newDocument(DOC)
    doc.UndoMode = 1
    a, b, c = FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(10, 0, 0), FreeCAD.Vector(20, 0, 0)
    spline = Part.BSplineCurve()
    spline.interpolate([b, FreeCAD.Vector(15, 0, 5), c])
    src = doc.addObject("Part::Feature", "Shape")
    src.Shape = Part.Compound([Part.Edge(Part.LineSegment(a, b)), spline.toShape()])
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
    view.X, view.Y = 120, 120
    doc.recompute()
    settle(4)
    line = curve = None
    n = 0
    while True:
        try:
            e = view.getEdgeByIndex(n)
        except Exception:
            break
        kind = e.Curve.__class__.__name__
        if kind == "Line" and line is None:
            line = "Edge%d" % n
        elif kind == "BSplineCurve" and curve is None:
            curve = "Edge%d" % n
        n += 1
    dim = doc.addObject("TechDraw::DrawViewDimension", "Dimension")
    dim.Type = "Distance"
    dim.References2D = [(view, line)]
    page.addView(dim)
    doc.recompute()
    settle(2)
    page.ViewObject.doubleClicked()
    settle(3)
    return doc, view, dim, line, curve


def misplace(doc, view, dim, line, curve):
    moved = Part.Edge(Part.LineSegment(FreeCAD.Vector(0, 0, 40), FreeCAD.Vector(10, 0, 40)))
    name = view.getGeometryName(curve)
    doc.openTransaction("misplace")
    dim.References2D = [(view, line)]
    dim.SavedGeometry = [moved]
    dim.SavedNames = [name]
    doc.commitTransaction()
    dim.touch()
    doc.recompute()
    settle(1)
    return name


def round_(doc, view, dim, line, curve, mode):
    name = misplace(doc, view, dim, line, curve)
    check("%s: the recompute leaves it on the line" % mode, sub(dim) == line, sub(dim))
    check("%s: and the dimension in error, saying why" % mode,
          "Invalid" in dim.State and "name" in dim.getStatusString(), dim.getStatusString())

    # edit: taken by name, and said
    dim.ViewObject.doubleClicked()
    settle(2)
    p = panel()
    if not check("%s: the dimension's panel open" % mode, p is not None):
        return
    check("%s: the edit takes the spline by its name" % mode, sub(dim) == curve, sub(dim))
    label = p.findChild(QtWidgets.QLabel, "lMessage")
    text = label.text() if label is not None else ""
    check("%s: the panel says it, in amber" % mode,
          label is not None and label.isVisible() and "Found again by name" in text
          and "#b07000" in text, text)
    lw = p.findChild(QtWidgets.QListWidget, "lwReferences")
    row = lw.item(0) if lw is not None and lw.count() else None
    check("%s: the row marked" % mode,
          row is not None and row.text() == curve and row.foreground().color().name() == "#b07000",
          (row.text(), row.foreground().color().name()) if row is not None else None)

    # Cancel: back on the line
    check("%s: Cancel pressed" % mode, press(QtWidgets.QDialogButtonBox.Cancel))
    check("%s: Cancel puts it back on the line" % mode, sub(dim) == line, sub(dim))
    check("%s: and the saved name back" % mode, list(dim.SavedNames) == [name],
          (list(dim.SavedNames), name))

    # edit again, and reattach to the line
    dim.ViewObject.doubleClicked()
    settle(2)
    p = panel()
    if not check("%s: the panel open again" % mode, p is not None):
        return
    lw = p.findChild(QtWidgets.QListWidget, "lwReferences")
    button = p.findChild(QtWidgets.QPushButton, "pbReattach")
    lw.setCurrentRow(0)
    button.click()
    settle(0.5)
    check("%s: Reattach down" % mode, button.isChecked())
    FreeCADGui.Selection.addSelection(doc.Name, view.Name, line)
    settle(1.5)
    check("%s: Reattach puts it on the line picked" % mode, sub(dim) == line, sub(dim))
    check("%s: Reattach up again" % mode, not button.isChecked())
    check("%s: measured, not in error" % mode,
          "Invalid" not in dim.State and abs(dim.getRawValue() - 10.0) < 1e-6,
          (dim.State, dim.getRawValue()))
    check("%s: OK pressed" % mode, press(QtWidgets.QDialogButtonBox.Ok))
    doc.recompute()
    settle(1)
    check("%s: OK keeps it" % mode, sub(dim) == line and "Invalid" not in dim.State,
          (sub(dim), dim.State))
    check("%s: nothing left to take by name" % mode, dim.findByName() == [], dim.findByName())


def run():
    try:
        doc, view, dim, line, curve = setup()
        note("INFO line %s spline %s" % (line, curve))
        if not check("the line and the spline found", line and curve, (line, curve)):
            return
        params = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
        for steps in (True, False):
            params.SetBool("EditStepEachChange", steps)
            round_(doc, view, dim, line, curve, "steps" if steps else "one transaction")
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
