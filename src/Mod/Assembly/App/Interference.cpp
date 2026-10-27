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
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepExtrema_ShapeProximity.hxx>
#include <BRepExtrema_TriangleSet.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Solid.hxx>
#include <TopTools_ListOfShape.hxx>

#include <App/DocumentObject.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Base/Console.h>
#include <Base/Sequencer.h>
#include <Mod/Part/App/PartFeature.h>

#include "AssemblyObject.h"
#include "AssemblyUtils.h"
#include "Interference.h"

FC_LOG_LEVEL_INIT("Assembly", true, true)

using namespace Assembly;

namespace
{

// A solid of a component where it is now and where it is checked (a frame
// puts it elsewhere), and its mesh (Fast) at both
struct Solid
{
    TopoDS_Shape base;      // located, where it is now
    TopoDS_Shape baseMesh;  // Fast: a meshed copy, at the same location
    TopoDS_Shape shape;     // where it is checked
    TopoDS_Shape mesh;
    double deflection = 0.0;
};

struct Component
{
    App::DocumentObject* obj = nullptr;
    std::vector<Solid> solids;
    TopoDS_Compound compound;  // the solids together, where checked
    Bnd_Box baseBox;           // now
    Bnd_Box box;               // where checked: baseBox moved, so a little larger
    // Over frames: the component's placement in the assembly now, the move
    // the frame last checked gave it, and whether that frame moved it
    Base::Placement now;
    Base::Placement move;
    bool moved = true;
    double volume = -1.0;     // computed when needed
    double deflection = 0.0;  // Fast: the largest of its solids'
    // Fast: the triangles of all its meshes' faces, made when a pair needs
    // them (null when it moved since): over frames, a component that moved
    // near nothing is not given any
    Handle(BRepExtrema_TriangleSet) triangles;
    TopoDS_Compound meshes;  // Fast: the meshed copies together
    // Fast: one per solid, made when a crossing is to be confirmed
    std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> classifiers;

