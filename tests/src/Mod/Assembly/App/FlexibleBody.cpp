// SPDX-License-Identifier: LGPL-2.1-or-later

// The Chrono backend's flexible bodies (docs/AssemblyDynamics.md, D4b): a
// steel bar of 10-node tetrahedra held at one end, reduced as a dynamic run
// reduces it (Craig-Bampton, the interface held), its modes against the
// clamped beam's.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <string>
#include <utility>

#include <Mod/Assembly/App/Solver/ChronoBackend.h>
#include <src/App/InitApplication.h>

using namespace Assembly::Solver;

namespace
{

constexpr double pi = std::numbers::pi;

// A box [0, lx] x [0, ly] x [0, lz] of nx * ny * nz cells, each cut into six
// tetrahedra about its diagonal (Kuhn's, conforming across the cells), with
// the nodes of their edges
struct BoxMesh
{
    std::vector<Base::Vector3d> nodes;
    std::vector<std::array<Id, 10>> tetrahedra;

    BoxMesh(double lx, double ly, double lz, int nx, int ny, int nz)
    {
        auto corner = [&](int i, int j, int k) {
            return static_cast<Id>((i * (ny + 1) + j) * (nz + 1) + k);
        };
        for (int i = 0; i <= nx; ++i) {
            for (int j = 0; j <= ny; ++j) {
                for (int k = 0; k <= nz; ++k) {
                    nodes.emplace_back(lx * i / nx, ly * j / ny, lz * k / nz);
                }
            }
        }
        std::map<std::pair<Id, Id>, Id> mids;
        auto mid = [&](Id a, Id b) {
            const std::pair<Id, Id> key(std::min(a, b), std::max(a, b));
            auto it = mids.find(key);
            if (it != mids.end()) {
                return it->second;
            }
            nodes.push_back((nodes[a] + nodes[b]) / 2.0);
            const Id n = static_cast<Id>(nodes.size() - 1);
            mids.emplace(key, n);
            return n;
        };
        const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        for (int i = 0; i < nx; ++i) {
            for (int j = 0; j < ny; ++j) {
                for (int k = 0; k < nz; ++k) {
                    for (const auto& p : perms) {
                        int at[3] = {0, 0, 0};
                        std::array<Id, 4> c {};
                        c[0] = corner(i, j, k);
                        for (int s = 0; s < 3; ++s) {
                            at[p[s]] = 1;
                            c[s + 1] = corner(i + at[0], j + at[1], k + at[2]);
                        }
                        const Base::Vector3d a = nodes[c[1]] - nodes[c[0]];
                        const Base::Vector3d b = nodes[c[2]] - nodes[c[0]];
                        const Base::Vector3d d = nodes[c[3]] - nodes[c[0]];
                        if (a.Dot(b.Cross(d)) < 0) {
                            std::swap(c[1], c[2]);
                        }
                        tetrahedra.push_back({c[0], c[1], c[2], c[3], mid(c[0], c[1]),
                                              mid(c[1], c[2]), mid(c[2], c[0]), mid(c[0], c[3]),
                                              mid(c[1], c[3]), mid(c[2], c[3])});
                    }
                }
            }
        }
    }
};

class FlexibleBody: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

}  // namespace

// A steel bar 200 x 10 x 10 mm held at its end x = 0: the clamped beam's
// bending, f = (beta L)^2 / (2 pi L^2) sqrt(E I / rho A), beta L 1.8751 and
// 4.6941, the same about both axes of the square section
TEST_F(FlexibleBody, cantileverModesAreTheClampedBeams)  // NOLINT
{
    if (!Chrono::hasFlexibleBodies()) {
        GTEST_SKIP() << "the Chrono backend has no modal module";
    }
    const double length = 200.0;
    const double side = 10.0;
    const BoxMesh box(length, side, side, 20, 2, 2);

    Model model;
    Body body;
    body.source = model.addSource("Bar");
    const Id bar = model.addBody(body);
    Frame end;
    end.body = bar;
    end.local = Base::Placement(Base::Vector3d(0, side / 2, side / 2), Base::Rotation());
    end.label = model.addSource("End");
    const Id frame = model.addFrame(end);

    Flexible flexible;
    flexible.source = model.addSource("Flexible");
    flexible.body = bar;
    flexible.nodes = box.nodes;
    flexible.tetrahedra = box.tetrahedra;
    flexible.youngs = 2.1e8;     // mN/mm^2, 210 GPa
    flexible.poisson = 0.3;
    flexible.density = 7.85e-6;  // kg/mm^3
    flexible.modes = 4;
    Flexible::Interface held;
    held.frame = frame;
    for (Id n = 0; n < box.nodes.size(); ++n) {
        if (box.nodes[n].x == 0.0) {
            held.nodes.push_back(n);
        }
    }
    flexible.interfaces.push_back(held);
    model.dynamics.emplace();
    model.dynamics->flexibles.push_back(flexible);

    std::string error;
    const std::vector<double> hz = Chrono::flexibleFrequencies(model, 0, &error);
    ASSERT_EQ(hz.size(), 4u) << error;

    // SI
    const double e = 2.1e11;
    const double rhoA = 7850.0 * 1e-4;
    const double inertia = std::pow(0.01, 4) / 12.0;
    const double l = 0.2;
    auto beam = [&](double betaL) {
        return betaL * betaL / (2.0 * pi * l * l) * std::sqrt(e * inertia / rhoA);
    };
    const double first = beam(1.8751);
    const double second = beam(4.6941);
    // 209.6 and 1299.2 Hz against 208.9 and 1309: the second lower, by shear
    EXPECT_NEAR(hz[0], first, 0.01 * first);
    EXPECT_NEAR(hz[1], first, 0.01 * first);
    EXPECT_NEAR(hz[2], second, 0.02 * second);
    EXPECT_NEAR(hz[3], second, 0.02 * second);
}
