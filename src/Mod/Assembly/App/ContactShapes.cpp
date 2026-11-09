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

#include <BRepAdaptor_Curve2d.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2dConvert_ApproxArcsSegments.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ2d.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Trsf.hxx>
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
// than the deflection. An edge whose two triangles' corners are all on one
// face of the solid that is convex at each ('convexOn', each point's faces
// convex there) is the mesher's: a ball flatted across its parallels came
// meshed with diagonals the wrong way by its rim, folds 0.11 mm deep at a
// deflection of 0.01 -- any four points of a sphere make a fold one way.
bool convexMesh(const std::vector<gp_XYZ>& points,
                const std::vector<std::array<int, 3>>& triangles,
                double tol,
                const std::vector<std::vector<int>>& convexOn)
{
    auto oneConvexFace = [&convexOn](const std::array<int, 4>& corners) {
        for (const int face : convexOn[static_cast<std::size_t>(corners[0])]) {
            bool all = true;
            for (int k = 1; k < 4 && all; ++k) {
                const auto& on = convexOn[static_cast<std::size_t>(corners[k])];
                all = std::find(on.begin(), on.end(), face) != on.end();
            }
            if (all) {
                return true;
            }
        }
        return false;
    };
    for (const auto& [key, on] : edgesOf(triangles)) {
        if (oneConvexFace({triangles[on[0].first][0],
                           triangles[on[0].first][1],
                           triangles[on[0].first][2],
                           triangles[on[1].first][(on[1].second + 2) % 3]})) {
            continue;
        }
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
    // The solid at each point: the faces of the mesh's copy it is a node
    // of, each one's normal and curvature there by its node's parameters --
    // a seam's node twice, one face's both times. OCCT's triangle set places
    // the nodes as here, so they meet the points exactly.
    std::vector<std::vector<Solver::ContactShape::Surface>> surfaces(points.size());
    // And the faces convex at each, by their number (convexMesh())
    std::vector<std::vector<int>> convexOn(points.size());
    std::vector<Solver::ContactShape::Round> rounds;
    int faceNumber = -1;
    for (TopExp_Explorer it(mesh->mesh(), TopAbs_FACE); it.More(); it.Next()) {
        ++faceNumber;
        const TopoDS_Face& face = TopoDS::Face(it.Current());
        TopLoc_Location location;
        const auto& triangulation = BRep_Tool::Triangulation(face, location);
        if (triangulation.IsNull() || !triangulation->HasUVNodes()) {
            continue;
        }
        const gp_Trsf trsf = location.Transformation();
        const BRepAdaptor_Surface surface(face);
        const double outward = face.Orientation() == TopAbs_REVERSED ? -1.0 : 1.0;
        // A cylinder's or a sphere's face: its own, to roll on; concave
        // where its normal at its middle points to the axis or the centre
        // (a bore, a socket)
        int round = -1;
        const bool cylinder = surface.GetType() == GeomAbs_Cylinder;
        if (cylinder || surface.GetType() == GeomAbs_Sphere) {
            const gp_Ax3 position =
                cylinder ? surface.Cylinder().Position() : surface.Sphere().Position();
            double u0 = 0.0;
            double u1 = 0.0;
            double v0 = 0.0;
            double v1 = 0.0;
            BRepTools::UVBounds(face, u0, u1, v0, v1);
            gp_Pnt middle;
            gp_Vec du;
            gp_Vec dv;
            surface.D1(0.5 * (u0 + u1), 0.5 * (v0 + v1), middle, du, dv);
            const gp_Vec axis(position.Direction());
            gp_Vec away(position.Location(), middle);
            if (cylinder) {
                away -= axis * away.Dot(axis);
            }
            {
                Solver::ContactShape::Round made;
                made.sphere = !cylinder;
                made.concave = du.Crossed(dv).Dot(away) * outward < 0.0;
                made.centre = vec(position.Location().XYZ());
                made.axis = vec(position.Direction().XYZ());
                made.way = vec(position.XDirection().XYZ());
                made.radius = cylinder ? surface.Cylinder().Radius() : surface.Sphere().Radius();
                made.from = v0;
                made.to = v1;
                // Its angle u about the axis, right handed or not
                made.angleFrom = position.Direct() ? u0 : -u1;
                made.angleTo = position.Direct() ? u1 : -u0;
                round = static_cast<int>(rounds.size());
                rounds.push_back(made);
            }
        }
        for (int i = 1; i <= triangulation->NbNodes(); ++i) {
            const gp_Pnt p = triangulation->Node(i).Transformed(trsf);
            const auto found = index.find(std::make_tuple(p.X(), p.Y(), p.Z()));
            if (found == index.end()) {
                continue;
            }
            const gp_Pnt2d uv = triangulation->UVNode(i);
            BRepLProp_SLProps props(surface, uv.X(), uv.Y(), 2, Precision::Confusion());
            // A pole has no normal: a hair off it, inside the parameters
            const double du = 1e-7 * (surface.LastUParameter() - surface.FirstUParameter());
            const double dv = 1e-7 * (surface.LastVParameter() - surface.FirstVParameter());
            for (const auto& [su, sv] : {std::pair {0, 1}, {0, -1}, {1, 0}, {-1, 0}}) {
                if (props.IsNormalDefined() && props.IsCurvatureDefined()) {
                    break;
                }
                props.SetParameters(std::clamp(uv.X() + su * du,
                                               surface.FirstUParameter(),
                                               surface.LastUParameter()),
                                    std::clamp(uv.Y() + sv * dv,
                                               surface.FirstVParameter(),
                                               surface.LastVParameter()));
            }
            if (!props.IsNormalDefined() || !props.IsCurvatureDefined()) {
                continue;
            }
            // OCCT's curvature is along the surface's normal, a convex
            // face's negative where that normal is outward
            const gp_Dir normal = props.Normal();
            gp_Dir way1;
            gp_Dir way2;
            if (props.IsUmbilic()) {
                const gp_Ax3 frame(props.Value(), normal);
                way1 = frame.XDirection();
                way2 = frame.YDirection();
            }
            else {
                props.CurvatureDirections(way1, way2);
            }
            Solver::ContactShape::Surface at;
            at.round = round;
            at.normal = vec(normal.XYZ()) * outward;
            at.way1 = vec(way1.XYZ());
            at.way2 = vec(way2.XYZ());
            at.k1 = -outward * props.MaxCurvature();
            at.k2 = -outward * props.MinCurvature();
            if (at.k2 > at.k1) {
                std::swap(at.k1, at.k2);
                std::swap(at.way1, at.way2);
            }
            surfaces[static_cast<std::size_t>(found->second)].push_back(at);
            if (at.k2 >= -1e-9) {
                convexOn[static_cast<std::size_t>(found->second)].push_back(faceNumber);
            }
        }
    }
    const bool closed = orientOutward(points, triangles);
    const bool convex = closed ? convexMesh(points, triangles, tol, convexOn)
                               : convexByPlanes(points, triangles, tol);
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
    auto turned = [&where](const Base::Vector3d& v) {
        return vec(gp_Vec(v.x, v.y, v.z).Transformed(where).XYZ());
    };
    for (auto& at : surfaces) {
        for (auto& face : at) {
            face.normal = turned(face.normal);
            face.way1 = turned(face.way1);
            face.way2 = turned(face.way2);
        }
    }
    out.surfaces = std::move(surfaces);
    for (auto& made : rounds) {
        gp_XYZ centre(made.centre.x, made.centre.y, made.centre.z);
        where.Transforms(centre);
        made.centre = vec(centre);
        made.axis = turned(made.axis);
        made.way = turned(made.way);
    }
    out.rounds = std::move(rounds);
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

std::optional<ProfileContactShape>
Assembly::profileOf(const TopoDS_Shape& shape, const TopoDS_Shape& solid, double tolerance)
{
    if (shape.IsNull() || shape.ShapeType() != TopAbs_FACE) {
        return std::nullopt;
    }
    const TopoDS_Face& face = TopoDS::Face(shape);
    BRepAdaptor_Surface surface(face, false);
    if (surface.GetType() != GeomAbs_Plane) {
        return std::nullopt;
    }
    // The plane's (u, v) are its frame's x and y
    const gp_Pln plane = surface.Plane();
    const gp_Ax3& axes = plane.Position();
    const gp_Dir x = axes.XDirection();
    const gp_Dir y = axes.YDirection();
    const gp_Dir z = x.Crossed(y);
    ProfileContactShape out;
    {
        Base::Matrix4D m;
        m.setCol(0, Base::Vector3d(x.X(), x.Y(), x.Z()));
        m.setCol(1, Base::Vector3d(y.X(), y.Y(), y.Z()));
        m.setCol(2, Base::Vector3d(z.X(), z.Y(), z.Z()));
        m.setCol(3, vec(axes.Location().XYZ()));
        out.placement = Base::Placement(m);
    }
    auto point = [](const gp_Pnt2d& p) {
        return Base::Vector3d(p.X(), p.Y(), 0.0);
    };
    // A line's or a circle's stretch from a to b, as a piece
    auto pieceOf = [&](const Adaptor2d_Curve2d& curve, double a, double b) {
        Solver::ProfilePiece piece;
        piece.from = point(curve.Value(a));
        piece.to = point(curve.Value(b));
        if (curve.GetType() == GeomAbs_Circle) {
            const gp_Circ2d circle = curve.Circle();
            piece.centre = point(circle.Location());
            gp_Pnt2d p;
            gp_Vec2d d1;
            curve.D1(a, p, d1);
            const gp_Vec2d r(circle.Location(), p);
            // A circle's parameter is its angle
            piece.sweep = (r.Crossed(d1) >= 0.0 ? 1.0 : -1.0) * (b - a);
        }
        return piece;
    };
    // The bounding box's size, for the side test's step
    double size = 0.0;
    {
        Bnd_Box box;
        BRepBndLib::Add(face, box);
        if (!box.IsVoid()) {
            size = std::sqrt(box.SquareExtent());
        }
    }
    for (TopExp_Explorer wires(face, TopAbs_WIRE); wires.More(); wires.Next()) {
        std::vector<Solver::ProfilePiece> loop;
        for (BRepTools_WireExplorer edges(TopoDS::Wire(wires.Current()), face); edges.More();
             edges.Next()) {
            const TopoDS_Edge& edge = edges.Current();
            if (BRep_Tool::Degenerated(edge)) {
                continue;
            }
            BRepAdaptor_Curve2d curve(edge, face);
            std::vector<Solver::ProfilePiece> pieces;
            const GeomAbs_CurveType type = curve.GetType();
            if (type == GeomAbs_Line || type == GeomAbs_Circle) {
                pieces.push_back(pieceOf(curve, curve.FirstParameter(), curve.LastParameter()));
            }
            else {
                Geom2dConvert_ApproxArcsSegments approx(curve, tolerance, 0.05);
                const auto& result = approx.GetResult();
                for (int i = 1; i <= result.Length(); ++i) {
                    if (result(i).IsNull()) {
                        continue;
                    }
                    Geom2dAdaptor_Curve part(result(i));
                    const GeomAbs_CurveType partType = part.GetType();
                    if (partType != GeomAbs_Line && partType != GeomAbs_Circle) {
                        return std::nullopt;
                    }
                    pieces.push_back(pieceOf(part, part.FirstParameter(), part.LastParameter()));
                }
                if (pieces.empty()) {
                    return std::nullopt;
                }
            }
            if (edge.Orientation() == TopAbs_REVERSED) {
                std::reverse(pieces.begin(), pieces.end());
                for (auto& piece : pieces) {
                    std::swap(piece.from, piece.to);
                    piece.sweep = -piece.sweep;
                }
            }
            loop.insert(loop.end(), pieces.begin(), pieces.end());
        }
        if (loop.empty()) {
            continue;
        }
        // The face to the left: tried a little to the left of the longest
        // piece's middle, the loop turned round when it is not there
        std::size_t longest = 0;
        double length = -1.0;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const auto& piece = loop[i];
            const double l = piece.sweep == 0.0
                ? (piece.to - piece.from).Length()
                : std::abs(piece.sweep) * (piece.from - piece.centre).Length();
            if (l > length) {
                length = l;
                longest = i;
            }
        }
        const auto& piece = loop[longest];
        Base::Vector3d middle;
        Base::Vector3d tangent;
        if (piece.sweep == 0.0) {
            middle = (piece.from + piece.to) / 2.0;
            tangent = piece.to - piece.from;
        }
        else {
            const Base::Vector3d r = piece.from - piece.centre;
            const double start = std::atan2(r.y, r.x);
            const double angle = start + piece.sweep / 2.0;
            const Base::Vector3d radial(std::cos(angle), std::sin(angle), 0.0);
            middle = piece.centre + radial * r.Length();
            tangent = Base::Vector3d(-radial.y, radial.x, 0.0) * piece.sweep;
        }
        tangent.Normalize();
        const double step = std::min(1e-4 * size, 0.01 * length);
        const Base::Vector3d left = middle + Base::Vector3d(-tangent.y, tangent.x, 0.0) * step;
        BRepClass_FaceClassifier classifier(face, gp_Pnt2d(left.x, left.y), Precision::Confusion());
        if (classifier.State() != TopAbs_IN) {
            std::reverse(loop.begin(), loop.end());
            for (auto& p : loop) {
                std::swap(p.from, p.to);
                p.sweep = -p.sweep;
            }
        }
        out.loops.push_back(static_cast<int>(out.pieces.size()));
        out.pieces.insert(out.pieces.end(), loop.begin(), loop.end());
    }
    if (out.pieces.empty()) {
        return std::nullopt;
    }
    // The solid's reach along z, in the plane's frame
    const TopoDS_Shape& reaching = solid.IsNull() ? shape : solid;
    gp_Trsf toPlane;
    toPlane.SetTransformation(gp_Ax3(axes.Location(), z, x));
    Bnd_Box box;
    BRepBndLib::AddOptimal(reaching.Moved(TopLoc_Location(toPlane)), box, false, false);
    if (!box.IsVoid()) {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        out.low = zmin;
        out.high = zmax;
    }
    return out;
}
