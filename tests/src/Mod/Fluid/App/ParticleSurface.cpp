// SPDX-License-Identifier: LGPL-2.1-or-later

// The surface of a frame of particles: what a particle backend's water is
// drawn as (docs/FluidSimulation.md sec 12.6).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

#include <Mod/Fluid/App/ParticleSurface.h>

using namespace Fluid;
using Fluid::Solver::Vec3;

namespace
{

// Particles on a grid of the spacing filling a box, as a backend starts
// them: the first half a spacing in.
std::vector<Vec3> block(const Vec3& min, int nx, int ny, int nz, double d)
{
    std::vector<Vec3> particles;
    for (int i = 0; i < nx; ++i) {
        for (int j = 0; j < ny; ++j) {
            for (int k = 0; k < nz; ++k) {
                particles.push_back(
                    {min[0] + (i + 0.5) * d, min[1] + (j + 0.5) * d, min[2] + (k + 0.5) * d});
            }
        }
    }
    return particles;
}

// The volume a closed surface wound outwards holds.
double volumeOf(const ParticleSurface& s)
{
    double six = 0.0;
    for (const auto& t : s.triangles) {
        const Vec3& a = s.points[static_cast<std::size_t>(t[0])];
        const Vec3& b = s.points[static_cast<std::size_t>(t[1])];
        const Vec3& c = s.points[static_cast<std::size_t>(t[2])];
        six += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
            + a[2] * (b[0] * c[1] - b[1] * c[0]);
    }
    return six / 6.0;
}

// Closed: every edge is two triangles', once each way.
bool closed(const ParticleSurface& s)
{
    std::map<std::pair<int, int>, int> edges;
    for (const auto& t : s.triangles) {
        for (std::size_t k = 0; k < 3; ++k) {
            const int a = t[k];
            const int b = t[(k + 1) % 3];
            if (a == b) {
                return false;
            }
            edges[{a, b}] += 1;
        }
    }
    return std::ranges::all_of(edges, [&](const auto& edge) {
        const auto back = edges.find({edge.first.second, edge.first.first});
        return edge.second == 1 && back != edges.end() && back->second == 1;
    });
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

TEST(ParticleSurface, aBlockOfWater)
{
    const double d = 0.01;
    ParticleSurface surface;
    surfaceOf(block({0.0, 0.0, 0.0}, 10, 8, 6, d), d, surface);
    ASSERT_FALSE(surface.triangles.empty());
    ASSERT_EQ(surface.normals.size(), surface.points.size());
    EXPECT_TRUE(closed(surface));
    // The water's edge is half a spacing past the outermost particles'
    // middles: the block's own faces. Its edges and corners are rounded.
    const double volume = volumeOf(surface);
    EXPECT_GT(volume, 0.90 * 0.1 * 0.08 * 0.06);
    EXPECT_LT(volume, 1.02 * 0.1 * 0.08 * 0.06);
    const Vec3 max {0.1, 0.08, 0.06};
    const Vec3 middle {0.05, 0.04, 0.03};
    Vec3 low {1.0, 1.0, 1.0};
    Vec3 high {-1.0, -1.0, -1.0};
    for (std::size_t i = 0; i < surface.points.size(); ++i) {
        const Vec3& p = surface.points[i];
        const Vec3& n = surface.normals[i];
        for (std::size_t k = 0; k < 3; ++k) {
            low[k] = std::min(low[k], p[k]);
            high[k] = std::max(high[k], p[k]);
        }
        EXPECT_NEAR(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1.0, 1e-6);
        // outwards: away from the block's middle
        EXPECT_GT(n[0] * (p[0] - middle[0]) + n[1] * (p[1] - middle[1])
                      + n[2] * (p[2] - middle[2]),
                  0.0);
    }
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(low[k], 0.0, 0.15 * d);
        EXPECT_NEAR(high[k], max[k], 0.15 * d);
    }
}

TEST(ParticleSurface, twoBodiesAndADrop)
{
    const double d = 0.01;
    std::vector<Vec3> particles = block({0.0, 0.0, 0.0}, 6, 6, 6, d);
    const std::vector<Vec3> second = block({0.2, 0.0, 0.0}, 6, 6, 6, d);
    particles.insert(particles.end(), second.begin(), second.end());
    ParticleSurface surface;
    surfaceOf(particles, d, surface);
    EXPECT_TRUE(closed(surface));
    const double one = 0.06 * 0.06 * 0.06;
    EXPECT_GT(volumeOf(surface), 2 * 0.88 * one);
    EXPECT_LT(volumeOf(surface), 2 * 1.02 * one);
    // nothing between them
    for (const Vec3& p : surface.points) {
        EXPECT_FALSE(p[0] > 0.08 && p[0] < 0.18);
    }

    // a particle alone is a drop, not nothing: spray stays in the picture
    ParticleSurface drop;
    surfaceOf({{0.5, 0.5, 0.5}}, d, drop);
    ASSERT_FALSE(drop.triangles.empty());
    EXPECT_TRUE(closed(drop));
    EXPECT_GT(volumeOf(drop), 0.0);
    // its points are on the ball, a few of them: what they hold is between
    // the eight-sided shape in it (0.12 of a spacing's cube) and the ball
    EXPECT_GT(volumeOf(drop), 0.12 * d * d * d);
    EXPECT_LT(volumeOf(drop), 0.39 * d * d * d);
    // a ball under half a spacing in radius, as near as the grid draws
    // one, its normals straight out
    ASSERT_EQ(drop.normals.size(), drop.points.size());
    for (std::size_t i = 0; i < drop.points.size(); ++i) {
        const Vec3& p = drop.points[i];
        const Vec3& n = drop.normals[i];
        const Vec3 out {p[0] - 0.5, p[1] - 0.5, p[2] - 0.5};
        const double r = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        EXPECT_NEAR(r, 0.45 * d, 0.08 * d);
        EXPECT_GT((n[0] * out[0] + n[1] * out[1] + n[2] * out[2]) / r, 0.95);
    }

    // Two a spacing apart run together, one drop; two and a half spacings
    // apart they are two.
    const auto parts = [](const ParticleSurface& s) {
        std::vector<int> root(s.points.size());
        for (std::size_t i = 0; i < root.size(); ++i) {
            root[i] = static_cast<int>(i);
        }
        const auto find = [&](int a) {
            while (root[static_cast<std::size_t>(a)] != a) {
                a = root[static_cast<std::size_t>(a)] =
                    root[static_cast<std::size_t>(root[static_cast<std::size_t>(a)])];
            }
            return a;
        };
        for (const auto& t : s.triangles) {
            root[static_cast<std::size_t>(find(t[1]))] = find(t[0]);
            root[static_cast<std::size_t>(find(t[2]))] = find(t[0]);
        }
        int count = 0;
        for (std::size_t i = 0; i < root.size(); ++i) {
            count += find(static_cast<int>(i)) == static_cast<int>(i) ? 1 : 0;
        }
        return count;
    };
    ParticleSurface close;
    surfaceOf({{0.5, 0.5, 0.5}, {0.5 + d, 0.5, 0.5}}, d, close);
    EXPECT_TRUE(closed(close));
    EXPECT_EQ(parts(close), 1);
    EXPECT_GT(volumeOf(close), 1.8 * volumeOf(drop));
    ParticleSurface apart;
    surfaceOf({{0.5, 0.5, 0.5}, {0.5 + 2.5 * d, 0.5, 0.5}}, d, apart);
    EXPECT_TRUE(closed(apart));
    EXPECT_EQ(parts(apart), 2);
    EXPECT_EQ(parts(drop), 1);

    // every particle is inside the surface: a strand off a block's corner,
    // each a spacing and a bit from the last
    std::vector<Vec3> strand = block({0.0, 0.0, 0.0}, 6, 6, 6, d);
    for (int i = 1; i <= 6; ++i) {
        strand.push_back({0.055 + 1.1 * d * i, 0.055 + 1.1 * d * i, 0.055 + 1.1 * d * i});
    }
    ParticleSurface whole;
    surfaceOf(strand, d, whole);
    EXPECT_TRUE(closed(whole));
    for (int i = 1; i <= 6; ++i) {
        const Vec3& p = strand[strand.size() - static_cast<std::size_t>(i)];
        // nearer to the surface's points than the strand is long, and a
        // point of it within a drop's radius or so
        double nearest = 1.0;
        for (const Vec3& q : whole.points) {
            nearest = std::min(nearest, std::sqrt((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1])
                                                  + (q[2] - p[2]) * (q[2] - p[2])));
        }
        EXPECT_LT(nearest, 1.2 * d) << "particle " << i;
        EXPECT_GT(nearest, 0.2 * d) << "particle " << i;
    }
}

