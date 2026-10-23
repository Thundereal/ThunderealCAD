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

// A solver backend and its sessions (docs/AssemblySolverInterface.md sec 4.3).
// Every argument and result is plain data, so a session can be served by
// another process.

#include <bitset>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Model.h"

namespace Assembly::Solver
{

enum class Status : std::int8_t
{
    Ok = 0,
    Failed = -1,  // -1 and -6 keep AssemblyObject::solve()'s return codes
    NotConverged = -2,
    Singular = -3,
    Unsupported = -4,
    NeedsReload = -5,
    NoGround = -6,
    Cancelled = -7,
};

struct Diagnostics
{
    enum Field : std::uint32_t
    {
        KnownDof = 1u << 0,
        KnownBodyDof = 1u << 1,
        KnownRedundant = 1u << 2,
        KnownConflicting = 1u << 3,
        KnownPartiallyRedundant = 1u << 4,
        KnownMalformed = 1u << 5,
        KnownUnsupported = 1u << 6,
        KnownOverDefinedGroups = 1u << 7,
    };

    int dof = -1;  // -1: unknown
    std::vector<int> bodyDof;  // per Model::bodies; empty: unknown
    std::vector<Id> redundant;  // joint ids
    std::vector<Id> conflicting;
    std::vector<Id> partiallyRedundant;
    std::vector<Id> malformed;  // e.g. a self-referencing joint
    std::vector<Id> unsupported;  // kinds this backend cannot express
    std::vector<std::vector<Id>> overDefinedGroups;  // minimal sets, optional
    std::uint32_t known = 0;  // a Field bit per field the backend computed
};

struct BodyPose
{
    Id body = NoId;
    Base::Placement placement;
};

struct Result
{
    Status status = Status::Failed;
    std::vector<BodyPose> moved;  // only bodies that moved beyond tolerance
    Diagnostics diagnostics;  // when requested
    double residual = 0.0;
    int iterations = 0;
    std::string message;
};

using JointKindSet = std::bitset<static_cast<std::size_t>(JointKind::Count_)>;

struct Capabilities
{
    JointKindSet joints;
    bool limits = false;
    bool drivers = false;
    bool drag = false;
    bool kinematics = false;
    bool dynamics = false;
    bool warmUpdate = false;
    bool redundancy = false;
    bool conflicts = false;
    bool bodyDof = false;
    bool outOfProcess = false;
    std::string nativeFormat;  // "asmt" for Ondsel, empty if none
};

struct SolveOptions
{
    double tolerance = 1e-9;
    int maxIterations = 0;  // 0: backend default
    bool diagnostics = true;
};

struct DragOptions
{
    bool bundleFixed = true;
};

struct SimulationSpec
{
    double tStart = 0.0;
    double tEnd = 1.0;
    double tOutput = 1e-2;
    double errorTolerance = 1e-6;
    double hMin = 1e-9;
    double hMax = 1.0;
};

// Poses of the bodies at each output time; owned by the neutral layer, so
// playback needs no live solver.
struct Trajectory
{
    std::vector<Id> bodies;
    std::vector<double> times;
    std::vector<Base::Placement> poses;  // times.size() * bodies.size(), row per time
};

class TrajectorySink
{
public:
    virtual ~TrajectorySink() = default;
    // false: cancel the run
    virtual bool frame(double time, const std::vector<BodyPose>& poses) = 0;
};

// One warm system: one connected component, or the whole model.
class Session
{
public:
    virtual ~Session() = default;

    virtual Status load(const Model& model) = 0;
    // NeedsReload if the backend cannot apply the change to the live system.
    // Takes the new model whole until ModelDelta exists (sec 4.2, step 8). A
    // change of Body::dragWeight alone is what a drag needs before beginDrag.
    virtual Status update(const Model& model)
    {
        (void)model;
        return Status::NeedsReload;
    }
    virtual Result solve(const SolveOptions& options) = 0;

    virtual Status beginDrag(const std::vector<Id>& bodies, const DragOptions& options) = 0;
    virtual Result dragStep(const std::vector<BodyPose>& targets) = 0;
    virtual Result endDrag() = 0;  // no full re-solve required

    // Every body's pose as the session holds it now, in Model::bodies order
    // (the entry of body i has body == i). Result::moved names only the bodies
    // that moved; writing back and checking grounded bodies need them all.
    virtual std::vector<BodyPose> poses() const = 0;

    virtual Diagnostics analyse() = 0;  // at the current state
    // A backend without kinematics of its own can answer with
    // simulateByPositionSolves() below (sec 4.6); the base class does not
    // keep the model, so it is not the default.
    virtual Status simulate(const SimulationSpec& spec, TrajectorySink& sink)
    {
        (void)spec;
        (void)sink;
        return Status::Unsupported;
    }
    virtual Status exportNative(const std::string& path)
    {
        (void)path;
        return Status::Unsupported;
    }
};

class Backend
{
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;  // "Ondsel"
    virtual std::string label() const = 0;  // translated, for the UI
    virtual Capabilities capabilities() const = 0;
    virtual std::unique_ptr<Session> createSession() = 0;
};

// The kinematic run of sec 4.6 on any session that solves positions: at each
// output time tStart + k * tOutput, every driver is held at its formula's value
// (a constant formula, printed to round-trip), the model is loaded and solved,
// starting from the previous frame's poses, and the poses go to the sink.
// Velocities and accelerations are not integrated; tolerances other than the
// output step are not used.
AssemblyExport Status simulateByPositionSolves(
    Session& session,
    const Model& model,
    const SimulationSpec& spec,
    TrajectorySink& sink
);

}  // namespace Assembly::Solver
