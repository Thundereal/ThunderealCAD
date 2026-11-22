// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "ParticleSurface.h"

using namespace Fluid;
using Fluid::Solver::Vec3;

namespace
{

// How far a particle is felt, in spacings, and the level the surface is
// at. The field is the particles' weights added up, scaled to 1 deep
// inside particles on a grid of the spacing; against a flat face of them
// it is then a half exactly at the fluid's edge, whatever the reach.
//
// The reach is long: a moving fluid's particles are on no grid, and a
// short reach shows each of them as grain on the surface and holes under
// it. A particle alone never rises over the level with it (to 0.2), nor
// do a few in a strand: the splash would be gone.
constexpr double Reach = 2.0;
constexpr double Level = 0.5;
// So the particles the long field leaves outside the surface -- the field
// at their own place is under the level -- add a second field to it, the
// drops': a particle alone is a ball of the DropRadius in it, and two
// nearer than about a spacing and a half run together, as drops do. A strand of
// particles is a thread of water, a sheet of them a film (what the
// meshers of the film trade do with a particle's radius and a pass of
// smoothing; it is what makes a splash look wet, where a ball a particle
// looks like hail). Only those particles: the body's surface stays the
// long field's, with no grain from the particles under it. And every
// particle is inside the surface, in the body or in a drop.
constexpr double DropReach = 1.2;
// in spacings. The ball of a particle's own volume is 0.62; so large, a
// splash of a few thousand of them hides what it falls on, and looks
// coarser than the water it came from.
constexpr double DropRadius = 0.45;
// grid cells a spacing
constexpr int Fine = 2;

// The weight at a distance, its square over the reach's square given.
double weight(double q2)
{
    const double left = 1.0 - q2;
    return left > 0.0 ? left * left * left : 0.0;
}

struct Grid
{
    std::array<long, 3> n {};  // nodes each way
    Vec3 origin {};
    double h = 0.0;
    std::vector<float> value;

    // The field at a place, between the nodes around it; 0 outside.
    double valueAt(const Vec3& p) const
    {
        std::array<long, 3> c {};
        std::array<double, 3> t {};
        for (std::size_t k = 0; k < 3; ++k) {
            const double at = (p[k] - origin[k]) / h;
            c[k] = static_cast<long>(std::floor(at));
            if (c[k] < 0 || c[k] + 1 >= n[k]) {
                return 0.0;
            }
            t[k] = at - static_cast<double>(c[k]);
        }
        double sum = 0.0;
        for (int corner = 0; corner < 8; ++corner) {
            const long i = corner & 1;
            const long j = (corner >> 1) & 1;
            const long k = (corner >> 2) & 1;
            const double w = (i != 0 ? t[0] : 1.0 - t[0]) * (j != 0 ? t[1] : 1.0 - t[1])
                * (k != 0 ? t[2] : 1.0 - t[2]);
            sum += w * value[this->at(c[0] + i, c[1] + j, c[2] + k)];
        }
        return sum;
    }

    std::size_t at(long i, long j, long k) const
    {
        return static_cast<std::size_t>((k * n[1] + j) * n[0] + i);
    }
    Vec3 place(long i, long j, long k) const
    {
        return {origin[0] + static_cast<double>(i) * h,
                origin[1] + static_cast<double>(j) * h,
                origin[2] + static_cast<double>(k) * h};
    }
};

}  // namespace

