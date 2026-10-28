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
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepExtrema_ShapeProximity.hxx>
#include <BRepExtrema_TriangleSet.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Mat.hxx>
#include <gp_Trsf.hxx>

#include <App/DocumentObject.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Base/Console.h>
#include <Base/Sequencer.h>
#include <Mod/Part/App/PartFeature.h>

#include "AssemblyObject.h"
#include "AssemblyUtils.h"
#include "ContactGeometry.h"
#include "Interference.h"

FC_LOG_LEVEL_INIT("Assembly", true, true)

using namespace Assembly;

namespace
{

using Vec = BVH_Vec3d;

Vec cross(const Vec& a, const Vec& b)
{
    return Vec(a.y() * b.z() - a.z() * b.y(),
               a.z() * b.x() - a.x() * b.z(),
               a.x() * b.y() - a.y() * b.x());
}

double dot(const Vec& a, const Vec& b)
{
    return a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
}

Vec apply(const gp_Trsf& trsf, const Vec& v)
{
    gp_XYZ p(v.x(), v.y(), v.z());
    trsf.Transforms(p);
    return Vec(p.X(), p.Y(), p.Z());
}

gp_Pnt pointOf(const Vec& v)
{
    return gp_Pnt(v.x(), v.y(), v.z());
}

// A solid of a component, where it is now and where it is checked (a frame,
// or a drag, puts it elsewhere)
struct Instance
{
    TopoDS_Shape base;   // located, where it is now
    TopoDS_Shape shape;  // Exact: where it is checked
    // Fast: its contact geometry, in the solid's own frame, shared by every
    // instance of the solid; and the solid's frame in the assembly's, now and
    // where it is checked
    std::shared_ptr<const ContactMesh> geometry;
    gp_Trsf now;
    gp_Trsf trsf;
    gp_Trsf inverse;
    Bnd_Box box;    // Fast: the geometry's box moved, so a little larger
    Bnd_Box exact;  // Fast: the box of its triangles where checked, made when needed
};

struct Component
{
    App::DocumentObject* obj = nullptr;
    std::vector<Instance> instances;
    TopoDS_Compound compound;  // Exact: the solids together, where checked
    Bnd_Box baseBox;           // now
    Bnd_Box box;               // where checked: baseBox moved, so a little larger
    Bnd_Box exact;             // Fast: its instances' exact boxes, made when needed
    // The component's placement in the assembly now, the move last given to
    // it, and whether the last posing moved it
    Base::Placement now;
    Base::Placement move;
    bool moved = true;
    double volume = -1.0;     // computed when needed
    double deflection = 0.0;  // Fast: the largest of its solids'
    TopoDS_Compound meshes;   // Fast, contacts: the meshes where checked, made when needed

    // Fast: whether a point (the assembly's coordinates) is inside one of the
    // solids by more than tol
    bool contains(const gp_Pnt& point, double tol) const
    {
        for (const auto& instance : instances) {
            if (instance.geometry
                && instance.geometry->contains(point.Transformed(instance.inverse), tol)) {
                return true;
            }
        }
        return false;
    }
};

// The path of an object from the assembly, "A.B.Obj.", through the
// sub-assemblies it is in; empty when it is not in the assembly
std::string pathFrom(const AssemblyObject* assembly, App::DocumentObject* obj)
{
    std::vector<App::DocumentObject*> chain {obj};
    auto parent = App::GeoFeatureGroupExtension::getGroupOfObject(obj);
    while (parent && parent != assembly) {
        chain.push_back(parent);
        parent = App::GeoFeatureGroupExtension::getGroupOfObject(parent);
    }
    if (parent != assembly) {
        return {};
    }
    std::string path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        path += (*it)->getNameInDocument();
        path += '.';
    }
    return path;
}

double volumeOf(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return std::abs(props.Mass());
}

bool boxesNear(const Bnd_Box& a, const Bnd_Box& b, double gap)
{
    if (a.IsVoid() || b.IsVoid()) {
        return false;
    }
    Bnd_Box enlarged = a;
    enlarged.Enlarge(gap);
    return !enlarged.IsOut(b);
}

// Fast: the box of the instance's triangles where it is checked, exact for the
// mesh (the geometry's box moved is larger: the containment tests need it
// exact)
const Bnd_Box& exactBox(Instance& instance)
{
    if (instance.exact.IsVoid() && instance.geometry) {
        BRepExtrema_TriangleSet& triangles = instance.geometry->triangles();
        for (int i = 0; i < triangles.Size(); ++i) {
            Vec p[3];
            triangles.GetVertices(i, p[0], p[1], p[2]);
            for (const Vec& v : p) {
                const Vec w = apply(instance.trsf, v);
                instance.exact.Update(w.x(), w.y(), w.z());
            }
        }
    }
    return instance.exact;
}

const Bnd_Box& exactBox(Component& component)
{
    if (component.exact.IsVoid()) {
        for (auto& instance : component.instances) {
            component.exact.Add(exactBox(instance));
        }
    }
    return component.exact;
}

// Fast, contacts: the meshes where checked
const TopoDS_Compound& meshesOf(Component& component)
{
    if (component.meshes.IsNull()) {
        BRep_Builder builder;
        builder.MakeCompound(component.meshes);
        for (const auto& instance : component.instances) {
            if (instance.geometry) {
                builder.Add(component.meshes,
                            instance.geometry->mesh().Located(TopLoc_Location(instance.trsf)));
            }
        }
    }
    return component.meshes;
}

// An edge of triangle p crossing triangle t, from one side of its plane to the
// other, one end more than tol from it, the other more than 1e-3 tol, handed
// to `confirm` with the point where it crosses, the edge's two ends, p's
// normal and t's; true when one is confirmed. Faces that only touch cross so
// where an edge of one meets the other's boundary, and meshes that touch (a
// pin in its hole) within their error: a crossing is only a candidate. The
// ends of edges on a face in contact are on its plane, to rounding: the
// 1e-3 tol leaves them out. Both ends beyond tol is not asked: a sphere's
// mesh sunk 0.08 mm into a face had its pole's rings 0.06 deep and within
// 0.001 of the face, and no edge across the band of 0.01 either side.
using Confirm = std::function<
    bool(const Vec& at, const Vec& from, const Vec& to, const Vec& normal, const Vec& crossed)>;

