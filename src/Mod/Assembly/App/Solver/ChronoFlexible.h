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

#include <array>
#include <cstdint>
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

// A flexible body's mesh surface, for a contact anywhere on it (D4b): the
// tetrahedra's faces no other shares, as triangles of their corners, in the
// body's frame, and a grid of them for the one nearest a point
class Surface
{
public:
    explicit Surface(const Flexible& flexible);

    // A place on it: a triangle's corners (the mesh's nodes), the weights of
    // the point on it, and that point, in the body's frame
    struct Place
    {
        std::array<Id, 3> nodes {};
        std::array<double, 3> weights {};
        chrono::ChVector3d at;
    };
    // The place nearest p, in the body's frame; false when it has none
    bool nearest(const chrono::ChVector3d& p, Place& place) const;
    std::size_t size() const
    {
        return corners.size();
    }

private:
    std::vector<chrono::ChVector3d> points;
    std::vector<std::array<Id, 3>> corners;
    chrono::ChVector3d low;
    double cell = 1.0;
    std::array<int, 3> count {1, 1, 1};
    std::vector<std::vector<std::uint32_t>> cells;
};

// Reduces it to flexible.modes, Craig-Bampton, damped in proportion to its
// stiffness (Flexible::damping at dampingFrequency); the system set up. The
// modes' frequencies (Hz), their interfaces held. A reduction of the part
// as it is now, kept -- by this process, or in the document
// (Flexible::reduction) -- is taken instead of made.
std::vector<double> reduce(chrono::ChSystem& sys, Built& built, const Flexible& flexible);

// Its reduction as numbers, for the document to keep (ModeShapes::reduction):
// a format number, its key's hash, the modes' frequencies 'hz' and our
// Chrono's ReducedData, but for Psi, which is made of the rest. Empty
// unless reduced.
std::vector<double> reductionNumbers(const Built& built, const std::vector<double>& hz);

// Numbers reductionNumbers() made, kept by this process for the next reduction
// of a part built as 'built' is (reduce()); false, and nothing kept, when
// they are not that part's -- its key's hash another -- or not such numbers
bool keepReduction(const Built& built, const std::vector<double>& numbers);

}  // namespace Assembly::Solver::ChronoFlexible
