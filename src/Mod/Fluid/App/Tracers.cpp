// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cmath>

#include "InsideCells.h"
#include "Tracers.h"

using namespace Fluid;
using Fluid::detail::Cubes;
using Fluid::detail::V3;

namespace
{
double speedOf(const std::array<double, 4>& value)
{
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}
}  // namespace

Paths Fluid::trace(const ArchivedFrame& frame,
                   const std::vector<float>& seeds,
                   double reach,
                   double interval,
                   std::size_t most)
{
    Paths paths;
    if (frame.inside.size() < 7 || !(reach > 0.0) || most == 0) {
        return paths;
    }
    double fastest = 0.0;
    for (std::size_t c = 0; c + 6 < frame.inside.size(); c += 7) {
        const double u = frame.inside[c + 3];
        const double v = frame.inside[c + 4];
        const double w = frame.inside[c + 5];
        fastest = std::max(fastest, std::sqrt(u * u + v * v + w * w));
    }
    if (!(fastest > 0.0)) {
        return paths;
    }
    if (!(interval > 0.0)) {
        interval = 0.5 * reach / (fastest * 1000.0);
    }
    paths.interval = interval;
    const Cubes cubes(frame.inside, reach);
    // m/s over a step, in mm
    const double step = interval * 1000.0;
    const double still = fastest * 1.0e-6;
    paths.starts.push_back(0);
    for (std::size_t s = 0; s + 2 < seeds.size(); s += 3) {
        V3 at {seeds[s], seeds[s + 1], seeds[s + 2]};
        for (std::size_t count = 0; count < most; ++count) {
            std::array<double, 4> here {};
            if (!cubes.valueAt(at, here)) {
                break;
            }
            const double speed = speedOf(here);
            for (double w : at) {
                paths.points.push_back(static_cast<float>(w));
            }
            paths.speeds.push_back(static_cast<float>(speed));
            if (speed < still) {
                break;
            }
            // by the flow half a step on: a turn is followed, not cut
            const V3 half {at[0] + 0.5 * step * here[0],
                           at[1] + 0.5 * step * here[1],
                           at[2] + 0.5 * step * here[2]};
            std::array<double, 4> there {};
            if (!cubes.valueAt(half, there)) {
                there = here;
            }
            at = {at[0] + step * there[0], at[1] + step * there[1], at[2] + step * there[2]};
        }
        paths.starts.push_back(static_cast<std::int32_t>(paths.speeds.size()));
    }
    return paths;
}
