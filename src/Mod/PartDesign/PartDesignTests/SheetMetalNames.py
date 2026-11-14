# SPDX-License-Identifier: LGPL-2.1-or-later

# What every sheet metal test's document is asked as it closes (the audit of
# 2026-11-10, docs/SheetMetal.md sec 12): of each sheet metal feature in it,
#
# - its tree highlight is the faces it made, all of them and no other. The
#   highlight is Feature.getGeneratedElements(), the element map asked which
#   faces trace to the feature. A face is the feature's own when it is off
#   the skin of the feature it builds on, somewhere; a face of that skin
#   kept or only trimmed is not, as a pocket leaves the faces it cut into
#   unlit. A face of that skin grown -- a wall's skin with a tab's on it --
#   keeps the name it had, and is lit or not as that name says; so does the
#   side of the sheet a fold or a jog carries, the wall's still;
# - every face, edge and vertex has a name of the element map's, so a
#   reference to one holds across an edit.
#
# A feature that failed, a suppressed one, one with nothing before it (its
# whole shape is new, and the view lights it whole) and one that leaves the
# solid as it was (a cross break; a second wall drawn where the first is) are
# not asked the first.

import FreeCAD
import Part

# how many elements a failure lists
SHOWN = 6
# the features that carry a side of the sheet elsewhere: of what is off the
# skin they started from, only their own pieces (the bends, a jog's run) are
# theirs
CARRYING = ("PartDesign::SheetMetalFold", "PartDesign::SheetMetalJog")
# and those that make the part anew of the one before them: all of it theirs
REMAKING = ("PartDesign::SheetMetalConvert",)


def faceKey(face):
    c = face.CenterOfMass
    return (face.Surface.TypeId, round(face.Area, 5), round(c.x, 5), round(c.y, 5), round(c.z, 5))


def on(point, shape):
    return Part.Vertex(point).distToShape(shape)[0] < 1e-5


def pointOn(face):
    """A point of the face inside its outline, or None"""
    u0, u1, v0, v1 = face.ParameterRange
    for s, t in ((0.5, 0.5), (0.31, 0.37), (0.71, 0.59), (0.23, 0.77), (0.83, 0.19)):
        point = face.valueAt(u0 + s * (u1 - u0), v0 + t * (v1 - v0))
        if on(point, face):
            return point
    return None


def sortFaces(feature, base):
    """feature's faces by name, against the shape base it builds on:

    - own, off base's skin: an edge of it, or its inside, is;
    - grown, a face of base's and more: it holds a point of a face base no
      longer has as it was, on a surface of the same kind. The join gives
      such a face one name, the base's (a reference to that face then holds),
      so it may be lit or not;

    the rest are base's faces kept or trimmed.
    """
    kept = {}
    for face in base.Faces:
        key = faceKey(face)
        kept[key] = kept.get(key, 0) + 1
    skin = Part.Compound(base.Faces)
    off = []
    for i, face in enumerate(feature.Faces):
        key = faceKey(face)
        if kept.get(key, 0) > 0:
            kept[key] -= 1
            continue
        points = [e.valueAt((e.FirstParameter + e.LastParameter) / 2) for e in face.Edges]
        inside = pointOn(face)
        if inside is not None:
            points.append(inside)
        if not all(on(point, skin) for point in points):
            off.append(("Face%d" % (i + 1), face))
    # the faces of base that are no more, a point of each
    gone = []
    for face in base.Faces:
        if kept.get(faceKey(face), 0) > 0:
            kept[faceKey(face)] -= 1
            point = pointOn(face)
            if point is not None:
                gone.append((face.Surface.TypeId, point))
    own, grown = [], []
    for name, face in off:
        kind = face.Surface.TypeId
        box = face.BoundBox
        box.enlarge(1e-4)
        if any(k == kind and box.isInside(point) and on(point, face) for k, point in gone):
            grown.append(name)
        else:
            own.append(name)
    return own, grown


