// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include <Mod/Fluid/App/Tracers.h>

namespace
{
// Cells of 1 mm filling 20 x 20 x 4 mm, the flow given by where the cell is.
template<typename Flow>
Fluid::ArchivedFrame cellsOf(Flow&& flow)
{
    Fluid::ArchivedFrame frame;
    for (int i = 0; i < 20; ++i) {
        for (int j = 0; j < 20; ++j) {
            for (int k = 0; k < 4; ++k) {
                const float x = 0.5F + static_cast<float>(i);
                const float y = 0.5F + static_cast<float>(j);
                const float z = 0.5F + static_cast<float>(k);
                const std::array<float, 3> v = flow(x, y);
                frame.inside.insert(frame.inside.end(), {x, y, z, v[0], v[1], v[2], 0.0F});
            }
        }
    }
    return frame;
}
}  // namespace

TEST(FluidTracers, carriedAlongAStraightFlow)
{
    // along x at 2 m/s everywhere
    const auto frame = cellsOf([](float, float) { return std::array<float, 3> {2.0F, 0.0F, 0.0F}; });
    const auto paths = Fluid::trace(frame, {1.0F, 10.0F, 2.0F, 1.0F, 5.0F, 2.0F}, 1.0, 0.0, 1000);
    // the fastest goes half the reach a step: 0.5 mm at 2 m/s
    EXPECT_NEAR(paths.interval, 0.00025, 1e-12);
    ASSERT_EQ(paths.starts.size(), 3U);
    EXPECT_EQ(paths.starts[0], 0);
    EXPECT_EQ(paths.starts[2], static_cast<std::int32_t>(paths.speeds.size()));
    ASSERT_EQ(paths.points.size(), paths.speeds.size() * 3);
    for (std::size_t path = 0; path < 2; ++path) {
        const auto from = static_cast<std::size_t>(paths.starts[path]);
        const auto to = static_cast<std::size_t>(paths.starts[path + 1]);
        // from x 1 to where the cells end, some way past x 20: 0.5 mm a point
        EXPECT_GE(to - from, 38U);
        EXPECT_LE(to - from, 44U);
        for (std::size_t p = from; p < to; ++p) {
            EXPECT_NEAR(paths.points[p * 3], 1.0F + 0.5F * static_cast<float>(p - from), 1e-3F);
            EXPECT_NEAR(paths.points[p * 3 + 1], path == 0 ? 10.0F : 5.0F, 1e-4F);
            EXPECT_NEAR(paths.speeds[p], 2.0F, 1e-5F);
        }
    }
    // an interval given is kept, and a path is cut at the most it may have
    const auto cut = Fluid::trace(frame, {1.0F, 10.0F, 2.0F}, 1.0, 0.001, 5);
    EXPECT_EQ(cut.interval, 0.001);
    ASSERT_EQ(cut.speeds.size(), 5U);
    EXPECT_NEAR(cut.points[4 * 3], 9.0F, 1e-3F);
}

TEST(FluidTracers, roundATurnAndBackToTheStart)
{
    // about the middle (10, 10), a turn each 2 pi ms: 1 m/s at 1 mm out
    const auto frame = cellsOf([](float x, float y) {
        return std::array<float, 3> {-(y - 10.0F), x - 10.0F, 0.0F};
    });
    // from 5 mm out; a point each 1/200 of a turn, some over a turn
    const double turn = 2.0 * std::numbers::pi * 1.0e-3;
    const auto paths = Fluid::trace(frame, {15.0F, 10.0F, 2.0F}, 1.0, turn / 200.0, 220);
    ASSERT_EQ(paths.speeds.size(), 220U);
    for (std::size_t p = 0; p < 220; ++p) {
        const double x = paths.points[p * 3] - 10.0;
        const double y = paths.points[p * 3 + 1] - 10.0;
        // it keeps its distance: followed round, not thrown out
        EXPECT_NEAR(std::hypot(x, y), 5.0, 0.1) << p;
        EXPECT_NEAR(paths.speeds[p], 5.0F, 0.15F) << p;
    }
    // a quarter of the way round, and back after the whole turn
    EXPECT_NEAR(paths.points[50 * 3], 10.0F, 0.15F);
    EXPECT_NEAR(paths.points[50 * 3 + 1], 15.0F, 0.15F);
    EXPECT_NEAR(paths.points[200 * 3], 15.0F, 0.15F);
    EXPECT_NEAR(paths.points[200 * 3 + 1], 10.0F, 0.3F);
}

TEST(FluidTracers, whatIsNotFollowed)
{
    const auto frame = cellsOf([](float, float) { return std::array<float, 3> {2.0F, 0.0F, 0.0F}; });
    // a seed out of the fluid has a path of nothing, the others theirs
    const auto paths = Fluid::trace(frame, {1.0F, 50.0F, 2.0F, 1.0F, 10.0F, 2.0F}, 1.0, 0.0, 100);
    ASSERT_EQ(paths.starts.size(), 3U);
    EXPECT_EQ(paths.starts[1], 0);
    EXPECT_GT(paths.starts[2], 30);
    // no fluid kept, nothing that moves, no reach, no points asked
    EXPECT_TRUE(Fluid::trace({}, {1.0F, 10.0F, 2.0F}, 1.0, 0.0, 100).starts.empty());
    const auto still = cellsOf([](float, float) { return std::array<float, 3> {}; });
    EXPECT_TRUE(Fluid::trace(still, {1.0F, 10.0F, 2.0F}, 1.0, 0.0, 100).starts.empty());
    EXPECT_TRUE(Fluid::trace(frame, {1.0F, 10.0F, 2.0F}, 0.0, 0.0, 100).starts.empty());
    EXPECT_TRUE(Fluid::trace(frame, {1.0F, 10.0F, 2.0F}, 1.0, 0.0, 0).starts.empty());
    // a tracer let go where the fluid stands still stays, a path of one point
    const auto half = cellsOf([](float x, float) {
        return std::array<float, 3> {x > 10.0F ? 2.0F : 0.0F, 0.0F, 0.0F};
    });
    const auto stood = Fluid::trace(half, {3.0F, 10.0F, 2.0F}, 1.0, 0.0, 100);
    ASSERT_EQ(stood.starts.size(), 2U);
    EXPECT_EQ(stood.starts[1], 1);
}
