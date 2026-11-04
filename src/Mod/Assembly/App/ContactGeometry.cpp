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
#include <limits>
#include <utility>
#include <vector>

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <NCollection_DynamicArray.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_TShape.hxx>

#include "ContactGeometry.h"

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

// Whether the ray from o along d crosses the triangle abc ahead of o
// (Moller-Trumbore); `grazing` set when it passes within rounding of an edge
// or a vertex, or along the triangle's plane, where a count of crossings is
// not to be trusted
bool rayHits(const Vec& o, const Vec& d, const Vec& a, const Vec& b, const Vec& c, bool& grazing)
{
    constexpr double eps = 1e-9;
    const Vec e1 = b - a;
    const Vec e2 = c - a;
    const Vec p = cross(d, e2);
    const double det = dot(e1, p);
    const double scale = std::sqrt(dot(e1, e1) * dot(e2, e2));
    if (std::abs(det) <= 1e-12 * scale) {
        // Along the plane: grazing if it is in it, near enough
        const Vec n = cross(e1, e2);
        const double size = std::sqrt(dot(n, n));
        if (size > 0.0 && std::abs(dot(n, o - a)) <= 1e-9 * size * std::sqrt(scale)) {
            grazing = true;
        }
        return false;
    }
    const double inv = 1.0 / det;
    const Vec s = o - a;
    const double u = dot(s, p) * inv;
    if (u < -eps || u > 1.0 + eps) {
        return false;
    }
    const Vec q = cross(s, e1);
    const double v = dot(d, q) * inv;
    if (v < -eps || u + v > 1.0 + eps) {
        return false;
    }
    const double t = dot(e2, q) * inv;
    if (t <= 0.0) {
        return false;
    }
    if (u < eps || v < eps || u + v > 1.0 - eps) {
        grazing = true;
    }
    return true;
}

// Whether the ray from o along d (inverse components given) can meet the box
bool rayMeetsBox(const Vec& o, const Vec& inverse, const Vec& low, const Vec& high)
{
    double entry = 0.0;
    double exit = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        double t1 = (low[axis] - o[axis]) * inverse[axis];
        double t2 = (high[axis] - o[axis]) * inverse[axis];
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        entry = std::max(entry, t1);
        exit = std::min(exit, t2);
        if (entry > exit) {
            return false;
        }
    }
    return true;
}

}  // namespace

ContactMesh::ContactMesh(const TopoDS_Shape& solid, double deflection)
    : bare(solid.Located(TopLoc_Location()))
    , chord(deflection)
{
    copy = BRepBuilderAPI_Copy(bare, true, false).Shape();
    BRepMesh_IncrementalMesh(copy, chord, false, 0.5, false);
    NCollection_DynamicArray<TopoDS_Shape> faces;
    for (TopExp_Explorer it(copy, TopAbs_FACE); it.More(); it.Next()) {
        faces.Append(it.Current());
    }
    set = new BRepExtrema_TriangleSet(faces);
    const auto& tree = set->BVH();
    if (!tree.IsNull() && tree->Length() > 0) {
        const auto& low = tree->MinPoint(0);
        const auto& high = tree->MaxPoint(0);
        bounds.Update(low.x(), low.y(), low.z(), high.x(), high.y(), high.z());
    }
}

int ContactMesh::meshSide(const gp_Pnt& point) const
{
    if (bounds.IsVoid() || bounds.IsOut(point)) {
        return 0;
    }
    const auto& tree = set->BVH();
    if (tree.IsNull() || tree->Length() == 0) {
        return -1;
    }
    const Vec o(point.X(), point.Y(), point.Z());
    // Directions no face of a part is likely to lie along
    static const Vec directions[] = {Vec(0.5772156649, 0.6180339887, 0.5338874301),
                                     Vec(-0.7071067812, 0.3141592654, 0.6324555320),
                                     Vec(0.2718281828, -0.8414709848, 0.4672191987)};
    for (const Vec& direction : directions) {
        const Vec d = direction * (1.0 / std::sqrt(dot(direction, direction)));
        const Vec inverse(1.0 / d.x(), 1.0 / d.y(), 1.0 / d.z());
        bool grazing = false;
        int crossings = 0;
        std::vector<int> stack {0};
        while (!stack.empty() && !grazing) {
            const int node = stack.back();
            stack.pop_back();
            if (!rayMeetsBox(o, inverse, tree->MinPoint(node), tree->MaxPoint(node))) {
                continue;
            }
            if (tree->IsOuter(node)) {
                for (int i = tree->BegPrimitive(node); i <= tree->EndPrimitive(node); ++i) {
                    Vec a;
                    Vec b;
                    Vec c;
                    set->GetVertices(i, a, b, c);
                    if (rayHits(o, d, a, b, c, grazing)) {
                        ++crossings;
                    }
                }
            }
            else {
                stack.push_back(tree->template Child<0>(node));
                stack.push_back(tree->template Child<1>(node));
            }
        }
        if (!grazing) {
            return crossings % 2;
        }
    }
    return -1;
}

bool ContactMesh::contains(const gp_Pnt& point, double tol) const
{
    if (meshSide(point) == 0) {
        return false;
    }
    if (!classifier) {
        classifier = std::make_unique<BRepClass3d_SolidClassifier>();
        classifier->Load(bare);
    }
    classifier->Perform(point, tol);
    return classifier->State() == TopAbs_IN;
}

ContactGeometryCache& ContactGeometryCache::instance()
{
    static ContactGeometryCache cache;
    return cache;
}

std::shared_ptr<const ContactMesh> ContactGeometryCache::get(const TopoDS_Shape& solid,
                                                             double deflection)
{
    if (solid.IsNull()) {
        return {};
    }
    const Key key {solid.TShape().get(), static_cast<int>(solid.Orientation()), deflection};
    auto it = entries.find(key);
    if (it != entries.end()) {
        return it->second;
    }
    double chord = deflection;
    if (chord <= 0.0) {
        Bnd_Box box;
        BRepBndLib::Add(solid.Located(TopLoc_Location()), box, false);
        chord = std::max(box.IsVoid() ? 0.0 : 1e-4 * std::sqrt(box.SquareExtent()), 0.01);
    }
    auto mesh = std::make_shared<const ContactMesh>(solid, chord);
    entries.emplace(key, mesh);
    if (entries.size() >= pruneAt) {
        prune();
    }
    return mesh;
}

void ContactGeometryCache::prune()
{
    // A solid nobody else holds: its TShape is held by the entry alone (the
    // unlocated solid). Held, its address is not reused while it is cached.
    // Those in use (a drag's collision watch) live on in their users.
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->second->solid().TShape()->GetRefCount() <= 1) {
            it = entries.erase(it);
        }
        else {
            ++it;
        }
    }
    pruneAt = std::max<std::size_t>(64, 2 * entries.size());
}

void ContactGeometryCache::clear()
{
    entries.clear();
    pruneAt = 64;
}
