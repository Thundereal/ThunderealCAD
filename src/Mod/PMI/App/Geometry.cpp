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
#include <cstring>
#include <limits>

#include <Bnd_Box.hxx>
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <GeomConvert_CurveToAnaCurve.hxx>
#include <Geom_Curve.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <gp_Ax1.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <Precision.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopLoc_Location.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>

#include "Geometry.h"

using namespace PMI;

Primitive Primitive::reference() const
{
    Primitive res = *this;
    switch (type) {
        case Cylinder:
        case Cone:
        case Circle:
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
        case Circle:
            return "circle";
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
            // v runs along the axis from its location
            res.from = surf.FirstVParameter();
            res.to = surf.LastVParameter();
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

Primitive Geom::ofEdge(const TopoDS_Shape& shape)
{
    Primitive res;
    if (shape.IsNull() || shape.ShapeType() != TopAbs_EDGE
        || BRep_Tool::Degenerated(TopoDS::Edge(shape))) {
        return res;
    }
    const TopoDS_Edge& edge = TopoDS::Edge(shape);
    BRepAdaptor_Curve curve(edge);
    GeomAdaptor_Curve analytic;
    const Adaptor3d_Curve* use = &curve;
    GeomAbs_CurveType type = curve.GetType();
    if (type == GeomAbs_BSplineCurve || type == GeomAbs_BezierCurve) {
        // A line or a circle written as a spline (FTC-10's hole rims): what
        // it is, within the edge's own tolerance
        TopLoc_Location loc;
        double first = 0, last = 0;
        Handle(Geom_Curve) spline = BRep_Tool::Curve(edge, loc, first, last);
        Handle(Geom_Curve) conv;
        double from = 0, to = 0;
        double tol = std::max(BRep_Tool::Tolerance(edge), Precision::Confusion());
        if (!spline.IsNull()
            && GeomConvert_CurveToAnaCurve(spline).ConvertToAnalytical(tol, conv, first, last,
                                                                       from, to)
            && !conv.IsNull()) {
            if (!loc.IsIdentity()) {
                conv = Handle(Geom_Curve)::DownCast(conv->Transformed(loc.Transformation()));
            }
            analytic.Load(conv, from, to);
            use = &analytic;
            type = analytic.GetType();
        }
    }
    switch (type) {
        case GeomAbs_Line: {
            gp_Lin line = use->Line();
            res.type = Primitive::Line;
            res.loc = use->Value(use->FirstParameter());
            res.dir = line.Direction();
            break;
        }
        case GeomAbs_Circle: {
            gp_Circ circ = use->Circle();
            res.type = Primitive::Circle;
            res.loc = circ.Location();
            res.dir = circ.Axis().Direction();
            res.radius = circ.Radius();
            break;
        }
        default:
            break;
    }
    return res;
}

Primitive Geom::ofElement(const TopoDS_Shape& shape)
{
    Primitive res;
    if (shape.IsNull()) {
        return res;
    }
    switch (shape.ShapeType()) {
        case TopAbs_FACE:
            return ofFace(shape);
        case TopAbs_EDGE:
            return ofEdge(shape);
        case TopAbs_VERTEX:
            res.type = Primitive::Point;
            res.loc = BRep_Tool::Pnt(TopoDS::Vertex(shape));
            return res;
        default:
            return res;
    }
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
        case Primitive::Line:
            return parallel(a.dir, b.dir, atol) && lineDistance(b.loc, a.loc, a.dir) < tol;
        case Primitive::Circle:
            return std::fabs(a.radius - b.radius) < tol && parallel(a.dir, b.dir, atol)
                && a.loc.Distance(b.loc) < tol;
        case Primitive::Point:
            return a.loc.Distance(b.loc) < tol;
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

bool Geom::depth(const std::vector<Primitive>& faces, double& depth, std::string& why)
{
    // The holes: cylinders by axis, all parallel (one hole, or a pattern "4X")
    struct Hole
    {
        gp_Ax1 axis;
        double radius = 0;
        double lo = 0, hi = 0;
    };
    std::vector<Hole> holes;
    const Primitive* first = nullptr;
    for (auto& p : faces) {
        if (p.type != Primitive::Cylinder) {
            continue;
        }
        if (!first) {
            first = &p;
        }
        else if (!p.dir.IsParallel(first->dir, 1e-9)) {
            why = "a depth needs holes whose axes are parallel";
            return false;
        }
        auto it = std::find_if(holes.begin(), holes.end(), [&](const Hole& h) {
            return gp_Lin(h.axis).Distance(p.loc) < SizeTol;
        });
        if (it == holes.end()) {
            holes.push_back({gp_Ax1(p.loc, first->dir), p.radius});
        }
        else {
            it->radius = std::max(it->radius, p.radius);
        }
    }
    if (holes.empty()) {
        why = "a depth needs a hole: cylindrical faces";
        return false;
    }
    // Along the axis, from a point all the holes share the measure of
    const gp_Vec dir(first->dir);
    auto at = [&](const gp_Pnt& p) {
        return gp_Vec(first->loc, p).Dot(dir);
    };
    for (auto& h : holes) {
        h.lo = std::numeric_limits<double>::max();
        h.hi = -h.lo;
        for (auto& p : faces) {
            if (p.type != Primitive::Cylinder || std::fabs(p.radius - h.radius) > SizeTol
                || gp_Lin(h.axis).Distance(p.loc) > SizeTol) {
                continue;
            }
            double s = at(p.loc);
            double sign = p.dir.Dot(first->dir) > 0 ? 1 : -1;
            for (double v : {p.from, p.to}) {
                h.lo = std::min(h.lo, s + sign * v);
                h.hi = std::max(h.hi, s + sign * v);
            }
        }
    }
    bool found = false;
    for (auto& h : holes) {
        // Its floor: the plane square to the axis nearest the bore's ends,
        // no further from them than the bore is wide (a counterbore's walls
        // end 0.2 short of its floor, STC-10)
        bool floor = false;
        double at_floor = 0, gap = h.radius;
        for (auto& p : faces) {
            if (p.type != Primitive::Plane || !p.dir.IsParallel(first->dir, 1e-9)) {
                continue;
            }
            double s = at(p.loc);
            double g = s < h.lo ? h.lo - s : s > h.hi ? s - h.hi : 0.0;
            if (g <= gap) {
                floor = true;
                at_floor = s;
                gap = g;
            }
        }
        double d = floor ? std::max(std::fabs(at_floor - h.lo), std::fabs(at_floor - h.hi))
                         : h.hi - h.lo;
        if (found && std::fabs(d - depth) > SizeTol * std::max(1.0, d)) {
            why = "a depth needs holes of one depth";
            return false;
        }
        depth = d;
        found = true;
    }
    if (depth <= SizeTol) {
        why = "a depth needs a hole of some length";
        return false;
    }
    return true;
}

bool Geom::distance(const Primitive& pa, const Primitive& pb, double& result, std::string& why)
{
    // A circle a plane is square to: the plane it lies in, a hole's rim
    auto lies = [](const Primitive& c, const Primitive& p) {
        Primitive res = c.reference();
        if (c.type == Primitive::Circle && p.type == Primitive::Plane
            && parallel(c.dir, p.dir, 1e-9)) {
            res.type = Primitive::Plane;
        }
        return res;
    };
    Primitive a = lies(pa, pb);
    Primitive b = lies(pb, pa);
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

std::vector<TopoDS_Shape> Geom::elements(const TopoDS_Shape& shape, const char* type)
{
    std::vector<TopoDS_Shape> res;
    if (shape.IsNull() || !type) {
        return res;
    }
    TopAbs_ShapeEnum t = TopAbs_SHAPE;
    if (std::strcmp(type, "Face") == 0) {
        t = TopAbs_FACE;
    }
    else if (std::strcmp(type, "Edge") == 0) {
        t = TopAbs_EDGE;
    }
    else if (std::strcmp(type, "Vertex") == 0) {
        t = TopAbs_VERTEX;
    }
    else {
        return res;
    }
    // The order TopoShape numbers them in: each once
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(shape, t, map);
    res.reserve(map.Extent());
    for (int i = 1; i <= map.Extent(); ++i) {
        res.push_back(map(i));
    }
    return res;
}

std::vector<TopoDS_Shape> Geom::members(const TopoDS_Shape& shape)
{
    std::vector<TopoDS_Shape> res;
    if (shape.IsNull()) {
        return res;
    }
    if (shape.ShapeType() != TopAbs_COMPOUND) {
        res.push_back(shape);
        return res;
    }
    for (TopoDS_Iterator it(shape); it.More(); it.Next()) {
        res.push_back(it.Value());
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
