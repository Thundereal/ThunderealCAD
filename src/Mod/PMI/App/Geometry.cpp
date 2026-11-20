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
#include <cmath>

#include <Bnd_Box.hxx>
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <gp_Ax1.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>

#include "Geometry.h"

using namespace PMI;

Primitive Primitive::reference() const
{
    Primitive res = *this;
    switch (type) {
        case Cylinder:
        case Cone:
            res.type = Line;
            break;
        case Sphere:
            res.type = Point;
            break;
        default:
            break;
    }
    return res;
}

const char* Primitive::typeName() const
{
    switch (type) {
        case Plane:
            return "plane";
        case Cylinder:
            return "cylinder";
        case Sphere:
            return "sphere";
        case Cone:
            return "cone";
        case Line:
            return "axis";
        case Point:
            return "point";
        case Torus:
            return "torus";
        default:
            return "unsupported surface";
    }
}

Primitive Geom::ofFace(const TopoDS_Shape& shape)
{
    Primitive res;
    if (shape.IsNull() || shape.ShapeType() != TopAbs_FACE) {
        return res;
    }
    const TopoDS_Face& face = TopoDS::Face(shape);
    BRepAdaptor_Surface surf(face);
    switch (surf.GetType()) {
        case GeomAbs_Plane: {
            gp_Pln pln = surf.Plane();
            res.type = Primitive::Plane;
            res.loc = pln.Location();
            gp_Dir n = pln.Axis().Direction();
            if (!pln.Direct()) {
                n.Reverse();
            }
            if (face.Orientation() == TopAbs_REVERSED) {
                n.Reverse();
            }
            res.dir = n;
            break;
        }
        case GeomAbs_Cylinder: {
            gp_Cylinder cyl = surf.Cylinder();
            res.type = Primitive::Cylinder;
            res.loc = cyl.Location();
            res.dir = cyl.Axis().Direction();
            res.radius = cyl.Radius();
            break;
        }
        case GeomAbs_Sphere: {
            gp_Sphere sph = surf.Sphere();
            res.type = Primitive::Sphere;
            res.loc = sph.Location();
            res.radius = sph.Radius();
            break;
        }
        case GeomAbs_Cone: {
            gp_Cone cone = surf.Cone();
            res.type = Primitive::Cone;
            res.loc = cone.Apex();
            res.dir = cone.Axis().Direction();
            res.angle = std::fabs(cone.SemiAngle());
            break;
        }
        case GeomAbs_Torus: {
            gp_Torus tor = surf.Torus();
            res.type = Primitive::Torus;
            res.loc = tor.Location();
            res.dir = tor.Axis().Direction();
            res.radius = tor.MinorRadius();
            res.major = tor.MajorRadius();
            break;
        }
        default:
            break;
    }
    return res;
}

namespace
{
double lineDistance(const gp_Pnt& p, const gp_Pnt& loc, const gp_Dir& dir)
{
    return gp_Lin(loc, dir).Distance(p);
}
bool parallel(const gp_Dir& a, const gp_Dir& b, double atol)
{
    return a.IsParallel(b, atol);
}
}  // namespace

bool Geom::sameSurface(const Primitive& a, const Primitive& b, double tol, double atol)
{
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case Primitive::Plane:
            return parallel(a.dir, b.dir, atol)
                && std::fabs(gp_Vec(a.loc, b.loc).Dot(gp_Vec(a.dir))) < tol;
        case Primitive::Cylinder:
            return std::fabs(a.radius - b.radius) < tol && parallel(a.dir, b.dir, atol)
                && lineDistance(b.loc, a.loc, a.dir) < tol;
        case Primitive::Sphere:
            return std::fabs(a.radius - b.radius) < tol && a.loc.Distance(b.loc) < tol;
        case Primitive::Cone:
            return std::fabs(a.angle - b.angle) < atol && parallel(a.dir, b.dir, atol)
                && a.loc.Distance(b.loc) < tol;
        case Primitive::Torus:
            return std::fabs(a.radius - b.radius) < tol && std::fabs(a.major - b.major) < tol
                && parallel(a.dir, b.dir, atol) && a.loc.Distance(b.loc) < tol;
        default:
            return false;
    }
}

