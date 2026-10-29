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
// first two rungs), in the coordinates the solid is given in: the sphere, box
// or cylinder the solid is -- every face of it the primitive's, and the solid's
// volume the primitive's to 1e-6 -- centred on 'placement' along its axes; else,
// when the solid is convex, the points of its contact mesh, the hull's.
struct SolidContactShape
{
    Solver::ContactShape::Kind kind {};
    Base::Placement placement;
    Base::Vector3d size;
    std::vector<Base::Vector3d> points;  // a hull's, in the given coordinates
    double deflection = 0.0;             // the mesh's chord error, a hull's
};

// Its contact shape, or none when the solid is no primitive and not convex:
// some point of its contact mesh more than twice the mesh's deflection in
// front of a triangle of it. deflection 0: ContactGeometryCache's own.
AssemblyExport std::optional<SolidContactShape> contactShapeOf(const TopoDS_Shape& solid,
                                                               double deflection = 0.0);

}  // namespace Assembly
