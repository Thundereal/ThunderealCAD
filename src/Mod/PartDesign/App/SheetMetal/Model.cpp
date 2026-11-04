// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/
#include "PreCompiled.h"

#include <algorithm>
#include <limits>
#include <map>

#include <BRepClass_FaceClassifier.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>

#include <Base/Exception.h>

#include "Model.h"

using namespace PartDesign::SheetMetal;

namespace
{

/** Whether a point lies behind the line where a bend ends, as the wall after
 * it runs off: none of that wall is. By angle about the axis alone, a point
 * far behind the bend, in its angle but nowhere near its radius, is not
 * before it, and the wall's slab goes on there.
 */
bool behindEnd(const CylBend& bend, const gp_Pnt& point, double tol)
{
    const gp_Ax1 axis(bend.frame.Location(), bend.frame.Direction());
    const gp_Dir out = bend.frame.YDirection().Rotated(axis, bend.angle);
    const gp_Pnt end = bend.frame.Location().Translated(
        gp_Vec(bend.frame.XDirection().Rotated(axis, bend.angle)) * bend.radius);
    return gp_Vec(end, point).Dot(gp_Vec(out)) < -tol;
}

/** Whether a point lies beside a bend, past one of its ends along its axis:
 * a bend stopping inside its wall's end bounds that wall over its own
 * length only, the wall running on beside it
 */
bool besideBend(const Element& bend, const gp_Pnt& point, double tol)
{
    for (const gp_Pln& limit : bend.limits) {
        const gp_Dir n = limit.Axis().Direction();
        if (n.IsParallel(bend.bend.frame.Direction(), Precision::Angular())
            && gp_Vec(limit.Location(), point).Dot(gp_Vec(n)) > tol) {
            return true;
        }
    }
    return false;
}

}  // namespace

int Model::addWall(const std::string& name, const gp_Ax3& frame)
{
    Element wall;
    wall.name = name;
    wall.frame = frame;
    elements.push_back(wall);
    return static_cast<int>(elements.size()) - 1;
}

int Model::addBend(const std::string& name, const CylBend& bend, int before, int after)
{
    for (int index : {before, after}) {
        checkIndex(index);
        if (elements[index].kind != Element::Kind::Wall) {
            FC_THROWM(Base::IndexError, "Sheet metal: element " << index << " is not a wall");
        }
    }
    Element element;
    element.kind = Element::Kind::Bend;
    element.name = name;
    element.bend = bend;
    element.before = before;
    element.after = after;
    elements.push_back(element);
    return static_cast<int>(elements.size()) - 1;
}

