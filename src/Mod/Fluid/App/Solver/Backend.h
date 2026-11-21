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
    bool movingWalls = false;  // walls moved as given
    bool freeBodies = false;   // walls moved by the fluid
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
    // One per monitor of the model, in its order.
    std::vector<double> monitors;
};

// Where a run's frames go as they are made, and where it asks whether to
// stop. Called from the run's thread.
class FluidExport FrameSink
{
public:
    virtual ~FrameSink() = default;
    virtual void frame(const Frame& frame) = 0;
    virtual bool cancelled() const
    {
        return false;
    }
};

class FluidExport Session
{
public:
    virtual ~Session() = default;
    // Takes the model and makes ready to run it: the particles sampled, or
    // the case written and meshed. why says what is wrong when it is not Ok.
    virtual Status prepare(const Model& model, std::string& why) = 0;
    // Runs to the model's end time, a frame at 0 and at each output
    // interval, until the sink says to stop.
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
