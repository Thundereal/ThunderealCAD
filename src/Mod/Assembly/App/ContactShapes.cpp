// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <tuple>

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SphericalSurface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>

#include <Base/Matrix.h>

#include "ContactGeometry.h"
#include "ContactShapes.h"
#include "ElementGeometry.h"

using namespace Assembly;
using Kind = Solver::ContactShape::Kind;

namespace
{

constexpr double pi = std::numbers::pi;
// A primitive's volume against the solid's
constexpr double volumeTolerance = 1e-6;

Base::Vector3d vec(const gp_XYZ& p)
{
    return Base::Vector3d(p.X(), p.Y(), p.Z());
}

bool sameVolume(double primitive, double solid)
{
    return std::abs(primitive - solid) <= volumeTolerance * std::max(primitive, solid);
}

struct Face
{
    TopoDS_Face face;
    ElementSurface surface;
};

std::optional<SolidContactShape> asSphere(const std::vector<Face>& faces, double volume, double tol)
{
    std::optional<gp_Sphere> sphere;
    for (const auto& f : faces) {
        auto geom = Handle(Geom_SphericalSurface)::DownCast(f.surface.surface);
        if (geom.IsNull()) {
            return std::nullopt;
        }
        const gp_Sphere s = geom->Sphere();
        if (!sphere) {
            sphere = s;
        }
        else if (s.Location().Distance(sphere->Location()) > tol
                 || std::abs(s.Radius() - sphere->Radius()) > tol) {
            return std::nullopt;
        }
    }
    if (!sphere) {
        return std::nullopt;
    }
    const double r = sphere->Radius();
    if (!sameVolume(4.0 / 3.0 * pi * r * r * r, volume)) {
        return std::nullopt;
    }
    SolidContactShape out;
    out.kind = Kind::Sphere;
    out.placement = Base::Placement(vec(sphere->Location().XYZ()), Base::Rotation());
    out.size = Base::Vector3d(r, r, r);
    return out;
}

std::optional<SolidContactShape>
asCylinder(const std::vector<Face>& faces, double volume, double tol)
{
    std::optional<gp_Cylinder> cylinder;
    std::vector<gp_Pln> ends;
    for (const auto& f : faces) {
        if (auto geom = Handle(Geom_CylindricalSurface)::DownCast(f.surface.surface);
            !geom.IsNull()) {
            const gp_Cylinder c = geom->Cylinder();
            if (!cylinder) {
                cylinder = c;
            }
            else if (!c.Axis().IsCoaxial(cylinder->Axis(), 1e-9, tol)
                     || std::abs(c.Radius() - cylinder->Radius()) > tol) {
                return std::nullopt;
            }
            continue;
        }
        auto plane = Handle(Geom_Plane)::DownCast(f.surface.surface);
        if (plane.IsNull()) {
            return std::nullopt;
        }
        ends.push_back(plane->Pln());
    }
    if (!cylinder || ends.size() != 2) {
        return std::nullopt;
    }
    const gp_Ax1 axis = cylinder->Axis();
    double at[2] {};
    for (int k = 0; k < 2; ++k) {
        if (!ends[k].Axis().IsParallel(axis, 1e-9)) {
            return std::nullopt;
        }
        at[k] = gp_Vec(axis.Location(), ends[k].Location()).Dot(gp_Vec(axis.Direction()));
    }
    const double r = cylinder->Radius();
    const double h = std::abs(at[1] - at[0]);
    if (!(h > tol) || !sameVolume(pi * r * r * h, volume)) {
        return std::nullopt;
    }
    const gp_XYZ mid = axis.Location().XYZ() + axis.Direction().XYZ() * (0.5 * (at[0] + at[1]));
    SolidContactShape out;
    out.kind = Kind::Cylinder;
    out.placement =
        Base::Placement(vec(mid),
                        Base::Rotation(Base::Vector3d(0, 0, 1), vec(axis.Direction().XYZ())));
    out.size = Base::Vector3d(r, r, h);
    return out;
}

std::optional<SolidContactShape>
asBox(const TopoDS_Shape& solid, const std::vector<Face>& faces, double volume)
{
    if (faces.size() != 6) {
        return std::nullopt;
    }
    // Three directions, each the normal of two faces
    std::vector<gp_Dir> axes;
    for (const auto& f : faces) {
        auto plane = Handle(Geom_Plane)::DownCast(f.surface.surface);
        if (plane.IsNull()) {
            return std::nullopt;
        }
        const gp_Dir n = plane->Pln().Axis().Direction();
        if (std::none_of(axes.begin(), axes.end(), [&](const gp_Dir& d) {
                return d.IsParallel(n, 1e-9);
            })) {
            axes.push_back(n);
        }
    }
    if (axes.size() != 3 || !axes[0].IsNormal(axes[1], 1e-9) || !axes[0].IsNormal(axes[2], 1e-9)
        || !axes[1].IsNormal(axes[2], 1e-9)) {
        return std::nullopt;
    }
    // Right-handed
    const gp_Dir x = axes[0];
    const gp_Dir y = axes[1];
    const gp_Dir z = x.Crossed(y);
    double low[3] = {1e300, 1e300, 1e300};
    double high[3] = {-1e300, -1e300, -1e300};
    for (TopExp_Explorer it(solid, TopAbs_VERTEX); it.More(); it.Next()) {
        const gp_XYZ p = BRep_Tool::Pnt(TopoDS::Vertex(it.Current())).XYZ();
        const double c[3] = {p.Dot(x.XYZ()), p.Dot(y.XYZ()), p.Dot(z.XYZ())};
        for (int k = 0; k < 3; ++k) {
            low[k] = std::min(low[k], c[k]);
            high[k] = std::max(high[k], c[k]);
        }
    }
    const Base::Vector3d size(high[0] - low[0], high[1] - low[1], high[2] - low[2]);
    if (!sameVolume(size.x * size.y * size.z, volume)) {
        return std::nullopt;
    }
    const gp_XYZ centre = x.XYZ() * (0.5 * (low[0] + high[0]))
        + y.XYZ() * (0.5 * (low[1] + high[1])) + z.XYZ() * (0.5 * (low[2] + high[2]));
    Base::Matrix4D turn;
    const gp_Dir columns[3] = {x, y, z};
    for (int col = 0; col < 3; ++col) {
        turn[0][col] = columns[col].X();
        turn[1][col] = columns[col].Y();
        turn[2][col] = columns[col].Z();
    }
    SolidContactShape out;
    out.kind = Kind::Box;
    out.placement = Base::Placement(vec(centre), Base::Rotation(turn));
    out.size = size;
    return out;
}

// The convex hull of the contact mesh when the mesh is convex: no point of
// it more than twice the deflection on both sides of a triangle's plane. The
// work is capped: past it, every k-th triangle is tried, a concavity spanning
// many of them.
std::optional<SolidContactShape> asHull(const TopoDS_Shape& solid, double deflection)
{
    const auto mesh = ContactGeometryCache::instance().get(solid, deflection);
    BRepExtrema_TriangleSet& set = mesh->triangles();
    const int triangles = set.Size();
    if (triangles < 4) {
        return std::nullopt;
    }
    std::set<std::tuple<double, double, double>> unique;
    std::vector<std::array<BVH_Vec3d, 3>> corners(static_cast<std::size_t>(triangles));
    for (int i = 0; i < triangles; ++i) {
        auto& t = corners[static_cast<std::size_t>(i)];
        set.GetVertices(i, t[0], t[1], t[2]);
        for (const auto& v : t) {
            unique.emplace(v.x(), v.y(), v.z());
        }
    }
    std::vector<gp_XYZ> points;
    points.reserve(unique.size());
    for (const auto& [px, py, pz] : unique) {
        points.emplace_back(px, py, pz);
    }
    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    const double diagonal = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
    const double tol = 2.0 * mesh->deflection() + 1e-9 * diagonal;
    const double work = static_cast<double>(points.size()) * triangles;
    const int stride = std::max(1, static_cast<int>(work / 5e7));
    for (int i = 0; i < triangles; i += stride) {
        const auto& t = corners[static_cast<std::size_t>(i)];
        const gp_XYZ a(t[0].x(), t[0].y(), t[0].z());
        const gp_XYZ b(t[1].x(), t[1].y(), t[1].z());
        const gp_XYZ c(t[2].x(), t[2].y(), t[2].z());
        gp_XYZ n = (b - a).Crossed(c - a);
        const double area = n.Modulus();
        if (!(area > 1e-24)) {
            continue;
        }
        n /= area;
        bool front = false;
        bool back = false;
        for (const auto& p : points) {
            const double d = n.Dot(p - a);
            front = front || d > tol;
            back = back || d < -tol;
            if (front && back) {
                return std::nullopt;
            }
        }
    }
    // The mesh is in the solid's own frame: into the coordinates given
    const gp_Trsf where = solid.Location().Transformation();
    SolidContactShape out;
    out.kind = Kind::ConvexHull;
    out.deflection = mesh->deflection();
    out.points.reserve(points.size());
    for (gp_XYZ p : points) {
        where.Transforms(p);
        out.points.push_back(vec(p));
    }
    return out;
}

}  // namespace

std::optional<SolidContactShape> Assembly::contactShapeOf(const TopoDS_Shape& solid,
                                                          double deflection)
{
    if (solid.IsNull()) {
        return std::nullopt;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    const double volume = std::abs(props.Mass());
    if (!(volume > 0.0)) {
        return std::nullopt;
    }
    std::vector<Face> faces;
    bool analytic = true;
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        Face f {TopoDS::Face(it.Current()), analyzeFace(TopoDS::Face(it.Current()))};
        analytic = analytic && !f.surface.surface.IsNull();
        faces.push_back(std::move(f));
    }
    if (analytic && !faces.empty()) {
        const double tol = recognitionTolerance(solid);
        if (auto sphere = asSphere(faces, volume, tol)) {
            return sphere;
        }
        if (auto cylinder = asCylinder(faces, volume, tol)) {
            return cylinder;
        }
        if (auto box = asBox(solid, faces, volume)) {
            return box;
        }
    }
    return asHull(solid, deflection);
}
