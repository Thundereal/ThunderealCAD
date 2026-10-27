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

// The reference backend (docs/AssemblySolverInterface.md sec 6.3 step 10,
// as built sec 6.13): the whole model at once, every equation written
// directly from the neutral Model with an analytic Jacobian, solved as the
// pose closest to the start (or to the drag targets) that satisfies them, and
// the rank of the Jacobian for the diagnostics. Simple and slow on purpose:
// it is the oracle other backends are tested against, not a production
// solver.

#include <memory>
#include <string>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

#include "Backend.h"

namespace Assembly::Solver
{

namespace Reference
{

// The neutral checker (sec 4.4 item 7): every joint's error at the given body
// poses (entry i for model joint i; poses by body id), the largest of its
// equations' errors in natural units -- mm for positions and distances, rad
// for angles, the cosine for an orientation equation (which is the angle,
// to first order). A coupler (Gear, CoaxialGear, RackPinion, Screw) is
// measured against the relation it had at the model's own poses, which is
// what every backend holds it to; its angles are taken less than half a turn
// from there. NaN for a joint that cannot be evaluated (a frame missing).
AssemblyExport std::vector<double>
jointResiduals(const Model& model, const std::vector<Base::Placement>& poses);

// The same for the Ground records (entry i for model ground i).
AssemblyExport std::vector<double>
groundResiduals(const Model& model, const std::vector<Base::Placement>& poses);

// One equation of the system, as the rank analysis sees it (for tests).
struct EquationInfo
{
    enum class Owner : std::uint8_t
    {
        Ground,
        Joint,
        Driver,
        Limit
    };
    // Ondsel's three categories, in the order the rank analysis takes them.
    enum class Category : std::uint8_t
    {
        Essential,
        Displacement,
        Perpendicular
    };
    Owner owner {};
    Id id = NoId;
    Category category {};
    double value = 0.0;  // the raw residual
    bool independent = true;  // false: in the span of the equations before it
};

}  // namespace Reference

class AssemblyExport ReferenceBackend: public Backend
{
public:
    std::string name() const override;
    std::string label() const override;
    Capabilities capabilities() const override;
    std::unique_ptr<Session> createSession() override;
};

class AssemblyExport ReferenceSession: public Session
{
public:
    ReferenceSession();
    ~ReferenceSession() override;

    Status load(const Model& model) override;
    // Takes new drag weights; anything else needs a load.
    Status update(const Model& model) override;
    Result solve(const SolveOptions& options) override;
    Status beginDrag(const std::vector<Id>& bodies, const DragOptions& options) override;
    Result dragStep(const std::vector<BodyPose>& targets) override;
    Result endDrag() override;
    std::vector<BodyPose> poses() const override;
    Status setPoses(const std::vector<BodyPose>& poses) override;
    Diagnostics analyse() override;
    // Position solves at each output time (simulateByPositionSolves).
    Status simulate(const SimulationSpec& spec, TrajectorySink& sink) override;
    // The neutral model as text: the bodies at their current poses, the
    // frames, joints, grounds, limits and drivers.
    Status exportNative(const std::string& path) override;

    // Every equation at the current poses, in the order the rank analysis
    // takes them, with its verdict (for tests).
    std::vector<Reference::EquationInfo> equations() const;
    // The rank of the Jacobian at the current poses, by column-pivoted QR.
    int rank() const;
    // The raw residual of every equation at the given body poses (grounds,
    // joints, drivers, in model order), with this session's constants and
    // angle readings, and its gradient: 6 entries per body, the position and
    // the spatial rotation vector (for tests).
    std::vector<double> residualsAt(const std::vector<Base::Placement>& poses) const;
    std::vector<std::vector<double>> jacobianAt(const std::vector<Base::Placement>& poses) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace Assembly::Solver