bool edgeCrosses(const Vec (&p)[3], const Vec (&t)[3], double tol, const Confirm& confirm)
{
    const Vec np = cross(p[1] - p[0], p[2] - p[0]);
    const Vec n = cross(t[1] - t[0], t[2] - t[0]);
    const double length = std::sqrt(dot(n, n));
    if (length <= 0.0) {
        return false;
    }
    const Vec unit = n * (1.0 / length);
    const double d[3] = {dot(unit, p[0] - t[0]), dot(unit, p[1] - t[0]), dot(unit, p[2] - t[0])};
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        const double a = d[i];
        const double b = d[j];
        if (a * b >= 0.0 || std::max(std::abs(a), std::abs(b)) <= tol
            || std::min(std::abs(a), std::abs(b)) <= 1e-3 * tol) {
            continue;
        }
        const Vec x = p[i] + (p[j] - p[i]) * (a / (a - b));
        // Inside the triangle: on the inner side of each edge
        bool inside = true;
        for (int k = 0; k < 3 && inside; ++k) {
            const Vec e = t[(k + 1) % 3] - t[k];
            inside = dot(cross(e, x - t[k]), n) >= 0.0;
        }
        if (inside && confirm(x, p[i], p[j], np, n)) {
            return true;
        }
    }
    return false;
}

bool boxesOverlap(const Vec& minA, const Vec& maxA, const Vec& minB, const Vec& maxB, double tol)
{
    return minA.x() <= maxB.x() + tol && minB.x() <= maxA.x() + tol && minA.y() <= maxB.y() + tol
        && minB.y() <= maxA.y() + tol && minA.z() <= maxB.z() + tol && minB.z() <= maxA.z() + tol;
}

// The candidates a pair is given before its meshes are taken to touch only
constexpr int maxCandidates = 64;

// A crossing confirmed: a point near it inside both components by more than
// tol (`witness`). Any such point is a witness: the classification is exact.
// Two kinds are tried:
// - between the two surfaces: the region inside both, at the crossing, is a
//   wedge between the edge's triangle and the one it crosses, and its
//   bisector is a sum of their normals, signs unknown. A step s along it is
//   s * cos(g/2) deep in both, g the angle between the normals going in, so
//   the step is sized to be 1.5 tol deep, up to the edge's length: a sphere
//   sunk into a box crosses its face at 54 deg, and a step along the edge and
//   off the sphere's triangle stays shallower than tol on one side.
// - a little along the edge, either way, then a little off the edge's own
//   triangle, either way: the overlap of two boxes is bounded by faces of
//   theirs, and its crossings lie on them.
// Surfaces that only touch have no such point.
bool confirmed(const std::function<bool(const Vec&)>& witness,
               const Vec& at,
               const Vec& from,
               const Vec& to,
               const Vec& normal,
               const Vec& crossed,
               double tol)
{
    const double size = std::sqrt(dot(normal, normal));
    const double crossedSize = std::sqrt(dot(crossed, crossed));
    if (size <= 0.0 || crossedSize <= 0.0) {
        return false;
    }
    const Vec off = normal * (1.0 / size);
    const Vec across = crossed * (1.0 / crossedSize);
    const double edge = std::sqrt(dot(to - from, to - from));
    for (double sign : {1.0, -1.0}) {
        const Vec bisector = off + across * sign;
        const double length = std::sqrt(dot(bisector, bisector));
        // cos(g/2) = |bisector| / 2 for the two unit normals summed
        if (length <= 1e-6) {
            continue;
        }
        const double step =
            std::min(std::max(3.0 * tol, 3.0 * tol / length), std::max(edge, 3.0 * tol));
        for (double side : {1.0, -1.0}) {
            if (witness(at + bisector * (side * step / length))) {
                return true;
            }
        }
    }
    for (const Vec& end : {from, to}) {
        const Vec along = end - at;
        const double length = std::sqrt(dot(along, along));
        if (length <= 0.0) {
            continue;
        }
        const double step = std::min(3.0 * tol, 0.9 * length);
        for (double side : {1.0, -1.0}) {
            if (witness(at + along * (step / length) + off * (side * step))) {
                return true;
            }
        }
    }
    return false;
}

// Each triangle of ia's mesh with each of ib's whose boxes are within tol of
// each other, handed to `visit` in the frame of ia's solid with their indices
// in the meshes, until it says to stop; true when it did. b's tree is carried over by the pose of
// its solid relative to a's, its boxes as boxes around them and its triangles moved as they are
// reached, so a move rebuilds nothing.
using VisitTriangles =
    std::function<bool(const Vec (&pa)[3], const Vec (&pb)[3], int indexA, int indexB)>;

bool visitNearTriangles(const Instance& ia,
                        const Instance& ib,
                        double tol,
                        const VisitTriangles& visit)
{
    BRepExtrema_TriangleSet& a = ia.geometry->triangles();
    BRepExtrema_TriangleSet& b = ib.geometry->triangles();
    if (a.Size() == 0 || b.Size() == 0) {
        return false;
    }
    const auto& ta = a.BVH();
    const auto& tb = b.BVH();
    if (ta.IsNull() || tb.IsNull()) {
        return false;
    }
    // b's solid frame to a's
    const gp_Trsf relative = ia.inverse.Multiplied(ib.trsf);
    const gp_Mat& rotation = relative.HVectorialPart();
    double weight[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            weight[r][c] = std::abs(rotation.Value(r + 1, c + 1)) * relative.ScaleFactor();
        }
    }
    auto boxOfB = [&](int node, Vec& low, Vec& high) {
        const Vec& mn = tb->MinPoint(node);
        const Vec& mx = tb->MaxPoint(node);
        const Vec centre = apply(relative, (mn + mx) * 0.5);
        const Vec half = (mx - mn) * 0.5;
        for (int r = 0; r < 3; ++r) {
            const double extent =
                weight[r][0] * half.x() + weight[r][1] * half.y() + weight[r][2] * half.z();
            low[r] = centre[r] - extent;
            high[r] = centre[r] + extent;
        }
    };

    std::vector<std::pair<int, int>> stack {{0, 0}};
    std::vector<std::pair<std::array<Vec, 3>, int>> leafB;
    while (!stack.empty()) {
        const auto [na, nb] = stack.back();
        stack.pop_back();
        Vec lowB;
        Vec highB;
        boxOfB(nb, lowB, highB);
        if (!boxesOverlap(ta->MinPoint(na), ta->MaxPoint(na), lowB, highB, tol)) {
            continue;
        }
        const bool leafA = ta->IsOuter(na);
        const bool isLeafB = tb->IsOuter(nb);
        if (leafA && isLeafB) {
            leafB.clear();
            for (int j = tb->BegPrimitive(nb); j <= tb->EndPrimitive(nb); ++j) {
                Vec pb[3];
                b.GetVertices(j, pb[0], pb[1], pb[2]);
                leafB.emplace_back(std::array<Vec, 3> {apply(relative, pb[0]),
                                                       apply(relative, pb[1]),
                                                       apply(relative, pb[2])},
                                   j);
            }
            for (int i = ta->BegPrimitive(na); i <= ta->EndPrimitive(na); ++i) {
                Vec pa[3];
                a.GetVertices(i, pa[0], pa[1], pa[2]);
                for (const auto& [moved, j] : leafB) {
                    const Vec pb[3] = {moved[0], moved[1], moved[2]};
                    if (visit(pa, pb, i, j)) {
                        return true;
                    }
                }
            }
            continue;
        }
        // Down the larger node, or the one that is not a leaf
        if (leafA || (!isLeafB && tb->NbPrimitives(nb) > ta->NbPrimitives(na))) {
            stack.emplace_back(na, tb->template Child<0>(nb));
            stack.emplace_back(na, tb->template Child<1>(nb));
        }
        else {
            stack.emplace_back(ta->template Child<0>(na), nb);
            stack.emplace_back(ta->template Child<1>(na), nb);
        }
    }
    return false;
}