namespace
{
constexpr double SizeTol = 1e-6;

/// Planes in pairs facing opposite ways, each pair the same width apart:
/// those facing one way, moved the width along their normal, are those
/// facing the other (side by side along it, or in a row across it)
bool widthPairs(const std::vector<const Primitive*>& planes, double& width)
{
    if (planes.empty() || planes.size() % 2 != 0) {
        return false;
    }
    const gp_Dir& n = planes.front()->dir;
    std::vector<double> one, other;
    for (auto p : planes) {
        double at = gp_Vec(p->loc.XYZ()).Dot(gp_Vec(n));
        if (p->dir.IsEqual(n, 1e-9)) {
            one.push_back(at);
        }
        else if (p->dir.IsOpposite(n, 1e-9)) {
            other.push_back(at);
        }
        else {
            return false;
        }
    }
    if (one.size() != other.size()) {
        return false;
    }
    std::sort(one.begin(), one.end());
    std::sort(other.begin(), other.end());
    double shift = other.front() - one.front();
    for (std::size_t i = 0; i < one.size(); ++i) {
        if (std::fabs(other[i] - one[i] - shift) > SizeTol) {
            return false;
        }
    }
    width = std::fabs(shift);
    return width > SizeTol;
}
}  // namespace

bool Geom::width(const std::vector<Primitive>& faces, double& width)
{
    std::vector<const Primitive*> planes;
    std::vector<const Primitive*> rounds;
    for (auto& p : faces) {
        if (p.type == Primitive::Plane) {
            planes.push_back(&p);
        }
        else if (p.type == Primitive::Cylinder) {
            rounds.push_back(&p);
        }
        else {
            return false;
        }
    }
    if (!widthPairs(planes, width)) {
        return false;
    }
    return std::all_of(rounds.begin(), rounds.end(), [&](const Primitive* c) {
        return std::fabs(2 * c->radius - width) < SizeTol
            && c->dir.IsNormal(planes.front()->dir, 1e-9);
    });
}

bool Geom::holes(const std::vector<Primitive>& faces, std::size_t& hole)
{
    std::vector<gp_Ax1> axes;
    std::vector<std::vector<double>> radii;
    auto axisOf = [&](const Primitive& p) -> int {
        for (std::size_t i = 0; i < axes.size(); ++i) {
            if (p.dir.IsParallel(axes[i].Direction(), 1e-9)
                && gp_Lin(axes[i]).Distance(p.loc) < SizeTol) {
                return int(i);
            }
        }
        return -1;
    };
    bool found = false;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const Primitive& p = faces[i];
        if (p.type != Primitive::Cylinder) {
            continue;
        }
        if (!found || p.radius < faces[hole].radius - SizeTol) {
            hole = i;
            found = true;
        }
        int a = axisOf(p);
        if (a < 0) {
            axes.emplace_back(p.loc, p.dir);
            radii.emplace_back();
            a = int(axes.size()) - 1;
        }
        auto& r = radii[a];
        if (std::none_of(r.begin(), r.end(), [&](double x) {
                return std::fabs(x - p.radius) < SizeTol;
            })) {
            r.push_back(p.radius);
        }
    }
    if (axes.empty()) {
        return false;
    }
    for (auto& r : radii) {
        std::sort(r.begin(), r.end());
        if (r.size() != radii.front().size()) {
            return false;
        }
        for (std::size_t i = 0; i < r.size(); ++i) {
            if (std::fabs(r[i] - radii.front()[i]) > SizeTol) {
                return false;
            }
        }
    }
    for (auto& p : faces) {
        if (p.type == Primitive::Cylinder) {
            continue;
        }
        bool fits = false;
        if (p.type == Primitive::Plane) {
            fits = std::any_of(axes.begin(), axes.end(), [&](const gp_Ax1& ax) {
                return p.dir.IsParallel(ax.Direction(), 1e-9);
            });
        }
        else if (p.type == Primitive::Cone) {
            fits = axisOf(p) >= 0;
        }
        if (!fits) {
            return false;
        }
    }
    return true;
}

