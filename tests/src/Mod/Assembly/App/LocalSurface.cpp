// SPDX-License-Identifier: LGPL-2.1-or-later

// The local surface of a flexible part's quadratic face, and what a contact
// is by the two surfaces' relative curvature (docs/AssemblyDynamics.md, D4b
// way 1)

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numbers>
#include <utility>

#include <Mod/Assembly/App/Solver/LocalSurface.h>

using namespace Assembly::Solver::LocalSurface;

namespace
{

constexpr double pi = std::numbers::pi;

Vec cylinder(double radius, double angle, double z)
{
    return {radius * std::cos(angle), radius * std::sin(angle), z};
}

Vec sphere(double radius, double theta, double phi)
{
    return {radius * std::sin(theta) * std::cos(phi),
            radius * std::sin(theta) * std::sin(phi),
            radius * std::cos(theta)};
}

// A face of a round R 5 along z, its nodes on the round: corners at the
// angles 0 and 'span' and the heights 0 and 'height', wound to face out of
// the round -- or into it, a bore's, 'hollow'
Face roundFace(double span, double height, bool hollow = false)
{
    const double r = 5.0;
    Face x;
    // Corners 0, 1, 2; then the nodes of the edges (0 1), (1 2), (2 0)
    x[0] = cylinder(r, 0.0, 0.0);
    x[1] = cylinder(r, span, 0.0);
    x[2] = cylinder(r, 0.0, height);
    x[3] = cylinder(r, 0.5 * span, 0.0);
    x[4] = cylinder(r, 0.5 * span, 0.5 * height);
    x[5] = cylinder(r, 0.0, 0.5 * height);
    if (hollow) {
        std::swap(x[1], x[2]);
        std::swap(x[3], x[5]);
    }
    return x;
}

}  // namespace

TEST(LocalSurface, aFlatFaceIsFlatBothWays)  // NOLINT
{
    Face x;
    x[0] = {0.0, 0.0, 0.0};
    x[1] = {4.0, 0.0, 0.0};
    x[2] = {0.0, 4.0, 0.0};
    x[3] = {2.0, 0.0, 0.0};
    x[4] = {2.0, 2.0, 0.0};
    x[5] = {0.0, 2.0, 0.0};
    Local at;
    ASSERT_TRUE(localSurface(x, {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}, at));
    EXPECT_NEAR(at.normal[2], 1.0, 1e-12);
    EXPECT_NEAR(at.k1, 0.0, 1e-12);
    EXPECT_NEAR(at.k2, 0.0, 1e-12);
    EXPECT_NEAR(at.point[0], 4.0 / 3.0, 1e-12);
    EXPECT_NEAR(at.point[1], 4.0 / 3.0, 1e-12);
}

TEST(LocalSurface, aRoundsFaceCurvesRoundItAndNotAlong)  // NOLINT
{
    // A 4 mm mesh's face on R 5, 0.8 rad round and 4 mm along: none along
    // it, and round it the quadratic's own curvature -- through three
    // points of the round, 1.3% over 1/R at the face's middle, 10% under
    // a tenth of the way round from its edge, 17% under at the edge
    const Face x = roundFace(0.8, 4.0);
    using Where = std::pair<std::array<double, 3>, double>;
    const std::array<Where, 3> wheres {Where {{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}, 0.03},
                                       Where {{0.8, 0.1, 0.1}, 0.12},
                                       Where {{0.1, 0.8, 0.1}, 0.12}};
    for (const auto& [L, within] : wheres) {
        Local at;
        ASSERT_TRUE(localSurface(x, L, at));
        // Out of the round
        const double radial = dot(at.normal, {at.point[0], at.point[1], 0.0});
        EXPECT_GT(radial, 0.0);
        EXPECT_NEAR(at.k1, 0.2, 0.2 * within);
        EXPECT_NEAR(at.k2, 0.0, 1e-9);
        // The flat way is the round's axis, the curved one round it
        EXPECT_NEAR(std::abs(at.way2[2]), 1.0, 1e-9);
        EXPECT_NEAR(at.way1[2], 0.0, 1e-9);
        EXPECT_NEAR(dot(at.way1, at.normal), 0.0, 1e-12);
        EXPECT_NEAR(dot(at.way2, at.normal), 0.0, 1e-12);
    }
}

TEST(LocalSurface, aBoresFaceCurvesAwayFromIt)  // NOLINT
{
    // The same face wound into the round, a bore's: hollow, its curvature
    // negative
    const Face x = roundFace(0.8, 4.0, true);
    Local at;
    ASSERT_TRUE(localSurface(x, {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}, at));
    EXPECT_LT(dot(at.normal, {at.point[0], at.point[1], 0.0}), 0.0);
    EXPECT_NEAR(at.k1, 0.0, 1e-9);
    EXPECT_NEAR(at.k2, -0.2, 0.2 * 0.03);
    EXPECT_NEAR(std::abs(at.way1[2]), 1.0, 1e-9);
}