// Whether two solids' meshes cross by a confirmed crossing, tested in the
// frame of ia's solid. `candidates` counts the unconfirmed ones across the
// pair's solids.
bool instancesCross(Component& ca,
                    const Instance& ia,
                    Component& cb,
                    const Instance& ib,
                    double tol,
                    int& candidates)
{
    // The witnesses are classified in the assembly's coordinates, against
    // all of each component's solids
    const std::function<bool(const Vec&)> witness = [&](const Vec& local) {
        const gp_Pnt point = pointOf(apply(ia.trsf, local));
        return cb.contains(point, tol) && ca.contains(point, tol);
    };
    const Confirm confirm =
        [&](const Vec& at, const Vec& from, const Vec& to, const Vec& normal, const Vec& crossed) {
            ++candidates;
            return confirmed(witness, at, from, to, normal, crossed, tol);
        };
    bool crossed = false;
    visitNearTriangles(ia, ib, tol, [&](const Vec(&pa)[3], const Vec(&pb)[3], int, int) {
        if (edgeCrosses(pa, pb, tol, confirm) || edgeCrosses(pb, pa, tol, confirm)) {
            crossed = true;
            return true;
        }
        return candidates >= maxCandidates;
    });
    return crossed;
}

// The triangle pairs looked at for a touch, at most: a part sunk deep into a
// finely meshed one is marked from what they show
constexpr int maxTouchPairs = 200000;

// Where two components that interfere meet, into result's touch. The points
// where an edge of a triangle of one passes through a triangle of the other
// (unconfirmed: the pair is known to interfere) make, for each pair of
// triangles, a segment of the curve along which the meshes cross. The middle
// of the segments, each weighted by its length, is the centre; the normals of
// the triangles of `hit` that edges of the other pass through, weighted so and
// turned to agree, the direction, then turned to point out of `hit`. (Not
// those of hit's own edges through the other: two boxes sunk into each other
// face to face have those on the faces either side, which the turning adds
// up.) One inside the other, nothing crosses: the middle of the common part of
// their boxes; and with no direction so, the one from hit's box to the
// other's. The faces of each in a crossing into otherFaces and hitFaces.
void touchOf(Component& other,
             Component& hit,
             double tol,
             InterferenceResult& result,
             std::vector<std::pair<int, int>>& otherFaces,
             std::vector<std::pair<int, int>>& hitFaces)
{
    std::set<std::pair<int, int>> crossedOther;
    std::set<std::pair<int, int>> crossedHit;
    Vec weighted(0.0, 0.0, 0.0);
    Vec normal(0.0, 0.0, 0.0);
    double total = 0.0;
    int pairs = 0;
    Vec points[6];
    int count = 0;
    Vec hitNormal(0.0, 0.0, 0.0);
    // An edge of other's through a triangle of hit's: crossed is hit's
    const Confirm throughHit =
        [&](const Vec& at, const Vec&, const Vec&, const Vec&, const Vec& crossed) {
            if (count < 6) {
                points[count++] = at;
            }
            hitNormal = crossed;
            return false;
        };
    // An edge of hit's through a triangle of other's: a point only
    const Confirm throughOther =
        [&](const Vec& at, const Vec&, const Vec&, const Vec&, const Vec&) {
            if (count < 6) {
                points[count++] = at;
            }
            return false;
        };
    for (std::size_t no = 0; no < other.instances.size(); ++no) {
        const auto& io = other.instances[no];
        for (std::size_t nh = 0; nh < hit.instances.size(); ++nh) {
            const auto& ih = hit.instances[nh];
            if (!io.geometry || !ih.geometry || !boxesNear(io.box, ih.box, tol)) {
                continue;
            }
            // Found in the frame of io's solid, summed in the assembly's
            const Vec origin = apply(io.trsf, Vec(0.0, 0.0, 0.0));
            visitNearTriangles(
                io,
                ih,
                tol,
                [&](const Vec(&po)[3], const Vec(&ph)[3], int indexO, int indexH) {
                    count = 0;
                    hitNormal = Vec(0.0, 0.0, 0.0);
                    edgeCrosses(po, ph, tol, throughHit);
                    edgeCrosses(ph, po, tol, throughOther);
                    if (count > 0) {
                        crossedOther.emplace(static_cast<int>(no),
                                             io.geometry->triangles().GetFaceID(indexO));
                        crossedHit.emplace(static_cast<int>(nh),
                                           ih.geometry->triangles().GetFaceID(indexH));
                        // The segment: the two points farthest apart
                        Vec from = points[0];
                        Vec to = points[0];
                        double best = 0.0;
                        for (int i = 0; i < count; ++i) {
                            for (int j = i + 1; j < count; ++j) {
                                const Vec d = points[j] - points[i];
                                if (dot(d, d) > best) {
                                    best = dot(d, d);
                                    from = points[i];
                                    to = points[j];
                                }
                            }
                        }
                        const double length = std::max(std::sqrt(best), tol);
                        weighted = weighted + apply(io.trsf, (from + to) * 0.5) * length;
                        total += length;
                        Vec unit = apply(io.trsf, hitNormal) - origin;
                        const double size = std::sqrt(dot(unit, unit));
                        if (size > 0.0) {
                            unit = unit * ((dot(unit, normal) < 0.0 ? -1.0 : 1.0) / size);
                            normal = normal + unit * length;
                        }
                    }
                    return ++pairs >= maxTouchPairs;
                });
        }
    }

    const Bnd_Box& bo = exactBox(other);
    const Bnd_Box& bh = exactBox(hit);
    if (total <= 0.0 && (bo.IsVoid() || bh.IsVoid())) {
        return;
    }
    double ox0 {}, oy0 {}, oz0 {}, ox1 {}, oy1 {}, oz1 {};
    double hx0 {}, hy0 {}, hz0 {}, hx1 {}, hy1 {}, hz1 {};
    if (!bo.IsVoid() && !bh.IsVoid()) {
        bo.Get(ox0, oy0, oz0, ox1, oy1, oz1);
        bh.Get(hx0, hy0, hz0, hx1, hy1, hz1);
    }
    Vec centre;
    if (total > 0.0) {
        centre = weighted * (1.0 / total);
    }
    else {
        centre = Vec(0.5 * (std::max(ox0, hx0) + std::min(ox1, hx1)),
                     0.5 * (std::max(oy0, hy0) + std::min(oy1, hy1)),
                     0.5 * (std::max(oz0, hz0) + std::min(oz1, hz1)));
    }
    if (dot(normal, normal) <= 0.0) {
        normal = Vec(ox0 + ox1 - hx0 - hx1, oy0 + oy1 - hy0 - hy1, oz0 + oz1 - hz0 - hz1);
    }
    const double size = std::sqrt(dot(normal, normal));
    if (size > 0.0) {
        normal = normal * (1.0 / size);
        // Out of hit: a step along it is not inside hit, a step back is.
        // Inside both ways (the middle deep in the overlap) or neither, the
        // way from hit's box to the other's.
        const double step = 3.0 * tol;
        const bool ahead = hit.contains(pointOf(centre + normal * step), tol);
        const bool behind = hit.contains(pointOf(centre - normal * step), tol);
        const Vec boxes(ox0 + ox1 - hx0 - hx1, oy0 + oy1 - hy0 - hy1, oz0 + oz1 - hz0 - hz1);
        if ((ahead && !behind) || (ahead == behind && dot(normal, boxes) < 0.0)) {
            normal = normal * -1.0;
        }
    }
    result.touched = true;
    result.touchCentre = Base::Vector3d(centre.x(), centre.y(), centre.z());
    result.touchNormal = Base::Vector3d(normal.x(), normal.y(), normal.z());
    otherFaces.assign(crossedOther.begin(), crossedOther.end());
    hitFaces.assign(crossedHit.begin(), crossedHit.end());
}
bool meshesCross(Component& ca, Component& cb, double tol)
{
    int candidates = 0;
    for (const auto& ia : ca.instances) {
        for (const auto& ib : cb.instances) {
            if (!ia.geometry || !ib.geometry || !boxesNear(ia.box, ib.box, tol)) {
                continue;
            }
            if (instancesCross(ca, ia, cb, ib, tol, candidates)) {
                return true;
            }
            if (candidates >= maxCandidates) {
                return false;
            }
        }
    }
    return false;
}

