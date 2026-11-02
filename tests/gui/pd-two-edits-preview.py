"""Two PartDesign edits at once, one per view (PerViewEdit), with
PartDesign's edit preview on and off (docs/MultiViewEdit.md sec 12.12):
each edit's end makes its own feature and leaves the other's as it was.

The preview (Mod/Part PreviewOnEdit) pauses the feature in edit: its
solid waits for the edit's end (PartDesign::Feature::setPauseRecompute).
Two views are what two users are; a served client's edit runs the same
code.

A body: Pad1, a 10 x 10 square padded 15, and Pad2, a 4 x 4 square on a
sketch at z = 15 padded 5 -- the body's height and volume say which
lengths it was made of. Two 3D windows: Pad1 in edit in the first, Pad2
in the second, each length typed into its panel. Asserted with the
preview on and off, the first-opened edit ended first and then the other
way round:
  - the values typed are the features'; with the preview the body waits,
    without it it is both at once;
  - the OK of one makes its feature, and with the preview the other's
    still waits for its own;
  - the OK of the other makes the body of both;
  - nothing is left paused: a length set by a script after is made.
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
state = {"done": False}


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
    """Type into the spin box `name` of the dialog of `view`."""
    activate(view)
    dlg = dialog(view)
    if dlg is None:
        return False
    box = None
    for w in dlg.getDialogContent():
        for b in w.findChildren(QtWidgets.QAbstractSpinBox):
            if b.objectName() == name and (box is None or not box.isVisible()):
                box = b
    if box is None:
        return False
    box.window().activateWindow()
    box.setFocus()
    settle(20)
    line = box.findChild(QtWidgets.QLineEdit)
    line.selectAll()
    QtTest.QTest.keyClicks(line, text)
    QtTest.QTest.keyClick(box, Qt.Key_Return)
    settle(1200)
    FreeCADGui.getMainWindow().setFocus()
    settle(300)
    return True


def rectangle(body, name, x0, size, z):
    sk = body.newObject("Sketcher::SketchObject", name)
    sk.AttachmentSupport = [(body.Origin.OriginFeatures[3], "")]   # XY plane
    sk.MapMode = "FlatFace"
    sk.AttachmentOffset = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation())
    p = [V(x0, x0, 0), V(x0 + size, x0, 0), V(x0 + size, x0 + size, 0), V(x0, x0 + size, 0)]
    for i in range(4):
        sk.addGeometry(Part.LineSegment(p[i], p[(i + 1) % 4]))
    return sk


def solid(doc):
    # the tip's own solid, as the views show the body; Body.Shape follows
    # only when the body recomputes
    shape = doc.getObject("Pad2").Shape
    return round(shape.BoundBox.ZMax, 3), round(shape.Volume, 1)


def expect(l1, l2):
    """The body for Pad1 length l1 and Pad2 length l2"""
    return round(max(l1, 15 + l2), 3), round(100 * l1 + 16 * max(0, 15 + l2 - l1), 1)


def made(doc, name):
    """The height of a feature's own solid, as it was last made"""
    return round(doc.getObject(name).Shape.BoundBox.ZMax, 3)


def edit(gdoc, view, obj):
    activate(view)
    FreeCAD.setActiveTransaction("Edit " + obj.Name)
    ok = gdoc.setEdit(obj, 0)
    settle(1200)
    return ok


def ok(view):
    activate(view)
    dialog(view).accept()
    settle(1500)


def run_mode(preview):
    tag = "preview" if preview else "no preview"
    PART.SetBool("PreviewOnEdit", preview)
    doc = FreeCAD.newDocument("PdTwoEdits")
    body = doc.addObject("PartDesign::Body", "Body")
    pad1 = body.newObject("PartDesign::Pad", "Pad1")
    pad1.Profile = rectangle(body, "Sketch1", 0, 10, 0)
    pad1.Length = 15
    pad2 = body.newObject("PartDesign::Pad", "Pad2")
    pad2.Profile = rectangle(body, "Sketch2", 3, 4, 15)
    pad2.Length = 5
    doc.recompute()
    doc.UndoMode = 1
    doc.clearUndos()
    gdoc = FreeCADGui.getDocument(doc.Name)
    settle()
    v1 = gdoc.mdiViewsOfType("Gui::View3DInventor")[0]
    activate(v1)
    FreeCADGui.runCommand("Std_ViewCreate")
    settle()
    v2 = gdoc.mdiViewsOfType("Gui::View3DInventor")[1]
    check("the body as built (%s)" % tag, solid(doc) == expect(15, 5), solid(doc))

    l1, l2 = 15, 5
    for rnd, (first, second) in enumerate((((v1, "Pad1"), (v2, "Pad2")),
                                           ((v2, "Pad2"), (v1, "Pad1")))):
        r = "%s, round %s" % (tag, "AB"[rnd])
        n1, n2 = l1 + 3, l2 + 2
        in1 = edit(gdoc, v1, pad1)
        t1 = type_into(v1, "lengthEdit", str(n1))
        in2 = edit(gdoc, v2, pad2)
        t2 = type_into(v2, "lengthEdit", str(n2))
        check("both in edit, the values typed are the features' (%s)" % r,
              in1 and in2 and t1 and t2 and round(pad1.Length.Value, 3) == n1
              and round(pad2.Length.Value, 3) == n2,
              (in1, in2, t1, t2, pad1.Length, pad2.Length))
        if preview:
            check("with the preview the body waits (%s)" % r,
                  solid(doc) == expect(l1, l2), (solid(doc), expect(l1, l2)))
        else:
            check("without the preview the body is both at once (%s)" % r,
                  solid(doc) == expect(n1, n2), (solid(doc), expect(n1, n2)))

        view, name = first
        ok(view)
        first_made = made(doc, name)
        if name == "Pad1":
            check("the OK of %s makes it (%s)" % (name, r), first_made == n1,
                  (first_made, n1))
        else:
            # Pad2's own solid is the body to it, on Pad1 as Pad1 is made
            base = l1 if preview else n1
            check("the OK of %s makes it (%s)" % (name, r),
                  first_made == max(base, 15 + n2), (first_made, max(base, 15 + n2)))
        other = second[1]
        if preview:
            if other == "Pad2":
                waits = solid(doc) == expect(l1, l2)
            else:
                waits = made(doc, "Pad1") == l1
            check("with the preview %s still waits for its own OK (%s)" % (other, r), waits,
                  (solid(doc), made(doc, "Pad1"), (l1, l2)))

        ok(second[0])
        check("the OK of %s makes the body of both (%s)" % (other, r),
              solid(doc) == expect(n1, n2) and gdoc.getInEdit() is None,
              (solid(doc), expect(n1, n2), gdoc.getInEdit()))
        l1, l2 = n1, n2

    pad1.Length = l1 + 1
    pad2.Length = l2 + 1
    doc.recompute()
    check("nothing is left paused: lengths set by a script after are made (%s)" % tag,
          solid(doc) == expect(l1 + 1, l2 + 1), (solid(doc), expect(l1 + 1, l2 + 1)))
    FreeCAD.closeDocument(doc.Name)
    settle(500)


def run():
    try:
        VIEW.SetBool("PerViewEdit", True)
        run_mode(True)
        run_mode(False)
    except Exception:
        note("FAIL run:\n" + traceback.format_exc())
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    PART.RemBool("PreviewOnEdit")
    VIEW.RemBool("PerViewEdit")
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
