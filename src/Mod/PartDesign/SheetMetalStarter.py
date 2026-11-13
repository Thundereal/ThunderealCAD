# SPDX-License-Identifier: LGPL-2.1-or-later
# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This library is free software; you can redistribute it and/or         *
# *   modify it under the terms of the GNU Library General Public           *
# *   License as published by the Free Software Foundation; either          *
# *   version 2 of the License, or (at your option) any later version.      *
# *                                                                         *
# ***************************************************************************
"""Sheet metal's starter shapes (docs/SheetMetal.md sec 5.1, ruled 2026-11-06)

An L, U, tub, hat or box made of ordinary features, each edited afterwards as
any is: a rectangle sketch whose Length and Width constraints are named, a
base wall on it, a flange on the edges the shape has, and for a hat or a box
a second flange, the lip, on the legs' top ends. In Python, not C++: the
sketch is Sketcher's, which PartDesign's library does not link.
"""

import FreeCAD

KINDS = ("L", "U", "Tub", "Hat", "Box")
_TOL = 1e-7


def _linesAt(shape, keep):
    """The names of shape's straight edges both of whose ends keep(point)"""
    res = []
    for i, e in enumerate(shape.Edges):
        if len(e.Vertexes) != 2 or e.Curve.TypeId != "Part::GeomLine":
            continue
        if all(keep(v.Point) for v in e.Vertexes):
            res.append("Edge%d" % (i + 1))
    return res


def _near(a, b):
    return abs(a - b) < _TOL


def _sketch(body, x0, y0, length, width, centered):
    import Part
    import Sketcher

    V = FreeCAD.Vector
    sketch = body.newObject("Sketcher::SketchObject", "Sketch")
    pts = [V(x0, y0, 0), V(x0 + length, y0, 0), V(x0 + length, y0 + width, 0), V(x0, y0 + width, 0)]
    for i in range(4):
        sketch.addGeometry(Part.LineSegment(pts[i], pts[(i + 1) % 4]))
    cons = [Sketcher.Constraint("Coincident", i, 2, (i + 1) % 4, 1) for i in range(4)]
    cons += [
        Sketcher.Constraint("Horizontal", 0),
        Sketcher.Constraint("Horizontal", 2),
        Sketcher.Constraint("Vertical", 1),
        Sketcher.Constraint("Vertical", 3),
    ]
    if centered:
        # opposite corners about the origin
        cons.append(Sketcher.Constraint("Symmetric", 0, 1, 2, 1, -1, 1))
    else:
        cons.append(Sketcher.Constraint("Coincident", 0, 1, -1, 1))
    sketch.addConstraint(cons)
    sketch.addConstraint(Sketcher.Constraint("DistanceX", 0, 1, 0, 2, length))
    sketch.renameConstraint(len(sketch.Constraints) - 1, "Length")
    sketch.addConstraint(Sketcher.Constraint("DistanceY", 1, 1, 1, 2, width))
    sketch.renameConstraint(len(sketch.Constraints) - 1, "Width")
    return sketch


def _refuse(made, doc, before, why):
    """Take back what was made, the rule too if it is new, and say why"""
    for name in [o.Name for o in doc.Objects if o.Name not in before]:
        obj = doc.getObject(name)
        if obj is None:
            continue
        for parent in obj.InList:
            if parent.isDerivedFrom("PartDesign::Body") and obj in parent.Group:
                parent.removeObject(obj)
    for name in reversed([o.Name for o in doc.Objects if o.Name not in before]):
        if doc.getObject(name) is not None:
            doc.removeObject(name)
    doc.recompute()
    raise ValueError(why)


