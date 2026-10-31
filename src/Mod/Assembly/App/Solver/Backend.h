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
    // Unused: the bodies Fixed joints hold together are one body of the model
    // already (groupedRigidly, sec 6.21).
    bool bundleFixed = true;
};

// What a simulation computes (docs/AssemblyDynamics.md sec 6.2): the
// motions' kinematics, the forward dynamics of the bodies' masses under
// gravity, or the static equilibrium at each output time (the model's
// dynamics block, which Dynamic and Static need)
enum class Analysis : std::uint8_t
{
    Kinematic,
    Dynamic,
    Static
};

struct SimulationSpec
{
    Analysis analysis = Analysis::Kinematic;
    // Dynamic: the integrator's largest step (s); 0, the backend's choice
    double maxStep = 0.0;
    // Dynamic: the coefficient of restitution at the limits, rigid stops
    double restitution = 0.0;
    double tStart = 0.0;
    double tEnd = 1.0;
    double tOutput = 1e-2;
    double errorTolerance = 1e-6;
    double hMin = 1e-9;
    double hMax = 1.0;
    // Kinematic: also the reactions and the motors' efforts the motions take
    // (inverse dynamics, docs/AssemblyDynamics.md sec 6.3), from the model's
    // dynamics block; a backend that runs no dynamics ignores it
    bool reactions = false;
};

// A value a run reports with each frame besides the poses
// (docs/AssemblyDynamics.md sec 8.4), in the model's units (mm, mN, mN mm)
// and, a vector's components, in the model's axes:
//   "Force", "Torque"  a joint's reaction on the body of its frame I (its
//                      first reference's part), the torque about frame I's
//                      origin; width 3
//   "MotorTorque"      the torque a rotation motor applies about its axis
//   "MotorForce"       the force a linear motor applies along its axis
//   "SpringForce"      a spring-damper's force, positive pushing its ends apart
//   "SpringLength"     its length
//   "SpringTorque"     a torsion spring's torque, positive turning its joint up
//   "SpringAngle"      its joint's turn, rad, as the joint's limits read it
//   "BushingForce", "BushingTorque"  a bushing's on the body of its frame I
//                      (its component), the torque about frame I's origin;
//                      width 3
//   "ContactForce"     a contact set's force on its group A's bodies; width 3
//   "ContactNormalForce", "ContactFrictionForce"  its parts along the
//                      contact points' normals and across them; width 3
//   "ContactPenetration"  the deepest of its points, 0 apart
//   "FrictionForce", "FrictionTorque"  a joint's friction on the body of its
//                      frame I, along and about the joint's axis
// 'unique' is false for a reaction a redundant joint shares with others (sec
// 6.1): the run took out the rows that repeat others, and what it reports is
// one split of the load among the joints, not the only one.
struct Channel
{
    std::string source;  // the model's source name: the joint's, the spring's
    std::string quantity;
    int width = 1;
    bool unique = true;
};

// Something a run found between its frames (docs/AssemblyDynamics.md sec
// 8.4's events): a sensor (Model.h) tripping, at the time located, and the
// value it read there; 'stopped' when the run ended there, its last frame
// at that time
struct Event
{
    double time = 0.0;
    std::string source;  // the model's source name of the sensor
    double value = 0.0;
    bool stopped = false;
};

// Poses of the bodies at each output time; owned by the neutral layer, so
// playback needs no live solver.
struct Trajectory
{
    std::vector<Id> bodies;
    std::vector<double> times;
    std::vector<Base::Placement> poses;  // times.size() * bodies.size(), row per time
    std::vector<Channel> channels;
    std::vector<double> values;  // times.size() * the channels' widths, row per time
    std::vector<Event> events;   // in time order
};

class TrajectorySink
{
public:
    virtual ~TrajectorySink() = default;
    // false: cancel the run
    virtual bool frame(double time, const std::vector<BodyPose>& poses) = 0;
    // Once, before the first frame, from a run that reports values: their
    // channels
    virtual void channels(const std::vector<Channel>& channels)
    {
        (void)channels;
    }
    // Before each frame: its values, the channels' components in order
    virtual void values(const std::vector<double>& values)
    {
        (void)values;
    }
    // An event, before the frame that follows it
    virtual void event(const Event& event)
    {
        (void)event;
    }
};

// What centredOnJoints gives each body: the centre of its joints in the
// body's frame, and its inertia; or, for a body whose MassProps are 'sized'
// (a group), the centre, inertia and mass given. 'sized' is false for a
// grounded body and a body without a joint frame, which keep their frame and
// an inertia of 1.
// 'body' is the body the solve moves it with: itself, or the first body of
// the group groupedRigidly put it in.
struct BodyMetric
{
    bool sized = false;
    Base::Vector3d centre;
    double inertia = 1.0;
    Id body = NoId;
    double mass = 1.0;  // a group weighs as many bodies as it has
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
    // Puts the bodies at these poses (the entry of body i has body == i;
    // bodies not given stay), without a solve: a drag step taken back, as
    // when it would make two parts collide. The poses are the session's own,
    // from poses(), so they satisfy the joints; grounded bodies keep theirs.
    virtual Status setPoses(const std::vector<BodyPose>& poses)
    {
        (void)poses;
        return Status::Unsupported;
    }

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
    // How the solve weighs each body of the model it was given (sec 6.20);
    // empty when the session does not say.
    virtual std::vector<BodyMetric> bodyMetric() const
    {
        return {};
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

// A session on another whose bodies are moved, in the model it is given, to a
// frame at the centre of their joints (sec 6.20): the centre of the box of the
// origins of the joint frames on the body, the body's axes kept; and whose
// bodies are given a size, an inertia of 4 r^2 for r half that box's diagonal
// (at least 1 mm). Every backend solves for the pose closest to the start;
// the distance is measured at the body's frame, and a turn is weighed by the
// inertia, so that turning a body costs as much as moving its joints along
// their arcs. Measured at a frame far from where the body is held, a turn
// about a free joint axis moved that frame, and the solve traded such turns
// against the distance -- in a chain of revolutes held at their parts'
// centres, a small edit had every part turn to keep its corner near; and
// weighed without a size, a turn of a radian cost what moving half a
// millimetre does, so the steps turned bodies through radians. Poses come
// out, and drag targets go in, in the bodies' own frames; grounded bodies and
// bodies without a joint frame are left as they are.
AssemblyExport std::unique_ptr<Session> centredOnJoints(std::unique_ptr<Session> session);

AssemblyExport std::vector<BodyMetric> jointMetric(const Model& model);

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
