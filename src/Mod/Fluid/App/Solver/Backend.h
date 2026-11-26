// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "Model.h"

// What a fluid solver is to the study (docs/FluidSimulation.md secs 11.2 to
// 11.4). Every argument and result is plain data, so a session can be served
// from another process.
namespace Fluid::Solver
{

enum class Status
{
    Ok = 0,
    Failed = -1,       // the solver stopped with an error
    Unsupported = -2,  // the backend cannot run this model
    NotFound = -3,     // what the backend needs is not on this machine
    Empty = -4,        // nothing to simulate: no fluid, or no space
    Cancelled = -7,
};

// What a backend's frames are, and so how they are drawn (sec 11.4).
enum class ResultKind
{
    Particles,  // points
    Surface,    // the free surface as triangles
    Fields,     // values on a mesh (not yet)
};

struct Capabilities
{
    ResultKind results = ResultKind::Particles;
    bool freeSurface = false;
    bool inlets = false;
    bool forces = false;       // the fluid's force on a wall, as a monitor
    bool movingWalls = false;  // walls moved as given
    bool freeBodies = false;   // walls moved by the fluid
    bool drivenWalls = false;  // walls moved from outside as the run goes
    bool heat = false;
    bool steady = false;
    bool repeatable = false;  // the same model gives the same frames, bit for bit
    bool inProcess = true;    // a library stepped here, or a program run over files
    // Test data, not a simulation: never offered for a study, only named.
    bool testData = false;
};

// Whether the backend can run on this machine now, and why not.
struct Availability
{
    bool available = true;
    std::string reason;   // why not, for the panel: what was looked for
    std::string version;  // of the program or library found
};

// One output frame, in the model's units.
struct Frame
{
    double time = 0.0;
    // Particles: the particles. Surface: the triangles' corners.
    std::vector<Vec3> points;
    // Surface only.
    std::vector<std::array<int, 3>> triangles;
    // Where the walls the fluid moves are: one per such wall, in the
    // model's order (freeWalls).
    std::vector<Motion> bodies;
};

// The fluid's energy at a time, J (docs/FluidSimulation.md sec 12.11): a
// run's own account of itself, for whoever asks whether to believe it.
// Kinetic and potential are the fluid's, the potential above the lowest
// point of its space; work is what the fluid has done so far on the walls
// it moves. The three together cannot rise in a run nothing feeds. Of a
// run with a mechanism, where a wall goes by the fluid's push of the
// stretch before: created is the work so far of that push less the push
// the fluid then gave, along the way the wall went -- energy of nothing
// but the lag -- and exchanged is how much went between the two in all,
// each stretch's taken without its sign.
struct Energy
{
    double time = 0.0;
    double kinetic = 0.0;
    double potential = 0.0;
    double work = 0.0;
    double created = 0.0;
    double exchanged = 0.0;
};

// Where a run's frames and its monitors' values go as they are made, and
// where it asks whether to stop. Called from the run's thread.
class FluidExport FrameSink
{
public:
    virtual ~FrameSink() = default;
    virtual void frame(const Frame& frame) = 0;
    // The monitors at a time, one value per monitor of the model in its
    // order, NaN for one that has none. On a time base of their own, in
    // increasing time: a solver reports them as often as it steps, which is
    // far oftener than it makes frames -- an impact's pressure peak lasts
    // milliseconds.
    virtual void sample(double time, const std::vector<double>& values) = 0;
    // The fluid's energy, from a solver that keeps account of it: with
    // each frame.
    virtual void energy(const Energy& /*energy*/)
    {}
    virtual bool cancelled() const
    {
        return false;
    }
};

// A driven wall as its mover has it: where it is from where it stood at
// the start, how fast its centre goes (the wall's mass centre, Wall::mass),
// and its angular velocity; m, m/s, rad/s, in the model's axes.
struct WallState
{
    Motion motion;
    Vec3 velocity {0.0, 0.0, 0.0};
    Vec3 spin {0.0, 0.0, 0.0};
};

// What the fluid pushes it with: a force at that centre where it is, and a
// torque; N, N m.
struct WallLoad
{
    Vec3 force {0.0, 0.0, 0.0};
    Vec3 torque {0.0, 0.0, 0.0};
};

// Who moves the driven walls of a run (docs/FluidSimulation.md sec 12.9),
// as the session sees it. Called from the run's thread, in this order:
// ready() once; then next() for a stretch of time and the walls' states at
// its start, and pushed() with the fluid's mean load on each over what it
// took to pass the stretch's end; next() again, until it says there is no
// more.
class FluidExport WallDriver
{
public:
    virtual ~WallDriver() = default;
    // The fluid is set up: the longest stretch it would be asked for, s.
    virtual void ready(double longest) = 0;
    // Waits for the next stretch: from 'time', 'length' long, the walls'
    // states at its start, one per driven wall in the model's order. false:
    // no more, the run ends here.
    virtual bool next(double& time, double& length, std::vector<WallState>& states) = 0;
    virtual void pushed(const std::vector<WallLoad>& loads) = 0;
    // The fluid's run is over, or failed: nothing more comes of it.
    virtual void ended(const std::string& why) = 0;
};

class FluidExport Session
{
public:
    virtual ~Session() = default;
    // Who moves the model's driven walls: before run(), and outliving it.
    // A session that drives none ignores it.
    virtual void drive(WallDriver* driver)
    {
        (void)driver;
    }
    // Takes the model and makes ready to run it: the particles sampled, or
    // the case written and meshed. why says what is wrong when it is not Ok.
    virtual Status prepare(const Model& model, std::string& why) = 0;
    // Runs to the model's end time, a frame at 0 and at each output
    // interval and the monitors at least as often, until the sink says to
    // stop.
    virtual Status run(FrameSink& sink, std::string& why) = 0;
};

class FluidExport Backend
{
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;   // what a study's Backend property holds
    virtual std::string label() const = 0;  // what the panel shows
    virtual Capabilities capabilities() const = 0;
    virtual Availability availability() const
    {
        return {};
    }
    virtual std::unique_ptr<Session> createSession() const = 0;
};

}  // namespace Fluid::Solver
