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

// The Chrono backend's flexible bodies (docs/AssemblyDynamics.md, D4b): a
// Flexible record as a ChModalAssembly -- its mesh of 10-node tetrahedra
// internal, a boundary body for each interface, the interface's nodes tied
// to it rigidly by internal links -- reduced by Craig-Bampton to its modes.
// Chrono's headers: included by the backend's sources only, built with
// ASSEMBLY_WITH_CHRONO_MODAL.

#include <memory>
#include <vector>

#include <chrono/core/ChFrame.h>
#include <chrono/fea/ChMesh.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChSystem.h>
#include <chrono_modal/ChModalAssembly.h>

#include "Model.h"

namespace Assembly::Solver::ChronoFlexible
{

// One interface as the builder takes it: where its boundary body is, in the
// flexible body's frame, and the nodes tied to it (none: the nearest)
struct Interface
{
    chrono::ChFramed local;
    std::vector<Id> nodes;
};

struct Built
{
    std::shared_ptr<chrono::modal::ChModalAssembly> assembly;
    std::shared_ptr<chrono::fea::ChMesh> mesh;
    // By the interfaces given
    std::vector<std::shared_ptr<chrono::ChBody>> interfaces;
    double mass = 0.0;  // the mesh's
    // What its reduction is made of, in its own frame: the same, and an
    // earlier reduction's data are its own (reduce()). Its tetrahedra,
    // material, modes, damping and interfaces' nodes exactly; its nodes and
    // its interfaces' frames (positions, rotation matrices) to 1e-9 of its
    // size: a part turned has them in its frame an ulp off (the probe's rod
    // started 25 deg down, 0.05 m as 0.05000000000000001)
    struct Key
    {
        std::vector<double> exact;
        std::vector<double> near;
    };
    Key key;
};

// The flexible body (in SI: Solver::scaled()) built into the system at its
// pose, not yet reduced. Throws std::runtime_error on a mesh it cannot take.
Built build(chrono::ChSystem& sys,
            const Flexible& flexible,
            const chrono::ChFramed& pose,
            const std::vector<Interface>& interfaces);

// Reduces it to flexible.modes, Craig-Bampton, damped in proportion to its
// stiffness (Flexible::damping at dampingFrequency); the system set up. The
// modes' frequencies (Hz), their interfaces held.
std::vector<double> reduce(chrono::ChSystem& sys, Built& built, const Flexible& flexible);

}  // namespace Assembly::Solver::ChronoFlexible
