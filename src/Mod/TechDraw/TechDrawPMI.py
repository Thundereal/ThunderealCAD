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
"""The 3D model's PMI in a drawing (docs/GDandT.md sec 9): Import Model
Annotations. For a part view, each PMI callout of its source -- a feature
control frame, a datum, a datum target, a note -- whose plane faces the view
and whose feature the view shows becomes a TechDraw::DrawPMICallout linked to
it: its leader ends on the feature's longest visible edge, and it stands
where the 3D view shows it -- or, where that is on another frame or on the
view's lines on paper, the nearest place clear of them. Each PMI dimension
whose features the view shows true -- a hole end-on, two faces edge-on --
becomes a TechDraw dimension on those edges, linked: its tolerance, basic
flag and decimals are the 3D one's. Run again, it follows the model: a
callout the user moved stays where they put it, one whose feature the view
no longer shows, or whose 3D object is gone, goes; a dimension keeps where
the user put it. Headless: no GUI needed."""

import heapq
import math

import FreeCAD as App
import Part

KINDS = ("PMI::GeometricTolerance", "PMI::Datum", "PMI::DatumTarget", "PMI::Note")
# How square to the view a callout's plane must be to be shown in it: 1 deg
PARALLEL = math.cos(math.radians(1.0))
# Where a callout whose plane is not the view's stands: off its leader's end
STAND_OFF = 12.0
# On paper, in mm: the space kept between a frame and another or a line
GAP = 1.0
# How many places are tried for a frame before it stands where it is asked
TRIES = 200


def _height(c=None):
    """The lettering height a callout is drawn at on paper: its own, or the
    TechDraw default a new one gets"""
    vo = getattr(c, "ViewObject", None) if c is not None else None
    if vo is not None and hasattr(vo, "Fontsize"):
        return vo.Fontsize.Value
    grp = App.ParamGet("User parameter:BaseApp/Preferences/Mod/TechDraw/Dimensions")
    return grp.GetFloat("FontSize", 5.0)


def _centre3d(callout):
    """The middle of its frame or symbol as the 3D view lays it out, in its
    plane's coordinates"""
    import PMI

    h = callout.TextHeight.Value if callout.TextHeight.Value > 0 else 3.5
    box = PMI.extent(callout, h)
    if box is None:
        return App.Vector()
    return App.Vector((box[0] + box[2]) / 2, (box[1] + box[3]) / 2, 0)


def _size(callout, height):
    """Its frame's width and height on paper at lettering \a height"""
    import PMI

    box = PMI.extent(callout, height)
    if box is None:
        return 0.0, 0.0
    return box[2] - box[0], box[3] - box[1]


def _clip(rect, a, b):
    """Whether the segment a-b crosses the rectangle (x0, y0, x1, y1):
    Liang-Barsky"""
    x0, y0, x1, y1 = rect
    t0, t1 = 0.0, 1.0
    dx, dy = b[0] - a[0], b[1] - a[1]
    for p, q in ((-dx, a[0] - x0), (dx, x1 - a[0]), (-dy, a[1] - y0), (dy, y1 - a[1])):
        if abs(p) < 1e-12:
            if q < 0:
                return False
            continue
        t = q / p
        if p < 0:
            t0 = max(t0, t)
        else:
            t1 = min(t1, t)
        if t0 > t1:
            return False
    return True


def _overlap(r, s):
    return r[0] < s[2] and s[0] < r[2] and r[1] < s[3] and s[1] < r[3]


def _place(wanted, size, boxes, segments):
    """The place nearest \a wanted, (x, y) on paper, where a frame of \a size
    is clear of \a boxes and of the lines \a segments; failing that, clear of
    the boxes; failing that, \a wanted"""
    w, h = size[0] / 2 + GAP, size[1] / 2 + GAP
    # Just clear, not touching: a place on the edge of what it clears is
    # in the way of it again
    e = 1e-6
    for lines in (segments, []):
        heap = [(0.0, wanted)]
        seen = set()
        while heap and len(seen) < TRIES:
            _d, (x, y) = heapq.heappop(heap)
            key = (round(x, 6), round(y, 6))
            if key in seen:
                continue
            seen.add(key)
            rect = (x - w, y - h, x + w, y + h)
            hit = [b for b in boxes if _overlap(rect, b)]
            for a, b in lines:
                if _clip(rect, a, b):
                    hit.append((min(a[0], b[0]), min(a[1], b[1]),
                                max(a[0], b[0]), max(a[1], b[1])))
            if not hit:
                return x, y
            # Just clear of each in the way: left, right, below, above
            for b in hit:
                for to in ((b[0] - w - e, y), (b[2] + w + e, y),
                           (x, b[1] - h - e), (x, b[3] + h + e)):
                    d = math.hypot(to[0] - wanted[0], to[1] - wanted[1])
                    heapq.heappush(heap, (d, to))
    return wanted


