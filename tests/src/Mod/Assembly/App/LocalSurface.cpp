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

namespace
{

// A round R 5's face from the angle 'from' over 'span', heights 0 to 4,
// its nodes on the round; and the offsets to the round of its nine quartic
// points (quarticPoints), the round's point straight out from the axis
Face roundFaceFrom(double from, double span)
{
    const double r = 5.0;
    const double h = 4.0;
    Face x;
    x[0] = cylinder(r, from, 0.0);
    x[1] = cylinder(r, from + span, 0.0);
    x[2] = cylinder(r, from, h);
    x[3] = cylinder(r, from + 0.5 * span, 0.0);
    x[4] = cylinder(r, from + 0.5 * span, 0.5 * h);
    x[5] = cylinder(r, from, 0.5 * h);
    return x;
}

Offsets offsetsToRound(const Face& x, double radius)
{
    Offsets offsets {};
    for (std::size_t p = 0; p < quarticPoints.size(); ++p) {
        const std::array<double, 3> L {quarticPoints[p][0] / 4.0,
                                       quarticPoints[p][1] / 4.0,
                                       quarticPoints[p][2] / 4.0};
        const Vec on = pointAt(x, nullptr, L);
        const double away = std::hypot(on[0], on[1]);
        const Vec round {on[0] * radius / away, on[1] * radius / away, on[2]};
        offsets[p] = add(round, on, -1.0);
    }
    return offsets;
}

double offRound(const Vec& p, double radius)
{
    return std::hypot(p[0], p[1]) - radius;
}

}  // namespace

// A six-node face stands in from its round between its nodes, 3.9 um on R 5
// over 0.8 rad; through nine more of the round's points, 0.11 um at most,
// near a corner (D4b way 1 step 3)
TEST(LocalSurface, nineMorePointsPutAFaceOnItsRound)  // NOLINT
{
    const Face x = roundFaceFrom(0.0, 0.8);
    const Offsets offsets = offsetsToRound(x, 5.0);
    double quadratic = 0.0;
    double quartic = 0.0;
    for (int i = 0; i <= 20; ++i) {
        for (int j = 0; i + j <= 20; ++j) {
            const std::array<double, 3> L {(20 - i - j) / 20.0, i / 20.0, j / 20.0};
            quadratic = std::max(quadratic, std::abs(offRound(pointAt(x, nullptr, L), 5.0)));
            quartic = std::max(quartic, std::abs(offRound(pointAt(x, &offsets, L), 5.0)));
        }
    }
    EXPECT_GT(quadratic, 3.5e-3);
    EXPECT_LT(quartic, 1.5e-4);
    // Its derivatives the round's tangents: square to the way out
    const std::array<double, 3> L {0.3, 0.45, 0.25};
    Vec ds;
    Vec dt;
    const Vec p = pointAt(x, &offsets, L, &ds, &dt);
    const Vec radial {p[0], p[1], 0.0};
    EXPECT_LT(std::abs(dot(ds, radial)) / (length(ds) * length(radial)), 1e-4);
    EXPECT_LT(std::abs(dot(dt, radial)) / (length(dt) * length(radial)), 1e-4);
}

