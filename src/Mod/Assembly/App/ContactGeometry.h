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

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <tuple>

#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_TriangleSet.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace Assembly
{

// A solid's contact geometry, in the solid's own frame (docs/AssemblyDynamics.md
// sec 7.2, docs/AssemblyInterference.md): its mesh, made on a copy so that the
// display's triangulation is left alone; the tree of the mesh's triangles; and
// the exact classification of points against the solid. Shared by every
// instance of the solid and every pose it is put in: moving one moves nothing
// here. Not thread safe: the classifier is made on first use.
class AssemblyExport ContactMesh
{
public:
    // The solid without its location, meshed at the deflection (mm, > 0)
    ContactMesh(const TopoDS_Shape& solid, double deflection);

    const TopoDS_Shape& solid() const
    {
        return bare;
    }
    const TopoDS_Shape& mesh() const
    {
        return copy;
    }
    double deflection() const
    {
        return chord;
    }
    // Its tree is built: BVH() only hands it out
    BRepExtrema_TriangleSet& triangles() const
    {
        return *set;
    }
    // The box of the mesh's triangles
    const Bnd_Box& box() const
    {
        return bounds;
    }
    // Whether a point, in the solid's frame, is inside it by more than tol.
    // Exact, but a point the mesh has outside is not classified: tol is at
    // least twice the deflection wherever this is asked (the Fast check's
    // pair tolerance), so a point inside the solid by more is inside the mesh
    // by more than the deflection.
    bool contains(const gp_Pnt& point, double tol) const;
    // Which side of the mesh the point (the solid's frame) is: 1 inside, 0
    // outside, -1 not known (every ray tried grazed an edge or a vertex), by
    // the parity of a ray's crossings
    int meshSide(const gp_Pnt& point) const;

private:
    TopoDS_Shape bare;
    TopoDS_Shape copy;
    double chord;
    Handle(BRepExtrema_TriangleSet) set;
    Bnd_Box bounds;
    mutable std::unique_ptr<BRepClass3d_SolidClassifier> classifier;
};

// The contact geometry of the solids, made once per solid (TShape and
// orientation) and deflection, for whoever asks: the interference check, its
// frames, a drag's collision watch, the dynamics' contact later.
class AssemblyExport ContactGeometryCache
{
public:
    static ContactGeometryCache& instance();

    // The solid's contact geometry, its location ignored. A deflection of 0 is
    // automatic: 1e-4 of the solid's box diagonal, at least 0.01 mm. Entries
    // whose solid nobody else holds any more are dropped as the cache grows.
    std::shared_ptr<const ContactMesh> get(const TopoDS_Shape& solid, double deflection = 0.0);
    std::size_t size() const
    {
        return entries.size();
    }
    void clear();

private:
    void prune();

    using Key = std::tuple<const void*, int, double>;
    std::map<Key, std::shared_ptr<const ContactMesh>> entries;
    std::size_t pruneAt = 64;
};

}  // namespace Assembly