void Fluid::surfaceOf(const std::vector<Vec3>& particles,
                      double spacing,
                      ParticleSurface& surface,
                      double sprayBelow)
{
    surface = ParticleSurface();
    if (particles.empty() || !(spacing > 0.0)) {
        return;
    }
    const double reach = Reach * spacing;
    Grid grid;
    grid.h = spacing / Fine;
    Vec3 low = particles.front();
    Vec3 high = particles.front();
    for (const Vec3& p : particles) {
        for (std::size_t k = 0; k < 3; ++k) {
            low[k] = std::min(low[k], p[k]);
            high[k] = std::max(high[k], p[k]);
        }
    }
    // a reach and a cell around the particles: the field is nothing at the
    // grid's rim, and the surface closes inside it
    for (std::size_t k = 0; k < 3; ++k) {
        grid.origin[k] = low[k] - reach - grid.h;
        grid.n[k] = static_cast<long>(std::ceil((high[k] - low[k] + 2 * (reach + grid.h)) / grid.h))
            + 1;
    }
    grid.value.assign(static_cast<std::size_t>(grid.n[0] * grid.n[1] * grid.n[2]), 0.0F);

    // 1 deep inside a grid of the spacing: the weights' sum over all space,
    // a particle a spacing's cube
    const double pi = 3.14159265358979323846;
    const double whole = 4.0 * pi * 16.0 / 315.0;
    const double scale = spacing * spacing * spacing / (reach * reach * reach * whole);
    // a particle's weights onto the nodes within a reach of it
    const auto splat = [&](const Vec3& p, double within, double times) {
        const long span = static_cast<long>(std::ceil(within / grid.h));
        const std::array<long, 3> c {
            static_cast<long>(std::floor((p[0] - grid.origin[0]) / grid.h)),
            static_cast<long>(std::floor((p[1] - grid.origin[1]) / grid.h)),
            static_cast<long>(std::floor((p[2] - grid.origin[2]) / grid.h))};
        for (long k = std::max(c[2] - span, 0L); k <= std::min(c[2] + span + 1, grid.n[2] - 1); ++k) {
            const double dz = grid.origin[2] + static_cast<double>(k) * grid.h - p[2];
            for (long j = std::max(c[1] - span, 0L); j <= std::min(c[1] + span + 1, grid.n[1] - 1);
                 ++j) {
                const double dy = grid.origin[1] + static_cast<double>(j) * grid.h - p[1];
                for (long i = std::max(c[0] - span, 0L);
                     i <= std::min(c[0] + span + 1, grid.n[0] - 1);
                     ++i) {
                    const double dx = grid.origin[0] + static_cast<double>(i) * grid.h - p[0];
                    const double w = weight((dx * dx + dy * dy + dz * dz) / (within * within));
                    if (w > 0.0) {
                        grid.value[grid.at(i, j, k)] += static_cast<float>(times * w);
                    }
                }
            }
        }
    };
    for (const Vec3& p : particles) {
        splat(p, reach, scale);
    }
    // the drops: found all before any is added, by the long field alone
    std::vector<std::size_t> drops;
    for (std::size_t i = 0; i < particles.size(); ++i) {
        const double here = grid.valueAt(particles[i]);
        if (here < sprayBelow * scale) {
            surface.spray.push_back(particles[i]);
        }
        else if (here < Level) {
            drops.push_back(i);
        }
    }
    // the two fields together at the level, at the DropRadius from a
    // particle alone
    const double dropScale = (Level - scale * weight(DropRadius * DropRadius / (Reach * Reach)))
        / weight(DropRadius * DropRadius / (DropReach * DropReach));
    for (std::size_t i : drops) {
        splat(particles[i], DropReach * spacing, dropScale);
    }

    // The field's slope at a node, by its neighbours either side.
    const auto slopeAt = [&](std::size_t node) {
        const long plane = grid.n[0] * grid.n[1];
        const long at = static_cast<long>(node);
        const std::array<long, 3> c {at % grid.n[0], (at / grid.n[0]) % grid.n[1], at / plane};
        const std::array<long, 3> step {1, grid.n[0], plane};
        Vec3 slope {};
        for (std::size_t x = 0; x < 3; ++x) {
            const long before = c[x] > 0 ? at - step[x] : at;
            const long after = c[x] + 1 < grid.n[x] ? at + step[x] : at;
            slope[x] = static_cast<double>(grid.value[static_cast<std::size_t>(after)])
                - static_cast<double>(grid.value[static_cast<std::size_t>(before)]);
        }
        return slope;
    };

    // The surface through the grid's cells, each cut into six tetrahedra
    // about its diagonal: the same cut in every cell, so neighbours agree
    // on the face between them and the surface has no gap. A point of it
    // is on a line between two nodes, and is one point for every
    // tetrahedron on that line.
    std::unordered_map<std::uint64_t, int> made;
    const auto nodes = static_cast<std::uint64_t>(grid.value.size());
    const auto pointOn = [&](std::size_t a, const Vec3& pa, float va,
                             std::size_t b, const Vec3& pb, float vb) {
        const std::uint64_t key = static_cast<std::uint64_t>(std::min(a, b)) * nodes
            + static_cast<std::uint64_t>(std::max(a, b));
        const auto found = made.find(key);
        if (found != made.end()) {
            return found->second;
        }
        // Where the field crosses the level between the two nodes. Not by
        // a straight line between their values: a drop is a node or two
        // across, the field over it far from straight, and its ball came
        // out a quarter too small and angular. By the curve through the
        // two values with the field's slopes there.
        const Vec3 sa = slopeAt(a);
        const Vec3 sb = slopeAt(b);
        const double h = grid.h;
        // the slopes are differences two cells apart; along the line they
        // are these a unit of the way
        const Vec3 along {(pb[0] - pa[0]) / (2.0 * h), (pb[1] - pa[1]) / (2.0 * h),
                          (pb[2] - pa[2]) / (2.0 * h)};
        const double ma = sa[0] * along[0] + sa[1] * along[1] + sa[2] * along[2];
        const double mb = sb[0] * along[0] + sb[1] * along[1] + sb[2] * along[2];
        const auto curve = [&](double u) {
            const double u2 = u * u;
            const double u3 = u2 * u;
            return (2 * u3 - 3 * u2 + 1) * va + (u3 - 2 * u2 + u) * ma + (-2 * u3 + 3 * u2) * vb
                + (u3 - u2) * mb;
        };
        double low = 0.0;
        double high = 1.0;
        const bool rising = vb > va;
        for (int step = 0; step < 12; ++step) {
            const double middle = 0.5 * (low + high);
            if ((curve(middle) > Level) == rising) {
                high = middle;
            }
            else {
                low = middle;
            }
        }
        const double t = 0.5 * (low + high);
        const int index = static_cast<int>(surface.points.size());
        surface.points.push_back({pa[0] + t * (pb[0] - pa[0]),
                                  pa[1] + t * (pb[1] - pa[1]),
                                  pa[2] + t * (pb[2] - pa[2])});
        // its normal: down the field's slope, which is smooth where the
        // triangles are not -- a drop of a few of them is lit as a ball
        Vec3 n {-(sa[0] + t * (sb[0] - sa[0])), -(sa[1] + t * (sb[1] - sa[1])),
                -(sa[2] + t * (sb[2] - sa[2]))};
        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        surface.normals.push_back(l > 0.0 ? Vec3 {n[0] / l, n[1] / l, n[2] / l}
                                          : Vec3 {0.0, 0.0, 1.0});
        made.emplace(key, index);
        return index;
    };
    // outwards is from the nodes inside to the ones outside
    const auto triangle = [&](int a, int b, int c, const Vec3& out) {
        if (a == b || b == c || a == c) {
            return;
        }
        const Vec3& pa = surface.points[static_cast<std::size_t>(a)];
        const Vec3& pb = surface.points[static_cast<std::size_t>(b)];
        const Vec3& pc = surface.points[static_cast<std::size_t>(c)];
        const Vec3 u {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const Vec3 v {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const Vec3 n {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        if (n[0] * out[0] + n[1] * out[1] + n[2] * out[2] >= 0.0) {
            surface.triangles.push_back({a, b, c});
        }
        else {
            surface.triangles.push_back({a, c, b});
        }
    };
    // a cell's corners: bit 0 along x, 1 along y, 2 along z; the six
    // tetrahedra share the diagonal from corner 0 to corner 7
    static constexpr int Tetrahedra[6][4] = {{0, 1, 3, 7}, {0, 3, 2, 7}, {0, 2, 6, 7},
                                             {0, 6, 4, 7}, {0, 4, 5, 7}, {0, 5, 1, 7}};
    for (long k = 0; k + 1 < grid.n[2]; ++k) {
        for (long j = 0; j + 1 < grid.n[1]; ++j) {
            for (long i = 0; i + 1 < grid.n[0]; ++i) {
                std::array<std::size_t, 8> node {};
                std::array<float, 8> value {};
                int inside = 0;
                for (int c = 0; c < 8; ++c) {
                    node[static_cast<std::size_t>(c)] =
                        grid.at(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                    value[static_cast<std::size_t>(c)] =
                        grid.value[node[static_cast<std::size_t>(c)]];
                    inside += value[static_cast<std::size_t>(c)] > Level ? 1 : 0;
                }
                if (inside == 0 || inside == 8) {
                    continue;
                }
                std::array<Vec3, 8> place {};
                for (int c = 0; c < 8; ++c) {
                    place[static_cast<std::size_t>(c)] =
                        grid.place(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                }
                for (const auto& tetrahedron : Tetrahedra) {
                    std::array<int, 4> in {};
                    std::array<int, 4> out {};
                    int ins = 0;
                    int outs = 0;
                    for (int c : tetrahedron) {
                        if (value[static_cast<std::size_t>(c)] > Level) {
                            in[static_cast<std::size_t>(ins++)] = c;
                        }
                        else {
                            out[static_cast<std::size_t>(outs++)] = c;
                        }
                    }
                    if (ins == 0 || outs == 0) {
                        continue;
                    }
                    Vec3 outwards {0.0, 0.0, 0.0};
                    for (int a = 0; a < ins; ++a) {
                        for (int b = 0; b < outs; ++b) {
                            for (std::size_t x = 0; x < 3; ++x) {
                                outwards[x] += place[static_cast<std::size_t>(out[static_cast<std::size_t>(b)])][x]
                                    - place[static_cast<std::size_t>(in[static_cast<std::size_t>(a)])][x];
                            }
                        }
                    }
                    const auto cut = [&](int a, int b) {
                        const auto ca = static_cast<std::size_t>(a);
                        const auto cb = static_cast<std::size_t>(b);
                        return pointOn(node[ca], place[ca], value[ca], node[cb], place[cb], value[cb]);
                    };
                    if (ins == 1) {
                        triangle(cut(in[0], out[0]), cut(in[0], out[1]), cut(in[0], out[2]), outwards);
                    }
                    else if (outs == 1) {
                        triangle(cut(in[0], out[0]), cut(in[1], out[0]), cut(in[2], out[0]), outwards);
                    }
                    else {
                        // two and two: a quadrangle, around in order
                        const int a = cut(in[0], out[0]);
                        const int b = cut(in[0], out[1]);
                        const int c = cut(in[1], out[1]);
                        const int e = cut(in[1], out[0]);
                        triangle(a, b, c, outwards);
                        triangle(a, c, e, outwards);
                    }
                }
            }
        }
    }
}
