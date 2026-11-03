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

#include <optional>
#include <vector>

#include <TopoDS_Shape.hxx>

#include <Base/Placement.h>
#include <Base/Vector3D.h>
#include <Mod/Assembly/AssemblyGlobal.h>

#include "Solver/Model.h"

namespace Assembly
{

// A solid's contact geometry (docs/AssemblyDynamics.md sec 7.1, the ladder's
// first rungs), in the coordinates the solid is given in: the sphere, box or
// cylinder the solid is -- every face of it the primitive's, and the solid's
// volume the primitive's to 1e-6 -- centred on 'placement' along its axes; else,
// when the solid is convex, the points of its contact mesh, the hull's; else
// that mesh, closed, its triangles wound outward (rung 3a).
struct SolidContactShape
{
    Solver::ContactShape::Kind kind {};
    Base::Placement placement;
    Base::Vector3d size;
    std::vector<Base::Vector3d> points;         // a hull's or a mesh's, in the given coordinates
    std::vector<std::array<int, 3>> triangles;  // on points, a hull's mesh's too
    double deflection = 0.0;                    // the mesh's chord error, a hull's or a mesh's
    // The solid's faces at each point (ContactShape::surfaces), a hull's or a mesh's
    std::vector<std::vector<Solver::ContactShape::Surface>> surfaces;
    // Its faces that are a convex cylinder's or sphere's (ContactShape::rounds)
    std::vector<Solver::ContactShape::Round> rounds;
};

// Its contact shape, or none when the solid is no primitive, not convex and
// its contact mesh not closed (not manifold, or in more than one piece).
// Convex: no point of the mesh more than twice its deflection in front of a
// neighbouring triangle. deflection 0: ContactGeometryCache's own.
AssemblyExport std::optional<SolidContactShape> contactShapeOf(const TopoDS_Shape& solid,
                                                               double deflection = 0.0);

// A planar face's profile (docs/AssemblyDynamics.md sec 7.1's rung 5, sec
// 7.4's 2D path), in the coordinates the face is given in: 'placement' its
// plane's frame, x and y the plane's, z their cross product; its wires' loops
// in that frame's xy plane, each piece a segment or an arc, the face on its
// left. Lines and circles as they are, any other curve in arcs and segments
// within 'tolerance' (mm). And how far 'solid' (the face's, given in the
// same coordinates) reaches along z, below and above the plane. None when
// the face is not planar or has no loop.
struct ProfileContactShape
{
    Base::Placement placement;
    std::vector<Solver::ProfilePiece> pieces;
    std::vector<int> loops;  // where each loop starts in pieces
    double low = 0.0;
    double high = 0.0;
};

AssemblyExport std::optional<ProfileContactShape>
profileOf(const TopoDS_Shape& face, const TopoDS_Shape& solid, double tolerance = 1e-3);

}  // namespace Assembly
