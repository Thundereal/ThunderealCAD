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

// The Chrono backend (docs/AssemblySolverInterface.md sec 6.15): the neutral
// model as a Project Chrono 10 system -- a ChBody per body, Chrono's links
// (ChLinkMate*, ChLinkDistance, ChLinkLockGear) per joint, and
// ChLinkMotorRotationAngle / ChLinkMotorLinearPosition per driver, driven by
// the neutral formula evaluator. Kinematics only: positions are found with
// Chrono's own constraint equations, Jacobians and KKT solver.
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

// Why Chrono cannot hold this joint, or empty when it can. Decided by the
// kind and the joint's values alone: an AngleZ of a right angle is
// ChLinkMateOrthogonal, and a Gear with a negative second radius (a belt) is
// refused. A Gear whose frames turn out coaxial, or whose shafts are not
// parallel, is refused when the model is loaded (ChronoSession::load).
AssemblyExport std::string refusal(const Joint& joint);

// The kinds Chrono expresses in general (Capabilities::joints). AngleZ is not
// among them, though a right angle is accepted.
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
    Result endDrag() override;
    std::vector<BodyPose> poses() const override;
    Diagnostics analyse() override;
    // Chrono's kinematic run: the motors follow the formulas, each output
    // interval split so that no driven angle turns more than a quarter
    // radian per step, each step predicted from the velocities and solved.
    Status simulate(const SimulationSpec& spec, TrajectorySink& sink) override;
    // The Chrono system as Chrono's own JSON archive.
    Status exportNative(const std::string& path) override;

    // Why the loaded model is refused; empty when it is not.
    std::string refusalMessage() const;
    // How many Chrono links the joints became (for tests).
    int linkCount() const;
    // The rank of Chrono's constraint Jacobian at the current poses (for tests).
    int rank() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace Assembly::Solver