    // Whether the point is inside one of the solids by more than tol
    bool contains(const gp_Pnt& point, double tol)
    {
        if (classifiers.empty()) {
            for (const auto& solid : solids) {
                auto classifier = std::make_unique<BRepClass3d_SolidClassifier>();
                classifier->Load(solid.shape);
                classifiers.push_back(std::move(classifier));
            }
        }
        for (auto& classifier : classifiers) {
            classifier->Perform(point, tol);
            if (classifier->State() == TopAbs_IN) {
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

double diagonalOf(const Bnd_Box& box)
{
    return box.IsVoid() ? 0.0 : std::sqrt(box.SquareExtent());
}

bool boxesNear(const Bnd_Box& a, const Bnd_Box& b, double gap)
{
    Bnd_Box enlarged = a;
    enlarged.Enlarge(gap);
    return !enlarged.IsOut(b);
}

// Fast: a solid's mesh, made once for every solid sharing its geometry, on a
// copy (the display's triangulation is left alone)
class Meshes
{
public:
    explicit Meshes(double deflection)
        : fixedDeflection(deflection)
    {}

    void mesh(Solid& solid)
    {
        const TopoDS_Shape bare = solid.base.Located(TopLoc_Location());
        auto it = copies.find(bare.TShape().get());
        if (it == copies.end()) {
            Bnd_Box box;
            BRepBndLib::Add(bare, box, false);
            const double deflection =
                fixedDeflection > 0.0 ? fixedDeflection : std::max(1e-4 * diagonalOf(box), 0.01);
            BRepBuilderAPI_Copy copier(bare, true, false);
            TopoDS_Shape copy = copier.Shape();
            BRepMesh_IncrementalMesh(copy, deflection, false, 0.5, false);
            it = copies.emplace(bare.TShape().get(), Entry {bare.TShape(), copy, deflection}).first;
        }
        solid.baseMesh = it->second.copy.Located(solid.base.Location());
        solid.deflection = it->second.deflection;
    }

private:
    struct Entry
    {
        Handle(TopoDS_TShape) tshape;  // held, so that its address is not reused
        TopoDS_Shape copy;
        double deflection;
    };
    double fixedDeflection;
    std::map<const TopoDS_TShape*, Entry> copies;
};

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
// tol. Any such point is a witness: the classification is exact. Two kinds
// are tried:
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
bool confirmed(Component& mine,
               Component& other,
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
    auto witness = [&](const Vec& point) {
        const gp_Pnt pnt(point.x(), point.y(), point.z());
        return other.contains(pnt, tol) && mine.contains(pnt, tol);
    };
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

BRepExtrema_TriangleSet& trianglesOf(Component& component)
{
    if (component.triangles.IsNull()) {
        NCollection_DynamicArray<TopoDS_Shape> faces;
        for (TopExp_Explorer it(component.meshes, TopAbs_FACE); it.More(); it.Next()) {
            faces.Append(it.Current());
        }
        component.triangles = new BRepExtrema_TriangleSet(faces);
    }
    return *component.triangles;
}

// Whether the two components' meshes cross by a confirmed crossing
bool meshesCross(Component& ca, Component& cb, double tol)
{
    BRepExtrema_TriangleSet& a = trianglesOf(ca);
    BRepExtrema_TriangleSet& b = trianglesOf(cb);
    if (a.Size() == 0 || b.Size() == 0) {
        return false;
    }
    int candidates = 0;
    // An edge of a's crossing a triangle of b goes into b, and the other way
    const Confirm intoB =
        [&](const Vec& at, const Vec& from, const Vec& to, const Vec& normal, const Vec& crossed) {
            ++candidates;
            return confirmed(ca, cb, at, from, to, normal, crossed, tol);
        };
    const Confirm intoA =
        [&](const Vec& at, const Vec& from, const Vec& to, const Vec& normal, const Vec& crossed) {
            ++candidates;
            return confirmed(cb, ca, at, from, to, normal, crossed, tol);
        };
    const auto& ta = a.BVH();
    const auto& tb = b.BVH();
    if (ta.IsNull() || tb.IsNull()) {
        return false;
    }
    std::vector<std::pair<int, int>> stack {{0, 0}};
    while (!stack.empty()) {
        const auto [na, nb] = stack.back();
        stack.pop_back();
        if (!boxesOverlap(ta->MinPoint(na),
                          ta->MaxPoint(na),
                          tb->MinPoint(nb),
                          tb->MaxPoint(nb),
                          tol)) {
            continue;
        }
        const bool leafA = ta->IsOuter(na);
        const bool leafB = tb->IsOuter(nb);
        if (leafA && leafB) {
            for (int i = ta->BegPrimitive(na); i <= ta->EndPrimitive(na); ++i) {
                Vec pa[3];
                a.GetVertices(i, pa[0], pa[1], pa[2]);
                for (int j = tb->BegPrimitive(nb); j <= tb->EndPrimitive(nb); ++j) {
                    Vec pb[3];
                    b.GetVertices(j, pb[0], pb[1], pb[2]);
                    if (edgeCrosses(pa, pb, tol, intoB) || edgeCrosses(pb, pa, tol, intoA)) {
                        return true;
                    }
                    if (candidates >= maxCandidates) {
                        return false;
                    }
                }
            }
            continue;
        }
        // Down the larger node, or the one that is not a leaf
        if (leafA || (!leafB && tb->NbPrimitives(nb) > ta->NbPrimitives(na))) {
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
    const auto& innerTree = trianglesOf(inner).BVH();
    const auto& outerTree = trianglesOf(outer).BVH();
    if (innerTree.IsNull() || outerTree.IsNull() || innerTree->Length() == 0
        || outerTree->Length() == 0) {
        return false;
    }
    const double margin = tol + std::max(inner.deflection, outer.deflection);
    const Vec minIn = innerTree->MinPoint(0);
    const Vec maxIn = innerTree->MaxPoint(0);
    const Vec minOut = outerTree->MinPoint(0);
    const Vec maxOut = outerTree->MaxPoint(0);
    for (int axis = 0; axis < 3; ++axis) {
        if (minIn[axis] < minOut[axis] - margin || maxIn[axis] > maxOut[axis] + margin) {
            return false;
        }
    }
    BRepExtrema_TriangleSet& triangles = trianglesOf(inner);
    const int count = triangles.Size();
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
            const Vec point = middle + normal * (side * 2.0 * tol / size);
            const gp_Pnt pnt(point.x(), point.y(), point.z());
            if (inner.contains(pnt, tol)) {
                return outer.contains(pnt, tol);
            }
        }
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
    const auto& ta = trianglesOf(a).BVH();
    const auto& tb = trianglesOf(b).BVH();
    if (ta.IsNull() || tb.IsNull() || ta->Length() == 0 || tb->Length() == 0) {
        return false;
    }
    Vec low;
    Vec high;
    int longest = 0;
    for (int axis = 0; axis < 3; ++axis) {
        low[axis] = std::max(ta->MinPoint(0)[axis], tb->MinPoint(0)[axis]);
        high[axis] = std::min(ta->MaxPoint(0)[axis], tb->MaxPoint(0)[axis]);
        if (high[axis] - low[axis] <= 2.0 * tol) {
            return false;
        }
        if (high[axis] - low[axis] > high[longest] - low[longest]) {
            longest = axis;
        }
    }
    Vec middle = (low + high) * 0.5;
    const double quarter = 0.25 * (high[longest] - low[longest]);
    for (double offset : {0.0, -quarter, quarter}) {
        Vec point = middle;
        point[longest] += offset;
        const gp_Pnt pnt(point.x(), point.y(), point.z());
        if (a.contains(pnt, tol) && b.contains(pnt, tol)) {
            return true;
        }
    }
    return false;
}

// Puts the component where `move` takes it from where it is now: its solids,
// meshes and box; its triangles (Fast) are made again when needed
void place(Component& component, const Base::Placement& move, bool fast)
{
    const TopLoc_Location location(Part::TopoShape::convert(move));
    BRep_Builder builder;
    builder.MakeCompound(component.compound);
    TopoDS_Compound meshes;
    builder.MakeCompound(meshes);
    for (auto& solid : component.solids) {
        solid.shape = solid.base.Moved(location);
        builder.Add(component.compound, solid.shape);
        if (fast) {
            solid.mesh = solid.baseMesh.Moved(location);
            builder.Add(meshes, solid.mesh);
        }
    }
    component.box = move.isIdentity() ? component.baseBox
                                      : component.baseBox.Transformed(location.Transformation());
    component.classifiers.clear();
    component.meshes = meshes;
    component.triangles.Nullify();
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
                BRepExtrema_ShapeProximity proximity(a.meshes, b.meshes, pairTol);
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
        for (const auto& solid : a.solids) {
            arguments.Append(solid.shape);
        }
        for (const auto& solid : b.solids) {
            tools.Append(solid.shape);
        }
        common.SetArguments(arguments);
        common.SetTools(tools);
        common.SetNonDestructive(Standard_True);
        common.Build();
        if (common.IsDone()) {
            const TopoDS_Shape overlap = common.Shape();
            const double volume = overlap.IsNull() ? 0.0 : volumeOf(overlap);
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
    const bool fast = options.mode == InterferenceOptions::Mode::Fast;
    const double tol = std::max(options.tolerance, 0.0);

    // The components, each with its solids in the assembly's coordinates
    std::vector<Component> components;
    Meshes meshes(options.deflection);
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
            FC_WARN(obj->getFullName() << ": no shape for the interference check: " << e.what());
            continue;
        }
        if (shape.isNull()) {
            continue;
        }
        Component component;
        component.obj = obj;
        BRep_Builder builder;
        TopoDS_Compound bases;
        builder.MakeCompound(bases);
        for (TopExp_Explorer it(shape.getShape(), TopAbs_SOLID); it.More(); it.Next()) {
            Solid solid;
            solid.base = it.Current();
            builder.Add(bases, solid.base);
            component.solids.push_back(solid);
        }
        if (component.solids.empty()) {
            continue;
        }
        BRepBndLib::Add(bases, component.baseBox, false);
        if (fast) {
            for (auto& solid : component.solids) {
                meshes.mesh(solid);
                component.deflection = std::max(component.deflection, solid.deflection);
            }
        }
        place(component, Base::Placement(), fast);
        components.push_back(std::move(component));
    }

    // The pairs where the components are now placed, findings into `found`
    // (keyed by pair); a pair neither of whose components moved has the
    // finding it had in `last`, if any
    using Findings = std::unordered_map<std::size_t, InterferenceResult>;
    const std::size_t n = components.size();
    auto checkAll = [&](long frame, double time, const Findings* last, Findings& found) {
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
                }
                result.frame = frame;
                result.time = time;
                results.push_back(result);
                found.emplace(key, std::move(result));
            }
        }
    };

    if (options.frames.empty()) {
        Findings found;
        checkAll(-1, 0.0, nullptr, found);
        return results;
    }

    for (auto& component : components) {
        component.now = placementIn(assembly, component.obj, -1);
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
        for (auto& component : components) {
            const Base::Placement move =
                placementIn(assembly, component.obj, static_cast<long>(frame))
                * component.now.inverse();
            const bool replace = !move.isSame(component.move, 1e-12);
            if (replace) {
                component.move = move;
                place(component, move, fast);
            }
            component.moved = first || replace;
        }
        Findings found;
        checkAll(static_cast<long>(frame),
                 assembly->timeOfFrame(frame),
                 first ? nullptr : &last,
                 found);
        last = std::move(found);
        first = false;
        progress.next(true);
    }
    return results;
}
