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

// The Chrono backend (docs/AssemblySolverInterface.md sec 6.15, 6.17): the
// neutral model as a Project Chrono 10 system -- a ChBody per body, Chrono's
// links (ChLinkMate*, ChLinkDistance, ChLinkLockGear) per joint, a link of
// our own holding the neutral layer's equations (NeutralLink) for the kinds
// Chrono has no link for, and ChLinkMotorRotationAngle /
// ChLinkMotorLinearPosition per driver, driven by the neutral formula
// evaluator. Kinematics only: positions are found with Chrono's constraint
// equations, Jacobians and KKT solver.
//
// Built only with BUILD_ASSEMBLY_CHRONO (the macro ASSEMBLY_WITH_CHRONO). No
// Chrono header is included here: the session keeps its Chrono system behind
// a pointer, so the rest of the module, and the tests, compile without them.

#include <memory>
#include <string>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

#include "Backend.h"

namespace Assembly::Solver
{

namespace Chrono
{

// Why the backend cannot hold this joint, or empty when it can. Decided by
// the kind and the joint's values alone: since sec 6.17 every kind is held,
// a gear with a zero second radius (no ratio) is refused. A coaxial gear
// without a carrier frame is refused when the model is loaded.
AssemblyExport std::string refusal(const Joint& joint);

// Whether one of Chrono's own links holds this joint (sec 6.15), else the
// neutral equations do (NeutralLink, sec 6.17): an AngleZ of a right angle
// is ChLinkMateOrthogonal, any other angle neutral; PointLineDistance,
// LineLineDistance, CoaxialGear, RackPinion and Screw are neutral; a Gear
// with both radii positive is ChLinkLockGear unless its frames turn out
// coaxial or its shafts not parallel at load, any other Gear (a belt) is
// neutral.
AssemblyExport bool nativeLink(const Joint& joint);

// The kinds the backend expresses (Capabilities::joints): all of them.
AssemblyExport JointKindSet supportedKinds();

// A motion's formula as the ChFunction the backend gives a Chrono motor: its
// GetVal, GetDer and GetDer2 at a time, which are the neutral evaluator's
// value and exact derivatives (for tests). NaN when the formula has none.
AssemblyExport FormulaValue motionFunction(const Formula& formula, double time);

}  // namespace Chrono

class AssemblyExport ChronoBackend: public Backend
{
public:
    std::string name() const override;
    std::string label() const override;
    Capabilities capabilities() const override;
    std::unique_ptr<Session> createSession() override;
};

class AssemblyExport ChronoSession: public Session
{
public:
    ChronoSession();
    ~ChronoSession() override;

    // Builds the Chrono system. A model with a joint Chrono cannot hold loads
    // (Ok), but every solve, drag step and simulation then answers
    // Unsupported with refusalMessage(), and analyse() names those joints in
    // Diagnostics::unsupported.
    Status load(const Model& model) override;
    // Takes new drag weights; anything else needs a load.
    Status update(const Model& model) override;
    Result solve(const SolveOptions& options) override;
    Status beginDrag(const std::vector<Id>& bodies, const DragOptions& options) override;
    Result dragStep(const std::vector<BodyPose>& targets) override;
    Status setPoses(const std::vector<BodyPose>& poses) override;
    Result endDrag() override;
    std::vector<BodyPose> poses() const override;
    Diagnostics analyse() override;
    // Chrono's kinematic run: the motors follow the formulas, each output
    // interval split so that no driven angle turns more than a quarter
    // radian per step, each step predicted from the velocities and solved.
    // A Dynamic spec runs simulateDynamics().
    Status simulate(const SimulationSpec& spec, TrajectorySink& sink) override;
    // The forward dynamics of the bodies' masses under gravity
    // (docs/AssemblyDynamics.md, phase D1): the model in SI (sec 8.1), each
    // body's frame moved to its centre of mass so that a ChBody holds its
    // inertia, in a system of its own; Chrono's HHT on the direct solver;
    // the poses back in the model's units and frames. Refused, with the
    // reason logged: a model without its dynamics block, a body free to move
    // without mass, and what a dynamic run does not take yet -- motions,
    // limits and cams.
    // With 'prescribed', a Kinematic spec's frames (times and poses), the
    // inverse dynamics of that motion instead: the frames as they are, with
    // the reactions and the motors' efforts it takes at each (sec 6.3).
    Status simulateDynamics(
        const SimulationSpec& spec,
        TrajectorySink& sink,
        const std::vector<std::pair<double, std::vector<BodyPose>>>* prescribed = nullptr);
    // The Chrono system as Chrono's own JSON archive.
    Status exportNative(const std::string& path) override;

    // Why the loaded model is refused; empty when it is not.
    std::string refusalMessage() const;
    // How many Chrono links the joints became, and how many of them hold the
    // neutral equations (for tests).
    int linkCount() const;
    int neutralLinkCount() const;
    // Chrono's constraint values and Jacobian at the given body poses, for
    // every link in creation order (for tests): the Jacobian with 6 columns
    // per body, its position and its angular velocity in its own frame
    // (Chrono's velocity coordinates), zero for a body Chrono holds fixed. The
    // session's poses and counts of turns are left as they were.
    std::vector<double> constraintsAt(const std::vector<Base::Placement>& poses);
    std::vector<std::vector<double>> jacobianAt(const std::vector<Base::Placement>& poses);
    // The rank of Chrono's constraint Jacobian at the current poses (for tests).
    int rank() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace Assembly::Solver
