"""A drag out of an Auto Delete link group is a move (docs/TransactionLogJournal.md sec 37.12).

A link group in Auto Delete mode removes an element taken out of its list:
that is what the mode is for, and a removal by the API keeps doing it. The
tree's drag took the element out the same way, so an element dragged out
of the group to another was removed before it could be dropped. Ruled
2026-10-22: the drag is a move.

What is done, by the calls the tree's drop makes as a drag ends (the old
parent's dragObject(), the new one's dropObject(), one transaction):
  - an element of a link group in Auto Delete mode -- the object itself,
    the mode makes no link -- is dragged onto a plain group;
  - an element of a group in Auto Unlink mode, a link the group made for
    the object put in it, is dragged onto the same plain group;
  - a copy a copy-on-change link made, from the group holding its copies,
    is dragged onto the same plain group.

Claims:
  - the element is moved: in the plain group, out of the link group;
  - one Std_Undo puts it back;
  - the link the group made is moved, and no longer the group's own
    (_LinkOwner cleared);
  - the copy is moved too, and the link still links it;
  - an element taken out by the API (setLink(i, None)) is still removed.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

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


def settle(n=20):
    for _ in range(n):
        QtCore.QCoreApplication.processEvents()


def drop(obj, parent, onto):
    """A drag of `obj` out of `parent` onto `onto`, by the calls the tree's
    drop makes (Tree.cpp, TreeWidget::dropEvent) in one transaction"""
    doc = obj.Document
    doc.openTransaction("drag")
    parent.ViewObject.dragObject(obj)
    if obj.isAttachedToDocument():
        onto.ViewObject.dropObject(obj)
    doc.commitTransaction()
    settle()


def build():
    try:
        doc = FreeCAD.newDocument("DragOut")
        doc.UndoMode = 1
        STATE["doc"] = doc.Name
        doc.openTransaction("make")
        obj = doc.addObject("App::FeatureTest", "Obj")
        obj.Label = "Obj"
        g = doc.addObject("App::LinkGroup", "G")
        g.LinkMode = "Auto Delete"
        g.setLink([obj])
        doc.addObject("App::DocumentObjectGroup", "Target").Label = "Target"
        doc.commitTransaction()
        doc.recompute()
        STATE["elem"] = obj.Name
        check("the case: the element is the object", g.ElementList == [obj],
              [o.Name for o in g.ElementList])
        settle(50)
        drop(obj, g, doc.Target)
        QtCore.QTimer.singleShot(500, dropped)
    except Exception:
        note("ABORT build:\n" + traceback.format_exc())
        finish()


def dropped():
    try:
        doc = FreeCAD.getDocument(STATE["doc"])
        elem = doc.getObject(STATE["elem"])
        check("the element is moved, not removed",
              elem is not None and elem in doc.Target.Group and elem not in doc.G.ElementList,
              (elem, [o.Name for o in doc.Target.Group], [o.Name for o in doc.G.ElementList]))
        FreeCADGui.runCommand("Std_Undo")
        QtCore.QTimer.singleShot(500, undone)
    except Exception:
        note("ABORT dropped:\n" + traceback.format_exc())
        finish()


def undone():
    try:
        doc = FreeCAD.getDocument(STATE["doc"])
        elem = doc.getObject(STATE["elem"])
        check("one undo puts it back",
              elem is not None and elem in doc.G.ElementList and elem not in doc.Target.Group,
              (elem, [o.Name for o in doc.G.ElementList], [o.Name for o in doc.Target.Group]))
        unlinkBuild()
    except Exception:
        note("ABORT undone:\n" + traceback.format_exc())
        finish()


def unlinkBuild():
    try:
        doc = FreeCAD.getDocument(STATE["doc"])
        doc.openTransaction("unlink")
        obj = doc.addObject("App::FeatureTest", "Obj2")
        obj.Label = "Obj2"
        u = doc.addObject("App::LinkGroup", "U")
        u.LinkMode = "Auto Unlink"
        u.setLink([obj])
        made = u.ElementList[0]
        made.Label = "Made"
        doc.commitTransaction()
        doc.recompute()
        STATE["made"] = made.Name
        check("the case: the element is a link the group made",
              made.isDerivedFrom("App::Link") and made._LinkOwner == u.ID,
              (made.TypeId, made.getPropertyByName("_LinkOwner"), u.ID))
        settle(50)
        drop(made, u, doc.Target)
        QtCore.QTimer.singleShot(500, unlinkDropped)
    except Exception:
        note("ABORT unlinkBuild:\n" + traceback.format_exc())
        finish()


def unlinkDropped():
    try:
        doc = FreeCAD.getDocument(STATE["doc"])
        made = doc.getObject(STATE["made"])
        check("the link the group made is moved, no longer the group's own",
              made is not None and made in doc.Target.Group and made not in doc.U.ElementList
              and made._LinkOwner == 0,
              (made, made and made._LinkOwner, [o.Name for o in doc.Target.Group]))
        copyBuild()
    except Exception:
        note("ABORT unlinkDropped:\n" + traceback.format_exc())
        finish()


def copyBuild():
    try:
        doc = FreeCAD.getDocument(STATE["doc"])
        doc.openTransaction("copy")
        src = doc.addObject("App::DocumentObjectGroup", "Src")
        src.Label = "Src"
        src.addProperty("App::PropertyInteger", "A")
        src.setPropertyStatus("A", "CopyOnChange")
        link = doc.addObject("App::Link", "Inst")
        link.Label = "Inst"
        link.LinkedObject = src
        link.LinkCopyOnChange = "Enabled"
        link.A = 5
        doc.recompute()
        doc.commitTransaction()
        copy = link.getLinkedObject(False)
        coc = link.LinkCopyOnChangeGroup
        STATE["copy"] = copy.Name
        check("the case: the link made a copy, held by its copy-on-change group",
              copy != src and coc is not None and copy in coc.ElementList,
              (copy.Name, coc and [o.Name for o in coc.ElementList]))
        settle(50)
        drop(copy, coc, doc.Target)
        QtCore.QTimer.singleShot(500, copyDropped)
    except Exception:
        note("ABORT copyBuild:\n" + traceback.format_exc())
        finish()


def copyDropped():
    try:
        doc = FreeCAD.getDocument(STATE["doc"])
        copy = doc.getObject(STATE["copy"])
        coc = doc.Inst.LinkCopyOnChangeGroup
        check("the copy is moved, and the link still links it",
              copy is not None and copy in doc.Target.Group and copy not in coc.ElementList
              and doc.Inst.getLinkedObject(False) == copy,
              (copy, [o.Name for o in doc.Target.Group], [o.Name for o in coc.ElementList]))
        doc.openTransaction("api")
        elem = doc.G.ElementList[0]
        name = elem.Name
        doc.G.setLink({0: None})
        doc.commitTransaction()
        check("an element taken out by the API is still removed", doc.getObject(name) is None,
              [o.Name for o in doc.Objects])
    except Exception:
        note("ABORT copyDropped:\n" + traceback.format_exc())
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