def _lines(view, names, scale):
    """The view's visible lines on paper, y up, as segments"""
    out = []
    for i, (name, _source, _index) in enumerate(names):
        if ";HLR:V" not in name:
            continue
        edge = view.getEdgeByIndex(i)
        pts = [(p.x * scale, -p.y * scale) for p in edge.discretize(Deflection=0.05)]
        out += list(zip(pts, pts[1:]))
    return out


def _box(c, scale):
    """Where a callout's frame stands on paper now"""
    w, h = _size(c.Source, _height(c))
    x, y = c.X.Value * scale, c.Y.Value * scale
    return (x - w / 2, y - h / 2, x + w / 2, y + h / 2)


def _page(view):
    for o in view.InList:
        if o.isDerivedFrom("TechDraw::DrawPage"):
            return o
    return None


def _sources(view):
    out = list(view.Source)
    out += [o for o in getattr(view, "XSource", [])]
    return out


def _callouts(sources):
    """The PMI callouts of the sets whose part is one of the view's sources,
    or a solid of one (a Body's)"""
    out = []
    for s in sources:
        doc = s.Document
        for o in doc.Objects:
            if o.TypeId != "PMI::AnnotationSet" or not o.Part:
                continue
            part = o.Part
            if part is s or s in part.InListRecursive or part in s.OutListRecursive:
                for c in o.Group:
                    if c.TypeId in KINDS and c not in out:
                        out.append(c)
    return out


def _set_of(callout):
    for o in callout.InList:
        if o.TypeId == "PMI::AnnotationSet":
            return o
    return None


def _global(obj, sub=""):
    """\a obj's shape, or its element \a sub, in the document's coordinates:
    a Body's solid is placed by the Body, which its own Placement is not"""
    shape = Part.getShape(obj, sub, needSubElement=bool(sub), transform=False).copy()
    shape.transformShape(obj.getGlobalPlacement().toMatrix())
    return shape


def _faces_of(feat):
    """A toleranced feature's faces, in the document's coordinates"""
    faces = []
    for obj, subs in feat.References:
        for sub in subs:
            if sub.startswith("Face"):
                try:
                    faces.append(_global(obj, sub))
                except Exception:
                    pass
    return faces


def _feature_faces(callout):
    """Its feature's faces, in the document's coordinates"""
    feat = getattr(callout, "Feature", None)
    return _faces_of(feat) if feat is not None else []


def _on(faces, edge):
    """Whether a 3D edge lies on one of \a faces: its ends and its middle"""
    t0, t1 = edge.FirstParameter, edge.LastParameter
    pts = [Part.Vertex(edge.valueAt(t)) for t in (t0, (t0 + t1) / 2, t1)]
    return any(all(f.distToShape(p)[0] <= 1e-6 for p in pts) for f in faces)


def _visible(names):
    """(i, n): the view's visible edge i, projected from its source's Edge n"""
    return [(i, index) for i, (name, _source, index) in enumerate(names)
            if index > 0 and ";HLR:V" in name]


def _paper(view, scale, p):
    return (p.x * scale, -p.y * scale)


def _end_on(view, names, shape, faces, direction):
    """The view's longest circle of \a faces whose axis is the view's: a
    cylinder seen end-on -- (i, its 2D edge) or None"""
    best = None
    for i, index in _visible(names):
        e = shape.Edges[index - 1]
        if not isinstance(e.Curve, Part.Circle):
            continue
        if abs(App.Vector(e.Curve.Axis).normalize().dot(direction)) < PARALLEL:
            continue
        if not _on(faces, e):
            continue
        e2 = view.getEdgeByIndex(i)
        if best is None or e2.Length > best[1].Length:
            best = (i, e2)
    return best