TEST(LocalSurface, aBallsFaceCurvesBothWays)  // NOLINT
{
    // A 4 mm mesh's face at a ball R 8's equator, 0.5 rad each way
    const double r = 8.0;
    const double h = 0.5 * pi;
    Face x;
    x[0] = sphere(r, h, 0.0);
    x[1] = sphere(r, h, 0.5);
    x[2] = sphere(r, h - 0.5, 0.0);
    x[3] = sphere(r, h, 0.25);
    x[4] = sphere(r, h - 0.25, 0.25);
    x[5] = sphere(r, h - 0.25, 0.0);
    Local at;
    ASSERT_TRUE(localSurface(x, {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}, at));
    EXPECT_GT(dot(at.normal, at.point), 0.0);
    EXPECT_NEAR(at.k1, 1.0 / r, 0.05 / r);
    EXPECT_NEAR(at.k2, 1.0 / r, 0.05 / r);
}

TEST(LocalSurface, aDegenerateFaceHasNoSurface)  // NOLINT
{
    Face x {};
    Local at;
    EXPECT_FALSE(localSurface(x, {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}, at));
}

TEST(LocalSurface, aRoundOnAPlaneTouchesAlongALine)  // NOLINT
{
    // R 5 pressed 50 um into a plane: 1.4 mm wide round it, along it as far
    // as the faces reach
    Local round;
    round.normal = {0.0, 0.0, -1.0};
    round.way1 = {1.0, 0.0, 0.0};
    round.way2 = {0.0, 1.0, 0.0};
    round.k1 = 0.2;
    round.k2 = 0.0;
    Local plane;
    plane.normal = {0.0, 0.0, 1.0};
    plane.way1 = {1.0, 0.0, 0.0};
    plane.way2 = {0.0, 1.0, 0.0};
    const Contact c = contactOf(round, plane, 0.05, 4.0);
    EXPECT_EQ(c.kind, Contact::Kind::Line);
    EXPECT_NEAR(std::abs(c.along[1]), 1.0, 1e-12);
    EXPECT_NEAR(c.across, 2.0 * std::sqrt(2.0 * 0.05 / 0.2), 1e-12);
    EXPECT_NEAR(c.k1, 0.2, 1e-12);
    EXPECT_NEAR(c.k2, 0.0, 1e-12);
}

TEST(LocalSurface, aRoundInABoreTouchesAlongALine)  // NOLINT
{
    // R 5 in a bore R 5.5, 50 um: 1/5 - 1/5.5 round, 25 deg either side,
    // 4.7 mm round -- past a 4 mm face, short of the rod's 10 mm
    Local rod;
    rod.normal = {0.0, 0.0, -1.0};
    rod.way1 = {1.0, 0.0, 0.0};
    rod.way2 = {0.0, 1.0, 0.0};
    rod.k1 = 0.2;
    Local bore;
    bore.normal = {0.0, 0.0, 1.0};
    bore.way1 = {0.0, 1.0, 0.0};
    bore.way2 = {1.0, 0.0, 0.0};
    bore.k1 = 0.0;
    bore.k2 = -1.0 / 5.5;
    const Contact c = contactOf(rod, bore, 0.05, 10.0);
    EXPECT_EQ(c.kind, Contact::Kind::Line);
    EXPECT_NEAR(c.k1, 0.2 - 1.0 / 5.5, 1e-12);
    EXPECT_NEAR(std::abs(c.along[1]), 1.0, 1e-12);
}

TEST(LocalSurface, crossedRoundsAndABallTouchAtAPoint)  // NOLINT
{
    Local a;
    a.normal = {0.0, 0.0, -1.0};
    a.way1 = {1.0, 0.0, 0.0};
    a.way2 = {0.0, 1.0, 0.0};
    a.k1 = 0.2;
    Local b;
    b.normal = {0.0, 0.0, 1.0};
    b.way1 = {0.0, 1.0, 0.0};
    b.way2 = {1.0, 0.0, 0.0};
    b.k1 = 0.2;
    EXPECT_EQ(contactOf(a, b, 0.05, 4.0).kind, Contact::Kind::Point);
    Local ball = a;
    ball.k2 = 0.2;
    Local plane;
    plane.normal = {0.0, 0.0, 1.0};
    plane.way1 = {1.0, 0.0, 0.0};
    plane.way2 = {0.0, 1.0, 0.0};
    EXPECT_EQ(contactOf(ball, plane, 0.05, 4.0).kind, Contact::Kind::Point);
}

TEST(LocalSurface, twoFlatFacesTouchOverAnArea)  // NOLINT
{
    Local a;
    a.normal = {0.0, 0.0, -1.0};
    a.way1 = {1.0, 0.0, 0.0};
    a.way2 = {0.0, 1.0, 0.0};
    Local b;
    b.normal = {0.0, 0.0, 1.0};
    b.way1 = {1.0, 0.0, 0.0};
    b.way2 = {0.0, 1.0, 0.0};
    EXPECT_EQ(contactOf(a, b, 0.05, 4.0).kind, Contact::Kind::Area);
    // And a round so large it is flat over a face: R 5 m
    a.k1 = 2e-4;
    EXPECT_EQ(contactOf(a, b, 0.05, 4.0).kind, Contact::Kind::Area);
}