// A round's face lying along a plate below it, the round's axis z: in the
// plane square to z at a height, its farthest down is the round's line on
// the plate, (0, -5) -- on the true surface to a hundredth of a micron, the
// six-node face's own microns above it
TEST(LocalSurface, aRoundsFarthestAcrossIsOnItsLine)  // NOLINT
{
    const Face x = roundFaceFrom(-0.5 * pi - 0.3, 0.8);
    const Offsets offsets = offsetsToRound(x, 5.0);
    const Vec out {0.0, -1.0, 0.0};
    const Vec way {0.0, 0.0, 1.0};
    const Vec c {0.0, 0.0, 1.0};
    std::array<double, 3> L {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    int leaving = 0;
    ASSERT_TRUE(farthestAcross(x, &offsets, out, c, way, L, leaving));
    EXPECT_EQ(leaving, -1);
    const Vec p = pointAt(x, &offsets, L);
    EXPECT_NEAR(p[0], 0.0, 1e-3);
    EXPECT_NEAR(p[1], -5.0, 2e-5);
    EXPECT_NEAR(p[2], 1.0, 1e-9);
    // The six-node face's own: above the round
    std::array<double, 3> own {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    ASSERT_TRUE(farthestAcross(x, nullptr, out, c, way, own, leaving));
    EXPECT_GT(pointAt(x, nullptr, own)[1], -5.0 + 5e-4);
}

// Where the line's point is beyond the face, the edge the cut goes out by,
// and the point where it crosses it, for the face beyond
TEST(LocalSurface, aFarthestAcrossBeyondTheFaceLeavesByAnEdge)  // NOLINT
{
    const Face x = roundFaceFrom(-0.5 * pi - 0.3, 0.8);
    const Offsets offsets = offsetsToRound(x, 5.0);
    std::array<double, 3> L {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    int leaving = -1;
    // At 3.5 of the face's 4, the round's bottom is past the edge (1 2)
    const Vec out {0.0, -1.0, 0.0};
    const Vec c {0.0, 0.0, 3.5};
    const Vec way {0.0, 0.0, 1.0};
    EXPECT_FALSE(farthestAcross(x, &offsets, out, c, way, L, leaving));
    EXPECT_EQ(leaving, 1);
    EXPECT_NEAR(L[0], 0.0, 1e-9);
    EXPECT_NEAR(pointAt(x, &offsets, L)[2], 3.5, 1e-6);
}

// The true surface's curvature: where a six-node face's own is 17% under a
// round's at its edge, through its nine more points within 2%
TEST(LocalSurface, aTrueSurfaceCurvesAsItsRound)  // NOLINT
{
    const Face x = roundFaceFrom(0.0, 0.8);
    const Offsets offsets = offsetsToRound(x, 5.0);
    for (const auto& L : {std::array<double, 3> {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0},
                          std::array<double, 3> {0.98, 0.01, 0.01},
                          std::array<double, 3> {0.5, 0.5, 0.0}}) {
        Local own;
        Local through;
        ASSERT_TRUE(localSurface(x, L, own));
        ASSERT_TRUE(localSurface(x, L, through, &offsets));
        EXPECT_NEAR(through.k1, 0.2, 0.004);
        EXPECT_NEAR(through.k2, 0.0, 0.004);
        // Round the round, not along it
        EXPECT_LT(std::abs(through.way1[2]), 0.01);
    }
    Local edge;
    ASSERT_TRUE(localSurface(x, {0.98, 0.01, 0.01}, edge));
    EXPECT_LT(edge.k1, 0.19);
}

// A plane that cuts a face by a corner alone: a cone's side by its narrow
// end, a plane square to the side 2 um in from a rim vertex, the cone's
// bottom beyond the edge (0 1). From the face's middle projected onto the
// plane, the point fell past the edge (1 2) and left by it, away from the
// bottom -- a rigid cone on a plate 0.5 um short of it there. From the
// middle of the cut, by the edge it is (D4b way 1 step 5)
TEST(LocalSurface, aCutByACornerLeavesTowardsTheFarthest)  // NOLINT
{
    const double r1 = 5.0;
    const double r2 = 4.0;
    const double h = 40.0;
    const double half = std::atan2(r1 - r2, h);
    auto cone = [&](double angle, double z) {
        const double r = r1 + (r2 - r1) * z / h;
        return Vec {r * std::cos(angle), r * std::sin(angle), z};
    };
    auto normalAt = [&](double angle) {
        return Vec {std::cos(angle) * std::cos(half),
                    std::sin(angle) * std::cos(half),
                    std::sin(half)};
    };
    // Two corners 2.1 mm in from the rim, one on it; its edges' middles out
    // by the sagitta its corners' normals give, as a rigid hull's
    const std::array<std::array<double, 2>, 3> corners {
        {{-0.1257, h - 2.1}, {-0.1995, h}, {-0.2516, h - 2.1}}};
    Face x;
    std::array<Vec, 3> n;
    for (std::size_t k = 0; k < 3; ++k) {
        x[k] = cone(corners[k][0], corners[k][1]);
        n[k] = normalAt(corners[k][0]);
    }
    for (std::size_t k = 0; k < 3; ++k) {
        const std::size_t j = (k + 1) % 3;
        Vec way = add(n[k], n[j]);
        normalize(way);
        x[3 + k] = add(scaled(add(x[k], x[j]), 0.5),
                       way,
                       dot(add(x[j], x[k], -1.0), add(n[j], n[k], -1.0)) / 8.0);
    }
    const double bottom = -0.169;
    const Vec out = normalAt(bottom);
    Vec way = add(cone(bottom, h), cone(bottom, 0.0), -1.0);
    way = add(way, out, -dot(way, out));
    normalize(way);
    const Vec c = add(x[1], way, -0.002);
    std::array<double, 3> L {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    int leaving = -1;
    EXPECT_FALSE(farthestAcross(x, nullptr, out, c, way, L, leaving));
    EXPECT_EQ(leaving, 0);
    EXPECT_NEAR(L[2], 0.0, 1e-9);
}