// The point of triangle t nearest to p (Ericson, Real-Time Collision
// Detection, 5.1.5)
Vec nearestOnTriangle(const Vec& p, const Vec (&t)[3])
{
    const Vec ab = t[1] - t[0];
    const Vec ac = t[2] - t[0];
    const Vec ap = p - t[0];
    const double d1 = dot(ab, ap);
    const double d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return t[0];
    }
    const Vec bp = p - t[1];
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return t[1];
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return t[0] + ab * (d1 / (d1 - d3));
    }
    const Vec cp = p - t[2];
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return t[2];
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return t[0] + ac * (d2 / (d2 - d6));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        return t[1] + (t[2] - t[1]) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const double denominator = 1.0 / (va + vb + vc);
    return t[0] + ab * (vb * denominator) + ac * (vc * denominator);
}

// Whether triangles p and t lie face to face: parallel, p within tol of t's
// plane, and, seen along its normal, overlapping by more than tol (no edge of
// either separates them, by the separating axis test). Faces that only meet
// at an edge or a corner -- the sides of two blocks one on the other, which
// share an edge and may share a plane -- do not.
bool faceToFace(const Vec (&p)[3], const Vec (&t)[3], double tol)
{
    const Vec np = cross(p[1] - p[0], p[2] - p[0]);
    const Vec nt = cross(t[1] - t[0], t[2] - t[0]);
    const double sp = std::sqrt(dot(np, np));
    const double st = std::sqrt(dot(nt, nt));
    if (sp <= 0.0 || st <= 0.0 || std::abs(dot(np, nt)) < 0.98 * sp * st) {
        return false;
    }
    const Vec n = nt * (1.0 / st);
    for (const Vec& v : p) {
        if (std::abs(dot(n, v - t[0])) > tol) {
            return false;
        }
    }
    for (const auto* triangle : {&p, &t}) {
        for (int i = 0; i < 3; ++i) {
            const Vec edge = (*triangle)[(i + 1) % 3] - (*triangle)[i];
            const Vec axis = cross(n, edge);
            const double size = std::sqrt(dot(axis, axis));
            if (size <= 0.0) {
                continue;
            }
            double lowP = 1e300;
            double highP = -1e300;
            double lowT = 1e300;
            double highT = -1e300;
            for (int k = 0; k < 3; ++k) {
                const double a = dot(axis, p[k]) / size;
                const double b = dot(axis, t[k]) / size;
                lowP = std::min(lowP, a);
                highP = std::max(highP, a);
                lowT = std::min(lowT, b);
                highT = std::max(highT, b);
            }
            if (highP < lowT + tol || highT < lowP + tol) {
                return false;
            }
        }
    }
    return true;
}

