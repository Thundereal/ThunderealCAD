// SPDX-License-Identifier: LGPL-2.1-or-later

// The clock the renderer's time-animated effects read, pinned by whoever
// steps time itself (Render::AnimationClock; docs/Animation.md sec 9).
// What the frame does with it is tests/gui/animation-export-clock.py's;
// this is the switch.

#include <gtest/gtest.h>

#include "Gui/Renderer/Renderer.h"

using Render::AnimationClock;

TEST(AnimationClock, itRunsOnTheWallClockUntilPinned)
{
    AnimationClock::release();
    double seconds = -1.0;
    EXPECT_FALSE(AnimationClock::pinned());
    EXPECT_FALSE(AnimationClock::pinned(&seconds));
    // Not pinned: what was asked into is left alone
    EXPECT_DOUBLE_EQ(seconds, -1.0);
}

TEST(AnimationClock, pinnedItReadsTheTimeItWasGiven)
{
    AnimationClock::pin(1.25);
    double seconds = 0.0;
    EXPECT_TRUE(AnimationClock::pinned());
    ASSERT_TRUE(AnimationClock::pinned(&seconds));
    EXPECT_DOUBLE_EQ(seconds, 1.25);
    // Set back: an export starting over
    AnimationClock::pin(0.0);
    ASSERT_TRUE(AnimationClock::pinned(&seconds));
    EXPECT_DOUBLE_EQ(seconds, 0.0);
    AnimationClock::release();
    EXPECT_FALSE(AnimationClock::pinned());
}
