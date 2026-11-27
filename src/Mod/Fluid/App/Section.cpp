// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "InsideCells.h"
#include "Section.h"

using namespace Fluid;

namespace
{
using Fluid::detail::Cubes;
using Fluid::detail::V3;

double dot(const V3& a, const V3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

V3 cross(const V3& a, const V3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

V3 scaled(const V3& a, double by)
{
    return {a[0] * by, a[1] * by, a[2] * by};
}

V3 added(const V3& a, const V3& b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

}  // namespace

Section Fluid::cut(const ArchivedFrame& frame,
                   const std::array<double, 3>& base,
                   const std::array<double, 3>& normal,
                   double step,
                   double reach)
{
    Section section;
    section.step = step;
    const double length = std::sqrt(dot(normal, normal));
    if (frame.inside.size() < 7 || !(length > 0.0) || !(step > 0.0) || !(reach > 0.0)) {
        return section;
    }
    const V3 n = scaled(normal, 1.0 / length);
    // two axes of the plane: the first across the model's axis the normal
    // is least along
    std::size_t least = 0;
    for (std::size_t i = 1; i < 3; ++i) {
        if (std::abs(n[i]) < std::abs(n[least])) {
            least = i;
        }
    }
    V3 axis {0.0, 0.0, 0.0};
    axis[least] = 1.0;
    V3 u = cross(n, axis);
    u = scaled(u, 1.0 / std::sqrt(dot(u, u)));
    const V3 v = cross(n, u);

    // Where the plane cuts the faces: a piece a triangle it passes through,
    // in the plane's two axes
    struct Piece
    {
        std::array<double, 2> from;
        std::array<double, 2> to;
    };
    std::vector<Piece> pieces;
    const auto corner = [&](std::int32_t at) {
        const auto i = static_cast<std::size_t>(at) * 3;
        return V3 {frame.points[i] - base[0],
                   frame.points[i + 1] - base[1],
                   frame.points[i + 2] - base[2]};
    };
    const std::size_t points = frame.points.size() / 3;
    for (std::size_t t = 0; t + 2 < frame.triangles.size(); t += 3) {
        std::array<V3, 3> p;
        std::array<double, 3> off {};
        bool whole = true;
        for (std::size_t k = 0; k < 3; ++k) {
            const std::int32_t at = frame.triangles[t + k];
            whole = whole && at >= 0 && static_cast<std::size_t>(at) < points;
            if (whole) {
                p[k] = corner(at);
                off[k] = dot(p[k], n);
            }
        }
        if (!whole) {
            continue;
        }
        // a corner on the plane counts as above it, so a face is cut once
        std::vector<std::array<double, 2>> ends;
        for (std::size_t k = 0; k < 3; ++k) {
            const std::size_t next = (k + 1) % 3;
            if ((off[k] >= 0.0) != (off[next] >= 0.0)) {
                const double along = off[k] / (off[k] - off[next]);
                const V3 where = added(p[k], scaled(added(p[next], scaled(p[k], -1.0)), along));
                ends.push_back({dot(where, u), dot(where, v)});
            }
        }
        if (ends.size() == 2) {
            pieces.push_back({ends[0], ends[1]});
        }
    }
    if (pieces.empty()) {
        return section;
    }
    std::array<double, 2> low {std::numeric_limits<double>::max(),
                               std::numeric_limits<double>::max()};
    std::array<double, 2> high {std::numeric_limits<double>::lowest(),
                                std::numeric_limits<double>::lowest()};
    for (const auto& piece : pieces) {
        for (const auto& end : {piece.from, piece.to}) {
            for (std::size_t k = 0; k < 2; ++k) {
                low[k] = std::min(low[k], end[k]);
                high[k] = std::max(high[k], end[k]);
            }
        }
        for (const auto& end : {piece.from, piece.to}) {
            const V3 where = added(V3 {base[0], base[1], base[2]},
                                   added(scaled(u, end[0]), scaled(v, end[1])));
            for (double w : where) {
                section.outline.push_back(static_cast<float>(w));
            }
        }
    }
    // The grid: its points on whole steps from the base, so that a cut moved
    // along itself keeps them
    const long firstA = std::lround(std::floor(low[0] / step));
    const long firstB = std::lround(std::floor(low[1] / step));
    const auto countA = static_cast<std::size_t>(std::lround(std::ceil(high[0] / step)) - firstA + 1);
    const auto countB = static_cast<std::size_t>(std::lround(std::ceil(high[1] / step)) - firstB + 1);
    if (countA * countB > 16000000) {
        return section;  // a step far too fine for the fluid's size
    }
    // What is inside: along each row of the grid, between an odd crossing
    // of the outline and the next
    std::vector<std::vector<double>> crossings(countB);
    for (const auto& piece : pieces) {
        const double b0 = std::min(piece.from[1], piece.to[1]);
        const double b1 = std::max(piece.from[1], piece.to[1]);
        if (!(b1 > b0)) {
            continue;
        }
        const long from = std::lround(std::ceil(b0 / step));
        const long to = std::lround(std::ceil(b1 / step));  // the row at b1 is the next piece's
        for (long row = std::max(from, firstB); row < to; ++row) {
            const auto at = static_cast<std::size_t>(row - firstB);
            if (at >= countB) {
                break;
            }
            const double b = static_cast<double>(row) * step;
            const double along = (b - piece.from[1]) / (piece.to[1] - piece.from[1]);
            crossings[at].push_back(piece.from[0] + along * (piece.to[0] - piece.from[0]));
        }
    }
    const Cubes cubes(frame.inside, reach);
    std::vector<std::int32_t> placed(countA * countB, -1);
    for (std::size_t j = 0; j < countB; ++j) {
        auto& row = crossings[j];
        std::ranges::sort(row);
        for (std::size_t k = 0; k + 1 < row.size(); k += 2) {
            const long from = std::lround(std::ceil(row[k] / step));
            const long to = std::lround(std::floor(row[k + 1] / step));
            for (long i = std::max(from, firstA); i <= to; ++i) {
                const auto at = static_cast<std::size_t>(i - firstA);
                if (at >= countA) {
                    break;
                }
                const V3 where =
                    added(V3 {base[0], base[1], base[2]},
                          added(scaled(u, static_cast<double>(i) * step),
                                scaled(v, static_cast<double>(firstB + static_cast<long>(j)) * step)));
                std::array<double, 4> value {};
                if (!cubes.valueAt(where, value)) {
                    continue;  // no cell near: the faces close in more than the cells fill
                }
                placed[j * countA + at] = static_cast<std::int32_t>(section.pressures.size());
                for (double w : where) {
                    section.points.push_back(static_cast<float>(w));
                }
                for (std::size_t k4 = 0; k4 < 3; ++k4) {
                    section.velocities.push_back(static_cast<float>(value[k4]));
                }
                section.pressures.push_back(static_cast<float>(value[3]));
                section.grid.push_back(static_cast<std::int32_t>(i));
                section.grid.push_back(
                    static_cast<std::int32_t>(firstB + static_cast<long>(j)));
            }
        }
    }
    for (std::size_t j = 0; j + 1 < countB; ++j) {
        for (std::size_t i = 0; i + 1 < countA; ++i) {
            const std::int32_t a = placed[j * countA + i];
            const std::int32_t b = placed[j * countA + i + 1];
            const std::int32_t c = placed[(j + 1) * countA + i + 1];
            const std::int32_t d = placed[(j + 1) * countA + i];
            if (a >= 0 && b >= 0 && c >= 0 && d >= 0) {
                section.triangles.insert(section.triangles.end(), {a, b, c, a, c, d});
            }
        }
    }
    return section;
}
