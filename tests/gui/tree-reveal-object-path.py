"""An object in error is revealed in the tree along its object path.

docs/HandsOnQueue.md entry 207, the user 2026-10-21: an object listed in two
places -- under a link group, its object path, and under a feature that only
groups it in the tree -- was revealed under one or the other from run to run
(the first of its rows by address); "Lens only group the binder in tree
hierarchy, not 3D scene. so it should not be treated as a parent".

A test feature claimed in the tree by a Python feature made first (so its row
comes first in the tree) and held by an App::LinkGroup. It fails on purpose;
both rows collapsed before each recompute. Claims, three times over:
  - it is revealed under the link group's row;
  - not under the claimer's.

Runs on the real screen: the tree's status update reveals what is shown.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


class Claimer:
    """A view provider that lists the test feature under its object"""

    def __init__(self, vobj, child):
        # kept before the proxy is set: attach() is not called for a proxy
        # set on a view provider already attached
        self.vobj = vobj
        self.child = child.Name
        vobj.Proxy = self

    def attach(self, vobj):
        self.vobj = vobj

    def claimChildren(self):
        obj = self.vobj.Object
        return [obj.Document.getObject(self.child)]

    def dumps(self):
        return self.child

    def loads(self, state):
        self.child = state


def tree():
    for t in FreeCADGui.getMainWindow().findChildren(QtWidgets.QTreeWidget):
        if t.metaObject().className() == "Gui::TreeWidget":
            return t
    return None


def dump():
    out = []

    def walk(item, depth):
        out.append("%s%s%s" % ("  " * depth, item.text(0),
                                " (open)" if item.isExpanded() else ""))
        for i in range(item.childCount()):
            walk(item.child(i), depth + 1)

    t = tree()
    for i in range(t.topLevelItemCount()):
        walk(t.topLevelItem(i), 0)
    return " | ".join(out)


def parents_of(label):
    """The labels of the rows a row of this label is listed under, by a walk
    of the tree (findItems() did not give the child rows here)"""
    found = []

    def walk(item):
        for i in range(item.childCount()):
            child = item.child(i)
            if child.text(0) == label:
                found.append(item.text(0))
            walk(child)

    t = tree()
    for i in range(t.topLevelItemCount()):
        walk(t.topLevelItem(i))
    return found


def rows(label):
    return tree().findItems(label, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive)


def run():
    try:
        doc = FreeCAD.newDocument("TreeRevealObjectPath")
        fail = doc.addObject("App::FeatureTest", "Failing")
        claimer = doc.addObject("App::FeaturePython", "AClaimer")
        Claimer(claimer.ViewObject, fail)
        group = doc.addObject("App::LinkGroup", "ZGroup")
        group.ElementList = [fail]
        doc.recompute()
        pump(80)
        note("INFO the parents by object path: %s" % [(p.Name, s) for p, s in fail.Parents])
        # both places shown
        for label in ("AClaimer", "ZGroup"):
            for item in rows(label):
                item.setExpanded(True)
        pump(40)
        places = parents_of(fail.Label)
        note("INFO its rows under %s" % places)
        note("INFO the claimer claims %s" % [o.Name for o in claimer.ViewObject.claimChildren()])
        note("INFO the tree: %s" % dump())
        check("it is listed under the claimer and under the link group",
              "AClaimer" in places and "ZGroup" in places, places)

        fail.ExceptionType = 2
        revealed = []
        for k in range(3):
            for label in ("AClaimer", "ZGroup"):
                for item in rows(label):
                    try:
                        item.setExpanded(False)
                    except RuntimeError:
                        pass
            pump(20)
            fail.touch()
            doc.recompute()
            pump(150)
            shown = []
            for label in ("AClaimer", "ZGroup"):
                for item in rows(label):
                    try:
                        if item.isExpanded():
                            shown.append(label)
                    except RuntimeError:
                        pass
            note("INFO recompute %d: valid %s; expanded %s" % (k, fail.isValid(), shown))
            revealed.append(tuple(shown))
        check("revealed under the link group's row every time",
              all("ZGroup" in r for r in revealed), revealed)
        check("... and never under the claimer's",
              not any("AClaimer" in r for r in revealed), revealed)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        for name in list(FreeCAD.listDocuments().keys()):
            FreeCAD.closeDocument(name)
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
