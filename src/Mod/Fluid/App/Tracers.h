// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "ResultsArchive.h"

// What the fluid of a frame would carry (docs/FluidSimulation.md sec 14.7):
// points let go in a steady flow and followed through it, each where it is
// at even times -- so that they can be played, the fast parts of the flow
// seen fast and the slow ones slow.
namespace Fluid
{

struct Paths
{
    // Where each tracer is at each time, mm, x y z a point; one path after
    // another.
    std::vector<float> points;
    // How fast the fluid goes at each of them, m/s.
    std::vector<float> speeds;
    // Where each path starts in the points, and after the last where they
    // end: one more than there are paths.
    std::vector<std::int32_t> starts;
    // The time from a point of a path to its next, s.
    double interval = 0.0;
};

// Follows a tracer from each seed (x y z each, mm) through the frame's
// fluid, a point each 'interval' (s), 'most' points a path at most. With
// no interval given (0), one that takes the fastest of the fluid half of
// 'reach' a step. 'reach' is how far about a point the cells are asked,
// mm: the solver's cell is the reach to give. A path ends where the tracer
// leaves the fluid -- no cell is near -- or stands still. Empty when the
// frame keeps no fluid inside or nothing in it moves.
FluidExport Paths trace(const ArchivedFrame& frame,
                        const std::vector<float>& seeds,
                        double reach,
                        double interval,
                        std::size_t most);

}  // namespace Fluid
