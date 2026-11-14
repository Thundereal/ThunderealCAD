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
#include <utility>
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

// An interface for a flexible body no joint holds and no contact faces are
// named of: the element at its mesh's middle, at that element's middle. Its
// boundary body is the body's motion as a whole, with nothing on it; the
// modes are the mesh's about that element. Throws std::runtime_error on a
// mesh with no volume.
Interface ownInterface(const Flexible& flexible);

// The flexible body (in SI: Solver::scaled()) built into the system at its
// pose, not yet reduced. Throws std::runtime_error on a mesh it cannot take.
Built build(chrono::ChSystem& sys,
            const Flexible& flexible,
            const chrono::ChFramed& pose,
            const std::vector<Interface>& interfaces);

// A flexible body's mesh surface, for a contact anywhere on it (D4b): the
// tetrahedra's faces no other shares, wound to face out of the body, in the
// body's frame, each its six nodes -- the elements' own surface, quadratic,
// a round face's to second order. A curved one is four flat triangles
// through its edges' nodes for collision detection, a quarter of the flat
// face's sagitta, a flat one its own; a point found on one is placed on the
// face (onFace(), farthest()). A grid of the triangles for the one nearest
// a point
class Surface
{
public:
    explicit Surface(const Flexible& flexible);

    // A place on it: its face's nodes -- the corners, then the nodes of the
    // edges (0 1), (1 2) and (2 0) -- the weights of the point on them, the
    // face's shape functions there, and that point, in the body's frame
    struct Place
    {
        std::array<Id, 6> nodes {};
        std::array<double, 6> weights {};
        chrono::ChVector3d at;
    };
    // The place nearest p, in the body's frame; false when it has none
    bool nearest(const chrono::ChVector3d& p, Place& place) const;
    // The point of triangle abc nearest p, as its weights on the corners
    static std::array<double, 3> weights(const chrono::ChVector3d& p,
                                         const chrono::ChVector3d& a,
                                         const chrono::ChVector3d& b,
                                         const chrono::ChVector3d& c);
    // The point of triangle i at weights w on its corners, on its face:
    // place's nodes and weights, and its point at rest
    void onFace(std::size_t i, const std::array<double, 3>& w, Place& place) const;
    // That point's area coordinates on its face (a flat face's, w)
    std::array<double, 3> faceCoordinates(std::size_t i, const std::array<double, 3>& w) const;
    // Whether triangle i is a quarter of a curved face; that face's nodes
    bool curved(std::size_t i) const
    {
        return pieces[i].second < 4;
    }
    const std::array<Id, 6>& faceOf(std::size_t i) const
    {
        return faces[pieces[i].first];
    }
    // Triangle i's face, by its number; a face's nodes by its number; and the
    // face across its edge e -- (0 1), (1 2), (2 0) -- or -1 where none is
    std::size_t faceIndex(std::size_t i) const
    {
        return pieces[i].first;
    }
    const std::array<Id, 6>& face(std::size_t f) const
    {
        return faces[f];
    }
    std::int64_t across(std::size_t f, int e) const
    {
        return neighbours[f][static_cast<std::size_t>(e)];
    }
    // The six-node triangle's shape functions at area coordinates L
    static std::array<double, 6> shape(const std::array<double, 3>& L);
    // The point of the quadratic face through x (its nodes in Place's
    // order) farthest along d, as its area coordinates; curve, whether the
    // face curves to it -- the way along d stationary there, inside it or
    // along a curved edge, not a corner a straight way runs to
    static std::array<double, 3> farthest(const std::array<chrono::ChVector3d, 6>& x,
                                          const chrono::ChVector3d& d,
                                          bool* curve = nullptr);
    std::size_t size() const
    {
        return corners.size();
    }
    // The farthest a face stands off its flat triangles, at rest
    double bulge() const
    {
        return most;
    }
    // The mesh's nodes at rest, in the body's frame, and the triangles, of
    // those nodes: for a collision mesh of it that the nodes move
    const std::vector<chrono::ChVector3d>& nodes() const
    {
        return points;
    }
    const std::vector<std::array<Id, 3>>& triangles() const
    {
        return corners;
    }

private:
    std::vector<chrono::ChVector3d> points;
    // The faces' six nodes; each triangle's face and which piece of it,
    // 0 to 3 a quarter (Surface()), 4 the whole: a flat face, its edges'
    // nodes at their middles, or one whose edges' nodes are not near them
    std::vector<std::array<Id, 6>> faces;
    std::vector<std::array<std::int64_t, 3>> neighbours;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> pieces;
    std::vector<std::array<Id, 3>> corners;
    double most = 0.0;
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
