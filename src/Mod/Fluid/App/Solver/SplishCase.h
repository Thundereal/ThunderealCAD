// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "Backend.h"

// What the SPlisHSPlasH backend hands its runner, and what it reads back
// (docs/FluidSimulation.md sec 12 S3). Nothing here needs the solver: the
// model becomes particles and a few lines of text, the runner's answers are
// lines and raw points.
namespace Fluid::Solver::Splish
{

// The lengths a run is made of, all from the model's resolution.
struct Sizes
{
    double radius = 0.0;       // a particle's: half the resolution
    double behind = 0.0;       // how far behind a wall's face its particles lie
    double wallSpacing = 0.0;  // between a wall's particles
};
FluidExport Sizes sizesOf(double resolution);

// The walls that stay as particles: one sheet behind every face the fluid
// can reach, of the fixed parts and of the space's closed sides. A face is
// left out when the place a particle of fluid would rest against it is
// inside another fixed part or outside the space. A part the fluid moves
// covers nothing: it will not stay where it is.
FluidExport std::vector<Vec3> boundaryOf(const Model& model);
// The same, and whose each particle is: the wall's place in the model's
// walls, or -1 for one of the space's sides.
FluidExport std::vector<Vec3> boundaryOf(const Model& model, std::vector<int>& owners);

// An inlet's face as the particles of one layer coming in through it: a
// grid of the resolution in the face's plane, the places whose middles are
// on the face. The grid lies along the face's longest edge and is centred
// on the face, so a rectangle of whole spacings is filled exactly. across
// is the grid's first axis, a unit vector. None, and why, for a face that
// is not flat and square to the inlet's direction, or too small.
FluidExport std::vector<Vec3>
inletOf(const Model& model, std::size_t inlet, Vec3& across, std::string& why);

// A wall the fluid moves as particles, where it stands at the start: one
// sheet behind every face of it. None for a wall that is not moved.
FluidExport std::vector<Vec3> bodyOf(const Model& model, std::size_t wall);

// The fluid as particles at the start: a grid of the resolution in each
// volume, less what is inside a part, fixed or moved.
FluidExport std::vector<Vec3> particlesOf(const Model& model);

struct CaseOptions
{
    int threads = 1;
    // The pull between the fluid's particles that holds thrown water
    // together in sheets and drops (Akinci et al. 2013): the solver's
    // coefficient, no unit of the model's. 0, none: the validated runs.
    double cohesion = 0.0;
};

struct Case
{
    // name -> contents: case.txt, and the points as three floats each:
    // fluid.points, boundary.points, body.<k>.points for the k-th wall
    // the fluid moves, forced.<k>.points for the k-th wall a force is asked
    // of and inlet.<k>.points for the k-th inlet
    std::map<std::string, std::string> files;
    std::size_t particles = 0;
    std::size_t boundary = 0;
    std::vector<std::size_t> bodies;  // the moved walls' particles
    // the walls a force is asked of, as their places in the model's walls,
    // and their particles: forced.<k>.points, taken out of boundary.points
    std::vector<int> forcedWalls;
    std::vector<std::size_t> forced;
    std::vector<std::size_t> inlets;  // a layer's particles: inlet.<k>.points
};

// The case of a model; false and why when it cannot be one.
FluidExport bool
makeCase(const Model& model, const CaseOptions& options, Case& made, std::string& why);

// A line the runner wrote.
struct Event
{
    enum Kind
    {
        Other,    // the solver's own talk
        Version,  // text: the solver's version
        Frame,    // index, time, count: frame.<index> holds count points
        Sample,   // time, values: the monitors' in the case's order
        Body,     // index, values: where the index-th moved wall is at the
                  // frame that follows, its translation and its rotation's
                  // x, y, z, w (Model.h's Motion)
        Ask,      // time, values[0]: the runner waits for its driven
                  // walls' first states; the longest stretch it would take
        Pushed,   // time, values: the runner is at that time, past the
                  // stretch it was given; the mean force (3) and torque (3,
                  // about its centre) on each driven wall since it last said
        Energy,   // time, values: the fluid's kinetic and potential
                  // energy, its work on the walls it moves, and of driven
                  // walls what the lag made and what went over (Backend.h's
                  // Energy), J
        Error,    // text
    };
    Kind kind = Other;
    int index = 0;
    double time = 0.0;
    std::size_t count = 0;
    std::vector<double> values;
    std::string text;
};
FluidExport Event parseLine(const std::string& line);

// What the runner is told of its driven walls for a stretch ending at
// 'until': a line, each wall's centre (where 'centres' has it at the
// start, moved), its turn, its centre's velocity and its angular velocity.
FluidExport std::string moveLine(double until,
                                 const std::vector<Vec3>& centres,
                                 const std::vector<WallState>& states);

// count points out of a file's bytes; false when they are not all there.
FluidExport bool pointsOf(const std::string& bytes, std::size_t count, std::vector<Vec3>& points);

}  // namespace Fluid::Solver::Splish
