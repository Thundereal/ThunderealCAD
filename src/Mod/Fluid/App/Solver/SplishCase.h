// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "Model.h"

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
    // fluid.points, boundary.points, and body.<k>.points for the k-th wall
    // the fluid moves
    std::map<std::string, std::string> files;
    std::size_t particles = 0;
    std::size_t boundary = 0;
    std::vector<std::size_t> bodies;  // the moved walls' particles
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

// count points out of a file's bytes; false when they are not all there.
FluidExport bool pointsOf(const std::string& bytes, std::size_t count, std::vector<Vec3>& points);

}  // namespace Fluid::Solver::Splish