int Model::find(const std::string& name) const
{
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (elements[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Model::checkIndex(int index) const
{
    if (index < 0 || index >= static_cast<int>(elements.size())) {
        FC_THROWM(Base::IndexError, "Sheet metal: no element " << index);
    }
}

bool Model::contains(int index, const gp_Pnt& point, double tol) const
{
    checkIndex(index);
    const Element& element = elements[index];
    for (const gp_Pln& limit : element.limits) {
        if (gp_Vec(limit.Location(), point).Dot(gp_Vec(limit.Axis().Direction())) > tol) {
            return false;
        }
    }
    if (element.kind == Element::Kind::Bend) {
        return element.bend.contains(point, tol);
    }
    const double depth =
        gp_Vec(element.frame.Location(), point).Dot(gp_Vec(element.frame.Direction()));
    if (depth < -tol || depth > thickness + tol) {
        return false;
    }
    // on the wall's side of every bend it meets
    for (const Element& bend : elements) {
        if (bend.kind != Element::Kind::Bend) {
            continue;
        }
        if (bend.before == index && bend.bend.side(point) > 0
            && !besideBend(bend, point, tol)) {
            return false;
        }
        if (bend.after == index
            && (bend.bend.sideAfter(point) < 0 || behindEnd(bend.bend, point, tol))
            && !besideBend(bend, point, tol)) {
            return false;
        }
    }
    return true;
}

int Model::owner(const gp_Pnt& point, double tol) const
{
    for (auto kind : {Element::Kind::Bend, Element::Kind::Wall}) {
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (elements[i].kind == kind && contains(static_cast<int>(i), point, tol)) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

namespace
{

/// A point inside the face, off its boundary: the middle of its parameter
/// box if that is in it, else the first in from a grid over the box
bool interiorPoint(const TopoDS_Face& face, gp_Pnt& point)
{
    double u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const Handle(Geom_Surface) surface = BRep_Tool::Surface(face);
    if (surface.IsNull()) {
        return false;
    }
    const double tol = BRep_Tool::Tolerance(face);
    const int n = 8;
    for (int step = 0; step <= n * n; ++step) {
        double u = (u0 + u1) / 2, v = (v0 + v1) / 2;
        if (step > 0) {
            const int i = (step - 1) / n, j = (step - 1) % n;
            u = u0 + (u1 - u0) * (i + 0.5) / n;
            v = v0 + (v1 - v0) * (j + 0.5) / n;
        }
        BRepClass_FaceClassifier inside(face, gp_Pnt2d(u, v), tol);
        if (inside.State() == TopAbs_IN) {
            point = surface->Value(u, v);
            return true;
        }
    }
    return false;
}

}  // namespace

int Model::owner(const TopoDS_Face& face, double tol) const
{
    gp_Pnt point;
    if (face.IsNull() || !interiorPoint(face, point)) {
        return -1;
    }
    // A wall's slab runs on in its plane past where the wall is, so a point
    // of another element's face can lie in it -- a flange's leg running down
    // past a plate's plane, its point at the plate's height; a mitre at a
    // rip, half in the other wall's slab. A face of the wall lies in its
    // slab whole: a wall the face sticks out of is passed over (2026-11-04).
    auto within = [&](const gp_Ax3& frame) {
        for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
            const TopoDS_Vertex& v = TopoDS::Vertex(it.Current());
            const double d =
                gp_Vec(frame.Location(), BRep_Tool::Pnt(v)).Dot(gp_Vec(frame.Direction()));
            const double t = 1e-6 + BRep_Tool::Tolerance(v);
            if (d < -t || d > thickness + t) {
                return false;
            }
        }
        return true;
    };
    int first = -1;
    for (auto kind : {Element::Kind::Bend, Element::Kind::Wall}) {
        for (std::size_t i = 0; i < elements.size(); ++i) {
            const Element& element = elements[i];
            if (element.kind != kind || !contains(static_cast<int>(i), point, tol)) {
                continue;
            }
            if (first < 0) {
                first = static_cast<int>(i);
            }
            if (kind == Element::Kind::Bend || within(element.frame)) {
                return static_cast<int>(i);
            }
        }
    }
    // none it fits: as before, the first that holds it
    return first;
}

void Model::transform(const gp_Trsf& trsf)
{
    for (Element& element : elements) {
        element.frame.Transform(trsf);
        element.bend.frame.Transform(trsf);
        for (gp_Pln& limit : element.limits) {
            limit.Transform(trsf);
        }
    }
}

gp_Trsf Model::placement(int index, int root) const
{
    checkIndex(index);
    checkIndex(root);
    if (elements[root].kind != Element::Kind::Wall) {
        FC_THROWM(Base::ValueError, "Sheet metal: the flat's root " << root << " is not a wall");
    }
    // a bend moves with the wall it starts from
    const int wall = elements[index].kind == Element::Kind::Bend ? elements[index].before : index;
    // out from the root over the bends, each unrolled the way it is crossed
    std::map<int, gp_Trsf> placed {{root, gp_Trsf()}};
    std::vector<int> next {root};
    while (!next.empty() && !placed.count(wall)) {
        const int from = next.back();
        next.pop_back();
        const gp_Trsf here = placed[from];
        for (const Element& bend : elements) {
            if (bend.kind != Element::Kind::Bend) {
                continue;
            }
            if (bend.before == from && !placed.count(bend.after)) {
                placed[bend.after] = here * bend.bend.unbend();
                next.push_back(bend.after);
            }
            else if (bend.after == from && !placed.count(bend.before)) {
                placed[bend.before] = here * bend.bend.refold();
                next.push_back(bend.before);
            }
        }
    }
    auto it = placed.find(wall);
    if (it == placed.end()) {
        FC_THROWM(Base::IndexError,
                  "Sheet metal: element " << index << " is not joined to the flat's root " << root);
    }
    return it->second;
}

gp_Pnt Model::toFlat(int index, const gp_Pnt& point, int root) const
{
    const gp_Trsf move = placement(index, root);
    if (elements[index].kind == Element::Kind::Bend) {
        return elements[index].bend.toFlat(point).Transformed(move);
    }
    return point.Transformed(move);
}