bool Geom::distance(const Primitive& pa, const Primitive& pb, double& result, std::string& why)
{
    Primitive a = pa.reference();
    Primitive b = pb.reference();
    if (a.type == Primitive::None || b.type == Primitive::None) {
        why = "a distance needs planes, axes or points";
        return false;
    }
    // Order: Plane < Line < Point, so only the upper triangle is spelled out
    auto rank = [](const Primitive& p) {
        return p.type == Primitive::Plane ? 0 : p.type == Primitive::Line ? 1 : 2;
    };
    if (rank(a) > rank(b)) {
        std::swap(a, b);
    }
    gp_Vec ab(a.loc, b.loc);
    if (a.type == Primitive::Plane) {
        if (b.type == Primitive::Plane) {
            if (!parallel(a.dir, b.dir, 1e-9)) {
                why = "the planes are not parallel";
                return false;
            }
        }
        else if (b.type == Primitive::Line) {
            if (std::fabs(gp_Vec(a.dir).Dot(gp_Vec(b.dir))) > 1e-9) {
                why = "the axis is not parallel to the plane";
                return false;
            }
        }
        result = std::fabs(ab.Dot(gp_Vec(a.dir)));
        return true;
    }
    if (a.type == Primitive::Line) {
        if (b.type == Primitive::Line && !parallel(a.dir, b.dir, 1e-9)) {
            why = "the axes are not parallel";
            return false;
        }
        result = lineDistance(b.loc, a.loc, a.dir);
        return true;
    }
    result = a.loc.Distance(b.loc);
    return true;
}

bool Geom::angle(const Primitive& pa, const Primitive& pb, double& result, std::string& why)
{
    Primitive a = pa.reference();
    Primitive b = pb.reference();
    if ((a.type != Primitive::Plane && a.type != Primitive::Line)
        || (b.type != Primitive::Plane && b.type != Primitive::Line)) {
        why = "an angle needs planes or axes";
        return false;
    }
    double c = std::fabs(gp_Vec(a.dir).Dot(gp_Vec(b.dir)));
    c = std::min(1.0, c);
    double ang = std::acos(c);
    // A plane's direction is its normal: against an axis the angle is to
    // the plane itself
    if ((a.type == Primitive::Plane) != (b.type == Primitive::Plane)) {
        ang = M_PI / 2 - ang;
    }
    result = ang;
    return true;
}

TopoDS_Shape Geom::axisEdge(const gp_Pnt& loc, const gp_Dir& dir, const TopoDS_Shape& extent)
{
    if (extent.IsNull()) {
        return {};
    }
    Bnd_Box box;
    BRepBndLib::Add(extent, box);
    if (box.IsVoid()) {
        return {};
    }
    double x[2], y[2], z[2];
    box.Get(x[0], y[0], z[0], x[1], y[1], z[1]);
    double tmin = 0.0, tmax = 0.0;
    bool first = true;
    for (double a : x) {
        for (double b : y) {
            for (double c : z) {
                double t = gp_Vec(loc, gp_Pnt(a, b, c)).Dot(gp_Vec(dir));
                tmin = first ? t : std::min(tmin, t);
                tmax = first ? t : std::max(tmax, t);
                first = false;
            }
        }
    }
    if (tmax - tmin < 1e-7) {
        return {};
    }
    return BRepBuilderAPI_MakeEdge(loc.Translated(gp_Vec(dir) * tmin),
                                   loc.Translated(gp_Vec(dir) * tmax))
        .Shape();
}

std::vector<TopoDS_Shape> Geom::faces(const TopoDS_Shape& shape)
{
    std::vector<TopoDS_Shape> res;
    if (shape.IsNull()) {
        return res;
    }
    for (TopExp_Explorer xp(shape, TopAbs_FACE); xp.More(); xp.Next()) {
        res.push_back(xp.Current());
    }
    return res;
}

TopoDS_Shape Geom::compound(const std::vector<TopoDS_Shape>& shapes)
{
    if (shapes.size() == 1) {
        return shapes.front();
    }
    TopoDS_Compound comp;
    BRep_Builder builder;
    builder.MakeCompound(comp);
    for (auto& s : shapes) {
        if (!s.IsNull()) {
            builder.Add(comp, s);
        }
    }
    return comp;
}
