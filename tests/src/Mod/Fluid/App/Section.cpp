// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>

#include <Mod/Fluid/App/Section.h>

namespace
{
// A box of fluid 10 x 4 x 4 mm, its faces as a frame's, and inside it cells
// of 1 mm: the flow along x, twice as fast (m/s) as it is high in y (mm),
// its pressure 100 Pa at x = 0 and 10 less each mm.
Fluid::ArchivedFrame boxOfFlow()
{
    Fluid::ArchivedFrame frame;
    const float size[3] = {10.0F, 4.0F, 4.0F};
    for (int k = 0; k < 8; ++k) {
        frame.points.push_back((k & 1) ? size[0] : 0.0F);
        frame.points.push_back((k & 2) ? size[1] : 0.0F);
        frame.points.push_back((k & 4) ? size[2] : 0.0F);
    }
    frame.triangles = {0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                       2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 3, 7, 1, 7, 5};
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 4; ++k) {
                const float x = 0.5F + static_cast<float>(i);
                const float y = 0.5F + static_cast<float>(j);
                const float z = 0.5F + static_cast<float>(k);
                frame.inside.insert(frame.inside.end(),
                                    {x, y, z, 2.0F * y, 0.0F, 0.0F, 100.0F - 10.0F * x});
            }
        }
    }
    return frame;
}
}  // namespace

TEST(FluidSection, aCutAcrossABox)
{
    const auto frame = boxOfFlow();
    const auto section = Fluid::cut(frame, {0.0, 0.0, 2.0}, {0.0, 0.0, 1.0}, 0.5, 1.0);
    const std::size_t count = section.pressures.size();
    ASSERT_EQ(section.points.size(), count * 3);
    ASSERT_EQ(section.velocities.size(), count * 3);
    ASSERT_EQ(section.grid.size(), count * 2);
    // a grid half a mm apart over 10 x 4 mm, give or take its rim
    EXPECT_GE(count, 20U * 8U);
    EXPECT_LE(count, 21U * 9U);
    EXPECT_GE(section.triangles.size(), 19U * 7U * 6U);
    for (std::int32_t at : section.triangles) {
        ASSERT_GE(at, 0);
        ASSERT_LT(static_cast<std::size_t>(at), count);
    }
    bool middle = false;
    for (std::size_t p = 0; p < count; ++p) {
        const float* at = section.points.data() + p * 3;
        EXPECT_GE(at[0], -1e-4F);
        EXPECT_LE(at[0], 10.0001F);
        EXPECT_GE(at[1], -1e-4F);
        EXPECT_LE(at[1], 4.0001F);
        EXPECT_NEAR(at[2], 2.0F, 1e-5F);
        // nowhere past what the cells have
        EXPECT_GE(section.velocities[p * 3], 1.0F - 1e-4F);
        EXPECT_LE(section.velocities[p * 3], 7.0F + 1e-4F);
        EXPECT_NEAR(section.velocities[p * 3 + 1], 0.0F, 1e-6F);
        if (std::abs(at[0] - 5.0F) < 1e-4F && std::abs(at[1] - 2.0F) < 1e-4F) {
            // between cells that are alike about it: their mean
            middle = true;
            EXPECT_NEAR(section.velocities[p * 3], 4.0F, 1e-4F);
            EXPECT_NEAR(section.pressures[p], 50.0F, 1e-3F);
        }
        if (std::abs(at[0] - 2.5F) < 1e-4F && std::abs(at[1] - 1.5F) < 1e-4F) {
            // level with a row of cells: theirs
            EXPECT_NEAR(section.velocities[p * 3], 3.0F, 1e-4F);
            EXPECT_NEAR(section.pressures[p], 75.0F, 1e-3F);
        }
    }
    EXPECT_TRUE(middle);
    // where the plane cuts the faces: the box's four sides, all at z = 2
    ASSERT_GE(section.outline.size(), 4U * 6U);
    for (std::size_t k = 2; k < section.outline.size(); k += 3) {
        EXPECT_NEAR(section.outline[k], 2.0F, 1e-5F);
    }
}

TEST(FluidSection, aSlantedCutAndOnesThatFindNothing)
{
    const auto frame = boxOfFlow();
    const double s = std::sqrt(0.5);
    const auto section = Fluid::cut(frame, {5.0, 2.0, 2.0}, {1.0, 1.0, 0.0}, 0.5, 1.0);
    ASSERT_GT(section.pressures.size(), 20U);
    EXPECT_FALSE(section.triangles.empty());
    for (std::size_t p = 0; p < section.pressures.size(); ++p) {
        const float* at = section.points.data() + p * 3;
        EXPECT_NEAR((at[0] - 5.0) * s + (at[1] - 2.0) * s, 0.0, 1e-4);
        EXPECT_GE(at[1], -1e-4F);
        EXPECT_LE(at[1], 4.0001F);
        EXPECT_GE(at[2], -1e-4F);
        EXPECT_LE(at[2], 4.0001F);
    }
    // a plane that misses the fluid, a normal of no length, no step
    EXPECT_TRUE(Fluid::cut(frame, {0.0, 0.0, 9.0}, {0.0, 0.0, 1.0}, 0.5, 1.0).points.empty());
    EXPECT_TRUE(Fluid::cut(frame, {0.0, 0.0, 2.0}, {0.0, 0.0, 0.0}, 0.5, 1.0).points.empty());
    EXPECT_TRUE(Fluid::cut(frame, {0.0, 0.0, 2.0}, {0.0, 0.0, 1.0}, 0.0, 1.0).points.empty());
    // a frame with no fluid inside kept
    auto bare = frame;
    bare.inside.clear();
    EXPECT_TRUE(Fluid::cut(bare, {0.0, 0.0, 2.0}, {0.0, 0.0, 1.0}, 0.5, 1.0).points.empty());
}