// Where two components that touch meet, into result's touch: the middle of
// the vertices of each mesh within tol of a triangle of the other's, each
// vertex once; the direction, the normals of the triangles of `hit` that lie
// face to face with one of the other's, turned to agree and then to point out
// of `hit` (a step along it leaves it; else the way from hit's box to the
// other's). With no vertex near (an edge across a face), the middle of the
// common part of the boxes.
void contactOf(Component& other, Component& hit, double tol, InterferenceResult& result)
{
    std::set<std::array<double, 3>> seen;
    Vec sum(0.0, 0.0, 0.0);
    Vec normal(0.0, 0.0, 0.0);
    int pairs = 0;
    for (const auto& io : other.instances) {
        for (const auto& ih : hit.instances) {
            if (!io.geometry || !ih.geometry || !boxesNear(io.box, ih.box, tol)) {
                continue;
            }
            const Vec origin = apply(io.trsf, Vec(0.0, 0.0, 0.0));
            visitNearTriangles(io, ih, tol, [&](const Vec(&po)[3], const Vec(&ph)[3], int, int) {
                bool touching = false;
                auto add = [&](const Vec& vertex, const Vec(&triangle)[3]) {
                    const Vec gap = vertex - nearestOnTriangle(vertex, triangle);
                    if (dot(gap, gap) > tol * tol) {
                        return;
                    }
                    touching = true;
                    const Vec at = apply(io.trsf, vertex);
                    if (seen.insert({at.x(), at.y(), at.z()}).second) {
                        sum = sum + at;
                    }
                };
                for (const Vec& vertex : po) {
                    add(vertex, ph);
                }
                for (const Vec& vertex : ph) {
                    add(vertex, po);
                }
                if (touching && faceToFace(po, ph, tol)) {
                    Vec unit = apply(io.trsf, cross(ph[1] - ph[0], ph[2] - ph[0])) - origin;
                    const double size = std::sqrt(dot(unit, unit));
                    if (size > 0.0) {
                        unit = unit * ((dot(unit, normal) < 0.0 ? -1.0 : 1.0) / size);
                        normal = normal + unit;
                    }
                }
                return ++pairs >= maxTouchPairs;
            });
        }
    }

    const Bnd_Box& bo = exactBox(other);
    const Bnd_Box& bh = exactBox(hit);
    if (seen.empty() && (bo.IsVoid() || bh.IsVoid())) {
        return;
    }
    double ox0 {}, oy0 {}, oz0 {}, ox1 {}, oy1 {}, oz1 {};
    double hx0 {}, hy0 {}, hz0 {}, hx1 {}, hy1 {}, hz1 {};
    if (!bo.IsVoid() && !bh.IsVoid()) {
        bo.Get(ox0, oy0, oz0, ox1, oy1, oz1);
        bh.Get(hx0, hy0, hz0, hx1, hy1, hz1);
    }
    const Vec boxes(ox0 + ox1 - hx0 - hx1, oy0 + oy1 - hy0 - hy1, oz0 + oz1 - hz0 - hz1);
    const Vec centre = !seen.empty() ? sum * (1.0 / static_cast<double>(seen.size()))
                                     : Vec(0.5 * (std::max(ox0, hx0) + std::min(ox1, hx1)),
                                           0.5 * (std::max(oy0, hy0) + std::min(oy1, hy1)),
                                           0.5 * (std::max(oz0, hz0) + std::min(oz1, hz1)));
    if (dot(normal, normal) <= 0.0) {
        normal = boxes;
    }
    const double size = std::sqrt(dot(normal, normal));
    if (size > 0.0) {
        normal = normal * (1.0 / size);
        const double step = 3.0 * tol;
        const bool ahead = hit.contains(pointOf(centre + normal * step), tol);
        const bool behind = hit.contains(pointOf(centre - normal * step), tol);
        if ((ahead && !behind) || (ahead == behind && dot(normal, boxes) < 0.0)) {
            normal = normal * -1.0;
        }
    }
    result.touched = true;
    result.touchCentre = Base::Vector3d(centre.x(), centre.y(), centre.z());
    result.touchNormal = Base::Vector3d(normal.x(), normal.y(), normal.z());
}

// Whether `inner` is inside `outer`, by more than tol: the one inside the
// other, which no crossing shows -- nor do two copies of a part in the same
// place, whose surfaces coincide. With no crossing, inner is wholly inside or
// wholly outside, and any point inside it tells which: one is found off the
// middle of a triangle of its mesh, either way, 2 tol (which is at least the
// mesh's error and tol more) -- a vertex would not do, as it can be on outer's
// surface. Only when inner's mesh box is in outer's, both exact for the
// meshes where they are (the boxes of moved components are larger).
bool inside(Component& inner, Component& outer, double tol)
{
    const Bnd_Box& in = exactBox(inner);
    const Bnd_Box& out = exactBox(outer);
    if (in.IsVoid() || out.IsVoid()) {
        return false;
    }
    const double margin = tol + std::max(inner.deflection, outer.deflection);
    const gp_Pnt minIn = in.CornerMin();
    const gp_Pnt maxIn = in.CornerMax();
    const gp_Pnt minOut = out.CornerMin();
    const gp_Pnt maxOut = out.CornerMax();
    for (int axis = 1; axis <= 3; ++axis) {
        if (minIn.Coord(axis) < minOut.Coord(axis) - margin
            || maxIn.Coord(axis) > maxOut.Coord(axis) + margin) {
            return false;
        }
    }
    for (const auto& instance : inner.instances) {
        if (!instance.geometry) {
            continue;
        }
        BRepExtrema_TriangleSet& triangles = instance.geometry->triangles();
        const int count = triangles.Size();
        if (count == 0) {
            continue;
        }
        // A few triangles across the mesh: one may be on a wall thinner than 4 tol
        for (int i : {0, count / 3, 2 * count / 3, count - 1}) {
            Vec p[3];
            triangles.GetVertices(i, p[0], p[1], p[2]);
            const Vec normal = cross(p[1] - p[0], p[2] - p[0]);
            const double size = std::sqrt(dot(normal, normal));
            if (size <= 0.0) {
                continue;
            }
            const Vec middle = (p[0] + p[1] + p[2]) * (1.0 / 3.0);
            for (double side : {1.0, -1.0}) {
                const gp_Pnt point =
                    pointOf(apply(instance.trsf, middle + normal * (side * 2.0 * tol / size)));
                if (inner.contains(point, tol)) {
                    return outer.contains(point, tol);
                }
            }
        }
        break;
    }
    return false;
}