def _edge_on(view, names, shape, faces, direction):
    """The view's longest straight edge of a plane of \a faces seen edge-on:
    (i, its 2D edge) or None"""
    flat = [f for f in faces if isinstance(f.Surface, Part.Plane)
            and abs(f.Surface.Axis.dot(direction)) <= math.sqrt(1 - PARALLEL * PARALLEL)]
    best = None
    for i, index in _visible(names):
        e = shape.Edges[index - 1]
        if not isinstance(e.Curve, Part.Line) or not _on(flat, e):
            continue
        e2 = view.getEdgeByIndex(i)
        if e2.Length > 1e-6 and (best is None or e2.Length > best[1].Length):
            best = (i, e2)
    return best


def _dimension(view, names, shape, dim, direction, scale, extent):
    """How a PMI dimension is drawn in the view, measured from what the view
    shows: (TechDraw Type, [the view's edges], where its text wants to stand
    on paper), or None when the view does not show it true"""
    if dim.Type in ("Diameter", "Radius"):
        faces = [f for feat in dim.Features for f in _faces_of(feat)]
        found = _end_on(view, names, shape, faces, direction)
        if found is None:
            return None
        i, e2 = found
        cx, cy = _paper(view, scale, e2.Curve.Center)
        out = e2.Curve.Radius * scale + 10.0
        return dim.Type, ["Edge%d" % i], (cx + out * 0.7071, cy + out * 0.7071)
    if dim.Type == "Width":
        faces = _faces_of(dim.Features[0]) if dim.Features else []
        sides = [[f] for f in faces]
    elif dim.Type == "Distance":
        sides = [_faces_of(f) for f in dim.Features]
    else:
        return None
    if len(sides) != 2:
        return None
    found = [_edge_on(view, names, shape, side, direction) for side in sides]
    if None in found:
        return None
    (i0, a), (i1, b) = found
    da, db = a.Curve.Direction, b.Curve.Direction
    if abs(abs(da.dot(db)) - 1) > 1e-9:
        return None
    ma = _paper(view, scale, a.valueAt((a.FirstParameter + a.LastParameter) / 2))
    mb = _paper(view, scale, b.valueAt((b.FirstParameter + b.LastParameter) / 2))
    mid = ((ma[0] + mb[0]) / 2, (ma[1] + mb[1]) / 2)
    x0, y0, x1, y1 = extent
    if abs(da.x) < 1e-9:
        # Up the page: measured across, the text above the view
        return "DistanceX", ["Edge%d" % i0, "Edge%d" % i1], (mid[0], y1 + 10.0)
    if abs(da.y) < 1e-9:
        return "DistanceY", ["Edge%d" % i0, "Edge%d" % i1], (x1 + 10.0, mid[1])
    # Askew: off the middle, square to the edges
    n = (-da.y, -da.x)
    return "Distance", ["Edge%d" % i0, "Edge%d" % i1], (mid[0] + 10 * n[0], mid[1] + 10 * n[1])


def _dimensions(sources):
    """The PMI dimensions of the sets whose part is one of the view's sources"""
    out = []
    for s in sources:
        for o in s.Document.Objects:
            if o.TypeId != "PMI::AnnotationSet" or not o.Part:
                continue
            part = o.Part
            if part is s or s in part.InListRecursive or part in s.OutListRecursive:
                out += [c for c in o.Group if c.TypeId == "PMI::Dimension" and c not in out]
    return out


def _old_dimensions(view):
    """The dimensions of the view linked to a PMI dimension, by it. One whose
    3D dimension is gone has no link left: it stays, a plain dimension that
    still measures the drawing"""
    out = {}
    for o in view.Document.Objects:
        if not o.isDerivedFrom("TechDraw::DrawViewDimension") or o.Source is None:
            continue
        refs = o.References2D
        if refs and refs[0][0] == view:
            out.setdefault(o.Source.Name, []).append(o)
    return out


