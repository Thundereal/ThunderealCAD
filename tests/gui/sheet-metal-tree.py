"""Sheet metal's rule in the tree (docs/SheetMetal.md sec 4.6).

  - a body's only solid, holding a sheet metal feature, has a solid body;
  - the rule is the first child of that solid body, before its features;
  - the body's top level shows the solid body, not the rule or the feature;
  - with the auto aux groups on, Misc does not hold the rule;
  - a second sheet in the body, taking the rule of the first, has the rule
    under its own solid body too;
  - a body whose solid holds no sheet metal is as before: no solid body.

Run through scripts/gui-test.sh; registered in ctest by tests/gui/CMakeLists.txt.
"""

import os
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "SheetMetalTree"
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


def names(objs):
    return [o.Name for o in objs]


def sketch(body, points, name):
    sk = body.newObject("Sketcher::SketchObject", name)
    for i in range(len(points)):
        a, b = points[i], points[(i + 1) % len(points)]
        sk.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
    return sk


def wall(body, sk, name, newSolid=False):
    w = body.newObject("PartDesign::SheetMetalBaseWall", name)
    w.Profile = sk
    if newSolid:
        w.NewSolid = True
    return w


def run():
    try:
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        body.setAutoAuxGroup(True)
        first = wall(body, sketch(body, [(0, 0), (20, 0), (20, 10), (0, 10)], "Sketch"), "Wall")
        doc.recompute()
        pump()
        rule = first.Rule
        solids = body.SolidBodies
        check("a lone sheet solid has a solid body", len(solids) == 1, names(solids))
        solid = solids[0]
        children = names(solid.ViewObject.claimChildren())
        check(
            "the rule is the solid body's first child, its features after",
            children[:2] == [rule.Name, first.Name],
            children,
        )
        top = names(body.ViewObject.claimChildren())
        check("the body shows the solid body", solid.Name in top, top)
        check(
            "and not the rule or the feature",
            rule.Name not in top and first.Name not in top,
            top,
        )
        misc = [o for o in body.Group if o.Name.startswith("Misc")]
        check(
            "Misc does not hold the rule",
            misc and rule not in misc[0].Group,
            names(misc[0].Group) if misc else "no Misc",
        )

        second = wall(
            body,
            sketch(body, [(50, 0), (70, 0), (70, 10), (50, 10)], "Sketch001"),
            "Wall001",
            newSolid=True,
        )
        doc.recompute()
        pump()
        check("the second sheet takes the rule", second.Rule == rule)
        other = body.getSolidBody(second)
        check("and has its solid body", other is not None and other != solid)
        children = names(other.ViewObject.claimChildren()) if other else []
        check("with the rule first under it", children[:1] == [rule.Name], children)

        plain = doc.addObject("PartDesign::Body", "Plain")
        box = plain.newObject("PartDesign::AdditiveBox", "Box")
        doc.recompute()
        pump()
        check("a body with no sheet metal has no solid body", plain.SolidBodies == [])
        check("its feature is at its top level", box.Name in names(plain.ViewObject.claimChildren()))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
