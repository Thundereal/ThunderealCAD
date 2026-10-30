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
#include <map>
#include <optional>
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

// Each edge of a triangle mesh, by its ends' indices, and the triangles on it
// with the corner the edge starts from
using MeshEdges = std::map<std::pair<int, int>, std::vector<std::pair<std::size_t, int>>>;

MeshEdges edgesOf(const std::vector<std::array<int, 3>>& triangles)
{
    MeshEdges edges;
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        for (int k = 0; k < 3; ++k) {
            const int u = triangles[t][k];
            const int v = triangles[t][(k + 1) % 3];
            edges[{std::min(u, v), std::max(u, v)}].emplace_back(t, k);
        }
    }
    return edges;
}

// Winds a closed triangle mesh's triangles to agree across their shared
// edges -- an edge runs one way in one triangle and the other way in the
// other -- and outward by the signed volume. False, the triangles as they
// were, for a mesh not closed, not manifold, not orientable or in more than
// one piece.
bool orientOutward(const std::vector<gp_XYZ>& points, std::vector<std::array<int, 3>>& triangles)
{
    const std::size_t count = triangles.size();
    if (count < 4) {
        return false;
    }
    MeshEdges edges = edgesOf(triangles);
    for (const auto& [key, on] : edges) {
        if (on.size() != 2) {
            return false;
        }
    }
    std::vector<int> flip(count, 0);  // 0 unvisited, +1 kept, -1 reversed
    std::vector<std::size_t> stack {0};
    flip[0] = 1;
    std::size_t reached = 1;
    while (!stack.empty()) {
        const std::size_t t = stack.back();
        stack.pop_back();
        for (int k = 0; k < 3; ++k) {
            const int u = triangles[t][k];
            const int v = triangles[t][(k + 1) % 3];
            for (const auto& [other, corner] : edges[{std::min(u, v), std::max(u, v)}]) {
                if (other == t) {
                    continue;
                }
                // The same direction in both as stored: one of them turns
                const bool sameStored = triangles[other][corner] == u;
                const int wanted = (sameStored == (flip[t] > 0)) ? -1 : 1;
                if (flip[other] == 0) {
                    flip[other] = wanted;
                    ++reached;
                    stack.push_back(other);
                }
                else if (flip[other] != wanted) {
                    return false;  // not orientable
                }
            }
        }
    }
    if (reached != count) {
        return false;
    }
    double volume = 0.0;
    for (std::size_t t = 0; t < count; ++t) {
        const gp_XYZ& a = points[triangles[t][0]];
        const gp_XYZ n = (points[triangles[t][1]] - a).Crossed(points[triangles[t][2]] - a);
        volume += flip[t] * a.Dot(n);
    }
    const int outward = volume > 0.0 ? 1 : -1;
    for (std::size_t t = 0; t < count; ++t) {
        if (flip[t] * outward < 0) {
            std::swap(triangles[t][1], triangles[t][2]);
        }
    }
    return true;
}

// Whether a closed triangle mesh wound outward (orientOutward()) is convex:
// convex at every edge, which for a closed connected surface is convex
// everywhere (Tietze). No triangle's neighbour across an edge may stand in
// front of its plane by more than tol. Local, so a long thin triangle -- an
// ellipsoid's at its poles -- is not held against points beyond its
// neighbours, which on a curved surface stand in front of its plane by more
// than the deflection.
bool convexMesh(const std::vector<gp_XYZ>& points,
                const std::vector<std::array<int, 3>>& triangles,
                double tol)
{
    for (const auto& [key, on] : edgesOf(triangles)) {
        for (int side = 0; side < 2; ++side) {
            const std::size_t t = on[side].first;
            const std::size_t other = on[1 - side].first;
            const gp_XYZ& a = points[triangles[t][0]];
            gp_XYZ n = (points[triangles[t][1]] - a).Crossed(points[triangles[t][2]] - a);
            const double length = n.Modulus();
            if (!(length > 1e-24)) {
                continue;
            }
            n /= length;
            const int corner = (on[1 - side].second + 2) % 3;  // opposite the edge
            if (n.Dot(points[triangles[other][corner]] - a) > tol) {
                return false;
            }
        }
    }
    return true;
}

// Whether no point of the mesh stands more than tol on both sides of a
// triangle's plane: what convexMesh() falls back to. Too strict on a curved
// surface's long thin triangles. The work is capped: past it, every k-th
// triangle is tried, a concavity spanning many of them.
bool convexByPlanes(const std::vector<gp_XYZ>& points,
                    const std::vector<std::array<int, 3>>& triangles,
                    double tol)
{
    const double work = static_cast<double>(points.size()) * triangles.size();
    const std::size_t stride = std::max<std::size_t>(1, static_cast<std::size_t>(work / 5e7));
    for (std::size_t i = 0; i < triangles.size(); i += stride) {
        const gp_XYZ& a = points[triangles[i][0]];
        gp_XYZ n = (points[triangles[i][1]] - a).Crossed(points[triangles[i][2]] - a);
        n /= n.Modulus();
        bool front = false;
        bool back = false;
        for (const auto& p : points) {
            const double d = n.Dot(p - a);
            front = front || d > tol;
            back = back || d < -tol;
            if (front && back) {
                return false;
            }
        }
    }
    return true;
}

// The convex hull of the contact mesh when the mesh is convex (convexMesh(),
// or convexByPlanes() for a mesh orientOutward() cannot wind, within twice the
// deflection); else the mesh itself when it is closed
std::optional<SolidContactShape> asHullOrMesh(const TopoDS_Shape& solid, double deflection)
{
    const auto mesh = ContactGeometryCache::instance().get(solid, deflection);
    BRepExtrema_TriangleSet& set = mesh->triangles();
    const int count = set.Size();
    if (count < 4) {
        return std::nullopt;
    }
    // The vertices by position: a face's triangulation shares its edges'
    // nodes with its neighbours'
    std::map<std::tuple<double, double, double>, int> index;
    std::vector<gp_XYZ> points;
    std::vector<std::array<int, 3>> triangles;
    triangles.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        BVH_Vec3d corners[3];
        set.GetVertices(i, corners[0], corners[1], corners[2]);
        std::array<int, 3> t {};
        for (int k = 0; k < 3; ++k) {
            const auto key = std::make_tuple(corners[k].x(), corners[k].y(), corners[k].z());
            auto [it, added] = index.emplace(key, static_cast<int>(points.size()));
            if (added) {
                points.emplace_back(corners[k].x(), corners[k].y(), corners[k].z());
            }
            t[k] = it->second;
        }
        // A pole's triangles with two corners at the pole have no area and
        // no plane
        const gp_XYZ n = (points[t[1]] - points[t[0]]).Crossed(points[t[2]] - points[t[0]]);
        if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2] && n.Modulus() > 1e-24) {
            triangles.push_back(t);
        }
    }
    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    const double diagonal = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
    const double tol = 2.0 * mesh->deflection() + 1e-9 * diagonal;
    const bool closed = orientOutward(points, triangles);
    const bool convex =
        closed ? convexMesh(points, triangles, tol) : convexByPlanes(points, triangles, tol);
    if (!convex && !closed) {
        return std::nullopt;
    }
    // The mesh is in the solid's own frame: into the coordinates given
    const gp_Trsf where = solid.Location().Transformation();
    SolidContactShape out;
    out.kind = convex ? Kind::ConvexHull : Kind::Mesh;
    // A hull's too: Hertz's law reads its curvature off them
    out.triangles = std::move(triangles);
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
    return asHullOrMesh(solid, deflection);
}