def partedOff(shape, base, name, lit):
    """Whether the face name of shape lies on a face of base that another
    face of shape, not among lit, lies on too"""
    point = pointOn(shape.getElement(name))
    if point is None:
        return False
    for whole in base.Faces:
        if not on(point, whole):
            continue
        for i, face in enumerate(shape.Faces):
            other = "Face%d" % (i + 1)
            if other == name or other in lit or not whole.BoundBox.intersect(face.BoundBox):
                continue
            inside = pointOn(face)
            if inside is not None and on(inside, whole):
                return True
    return False


def same(shape, base):
    """Whether shape is base still: its volume and its box"""
    a, b = shape.BoundBox, base.BoundBox
    sides = ("XMin", "YMin", "ZMin", "XMax", "YMax", "ZMax")
    return abs(shape.Volume - base.Volume) < 1e-9 * shape.Volume and all(
        abs(getattr(a, side) - getattr(b, side)) < 1e-7 for side in sides
    )


def unnamed(shape):
    """The elements of shape the element map has no name for"""
    res = []
    for kind, count in (
        ("Face", len(shape.Faces)),
        ("Edge", len(shape.Edges)),
        ("Vertex", len(shape.Vertexes)),
    ):
        for i in range(count):
            name = "%s%d" % (kind, i + 1)
            mapped = shape.getElementName(name, 1)
            if not mapped or mapped == name:
                res.append(name)
    return res


def listed(names):
    return ", ".join(names[:SHOWN]) + (" ..." if len(names) > SHOWN else "")


def problems(doc):
    res = []
    for obj in doc.Objects:
        if not obj.TypeId.startswith("PartDesign::SheetMetal"):
            continue
        if not obj.isDerivedFrom("Part::Feature") or not obj.isValid():
            continue
        shape = obj.Shape
        if shape.isNull():
            continue
        missing = unnamed(shape)
        if missing:
            res.append(
                "%s (%s): %d elements without a name: %s"
                % (obj.Name, obj.TypeId, len(missing), listed(missing))
            )
        if not obj.isDerivedFrom("PartDesign::Feature") or obj.Suppress:
            continue
        base = obj.BaseFeature
        if base is None or base.Shape.isNull() or not base.Shape.Faces:
            continue
        if same(shape, base.Shape):
            continue
        own, grown = (set(names) for names in sortFaces(shape, base.Shape))
        lit = {name for name in obj.getGeneratedElements() if name.startswith("Face")}
        tool = getattr(obj, "AddSubShape", None)
        pieces = None
        if tool is not None and not tool.isNull() and tool.Faces:
            pieces = Part.Compound(tool.Faces)

        def onPieces(name):
            point = pointOn(shape.getElement(name))
            return pieces is not None and point is not None and on(point, pieces)

        if obj.TypeId in REMAKING:
            if len(lit) != len(shape.Faces):
                res.append(
                    "%s (%s): %d of its %d faces in its highlight, where it remakes the part"
                    % (obj.Name, obj.TypeId, len(lit), len(shape.Faces))
                )
            continue
        if obj.TypeId in CARRYING:
            # the side carried is the wall's still, under the names it had
            carried = {name for name in own if not onPieces(name)}
            own -= carried
            grown |= carried
        if own - lit:
            res.append(
                "%s (%s): %d of its %d own faces not in its highlight: %s"
                % (obj.Name, obj.TypeId, len(own - lit), len(own), listed(sorted(own - lit)))
            )
        # a piece of the feature's standing where it cut the base back, its
        # face flush with the skin that was there: the feature's still
        wrong = [name for name in sorted(lit - own - grown) if not onPieces(name)]
        # and a part of a face of the skin the feature parted from the rest
        # of it -- a gusset's foot on the bend it stiffens -- is one it made,
        # where the rest, unlit, is the face still
        wrong = [name for name in wrong if not partedOff(shape, base.Shape, name, lit)]
        if wrong:
            res.append(
                "%s (%s): %d faces of the feature before it in its highlight: %s"
                % (obj.Name, obj.TypeId, len(wrong), listed(wrong))
            )
    return res


def close(test, doc):
    """Close doc, a sheet metal test's, failing the test for what its
    features' names are missing"""
    try:
        found = problems(doc)
    finally:
        FreeCAD.closeDocument(doc.Name)
    if found:
        test.fail("\n".join(found))