// Whether a point of the part the two meshes' boxes have in common is inside
// both, by more than tol: its middle, and the middles of its halves along its
// longest side. Two filleted plates of one thickness slid into each other edge
// to edge overlap in a strip bounded by coplanar faces and blends tangent to
// the other's faces: no surfaces cross by more than tol, and neither is inside
// the other. Never a false positive (the point is a witness); for faces in
// contact the common part is a slab and its middle on them.
bool middleInside(Component& a, Component& b, double tol)
{
    const Bnd_Box& ba = exactBox(a);
    const Bnd_Box& bb = exactBox(b);
    if (ba.IsVoid() || bb.IsVoid()) {
        return false;
    }
    double low[3];
    double high[3];
    int longest = 0;
    for (int axis = 0; axis < 3; ++axis) {
        low[axis] = std::max(ba.CornerMin().Coord(axis + 1), bb.CornerMin().Coord(axis + 1));
        high[axis] = std::min(ba.CornerMax().Coord(axis + 1), bb.CornerMax().Coord(axis + 1));
        if (high[axis] - low[axis] <= 2.0 * tol) {
            return false;
        }
        if (high[axis] - low[axis] > high[longest] - low[longest]) {
            longest = axis;
        }
    }
    const double quarter = 0.25 * (high[longest] - low[longest]);
    for (double offset : {0.0, -quarter, quarter}) {
        double point[3];
        for (int axis = 0; axis < 3; ++axis) {
            point[axis] = 0.5 * (low[axis] + high[axis]);
        }
        point[longest] += offset;
        const gp_Pnt pnt(point[0], point[1], point[2]);
        if (a.contains(pnt, tol) && b.contains(pnt, tol)) {
            return true;
        }
    }
    return false;
}

// Puts the component where `move` takes it from where it is now: in Exact its
// solids, in Fast its solids' poses (the meshes and their trees stay)
void place(Component& component, const Base::Placement& move, bool fast)
{
    const gp_Trsf moving = Part::TopoShape::convert(move);
    if (fast) {
        for (auto& instance : component.instances) {
            instance.trsf = moving.Multiplied(instance.now);
            instance.inverse = instance.trsf.Inverted();
            instance.box =
                instance.geometry ? instance.geometry->box().Transformed(instance.trsf) : Bnd_Box();
            instance.exact.SetVoid();
        }
        component.exact.SetVoid();
        component.meshes.Nullify();
    }
    else {
        const TopLoc_Location location(moving);
        BRep_Builder builder;
        builder.MakeCompound(component.compound);
        for (auto& instance : component.instances) {
            instance.shape = instance.base.Moved(location);
            builder.Add(component.compound, instance.shape);
        }
    }
    component.box = move.isIdentity() ? component.baseBox : component.baseBox.Transformed(moving);
}

// An Exact component as Fast has it, where it is checked: its solids' contact
// geometry, made by the cache (once for each solid), for where a contact
// touches
Component meshedCopy(const Component& exact)
{
    Component fast;
    fast.obj = exact.obj;
    for (const auto& instance : exact.instances) {
        Instance copy;
        copy.base = instance.base;
        gp_Trsf located = instance.shape.Location().Transformation();
        TopoDS_Shape solid = instance.shape;
        if (std::abs(located.ScaleFactor() - 1.0) > 1e-12 || located.IsNegative()) {
            // Scaled or mirrored: its own geometry, not shared
            solid =
                BRepBuilderAPI_Transform(instance.shape.Located(TopLoc_Location()), located, true)
                    .Shape();
            located = gp_Trsf();
        }
        copy.geometry = ContactGeometryCache::instance().get(solid, 0.0);
        if (!copy.geometry) {
            continue;
        }
        copy.now = located;
        copy.trsf = located;
        copy.inverse = located.Inverted();
        copy.box = copy.geometry->box().Transformed(located);
        fast.deflection = std::max(fast.deflection, copy.geometry->deflection());
        fast.instances.push_back(std::move(copy));
    }
    return fast;
}

// The pair where its components are: true, with `result` filled, when they
// interfere, or touch and contacts are asked for
bool checkPair(Component& a,
               Component& b,
               const InterferenceOptions& options,
               double tol,
               double pairTol,
               InterferenceResult& result)
{
    result.first = a.obj;
    result.second = b.obj;
    result.tolerance = pairTol;
    try {
        if (options.mode == InterferenceOptions::Mode::Fast) {
            if (meshesCross(a, b, pairTol) || inside(a, b, pairTol) || inside(b, a, pairTol)
                || middleInside(a, b, pairTol)) {
                result.kind = InterferenceResult::Kind::Interference;
                return true;
            }
            if (options.contacts) {
                BRepExtrema_ShapeProximity proximity(meshesOf(a), meshesOf(b), pairTol);
                proximity.Perform();
                if (proximity.IsDone() && !proximity.OverlapSubShapes1().IsEmpty()) {
                    result.kind = InterferenceResult::Kind::Contact;
                    result.distance = pairTol;
                    return true;
                }
            }
            return false;
        }

        // Non-destructive, as Part's booleans: the solids are the document's
        // own, and a destructive boolean may raise the tolerances of their
        // vertices and edges. The solids themselves, not their compounds:
        // a sphere touching a cylinder's face at a point failed (IsDone()
        // false) as compounds, destructive.
        BRepAlgoAPI_Common common;
        TopTools_ListOfShape arguments;
        TopTools_ListOfShape tools;
        for (const auto& instance : a.instances) {
            arguments.Append(instance.shape);
        }
        for (const auto& instance : b.instances) {
            tools.Append(instance.shape);
        }
        common.SetArguments(arguments);
        common.SetTools(tools);
        common.SetNonDestructive(Standard_True);
        common.Build();
        if (common.IsDone()) {
            const TopoDS_Shape overlap = common.Shape();
            GProp_GProps props;
            if (!overlap.IsNull()) {
                BRepGProp::VolumeProperties(overlap, props);
            }
            const double volume = overlap.IsNull() ? 0.0 : std::abs(props.Mass());
            if (a.volume < 0.0) {
                a.volume = volumeOf(a.compound);
            }
            if (b.volume < 0.0) {
                b.volume = volumeOf(b.compound);
            }
            // Below this the volume is the boolean's rounding: faces in
            // contact give a common of no volume, or of nearly none
            const double floor = std::max(1e-9 * std::min(a.volume, b.volume), 1e-12);
            if (volume > floor) {
                result.kind = InterferenceResult::Kind::Interference;
                result.volume = volume;
                result.overlap = Part::TopoShape(overlap);
                // Where they meet: the middle of the solid in common, and
                // out of b, the one hit, towards a
                const gp_Pnt centre = props.CentreOfMass();
                result.touched = true;
                result.touchCentre = Base::Vector3d(centre.X(), centre.Y(), centre.Z());
                if (!a.box.IsVoid() && !b.box.IsVoid()) {
                    double ax0 {}, ay0 {}, az0 {}, ax1 {}, ay1 {}, az1 {};
                    double bx0 {}, by0 {}, bz0 {}, bx1 {}, by1 {}, bz1 {};
                    a.box.Get(ax0, ay0, az0, ax1, ay1, az1);
                    b.box.Get(bx0, by0, bz0, bx1, by1, bz1);
                    Base::Vector3d out(ax0 + ax1 - bx0 - bx1,
                                       ay0 + ay1 - by0 - by1,
                                       az0 + az1 - bz0 - bz1);
                    if (out.Length() > 0.0) {
                        result.touchNormal = out.Normalize();
                    }
                }
                return true;
            }
        }
        else {
            FC_WARN("The interference of " << a.obj->getFullName() << " and "
                                           << b.obj->getFullName() << " was not computed");
        }
        if (options.contacts) {
            BRepExtrema_DistShapeShape distance(a.compound, b.compound);
            if (distance.IsDone() && distance.Value() <= tol + Precision::Confusion()) {
                result.kind = InterferenceResult::Kind::Contact;
                result.distance = distance.Value();
                // Where they touch: as Fast finds it, on the solids' meshes,
                // within the tolerance and at least twice their error
                Component meshedA = meshedCopy(a);
                Component meshedB = meshedCopy(b);
                contactOf(meshedA,
                          meshedB,
                          std::max(tol, 2.0 * std::max(meshedA.deflection, meshedB.deflection)),
                          result);
                return true;
            }
        }
    }
    catch (const Standard_Failure& e) {
        FC_WARN("The interference of " << a.obj->getFullName() << " and " << b.obj->getFullName()
                                       << " failed: " << e.GetMessageString());
    }
    return false;
}