def _subs(d):
    out = []
    for obj, sub in d.References2D:
        out += list(sub) if isinstance(sub, (list, tuple)) else [sub]
    return out


def _old(view):
    """The callouts imported into the view before, by the PMI object each shows"""
    out = {}
    for o in view.Document.Objects:
        if o.isDerivedFrom("TechDraw::DrawPMICallout") and o.SourceView == view:
            out.setdefault(o.Source.Name if o.Source else None, []).append(o)
    return out


def _moved(c):
    at = c.ImportOffset
    if len(at) != 2:
        return True
    return (abs(c.X.Value - c.OriginX.Value - at[0]) > 1e-6
            or abs(c.Y.Value - c.OriginY.Value - at[1]) > 1e-6)


def importAnnotations(view, every=False):
    """Import or refresh the PMI callouts and dimensions of \\a view's source;
    with \\a every, those whose plane is not the view's too. Returns (made,
    updated, removed), or None when the view has no projection yet (nothing
    changes then: run it once the view is computed)"""
    names = view.getEdgeNames()
    if not names:
        return None
    page = _page(view)
    sources = _sources(view)
    # The shape the view projected: its Edge n is a projected edge's
    # sourceIndex n (the same order: the projection works on a copy)
    shape = Part.makeCompound([_global(o) for o in sources])
    if len(shape.Edges) != len(view.getProjectionShape().Edges):
        App.Console.PrintWarning("Import model annotations: %s's source is not the shape it "
                                 "projected; nothing imported\n" % view.Label)
        return None
    direction = App.Vector(view.Direction).normalize()
    old = _old(view)
    made, updated, keep, plans = [], [], set(), []
    for callout in _callouts(sources):
        pset = _set_of(callout)
        if pset is None:
            continue
        to_doc = pset.Part.getGlobalPlacement() if pset.Part else App.Placement()
        normal = to_doc.Rotation.multVec(callout.Plane.Rotation.multVec(App.Vector(0, 0, 1)))
        square = abs(normal.dot(direction)) >= PARALLEL
        if not square and not every:
            continue
        faces = _feature_faces(callout)
        if not faces:
            continue
        # Its feature's longest visible edge: the leader's end, at its middle
        best = None
        for i, (name, _source, index) in enumerate(names):
            if index <= 0 or ";HLR:V" not in name:
                continue
            edge3d = shape.Edges[index - 1]
            mid3d = edge3d.valueAt((edge3d.FirstParameter + edge3d.LastParameter) / 2)
            if min(f.distToShape(Part.Vertex(mid3d))[0] for f in faces) > 1e-6:
                continue
            edge2d = view.getEdgeByIndex(i)
            if best is None or edge2d.Length > best[0]:
                mid2d = edge2d.valueAt((edge2d.FirstParameter + edge2d.LastParameter) / 2)
                best = (edge2d.Length, mid2d, mid3d)
        if best is None:
            continue
        _length, mid2d, mid3d = best
        # The view's frame, a balloon's Origin and X/Y: unscaled, y up -- the
        # edges given unscaled, y down
        tip = (mid2d.x, -mid2d.y)
        if square:
            # Where the 3D view shows it: the middle of its frame, projected
            at = callout.Plane.multVec(_centre3d(callout))
            offset = view.projectPoint(to_doc.multVec(at) - mid3d, False)
            stand = (offset.x, offset.y)
        else:
            stand = (STAND_OFF, STAND_OFF)
        olds = old.get(callout.Name, [])
        c = olds[0] if olds else None
        for extra in olds[1:]:
            view.Document.removeObject(extra.Name)
        plans.append((callout, c, tip, stand, c is not None and _moved(c)))
        if c is not None:
            keep.add(c.Name)
    # On paper: the moved ones where the user put them, and the ones kept;
    # each other nearest where the 3D view shows it, clear of those, of the
    # ones placed before it and of the view's lines
    scale = view.getScale()
    segments = _lines(view, names, scale)
    boxes = []
    for callout, c, tip, _stand, moved in plans:
        if moved:
            # Its leader to the feature as it is now, the frame where it was
            # against it
            dx, dy = c.X.Value - c.OriginX.Value, c.Y.Value - c.OriginY.Value
            c.OriginX, c.OriginY = tip
            c.X, c.Y = tip[0] + dx, tip[1] + dy
            updated.append(c)
            boxes.append(_box(c, scale))
    for source, olds in old.items():
        for c in olds:
            if c.Name not in keep and source is not None and c.Source is not None \
                    and _moved(c):
                boxes.append(_box(c, scale))
    for callout, c, tip, stand, moved in plans:
        if moved:
            continue
        size = _size(callout, _height(c))
        x, y = _place(((tip[0] + stand[0]) * scale, (tip[1] + stand[1]) * scale), size,
                      boxes, segments)
        boxes.append((x - size[0] / 2, y - size[1] / 2, x + size[0] / 2, y + size[1] / 2))
        stand = (x / scale - tip[0], y / scale - tip[1])
        if c is not None:
            c.OriginX, c.OriginY = tip
            c.X, c.Y = tip[0] + stand[0], tip[1] + stand[1]
            c.ImportOffset = [stand[0], stand[1]]
            updated.append(c)
            continue
        c = view.Document.addObject("TechDraw::DrawPMICallout", "PMICallout")
        # Its view and place first: the page makes its item when it is
        # added, a child of the view it then knows
        c.SourceView = view
        c.Source = callout
        c.Label = callout.Label
        c.EndType = "Filled Triangle" if callout.TypeId == "PMI::Datum" else "Filled Arrow"
        c.OriginX, c.OriginY = tip
        c.X, c.Y = tip[0] + stand[0], tip[1] + stand[1]
        c.ImportOffset = [stand[0], stand[1]]
        if page is not None:
            page.addView(c)
        made.append(c)
        keep.add(c.Name)
    # The dimensions: where the view shows what they measure true, linked
    # TechDraw dimensions on those edges, the text clear of the frames
    xs = [p[0] for seg in segments for p in seg] or [0.0]
    ys = [p[1] for seg in segments for p in seg] or [0.0]
    extent = (min(xs), min(ys), max(xs), max(ys))
    olddims = _old_dimensions(view)
    keepdims = set()
    height = _height()
    for dim in _dimensions(sources):
        pset = _set_of(dim)
        if pset is None:
            continue
        to_doc = pset.Part.getGlobalPlacement() if pset.Part else App.Placement()
        normal = to_doc.Rotation.multVec(dim.Plane.Rotation.multVec(App.Vector(0, 0, 1)))
        if abs(normal.dot(direction)) < PARALLEL and not every:
            continue
        plan = _dimension(view, names, shape, dim, direction, scale, extent)
        if plan is None:
            continue
        kind, subs, wanted = plan
        ds = olddims.get(dim.Name, [])
        d = ds[0] if ds else None
        for extra in ds[1:]:
            view.Document.removeObject(extra.Name)
        if d is not None:
            # Where the user has it; on what the view shows now
            if d.Type != kind:
                d.Type = kind
            if _subs(d) != subs:
                d.References2D = [(view, sub) for sub in subs]
            updated.append(d)
        else:
            size = (0.6 * height * (len(dim.EquivalentText) + 2), height)
            x, y = _place(wanted, size, boxes, segments)
            boxes.append((x - size[0] / 2, y - size[1] / 2, x + size[0] / 2, y + size[1] / 2))
            d = view.Document.addObject("TechDraw::DrawViewDimension", "Dimension")
            d.Type = kind
            d.MeasureType = "Projected"
            d.References2D = [(view, sub) for sub in subs]
            d.Source = dim
            d.Label = dim.Label
            d.X, d.Y = x, y
            if page is not None:
                page.addView(d)
            made.append(d)
        keepdims.add(d.Name)
    removed = []
    for olds in olddims.values():
        for d in olds:
            if d.Name not in keepdims:
                # What it measures not shown true in the view any more
                removed.append(d.Label)
                view.Document.removeObject(d.Name)
    for source, olds in old.items():
        for c in olds:
            if c.Name in keep:
                continue
            # Its 3D object gone, or its feature not shown: gone, unless
            # the user placed it where they want it
            if source is not None and c.Source is not None and _moved(c):
                continue
            removed.append(c.Label)
            view.Document.removeObject(c.Name)
    return made, updated, removed