TEST(ParticleSurface, sprayIsLeftOut)
{
    // a block, and off it a particle alone, a pair a spacing apart, and a
    // clump of eight
    const double d = 0.01;
    std::vector<Vec3> particles = block({0.0, 0.0, 0.0}, 6, 6, 6, d);
    const std::size_t body = particles.size();
    particles.push_back({0.2, 0.03, 0.03});
    particles.push_back({0.3, 0.03, 0.03});
    particles.push_back({0.3 + d, 0.03, 0.03});
    const std::vector<Vec3> clump = block({0.4, 0.02, 0.02}, 2, 2, 2, d);
    particles.insert(particles.end(), clump.begin(), clump.end());

    // not asked for: every one is in the surface
    ParticleSurface all;
    surfaceOf(particles, d, all);
    EXPECT_TRUE(all.spray.empty());
    const auto around = [](const ParticleSurface& s, double x) {
        return std::ranges::any_of(s.points, [&](const Vec3& p) {
            return std::abs(p[0] - x) < 0.03;
        });
    };
    EXPECT_TRUE(around(all, 0.2));
    EXPECT_TRUE(around(all, 0.3));
    EXPECT_TRUE(around(all, 0.41));

    // asked for: the one and the pair are spray and no surface, the clump
    // is a drop still and the block as it was
    ParticleSurface some;
    surfaceOf(particles, d, some, SprayBelow);
    ASSERT_EQ(some.spray.size(), 3U);
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t x = 0; x < 3; ++x) {
            EXPECT_DOUBLE_EQ(some.spray[i][x], particles[body + i][x]);
        }
    }
    EXPECT_TRUE(closed(some));
    EXPECT_FALSE(around(some, 0.2));
    EXPECT_FALSE(around(some, 0.3));
    EXPECT_TRUE(around(some, 0.41));
    ParticleSurface alone;
    surfaceOf(block({0.0, 0.0, 0.0}, 6, 6, 6, d), d, alone);
    EXPECT_NEAR(volumeOf(some) - volumeOf(alone), volumeOf(all) - volumeOf(alone), 2 * d * d * d);
    EXPECT_GT(volumeOf(some), volumeOf(alone));

    // spray only: no surface at all
    ParticleSurface none;
    surfaceOf({{0.5, 0.5, 0.5}}, d, none, SprayBelow);
    EXPECT_TRUE(none.triangles.empty());
    EXPECT_EQ(none.spray.size(), 1U);
}