// The object's placement in the assembly: its own and its containers', each
// as the frame puts it when the simulation moves it, else as it is now (and
// all as they are now for frame -1)
Base::Placement placementIn(const AssemblyObject* assembly, App::DocumentObject* obj, long frame)
{
    Base::Placement placement;
    for (auto* step = obj; step && step != assembly;
         step = App::GeoFeatureGroupExtension::getGroupOfObject(step)) {
        Base::Placement local;
        if (frame < 0
            || !assembly->placementAtFrame(step, static_cast<std::size_t>(frame), local)) {
            auto* prop = step->getPlacementProperty();
            local = prop ? prop->getValue() : Base::Placement();
        }
        placement = local * placement;
    }
    return placement;
}

// The findings of a posing, keyed by pair
using Findings = std::unordered_map<std::size_t, InterferenceResult>;

// The components of an assembly, built once and posed as often as asked
struct Scene
{
    const AssemblyObject* assembly;
    InterferenceOptions options;
    bool fast;
    double tol;
    std::vector<Component> components;

    Scene(const AssemblyObject* assembly,
          const InterferenceOptions& options,
          const std::vector<App::DocumentObject*>& only)
        : assembly(assembly)
        , options(options)
        , fast(options.mode == InterferenceOptions::Mode::Fast)
        , tol(std::max(options.tolerance, 0.0))
    {
        for (auto* obj : getAssemblyComponents(assembly)) {
            if (!obj || (!only.empty() && std::find(only.begin(), only.end(), obj) == only.end())) {
                continue;
            }
            const std::string path = pathFrom(assembly, obj);
            if (path.empty()) {
                continue;
            }
            Part::TopoShape shape;
            try {
                shape = Part::Feature::getTopoShape(assembly,
                                                    path.c_str(),
                                                    false,
                                                    nullptr,
                                                    nullptr,
                                                    true,
                                                    false);
            }
            catch (const Base::Exception& e) {
                FC_WARN(obj->getFullName()
                        << ": no shape for the interference check: " << e.what());
                continue;
            }
            if (shape.isNull()) {
                continue;
            }
            Component component;
            component.obj = obj;
            component.now = placementIn(assembly, obj, -1);
            BRep_Builder builder;
            TopoDS_Compound bases;
            builder.MakeCompound(bases);
            for (TopExp_Explorer it(shape.getShape(), TopAbs_SOLID); it.More(); it.Next()) {
                Instance instance;
                instance.base = it.Current();
                builder.Add(bases, instance.base);
                if (fast) {
                    instance.now = instance.base.Location().Transformation();
                    TopoDS_Shape solid = instance.base;
                    if (std::abs(instance.now.ScaleFactor() - 1.0) > 1e-12
                        || instance.now.IsNegative()) {
                        // Scaled or mirrored: its own geometry, not shared
                        solid = BRepBuilderAPI_Transform(instance.base.Located(TopLoc_Location()),
                                                         instance.now,
                                                         true)
                                    .Shape();
                        instance.now = gp_Trsf();
                    }
                    instance.geometry =
                        ContactGeometryCache::instance().get(solid, options.deflection);
                    if (!instance.geometry) {
                        continue;
                    }
                    component.deflection =
                        std::max(component.deflection, instance.geometry->deflection());
                }
                component.instances.push_back(std::move(instance));
            }
            if (component.instances.empty()) {
                continue;
            }
            BRepBndLib::Add(bases, component.baseBox, false);
            place(component, Base::Placement(), fast);
            components.push_back(std::move(component));
        }
    }

    // Each component where `placementOf` puts it in the assembly; `moved` set
    // for those whose move changed by more than `same` (all, when `first`)
    void pose(const std::function<Base::Placement(App::DocumentObject*)>& placementOf,
              bool first,
              double same)
    {
        for (auto& component : components) {
            const Base::Placement move = placementOf(component.obj) * component.now.inverse();
            const bool replace = !move.isSame(component.move, same);
            if (replace) {
                component.move = move;
                place(component, move, fast);
            }
            component.moved = first || replace;
        }
    }

