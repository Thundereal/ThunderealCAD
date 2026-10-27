"""A paste onto objects already there (docs/TransactionLogJournal.md sec 36.4, 37.9).

`Obj` of document A, and `LA` linking it, are copied into document B, where
`Obj` is changed and linked by a new `L`. B's three are copied and pasted
into A, where `Obj` and `LA` are live under the same UIDs. The paste asks
before its step opens, in the dependency dialog: a check leaves an object
out, a second assigns it.

Claims:
  - the paste asks, listing their `Obj` and `LA` against ours;
  - `LA` assigned: `Obj`, which it depends on and which is here too, is
    assigned with it (it may not be duplicated); ours take their state, the
    `L` pasted links our `Obj`, nothing else is added;
  - one Std_Undo takes it back;
  - `Obj` left out: ours is kept as it is, and the `L` pasted links it;
  - pasted again with nothing changed: a second object beside ours, of a
    UID of its own;
  - pasted again and cancelled: nothing is added;
  - pasted into B, where it was copied from: nothing is asked;
  - an App::Part assigned: its origin follows it -- the rows of the origin
    and its axes say so and are not checked by hand -- and the part keeps
    its live origin, with no second origin left over.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
STATE = {}
Qt = QtCore.Qt


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def dialog():
    for w in QtWidgets.QApplication.topLevelWidgets():
        if w.isVisible() and w.objectName() == "DlgPasteSelection":
            return w
    return None


def tree(dlg):
    return dlg.findChild(QtWidgets.QTreeWidget, "treeWidget")


def items(dlg):
    # By label; a link's row reads "LA(Obj)", listed as "LA"
    t = tree(dlg)
    out = {}
    for i in range(t.topLevelItemCount()):
        it = t.topLevelItem(i)
        if not it.text(0).startswith("<"):
            out[it.text(0).split("(")[0]] = it
    return out


def rows(dlg):
    """label -> (included, here column, assigned, checkable by hand)"""
    return {
        label: (
            it.checkState(0) != Qt.Unchecked,
            it.text(1),
            it.checkState(1) == Qt.Checked,
            bool(it.flags() & Qt.ItemIsUserCheckable),
        )
        for label, it in items(dlg).items()
    }


def docs():
    return FreeCAD.getDocument(STATE["a"]), FreeCAD.getDocument(STATE["b"])


def paste(into, how, then):
    """Paste into `into`, answering the dialog: `how` is "reject", or a list
    of (label, "assign" | "exclude") done before OK; `then` runs after."""
    STATE["asked"] = None
    STATE["after"] = None

    def accept(dlg):
        STATE["after"] = rows(dlg)
        box = dlg.findChild(QtWidgets.QDialogButtonBox, "buttonBox")
        box.button(QtWidgets.QDialogButtonBox.Ok).click()

    def drive(left=[25]):
        dlg = dialog()
        if dlg is None:
            left[0] -= 1
            if left[0] > 0:
                QtCore.QTimer.singleShot(100, drive)
            else:
                left[0] = 25
            return
        left[0] = 25
        STATE["asked"] = rows(dlg)
        if how == "reject":
            dlg.reject()
            return
        found = items(dlg)
        missing = [label for label, _ in how if label not in found]
        if missing:
            note("FAIL the rows to answer are listed | %r not in %r" % (missing, list(found)))
            dlg.reject()
            return
        for label, what in how:
            if what == "assign":
                found[label].setCheckState(1, Qt.Checked)
            else:
                found[label].setCheckState(0, Qt.Unchecked)
        # The dialog settles a change on a short timer
        QtCore.QTimer.singleShot(300, lambda: accept(dlg))

    FreeCAD.setActiveDocument(into.Name)
    FreeCADGui.ActiveDocument = FreeCADGui.getDocument(into.Name)
    FreeCADGui.Selection.clearSelection()
    QtCore.QTimer.singleShot(100, drive)
    FreeCADGui.runCommand("Std_Paste")
    QtCore.QTimer.singleShot(3000, then)


def copyFrom(doc, objs):
    FreeCAD.setActiveDocument(doc.Name)
    FreeCADGui.ActiveDocument = FreeCADGui.getDocument(doc.Name)
    FreeCADGui.Selection.clearSelection()
    for o in objs:
        FreeCADGui.Selection.addSelection(o)
    FreeCADGui.runCommand("Std_Copy")


def build():
    try:
        a = FreeCAD.newDocument("ClashA")
        a.UndoMode = 1
        a.openTransaction("make")
        obj = a.addObject("App::FeatureTest", "Obj")
        la = a.addObject("App::Link", "LA")
        la.LinkedObject = obj
        a.commitTransaction()
        b = FreeCAD.newDocument("ClashB")
        away = b.copyObject([obj, la])
        bobj = b.getObject(away[0].Name)
        bobj.Integer = 5
        b.addObject("App::Link", "L").LinkedObject = bobj
        STATE["a"], STATE["b"] = a.Name, b.Name
        STATE["uid"] = obj.ObjectUid
        STATE["before"] = obj.Integer
        STATE["count"] = len(a.Objects)
        copyFrom(b, [bobj, b.getObject(away[1].Name), b.L])
        paste(a, [("LA", "assign")], assigned)
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()


def assigned():
    try:
        a, b = docs()
        asked = STATE["asked"] or {}
        check("the paste asks, listing their Obj and LA against ours",
              "Assign to Obj" == asked.get("Obj", (0, ""))[1]
              and asked.get("LA", (0, ""))[1].startswith("Assign to LA")
              and asked.get("L", (0, "x"))[1] == "", asked)
        after = STATE["after"] or {}
        check("LA assigned: Obj, which it depends on, is assigned with it",
              after.get("LA", (0, 0, False))[2] and after.get("Obj", (0, 0, False))[2], after)
        links = [o for o in a.Objects if o.isDerivedFrom("App::Link") and o.Name != "LA"]
        check("ours take their state, the link pasted links ours, nothing else is added",
              a.Obj.Integer == 5 and len(links) == 1 and links[0].LinkedObject == a.Obj
              and a.LA.LinkedObject == a.Obj and len(a.Objects) == STATE["count"] + 1,
              (a.Obj.Integer, [o.Name for o in a.Objects]))
        FreeCADGui.runCommand("Std_Undo")
        QtCore.QTimer.singleShot(500, undone)
    except Exception:
        note("ABORT assigned:\n" + traceback.format_exc())
        finish()


def undone():
    try:
        a, b = docs()
        check("one undo takes it back",
              a.Obj.Integer == STATE["before"] and len(a.Objects) == STATE["count"],
              (a.Obj.Integer, [o.Name for o in a.Objects]))
        paste(a, [("Obj", "exclude")], excluded)
    except Exception:
        note("ABORT undone:\n" + traceback.format_exc())
        finish()


def excluded():
    try:
        a, b = docs()
        made = [o for o in a.Objects if o.isDerivedFrom("App::FeatureTest")]
        links = [o for o in a.Objects if o.isDerivedFrom("App::Link") and o.Name != "LA"]
        check("Obj left out: ours kept as it is, and what was pasted links it",
              len(made) == 1 and a.Obj.Integer == STATE["before"] and links
              and all(l.LinkedObject == a.Obj for l in links),
              (a.Obj.Integer, [(o.Name, getattr(o, "LinkedObject", None)) for o in a.Objects]))
        FreeCADGui.runCommand("Std_Undo")
        QtCore.QTimer.singleShot(500, lambda: paste(a, [], duplicated))
    except Exception:
        note("ABORT excluded:\n" + traceback.format_exc())
        finish()


def duplicated():
    try:
        a, b = docs()
        made = [o for o in a.Objects if o.isDerivedFrom("App::FeatureTest") and o.Name != "Obj"]
        check("nothing changed in the dialog: a second object, a UID of its own",
              STATE["asked"] is not None and len(made) == 1
              and made[0].ObjectUid != STATE["uid"] and a.Obj.Integer == STATE["before"],
              [(o.Name, o.ObjectUid) for o in made])
        STATE["count"] = len(a.Objects)
        paste(a, "reject", cancelled)
    except Exception:
        note("ABORT duplicated:\n" + traceback.format_exc())
        finish()


def cancelled():
    try:
        a, b = docs()
        check("cancelled, nothing is added",
              STATE["asked"] is not None and len(a.Objects) == STATE["count"],
              [o.Name for o in a.Objects])
        STATE["countB"] = len(b.Objects)
        paste(b, "reject", beside)
    except Exception:
        note("ABORT cancelled:\n" + traceback.format_exc())
        finish()


def beside():
    try:
        a, b = docs()
        check("pasted where it was copied from, nothing is asked",
              STATE["asked"] is None and len(b.Objects) == STATE["countB"] + 3,
              (STATE["asked"], [o.Name for o in b.Objects]))
        partBuild()
    except Exception:
        note("ABORT beside:\n" + traceback.format_exc())
        finish()


def partBuild():
    try:
        a, b = docs()
        a.openTransaction("part")
        part = a.addObject("App::Part", "Part")
        a.commitTransaction()
        features = part.Origin.OriginFeatures   # made: they are lazy
        STATE["origin"] = part.Origin.Name
        STATE["origins"] = len([o for o in a.Objects if o.isDerivedFrom("App::Origin")])
        STATE["count"] = len(a.Objects)
        copied = b.copyObject(part, True, True)
        bpart = [o for o in copied if o.isDerivedFrom("App::Part")][0]
        bpart.Placement = FreeCAD.Placement(FreeCAD.Vector(7, 0, 0), FreeCAD.Rotation())
        STATE["partLabels"] = [bpart.Origin.Label] + [f.Label for f in bpart.Origin.OriginFeatures]
        copyFrom(b, [bpart, bpart.Origin] + list(bpart.Origin.OriginFeatures))
        paste(a, [("Part", "assign")], partAssigned)
    except Exception:
        note("ABORT partBuild:\n" + traceback.format_exc())
        finish()


def partAssigned():
    try:
        a, b = docs()
        after = STATE["after"] or {}
        owned = [after.get(label) for label in STATE["partLabels"]]
        check("the origin's rows follow the part, kept, not checked by hand",
              owned and all(r is not None and not r[0] and not r[3] and "kept" in r[1]
                            for r in owned), after)
        origins = [o for o in a.Objects if o.isDerivedFrom("App::Origin")]
        check("the part assigned keeps its live origin, and no second origin is left",
              a.Part.Placement.Base.x == 7 and a.Part.Origin.Name == STATE["origin"]
              and len(origins) == STATE["origins"] and len(a.Objects) == STATE["count"],
              (a.Part.Placement.Base.x, a.Part.Origin.Name, [o.Name for o in a.Objects]))
    except Exception:
        note("ABORT partAssigned:\n" + traceback.format_exc())
    finish()


def finish():
    try:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
    except Exception:
        note("ABORT finish:\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, build)
