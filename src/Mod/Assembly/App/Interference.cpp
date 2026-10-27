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

#include <App/DocumentObject.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Base/Console.h>
#include <Mod/Part/App/PartFeature.h>

#include "AssemblyObject.h"
#include "AssemblyUtils.h"
#include "Interference.h"

FC_LOG_LEVEL_INIT("Assembly", true, true)

using namespace Assembly;

namespace
{

// A solid of a component where it is, and its mesh (Fast) there
struct Solid
{
    TopoDS_Shape shape;  // located
    TopoDS_Shape mesh;  // Fast: a meshed copy, at the same location
    double deflection = 0.0;
};

struct Component
{
    App::DocumentObject* obj = nullptr;
    std::vector<Solid> solids;
    TopoDS_Compound compound;  // the solids together
    Bnd_Box box;
    double volume = -1.0;  // computed when needed
    double deflection = 0.0;  // Fast: the largest of its solids'
    Handle(BRepExtrema_TriangleSet) triangles;  // Fast: of all its faces
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
        const TopoDS_Shape bare = solid.shape.Located(TopLoc_Location());
        auto it = copies.find(bare.TShape().get());
        if (it == copies.end()) {
            Bnd_Box box;
            BRepBndLib::Add(bare, box, false);
            const double deflection = fixedDeflection > 0.0
                ? fixedDeflection
                : std::max(1e-4 * diagonalOf(box), 1e-6);
            BRepBuilderAPI_Copy copier(bare, true, false);
            TopoDS_Shape copy = copier.Shape();
            BRepMesh_IncrementalMesh(copy, deflection, false, 0.5, false);
            it = copies.emplace(bare.TShape().get(), Entry {bare.TShape(), copy, deflection}).first;
        }
        solid.mesh = it->second.copy.Located(solid.shape.Location());
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
    return Vec(a.y() * b.z() - a.z() * b.y(), a.z() * b.x() - a.x() * b.z(), a.x() * b.y() - a.y() * b.x());
}

double dot(const Vec& a, const Vec& b)
{
    return a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
}

// An edge of triangle p crossing triangle t, from more than tol on one side of
// its plane to more than tol on the other, handed to `confirm` with the point
// where it crosses, the edge's two ends and p's normal; true when one is
// confirmed. Faces that only touch cross so where an edge of one meets the
// other's boundary, and meshes that touch (a pin in its hole) within their
// error: a crossing is only a candidate.
using Confirm =
    std::function<bool(const Vec& at, const Vec& from, const Vec& to, const Vec& normal)>;

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
        if (!((a > tol && b < -tol) || (a < -tol && b > tol))) {
            continue;
        }
        const Vec x = p[i] + (p[j] - p[i]) * (a / (a - b));
        // Inside the triangle: on the inner side of each edge
        bool inside = true;
        for (int k = 0; k < 3 && inside; ++k) {
            const Vec e = t[(k + 1) % 3] - t[k];
            inside = dot(cross(e, x - t[k]), n) >= 0.0;
        }
        if (inside && confirm(x, p[i], p[j], np)) {
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
// tol -- a little along the edge, either way, then a little off the edge's
// own triangle, either way (the overlap of two boxes is bounded by faces of
// theirs: a point along the edge alone stays on a surface). Surfaces that only
// touch have no such point.
bool confirmed(Component& mine, Component& other, const Vec& at, const Vec& from, const Vec& to,
               const Vec& normal, double tol)
{
    const double size = std::sqrt(dot(normal, normal));
    if (size <= 0.0) {
        return false;
    }
    const Vec off = normal * (1.0 / size);
    for (const Vec& end : {from, to}) {
        const Vec along = end - at;
        const double length = std::sqrt(dot(along, along));
        if (length <= 0.0) {
            continue;
        }
        const double step = std::min(3.0 * tol, 0.9 * length);
        for (double side : {1.0, -1.0}) {
            const Vec point = at + along * (step / length) + off * (side * step);
            const gp_Pnt pnt(point.x(), point.y(), point.z());
            if (other.contains(pnt, tol) && mine.contains(pnt, tol)) {
                return true;
            }
        }
    }
    return false;
}

// Whether the two components' meshes cross by a confirmed crossing
bool meshesCross(Component& ca, Component& cb, double tol)
{
    BRepExtrema_TriangleSet& a = *ca.triangles;
    BRepExtrema_TriangleSet& b = *cb.triangles;
    if (a.Size() == 0 || b.Size() == 0) {
        return false;
    }
    int candidates = 0;
    // An edge of a's crossing a triangle of b goes into b, and the other way
    const Confirm intoB = [&](const Vec& at, const Vec& from, const Vec& to, const Vec& normal) {
        ++candidates;
        return confirmed(ca, cb, at, from, to, normal, tol);
    };
    const Confirm intoA = [&](const Vec& at, const Vec& from, const Vec& to, const Vec& normal) {
        ++candidates;
        return confirmed(cb, ca, at, from, to, normal, tol);
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
        if (!boxesOverlap(ta->MinPoint(na), ta->MaxPoint(na), tb->MinPoint(nb), tb->MaxPoint(nb), tol)) {
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

// Whether a point of `inner` is inside a solid of `outer`, by more than tol:
// the one inside the other, which no crossing shows
bool inside(const Component& inner, const Component& outer, double tol)
{
    Bnd_Box enlarged = outer.box;
    enlarged.Enlarge(tol);
    Bnd_Box innerBox = inner.box;
    if (innerBox.IsVoid() || enlarged.IsOut(innerBox.CornerMin()) || enlarged.IsOut(innerBox.CornerMax())) {
        return false;
    }
    TopExp_Explorer vertex(inner.compound, TopAbs_VERTEX);
    if (!vertex.More()) {
        return false;
    }
    const gp_Pnt point = BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current()));
    for (const auto& solid : outer.solids) {
        BRepClass3d_SolidClassifier classifier(solid.shape, point, tol);
        if (classifier.State() == TopAbs_IN) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<InterferenceResult> Assembly::findInterferences(
    const AssemblyObject* assembly,
    const InterferenceOptions& options,
    const std::vector<App::DocumentObject*>& only
)
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
            shape = Part::Feature::getTopoShape(assembly, path.c_str(), false, nullptr, nullptr, true, false);
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
        builder.MakeCompound(component.compound);
        for (TopExp_Explorer it(shape.getShape(), TopAbs_SOLID); it.More(); it.Next()) {
            Solid solid;
            solid.shape = it.Current();
            builder.Add(component.compound, solid.shape);
            component.solids.push_back(solid);
        }
        if (component.solids.empty()) {
            continue;
        }
        BRepBndLib::Add(component.compound, component.box, false);
        if (fast) {
            NCollection_DynamicArray<TopoDS_Shape> faces;
            builder.MakeCompound(component.meshes);
            for (auto& solid : component.solids) {
                meshes.mesh(solid);
                component.deflection = std::max(component.deflection, solid.deflection);
                builder.Add(component.meshes, solid.mesh);
                for (TopExp_Explorer it(solid.mesh, TopAbs_FACE); it.More(); it.Next()) {
                    faces.Append(it.Current());
                }
            }
            component.triangles = new BRepExtrema_TriangleSet(faces);
        }
        components.push_back(std::move(component));
    }

    for (std::size_t i = 0; i < components.size(); ++i) {
        for (std::size_t j = i + 1; j < components.size(); ++j) {
            Component& a = components[i];
            Component& b = components[j];
            const double pairTol = fast ? std::max(tol, 2.0 * std::max(a.deflection, b.deflection))
                                        : tol;
            if (!boxesNear(a.box, b.box, pairTol + Precision::Confusion())) {
                continue;
            }
            InterferenceResult result;
            result.first = a.obj;
            result.second = b.obj;
            result.tolerance = pairTol;
            try {
                if (fast) {
                    if (meshesCross(a, b, pairTol) || inside(a, b, pairTol)
                        || inside(b, a, pairTol)) {
                        result.kind = InterferenceResult::Kind::Interference;
                        results.push_back(std::move(result));
                    }
                    else if (options.contacts) {
                        BRepExtrema_ShapeProximity proximity(a.meshes, b.meshes, pairTol);
                        proximity.Perform();
                        if (proximity.IsDone() && !proximity.OverlapSubShapes1().IsEmpty()) {
                            result.kind = InterferenceResult::Kind::Contact;
                            result.distance = pairTol;
                            results.push_back(std::move(result));
                        }
                    }
                    continue;
                }

                BRepAlgoAPI_Common common(a.compound, b.compound);
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
                        results.push_back(std::move(result));
                        continue;
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
                        results.push_back(std::move(result));
                    }
                }
            }
            catch (const Standard_Failure& e) {
                FC_WARN("The interference of " << a.obj->getFullName() << " and "
                        << b.obj->getFullName() << " failed: " << e.GetMessageString());
            }
        }
    }
    return results;
}