    // The pairs where the components are now placed, findings into `found`
    // (and `out`, in pair order); a pair neither of whose components moved
    // has the finding it had in `last`, if any
    void checkAll(long frame,
                  double time,
                  const Findings* last,
                  Findings& found,
                  std::vector<InterferenceResult>* out)
    {
        const std::size_t n = components.size();
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                Component& a = components[i];
                Component& b = components[j];
                const std::size_t key = i * n + j;
                InterferenceResult result;
                if (last && !a.moved && !b.moved) {
                    auto it = last->find(key);
                    if (it == last->end()) {
                        continue;
                    }
                    result = it->second;
                }
                else {
                    const double pairTol =
                        fast ? std::max(tol, 2.0 * std::max(a.deflection, b.deflection)) : tol;
                    if (!boxesNear(a.box, b.box, pairTol + Precision::Confusion())
                        || !checkPair(a, b, options, tol, pairTol, result)) {
                        continue;
                    }
                    if (fast && result.kind == InterferenceResult::Kind::Interference) {
                        // The one hit: the one that did not move, else b
                        const bool aHit = b.moved && !a.moved;
                        try {
                            touchOf(aHit ? b : a,
                                    aHit ? a : b,
                                    pairTol,
                                    result,
                                    aHit ? result.touchFaces2 : result.touchFaces1,
                                    aHit ? result.touchFaces1 : result.touchFaces2);
                        }
                        catch (const Standard_Failure& e) {
                            FC_WARN("Where " << a.obj->getFullName() << " and "
                                             << b.obj->getFullName()
                                             << " meet was not found: " << e.GetMessageString());
                        }
                    }
                    else if (fast && result.kind == InterferenceResult::Kind::Contact) {
                        const bool aHit = b.moved && !a.moved;
                        try {
                            contactOf(aHit ? b : a, aHit ? a : b, pairTol, result);
                        }
                        catch (const Standard_Failure& e) {
                            FC_WARN("Where " << a.obj->getFullName() << " and "
                                             << b.obj->getFullName()
                                             << " touch was not found: " << e.GetMessageString());
                        }
                    }
                }
                result.frame = frame;
                result.time = time;
                if (out) {
                    out->push_back(result);
                }
                found.emplace(key, std::move(result));
            }
        }
    }
};

}  // namespace

std::vector<InterferenceResult>
Assembly::findInterferences(const AssemblyObject* assembly,
                            const InterferenceOptions& options,
                            const std::vector<App::DocumentObject*>& only)
{
    std::vector<InterferenceResult> results;
    if (!assembly) {
        return results;
    }
    Scene scene(assembly, options, only);

    if (options.frames.empty()) {
        Findings found;
        scene.checkAll(-1, 0.0, nullptr, found, &results);
        return results;
    }

    Base::SequencerLauncher progress("Checking interference over the frames",
                                     options.frames.size());
    Findings last;
    bool first = true;
    for (std::size_t frame : options.frames) {
        if (frame >= assembly->numberOfFrames()) {
            progress.next(true);
            continue;
        }
        scene.pose(
            [&](App::DocumentObject* obj) {
                return placementIn(assembly, obj, static_cast<long>(frame));
            },
            first,
            1e-12);
        Findings found;
        scene.checkAll(static_cast<long>(frame),
                       assembly->timeOfFrame(frame),
                       first ? nullptr : &last,
                       found,
                       &results);
        last = std::move(found);
        first = false;
        progress.next(true);
    }
    return results;
}

struct CollisionWatch::Private
{
    Scene scene;
    std::set<std::size_t> initial;
    Findings last;

    Private(const AssemblyObject* assembly, const InterferenceOptions& options)
        : scene(assembly, options, {})
    {}
};

namespace
{
InterferenceOptions watchOptions(double tolerance, double deflection)
{
    InterferenceOptions options;
    options.mode = InterferenceOptions::Mode::Fast;
    options.tolerance = tolerance;
    options.deflection = deflection;
    return options;
}
}  // namespace

CollisionWatch::CollisionWatch(const AssemblyObject* assembly, double tolerance, double deflection)
    : d(std::make_unique<Private>(assembly, watchOptions(tolerance, deflection)))
{
    Findings found;
    d->scene.checkAll(-1, 0.0, nullptr, found, nullptr);
    for (const auto& entry : found) {
        d->initial.insert(entry.first);
    }
    d->last = std::move(found);
}

CollisionWatch::~CollisionWatch() = default;

std::vector<InterferenceResult> CollisionWatch::check()
{
    const AssemblyObject* assembly = d->scene.assembly;
    // A pose that changed by less than this is the solver's rounding
    d->scene.pose(
        [&](App::DocumentObject* obj) {
            return placementIn(assembly, obj, -1);
        },
        false,
        1e-9);
    Findings found;
    d->scene.checkAll(-1, 0.0, &d->last, found, nullptr);
    std::vector<std::pair<std::size_t, InterferenceResult>> fresh;
    for (const auto& entry : found) {
        if (!d->initial.count(entry.first)) {
            fresh.emplace_back(entry);
        }
    }
    d->last = std::move(found);
    std::sort(fresh.begin(), fresh.end(), [](const auto& x, const auto& y) {
        return x.first < y.first;
    });
    std::vector<InterferenceResult> results;
    results.reserve(fresh.size());
    for (auto& entry : fresh) {
        results.push_back(std::move(entry.second));
    }
    return results;
}

std::size_t CollisionWatch::initialInterferences() const
{
    return d->initial.size();
}

std::size_t CollisionWatch::components() const
{
    return d->scene.components.size();
}

bool CollisionWatch::meshOf(const App::DocumentObject* obj,
                            std::vector<Base::Vector3d>& points,
                            const std::vector<std::pair<int, int>>* faces) const
{
    for (const auto& component : d->scene.components) {
        if (component.obj != obj) {
            continue;
        }
        const gp_Trsf frame = Part::TopoShape::convert(component.now).Inverted();
        for (std::size_t n = 0; n < component.instances.size(); ++n) {
            const auto& instance = component.instances[n];
            if (!instance.geometry) {
                continue;
            }
            // The solid's frame to the component's
            const gp_Trsf trsf = frame.Multiplied(instance.now);
            BRepExtrema_TriangleSet& triangles = instance.geometry->triangles();
            for (int i = 0; i < triangles.Size(); ++i) {
                if (faces
                    && !std::binary_search(
                        faces->begin(),
                        faces->end(),
                        std::make_pair(static_cast<int>(n), triangles.GetFaceID(i)))) {
                    continue;
                }
                Vec p[3];
                triangles.GetVertices(i, p[0], p[1], p[2]);
                for (const Vec& v : p) {
                    const Vec w = apply(trsf, v);
                    points.emplace_back(w.x(), w.y(), w.z());
                }
            }
        }
        return true;
    }
    return false;
}
