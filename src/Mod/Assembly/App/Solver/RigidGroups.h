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

#include <memory>

#include <Mod/Assembly/AssemblyGlobal.h>

#include "Backend.h"

namespace Assembly::Solver
{

// A session on another that is given a smaller model (sec 6.21): the bodies
// the joints hold rigidly, against the ground or against each other, are
// solved before the backend sees the model, and leave it.
//
// Starting from the grounded bodies, a body whose joints to the fixed ones
// leave it no freedom -- a Fixed joint, or joints whose equations, solved for
// that body alone, have rank 6 and none dependent -- is placed (a Fixed joint
// by multiplying its frames, as Assembly3 places an Attachment; any other set
// by the Reference backend, six unknowns) and becomes fixed in turn: its
// frames move onto the ground, the joints that placed it leave the model.
// When no body is left that can be placed so, the first remaining body (in
// model order) is taken as a ground of its own, and the bodies it holds
// rigidly the same way become one body with it, each at the offset the
// placing gave it; then the next remaining body, until none is left. A set of
// joints with a dependent equation is never taken (it stays with the backend,
// which reports it), nor is a body that a limit or a driver touches, nor a
// joint that couples rotations (gears, rack and pinion, screw) or holds a
// point on a curve.
//
// Poses come out, drag targets and diagnostics are mapped back to the bodies
// and joints of the model the session was given: a placed body stays where it
// was placed, a member of a group goes with the group, the joints that placed
// a body are never redundant, and the DoF is the same, each body taken out
// with six independent equations. A model with nothing to take out goes to
// the backend as it is.
AssemblyExport std::unique_ptr<Session> groupedRigidly(std::unique_ptr<Session> session);

}  // namespace Assembly::Solver
