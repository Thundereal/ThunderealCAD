// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Model.h"

using namespace Fluid::Solver;

namespace
{
// FNV-1a, 64 bit.
struct Hash
{
    std::uint64_t value = 14695981039346656037ULL;

    void bytes(const void* data, std::size_t size)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value = (value ^ p[i]) * 1099511628211ULL;
        }
    }
    void number(double v)
    {
        // -0.0 and 0.0 are the same model
        if (v == 0.0) {
            v = 0.0;
        }
        bytes(&v, sizeof(v));
    }
    void integer(std::int64_t v)
    {
        bytes(&v, sizeof(v));
    }
    void text(const std::string& v)
    {
        integer(static_cast<std::int64_t>(v.size()));
        bytes(v.data(), v.size());
    }
    void vec(const Vec3& v)
    {
        number(v[0]);
        number(v[1]);
        number(v[2]);
    }
    void surface(const Surface& s)
    {
        integer(static_cast<std::int64_t>(s.points.size()));
        for (const auto& p : s.points) {
            vec(p);
        }
        integer(static_cast<std::int64_t>(s.triangles.size()));
        for (const auto& t : s.triangles) {
            integer(t[0]);
            integer(t[1]);
            integer(t[2]);
        }
        for (int patch : s.patches) {
            integer(patch);
        }
    }
};

// Where the line through (x, y) along z crosses the surface, sorted. The
// line is taken a hair off (x, y): a grid's lines pass exactly through the
// diagonals a box's faces are split by, and a crossing on an edge would
// count for both triangles or neither.
std::vector<double> crossings(const Surface& surface, double x, double y, double hair)
{
    x += 0.381966011 * hair;
    y += 0.618033989 * hair;
    std::vector<double> at;
    for (const auto& t : surface.triangles) {
        const Vec3& a = surface.points[static_cast<std::size_t>(t[0])];
        const Vec3& b = surface.points[static_cast<std::size_t>(t[1])];
        const Vec3& c = surface.points[static_cast<std::size_t>(t[2])];
        const double d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (d == 0.0) {
            continue;  // seen edge on
        }
        const double u = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d;
        const double v = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d;
        const double w = 1.0 - u - v;
        if (u < 0.0 || v < 0.0 || w < 0.0) {
            continue;
        }
        at.push_back(u * a[2] + v * b[2] + w * c[2]);
    }
    std::ranges::sort(at);
    return at;
}

double hairOf(const Vec3& min, const Vec3& max)
{
    return 1.0e-7 * std::max({max[0] - min[0], max[1] - min[1], max[2] - min[2], 1.0e-9});
}
}  // namespace

std::uint64_t Fluid::Solver::signature(const Model& model)
{
    Hash hash;
    hash.integer(model.version);
    hash.integer(static_cast<std::int64_t>(model.walls.size()));
    for (const auto& wall : model.walls) {
        hash.text(wall.source);
        hash.surface(wall.surface);
        hash.integer(static_cast<int>(wall.motion));
        if (wall.motion == WallMotion::Free) {
            // To nine digits, the centre to a nanometre: they are worked
            // out from the shape, and a shape read back from its file
            // gives them a rounding apart from the one it was made as.
            const auto grained = [](double value, double grain) {
                return grain > 0.0 ? static_cast<std::int64_t>(std::llround(value / grain))
                                   : std::int64_t {0};
            };
            const MassProps& mass = wall.mass;
            const double digits = std::fabs(mass.mass) > 0.0
                ? std::pow(10.0, std::floor(std::log10(std::fabs(mass.mass))) - 8.0)
                : 0.0;
            hash.integer(grained(mass.mass, digits));
            hash.number(digits);
            for (double v : mass.centre) {
                hash.integer(grained(v, 1e-9));
            }
            const double most = std::max({std::fabs(mass.inertia[0]),
                                          std::fabs(mass.inertia[1]),
                                          std::fabs(mass.inertia[2])});
            for (double v : mass.inertia) {
                hash.integer(grained(v, 1e-9 * most));
            }
        }
    }
    hash.integer(static_cast<std::int64_t>(model.fluids.size()));
    for (const auto& fluid : model.fluids) {
        hash.text(fluid.source);
        hash.surface(fluid.surface);
        hash.number(fluid.material.density);
        hash.number(fluid.material.viscosity);
    }
    hash.vec(model.space.min);
    hash.vec(model.space.max);
    for (bool open : model.space.open) {
        hash.integer(open ? 1 : 0);
    }
    hash.vec(model.gravity);
    hash.number(model.timeEnd);
    hash.number(model.outputInterval);
    hash.number(model.resolution);
    hash.integer(static_cast<std::int64_t>(model.monitors.size()));
    for (const auto& monitor : model.monitors) {
        hash.text(monitor.source);
        hash.integer(static_cast<int>(monitor.kind));
        hash.vec(monitor.point);
    }
    return hash.value;
}

std::vector<std::size_t> Fluid::Solver::freeWalls(const Model& model)
{
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < model.walls.size(); ++i) {
        if (model.walls[i].motion == WallMotion::Free) {
            found.push_back(i);
        }
    }
    return found;
}

bool Fluid::Solver::boxOf(const Surface& surface, Vec3& min, Vec3& max)
{
    if (surface.points.empty()) {
        return false;
    }
    min = max = surface.points.front();
    for (const auto& p : surface.points) {
        for (std::size_t i = 0; i < 3; ++i) {
            min[i] = std::min(min[i], p[i]);
            max[i] = std::max(max[i], p[i]);
        }
    }
    return true;
}

bool Fluid::Solver::inside(const Surface& surface, const Vec3& point)
{
    Vec3 min;
    Vec3 max;
    if (!boxOf(surface, min, max)) {
        return false;
    }
    const auto at = crossings(surface, point[0], point[1], hairOf(min, max));
    const auto above = std::ranges::count_if(at, [&](double z) {
        return z > point[2];
    });
    return above % 2 == 1;
}

std::vector<Vec3> Fluid::Solver::sampled(const Surface& surface, double spacing)
{
    std::vector<Vec3> points;
    Vec3 min;
    Vec3 max;
    if (spacing <= 0.0 || !boxOf(surface, min, max)) {
        return points;
    }
    const double hair = hairOf(min, max);
    auto count = [&](std::size_t i) {
        return static_cast<long>(std::floor((max[i] - min[i]) / spacing + 0.5 + 1.0e-9));
    };
    const long nx = count(0);
    const long ny = count(1);
    for (long ix = 0; ix < nx; ++ix) {
        const double x = min[0] + (static_cast<double>(ix) + 0.5) * spacing;
        for (long iy = 0; iy < ny; ++iy) {
            const double y = min[1] + (static_cast<double>(iy) + 0.5) * spacing;
            const auto at = crossings(surface, x, y, hair);
            // inside between each pair of crossings, going up
            for (std::size_t k = 0; k + 1 < at.size(); k += 2) {
                const long from = static_cast<long>(
                    std::ceil((at[k] - min[2]) / spacing - 0.5 - 1.0e-9));
                const long to = static_cast<long>(
                    std::floor((at[k + 1] - min[2]) / spacing - 0.5 + 1.0e-9));
                for (long iz = std::max(from, 0L); iz <= to; ++iz) {
                    const double z = min[2] + (static_cast<double>(iz) + 0.5) * spacing;
                    if (z > at[k] && z < at[k + 1]) {
                        points.push_back({x, y, z});
                    }
                }
            }
        }
    }
    return points;
}
