// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "ResultsArchive.h"

// A flat cut through a frame's fluid (docs/FluidSimulation.md sec 14.5):
// what the flow is doing inside, where the faces show only what it does to
// them.
namespace Fluid
{

struct Section
{
    // A grid of points on the plane, those inside the fluid: mm, x y z each.
    std::vector<float> points;
    // Three a triangle, into the points: the squares of the grid whose four
    // corners are all inside.
    std::vector<std::int32_t> triangles;
    std::vector<float> velocities;  // m/s, x y z a point
    std::vector<float> pressures;   // Pa, one a point
    // Where on the grid each point is: two a point, how many steps along
    // the plane's two axes. To take every few of them, as for arrows.
    std::vector<std::int32_t> grid;
    // Where the plane cuts the faces the fluid touches: six a piece, its
    // two ends, mm.
    std::vector<float> outline;
    double step = 0.0;  // between the grid's points, mm
};

// Cuts the frame's fluid by the plane through 'base' (mm) across 'normal'.
// The grid's points are 'step' apart, mm; the values at each are of the
// cells' centres within 'reach' of it, mm, the nearer the more -- the
// solver's cell is the reach to give. What is inside is told by the
// frame's own faces, which close the fluid in. Empty when the frame has no
// fluid inside kept, the plane misses it, or the normal has no length.
FluidExport Section cut(const ArchivedFrame& frame,
                        const std::array<double, 3>& base,
                        const std::array<double, 3>& normal,
                        double step,
                        double reach);

}  // namespace Fluid
