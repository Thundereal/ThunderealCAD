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
it: its leader ends on the feature's longest visible edge, and it stands where
the 3D view shows it. Run again, it follows the model: a callout the user
moved stays where they put it, one whose feature the view no longer shows,
or whose 3D object is gone, goes. Headless: no GUI needed."""

import math

import FreeCAD as App
import Part

KINDS = ("PMI::GeometricTolerance", "PMI::Datum", "PMI::DatumTarget", "PMI::Note")
# How square to the view a callout's plane must be to be shown in it: 1 deg
PARALLEL = math.cos(math.radians(1.0))
# Where a callout whose plane is not the view's stands: off its leader's end
STAND_OFF = 12.0


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


def _feature_faces(callout):
    """Its feature's faces, in the document's coordinates"""
    feat = getattr(callout, "Feature", None)
    if feat is None:
        return []
    faces = []
    for obj, subs in feat.References:
        for sub in subs:
            if sub.startswith("Face"):
                try:
                    faces.append(_global(obj, sub))
                except Exception:
                    pass
    return faces


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
    """Import or refresh the PMI callouts of \\a view's source; with \\a every,
    those whose plane is not the view's too. Returns (made, updated, removed),
    or None when the view has no projection yet (nothing changes then: run it
    once the view is computed)"""
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
    made, updated, keep = [], [], set()
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
            # (UprightAt is in its plane's coordinates)
            at = callout.Plane.multVec(callout.UprightAt[0]) if callout.UprightAt \
                else callout.Plane.Base
            offset = view.projectPoint(to_doc.multVec(at) - mid3d, False)
            stand = (offset.x, offset.y)
        else:
            stand = (STAND_OFF, STAND_OFF)
        olds = old.get(callout.Name, [])
        if olds:
            c = olds[0]
            for extra in olds[1:]:
                view.Document.removeObject(extra.Name)
            moved = _moved(c)
            dx, dy = c.X.Value - c.OriginX.Value, c.Y.Value - c.OriginY.Value
            c.OriginX, c.OriginY = tip
            if moved:
                c.X, c.Y = tip[0] + dx, tip[1] + dy
            else:
                c.X, c.Y = tip[0] + stand[0], tip[1] + stand[1]
                c.ImportOffset = [stand[0], stand[1]]
            updated.append(c)
        else:
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
    removed = []
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