TEST(ParticleSurface, particlesOutOfOrder)
{
    // A moving fluid's particles are on no grid: each a third of a spacing
    // off its place, by a rule that is the same every run. The surface is
    // still the block's -- no holes inside it, no grain on its faces.
    const double d = 0.01;
    std::vector<Vec3> particles = block({0.0, 0.0, 0.0}, 12, 12, 12, d);
    std::uint32_t state = 12345;
    const auto next = [&]() {
        state = state * 1664525U + 1013904223U;
        return (static_cast<double>(state >> 8U) / 16777216.0 - 0.5) * 2.0;
    };
    for (Vec3& p : particles) {
        for (std::size_t k = 0; k < 3; ++k) {
            p[k] += 0.33 * d * next();
        }
    }
    ParticleSurface surface;
    surfaceOf(particles, d, surface);
    EXPECT_TRUE(closed(surface));
    double area = 0.0;
    double flat = 0.0;    // of the top's triangles, their area
    double upward = 0.0;  // and how much of it faces up
    for (const auto& t : surface.triangles) {
        const Vec3& a = surface.points[static_cast<std::size_t>(t[0])];
        const Vec3& b = surface.points[static_cast<std::size_t>(t[1])];
        const Vec3& c = surface.points[static_cast<std::size_t>(t[2])];
        const Vec3 u {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 v {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const Vec3 n {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const double twice = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        area += 0.5 * twice;
        const bool top = std::min({a[2], b[2], c[2]}) > 0.11
            && std::min({a[0], b[0], c[0], a[1], b[1], c[1]}) > 0.02
            && std::max({a[0], b[0], c[0], a[1], b[1], c[1]}) < 0.10;
        if (top) {
            flat += 0.5 * twice;
            upward += 0.5 * n[2];
        }
    }
    const double box = 6 * 0.12 * 0.12;
    EXPECT_LT(area, 1.05 * box);
    EXPECT_GT(area, 0.85 * box);
    ASSERT_GT(flat, 0.0);
    EXPECT_GT(upward / flat, 0.97);
    EXPECT_NEAR(volumeOf(surface), 0.12 * 0.12 * 0.12, 0.06 * 0.12 * 0.12 * 0.12);
}

namespace
{

// Water 60 deep in a tank 120 x 120 inside -- its floor z = 0, its walls
// x = 0, x = 0.12, y = 0 and y = 0.12 -- its particles each a third of a
// spacing off their places (as in particlesOutOfOrder), and the tank as
// wall points.
struct Tank
{
    std::vector<Vec3> particles;
    std::vector<WallPoint> walls;
};

Tank tank(double d)
{
    Tank c;
    c.particles = block({0.0, 0.0, 0.0}, 12, 12, 6, d);
    std::uint32_t state = 12345;
    const auto next = [&]() {
        state = state * 1664525U + 1013904223U;
        return (static_cast<double>(state >> 8U) / 16777216.0 - 0.5) * 2.0;
    };
    for (Vec3& p : c.particles) {
        for (std::size_t k = 0; k < 3; ++k) {
            p[k] += 0.33 * d * next();
        }
        // and none past a wall
        for (std::size_t k = 0; k < 3; ++k) {
            p[k] = std::max(p[k], 0.2 * d);
        }
        for (std::size_t k = 0; k < 2; ++k) {
            p[k] = std::min(p[k], 0.12 - 0.2 * d);
        }
    }
    // each wall a square, two triangles wound out of the solid: into the
    // tank
    Solver::Surface solid;
    const double l = 0.12;
    const double h = 0.2;
    const auto square = [&](const Vec3& o, const Vec3& u, const Vec3& v) {
        const int at = static_cast<int>(solid.points.size());
        solid.points.push_back(o);
        solid.points.push_back({o[0] + u[0], o[1] + u[1], o[2] + u[2]});
        solid.points.push_back({o[0] + u[0] + v[0], o[1] + u[1] + v[1], o[2] + u[2] + v[2]});
        solid.points.push_back({o[0] + v[0], o[1] + v[1], o[2] + v[2]});
        solid.triangles.push_back({at, at + 1, at + 2});
        solid.triangles.push_back({at, at + 2, at + 3});
    };
    square({0.0, 0.0, 0.0}, {l, 0.0, 0.0}, {0.0, l, 0.0});  // the floor, up
    square({0.0, 0.0, 0.0}, {0.0, l, 0.0}, {0.0, 0.0, h});  // x = 0, along +x
    square({0.0, 0.0, 0.0}, {0.0, 0.0, h}, {l, 0.0, 0.0});  // y = 0, along +y
    square({l, 0.0, 0.0}, {0.0, 0.0, h}, {0.0, l, 0.0});    // x = l, along -x
    square({0.0, l, 0.0}, {l, 0.0, 0.0}, {0.0, 0.0, h});    // y = l, along -y
    c.walls = wallPointsOf(solid, 0.5 * d);
    return c;
}

// How high the water's top is, in the mean: in the open, and along the
// walls within a spacing of them.
std::pair<double, double> topOf(const ParticleSurface& s, double d)
{
    double open = 0.0;
    double along = 0.0;
    int opens = 0;
    int alongs = 0;
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        const Vec3& p = s.points[i];
        if (s.normals[i][2] < 0.7 || p[2] < 0.04) {
            continue;
        }
        const double wall = std::min({p[0], p[1], 0.12 - p[0], 0.12 - p[1]});
        if (wall > 3 * d) {
            open += p[2];
            ++opens;
        }
        else if (wall < d) {
            along += p[2];
            ++alongs;
        }
    }
    return {open / std::max(opens, 1), along / std::max(alongs, 1)};
}

}  // namespace

TEST(ParticleSurface, wallPointsFaceOutOfTheSolid)
{
    const Tank c = tank(0.01);
    ASSERT_FALSE(c.walls.empty());
    for (const WallPoint& w : c.walls) {
        EXPECT_NEAR(std::sqrt(w.out[0] * w.out[0] + w.out[1] * w.out[1] + w.out[2] * w.out[2]),
                    1.0,
                    1e-9);
        // into the tank: from the middle of the floor, the point is behind
        // its face
        const Vec3 to {0.06 - w.at[0], 0.06 - w.at[1], 0.03 - w.at[2]};
        EXPECT_GT(to[0] * w.out[0] + to[1] * w.out[1] + to[2] * w.out[2], 0.0);
    }
    // half a spacing apart or nearer: a square 0.12 across has 25 rows
    EXPECT_GE(c.walls.size(), 5U * 25U * 25U);
}

TEST(ParticleSurface, waterAgainstAWall)
{
    // Beside a wall half of a particle's surroundings is the wall's, and
    // the field the surface is cut from read low there: the surface dipped
    // to the particles, and the ones at the top counted as drops or spray,
    // flipping from one frame to the next. Given the walls, the particles
    // near one have their mirror images across it in the field.
    const double d = 0.01;
    const Tank c = tank(d);
    ParticleSurface bare;
    surfaceOf(c.particles, d, bare, SprayBelow);
    ParticleSurface walled;
    surfaceOf(c.particles, d, walled, SprayBelow, c.walls);
    EXPECT_TRUE(walled.spray.empty());
    EXPECT_TRUE(closed(walled));
    const auto [bareOpen, bareAlong] = topOf(bare, d);
    const auto [open, along] = topOf(walled, d);
    // the open top where the particles say, half a spacing past them
    EXPECT_NEAR(open, 0.06, 0.25 * d);
    // measured: 0.09 mm off the open top along the walls, 1.2 without
    // them, and 9 of the particles spray without them
    EXPECT_NEAR(along, open, 0.1 * d);
    EXPECT_GT(bareOpen - bareAlong, open - along);
    // nothing of it in the walls more than a node of the grid deep
    for (const Vec3& p : walled.points) {
        EXPECT_GT(std::min({p[0], p[1], p[2], 0.12 - p[0], 0.12 - p[1]}), -0.5 * d);
    }
}

TEST(ParticleSurface, stillWaterBesideAWallDoesNotFlicker)
{
    // Frame after frame of still water: each particle a twentieth of a
    // spacing from where it was, at random. The water's volume barely
    // changes from one to the next -- unless particles beside a wall flip
    // between the body, a drop and the spray, each adding or taking a
    // ball of its own at once: what played as popping along the walls.
    const double d = 0.01;
    const Tank c = tank(d);
    const auto worstStep = [&](const std::vector<WallPoint>& walls) {
        std::vector<Vec3> particles = c.particles;
        std::uint32_t state = 777;
        const auto next = [&]() {
            state = state * 1664525U + 1013904223U;
            return (static_cast<double>(state >> 8U) / 16777216.0 - 0.5) * 2.0;
        };
        double worst = 0.0;
        double before = -1.0;
        for (int frame = 0; frame < 20; ++frame) {
            ParticleSurface s;
            surfaceOf(particles, d, s, SprayBelow, walls);
            const double volume = volumeOf(s);
            if (before >= 0.0) {
                worst = std::max(worst, std::abs(volume - before));
            }
            before = volume;
            for (Vec3& p : particles) {
                for (std::size_t k = 0; k < 3; ++k) {
                    p[k] += 0.05 * d * next();
                }
                for (std::size_t k = 0; k < 3; ++k) {
                    p[k] = std::max(p[k], 0.2 * d);
                }
                for (std::size_t k = 0; k < 2; ++k) {
                    p[k] = std::min(p[k], 0.12 - 0.2 * d);
                }
            }
        }
        return worst / (d * d * d);
    };
    // measured: 3.77 of a spacing's cube without the walls, 1.23 with
    const double bare = worstStep({});
    const double walled = worstStep(c.walls);
    EXPECT_LT(walled, 0.5 * bare);
}

TEST(ParticleSurface, nothingOfNothing)
{
    ParticleSurface surface;
    surface.points.push_back({1.0, 2.0, 3.0});
    surfaceOf({}, 0.01, surface);
    EXPECT_TRUE(surface.points.empty());
    EXPECT_TRUE(surface.triangles.empty());
    surfaceOf({{0.0, 0.0, 0.0}}, 0.0, surface);
    EXPECT_TRUE(surface.triangles.empty());
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
