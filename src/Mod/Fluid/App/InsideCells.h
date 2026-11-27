// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

// The fluid inside a frame (ArchivedFrame::inside: a cell a row, its centre
// in mm, its velocity in m/s, its pressure in Pa) as something to ask the
// flow at a point of. Shared by the cut (Section.cpp) and the tracers
// (Tracers.cpp); not part of the module's interface.
namespace Fluid::detail
{
using V3 = std::array<double, 3>;

// The cells' centres in cubes of a side, to find those about a point.
class Cubes
{
public:
    Cubes(const std::vector<float>& inside, double side)
        : inside(inside)
        , side(side)
    {
        const std::size_t count = inside.size() / 7;
        cells.reserve(count);
        for (std::size_t c = 0; c < count; ++c) {
            cells[keyOf(at(c))].push_back(static_cast<std::int32_t>(c));
        }
    }

    V3 at(std::size_t c) const
    {
        return {inside[c * 7], inside[c * 7 + 1], inside[c * 7 + 2]};
    }

    // Each cell in the cubes about the point: its place in the kept rows
    // and how far its centre is.
    template<typename Each>
    void about(const V3& p, Each&& each) const
    {
        const auto home = cubeOf(p);
        for (long i = home[0] - 1; i <= home[0] + 1; ++i) {
            for (long j = home[1] - 1; j <= home[1] + 1; ++j) {
                for (long k = home[2] - 1; k <= home[2] + 1; ++k) {
                    const auto found = cells.find(keyOf(std::array<long, 3> {i, j, k}));
                    if (found == cells.end()) {
                        continue;
                    }
                    for (std::int32_t c : found->second) {
                        const V3 centre = at(static_cast<std::size_t>(c));
                        const V3 off {centre[0] - p[0], centre[1] - p[1], centre[2] - p[2]};
                        each(static_cast<std::size_t>(c),
                             std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]));
                    }
                }
            }
        }
    }

private:
    std::array<long, 3> cubeOf(const V3& p) const
    {
        return {std::lround(std::floor(p[0] / side)),
                std::lround(std::floor(p[1] / side)),
                std::lround(std::floor(p[2] / side))};
    }

    static std::uint64_t keyOf(const std::array<long, 3>& cube)
    {
        const auto part = [](long v) {
            return static_cast<std::uint64_t>(v + (1L << 20)) & 0x1FFFFFU;
        };
        return (part(cube[0]) << 42U) | (part(cube[1]) << 21U) | part(cube[2]);
    }

    std::uint64_t keyOf(const V3& p) const
    {
        return keyOf(cubeOf(p));
    }

    const std::vector<float>& inside;
    double side;

public:
    // The flow at a point: its velocity (m/s) and its pressure (Pa), of
    // the cells' centres within a side and a half of it, the nearer the
    // more. A plane is fitted through them -- a flow that changes evenly
    // from cell to cell is given exactly -- and what it says is held
    // within what those cells have, so that nothing is made up past a
    // wall. With too few cells for a fit, their mean by nearness; with none
    // that near, the nearest in the cubes about. false when no cell is in
    // the cubes about the point: it is out of the fluid.
    bool valueAt(const V3& where, std::array<double, 4>& value) const
    {
        // one side and a half: some fourteen cells of a mesh of that side,
        // where one side has four, too few to fit a plane through
        const double span = 1.5 * side;
        double weights = 0.0;
        std::array<double, 4> sum {};
        // the fit's sums: over (1, dx, dy, dz), the offsets in sides
        std::array<std::array<double, 4>, 4> normal {};
        std::array<std::array<double, 4>, 4> right {};  // a row a value
        std::array<double, 4> least {};
        std::array<double, 4> most {};
        least.fill(std::numeric_limits<double>::max());
        most.fill(std::numeric_limits<double>::lowest());
        double nearest = std::numeric_limits<double>::max();
        std::size_t nearestCell = 0;
        bool on = false;
        about(where, [&](std::size_t c, double away) {
            if (on) {
                return;
            }
            if (away < nearest) {
                nearest = away;
                nearestCell = c;
            }
            if (away < side * 1e-6) {
                on = true;
                return;
            }
            if (away < span) {
                const double weight = std::pow((span - away) / (span * away), 2);
                weights += weight;
                const V3 centre = at(c);
                const std::array<double, 4> term {1.0,
                                                  (centre[0] - where[0]) / side,
                                                  (centre[1] - where[1]) / side,
                                                  (centre[2] - where[2]) / side};
                for (std::size_t i = 0; i < 4; ++i) {
                    for (std::size_t j = 0; j < 4; ++j) {
                        normal[i][j] += weight * term[i] * term[j];
                    }
                }
                for (std::size_t k = 0; k < 4; ++k) {
                    const double has = inside[c * 7 + 3 + k];
                    sum[k] += weight * has;
                    least[k] = std::min(least[k], has);
                    most[k] = std::max(most[k], has);
                    for (std::size_t i = 0; i < 4; ++i) {
                        right[k][i] += weight * term[i] * has;
                    }
                }
            }
        });
        if (nearest == std::numeric_limits<double>::max()) {
            return false;
        }
        if (on || !(weights > 0.0)) {
            for (std::size_t k = 0; k < 4; ++k) {
                value[k] = inside[nearestCell * 7 + 3 + k];
            }
            return true;
        }
        for (std::size_t k = 0; k < 4; ++k) {
            value[k] = sum[k] / weights;
        }
        // The fit, by elimination; cells all in one plane, or too few, have
        // no fit and keep the mean
        for (std::size_t i = 0; i < 4; ++i) {
            std::size_t pivot = i;
            for (std::size_t r = i + 1; r < 4; ++r) {
                if (std::abs(normal[r][i]) > std::abs(normal[pivot][i])) {
                    pivot = r;
                }
            }
            if (!(std::abs(normal[pivot][i]) > 1e-9 * weights)) {
                return true;
            }
            std::swap(normal[i], normal[pivot]);
            for (auto& row : right) {
                std::swap(row[i], row[pivot]);
            }
            for (std::size_t r = i + 1; r < 4; ++r) {
                const double by = normal[r][i] / normal[i][i];
                for (std::size_t j = i; j < 4; ++j) {
                    normal[r][j] -= by * normal[i][j];
                }
                for (auto& row : right) {
                    row[r] -= by * row[i];
                }
            }
        }
        for (std::size_t k = 0; k < 4; ++k) {
            // only the first unknown is asked: the value at the point
            std::array<double, 4> solved {};
            for (std::size_t back = 4; back-- > 0;) {
                double rest = right[k][back];
                for (std::size_t j = back + 1; j < 4; ++j) {
                    rest -= normal[back][j] * solved[j];
                }
                solved[back] = rest / normal[back][back];
            }
            value[k] = std::clamp(solved[0], least[k], most[k]);
        }
        return true;
    }

private:
    std::unordered_map<std::uint64_t, std::vector<std::int32_t>> cells;
};

}  // namespace Fluid::detail