def makeStarterShape(body, kind, length, width, height, FlangeWidth=5.0, Centered=True):
    """makeStarterShape(body, kind, length, width, height, FlangeWidth=5, Centered=True)
    -> list -- an L, U, tub, hat or box made in body of ordinary features: the
    sketch, the base wall, the flange and, for a hat or a box, the lip.

    kind is "L", "U", "Tub", "Hat" or "Box". The sizes are the outside ones
    (mm): length along X, width along Y, height up from the plate's
    underside. An L's leg stands on the far long edge (+Y), a U's on both long
    edges, a tub's, a hat's and a box's on all four. A hat's lip reaches
    FlangeWidth out past the legs, a box's in from their outsides. Centred on
    the origin, or from it along X and Y. Throws ValueError for an unknown
    kind or sizes too small for the sheet's bends, making nothing."""
    if not hasattr(body, "isDerivedFrom") or not body.isDerivedFrom("PartDesign::Body"):
        raise TypeError("A starter shape is made in a PartDesign body")
    if kind not in KINDS:
        raise ValueError("No starter shape %r: one of %s" % (kind, ", ".join(KINDS)))
    length, width, height = float(length), float(width), float(height)
    flangeWidth = float(FlangeWidth)
    if min(length, width, height) <= 0:
        raise ValueError("A starter shape's sizes are above 0")
    doc = body.Document
    before = {o.Name for o in doc.Objects}
    made = []

    x0, y0 = (-length / 2, -width / 2) if Centered else (0.0, 0.0)
    sketch = _sketch(body, x0, y0, length, width, Centered)
    made.append(sketch)
    wall = body.newObject("PartDesign::SheetMetalBaseWall", "BaseWall")
    wall.Profile = sketch
    made.append(wall)
    # hidden, as Base Wall's command hides its profile
    sketch.Visibility = False
    doc.recompute()
    if "Invalid" in wall.State or wall.Shape.isNull():
        _refuse(made, doc, before, "The base wall could not be made")
    rule = wall.Rule
    thickness = rule.Thickness.Value
    bend = thickness + (rule.BendRadius.Value if wall.UseRuleRadius else wall.BendRadius.Value)

    # what the bends need, checked before any flange is made
    lipped = kind in ("Hat", "Box")
    if height <= (2 if lipped else 1) * bend:
        _refuse(
            made,
            doc,
            before,
            "A %s %g mm high is too low for its bends: above %g mm"
            % (kind, height, (2 if lipped else 1) * bend),
        )
    across = {"L": [width], "U": [width]}.get(kind, [length, width])
    sides = 1 if kind == "L" else 2
    if min(across) <= sides * bend:
        _refuse(
            made,
            doc,
            before,
            "A %s %g by %g mm is too small for its bends: above %g mm across"
            % (kind, length, width, sides * bend),
        )
    if lipped and flangeWidth <= bend:
        _refuse(
            made,
            doc,
            before,
            "A lip %g mm wide is too narrow for its bend: above %g mm" % (flangeWidth, bend),
        )

    # the legs, on the plate's top skin edges
    x1, y1 = x0 + length, y0 + width
    shape = wall.Shape
    top = shape.BoundBox.ZMax

    def onTop(p, alongY=None, alongX=None):
        if not _near(p.z, top):
            return False
        if alongY is not None:
            return _near(p.y, alongY)
        return _near(p.x, alongX)

    edges = _linesAt(shape, lambda p: onTop(p, alongY=y1))
    if kind != "L":
        edges += _linesAt(shape, lambda p: onTop(p, alongY=y0))
    if kind not in ("L", "U"):
        edges += _linesAt(shape, lambda p: onTop(p, alongX=x0))
        edges += _linesAt(shape, lambda p: onTop(p, alongX=x1))
    flange = body.newObject("PartDesign::SheetMetalFlange", "Flange")
    flange.Edges = (wall, edges)
    flange.Length = height
    flange.LengthType = "OuterSharp"
    flange.BendPosition = "MaterialInside"
    made.append(flange)
    doc.recompute()
    if "Invalid" in flange.State:
        _refuse(made, doc, before, "The legs could not be made: %s" % flange.getStatusString())
    if not lipped:
        return made

    # the lip, on the legs' top ends: from their outer skins for a hat,
    # bending out, their inner ones for a box, bending in
    shape = flange.Shape
    inset = 0.0 if kind == "Hat" else thickness

    def onEnd(p):
        if not _near(p.z, height):
            return False
        return (
            _near(p.x, x0 + inset)
            or _near(p.x, x1 - inset)
            or _near(p.y, y0 + inset)
            or _near(p.y, y1 - inset)
        )

    lip = body.newObject("PartDesign::SheetMetalFlange", "Lip")
    names = _linesAt(shape, onEnd)
    lip.Edges = (flange, names)
    lip.LengthType = "OuterSharp"
    lip.BendPosition = "MaterialInside"
    if kind == "Box":
        # A box's lips bend in, toward each other at the corners, where a
        # press could not form them mitered (they lie flat mitered, gaps
        # 0): each stops square, clear of its neighbour's by the rule's gap. The gap is off each end, measured from the
        # outside of the leg across it, as the lip's width is.
        clear = flangeWidth + rule.Gap.Value
        gaps = []
        for name in names:
            edge = shape.getElement(name)
            ends = []
            for v in edge.Vertexes[:1] + edge.Vertexes[-1:]:
                p = v.Point
                across = (
                    min(abs(p.y - y0), abs(p.y - y1))
                    if _near(p.x, x0 + inset) or _near(p.x, x1 - inset)
                    else min(abs(p.x - x0), abs(p.x - x1))
                )
                ends.append(clear - across)
            gaps.append(ends)
        # one gap pair for the feature: the lips are alike on each side
        lip.Gap1 = max(g[0] for g in gaps)
        lip.Gap2 = max(g[1] for g in gaps)
    if kind == "Hat":
        # measured from where the leg's inner skin meets the lip's top: the
        # thickness more than how far it reaches past the leg, whatever the
        # thickness becomes
        lip.Length = flangeWidth + thickness
        lip.setExpression("Length", "%.12g mm + %s.Thickness" % (flangeWidth, rule.Name))
    else:
        lip.Length = flangeWidth
    made.append(lip)
    doc.recompute()
    if "Invalid" in lip.State:
        _refuse(made, doc, before, "The lip could not be made: %s" % lip.getStatusString())
    return made
