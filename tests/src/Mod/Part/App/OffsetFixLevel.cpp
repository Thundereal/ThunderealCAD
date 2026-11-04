// SPDX-License-Identifier: LGPL-2.1-or-later

#include "gtest/gtest.h"

#include <App/Document.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include "Mod/Part/App/OCCTFork.h"
#include <src/App/InitApplication.h>

#include "PartTestHelpers.h"

// The OCCT fork's offset fix level (BRepOffset_SetFixLevel, OCCTFork.h): a
// feature that offsets holds its _OffsetFixLevel on the thread while it
// recomputes, and puts back the level it found. No level makes a shape
// differently yet (the fork's Thickness.md sec 32), so nothing a test sees
// from Python tells the levels apart: what the thread's level is after a
// recompute is asked of the fork here.
class OffsetFixLevelTest: public ::testing::Test, public PartTestHelpers::PartTestHelperClass
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        createTestDoc();
    }
};

TEST_F(OffsetFixLevelTest, recomputePutsTheLevelBack)
{
    if (Part::setOCCTOffsetFixLevel(0) < 0) {
        GTEST_SKIP() << "needs the OCCT fork's BRepOffset_SetFixLevel (TKOffset)";
    }
    // Arrange
    auto thickness = _doc->addObject("Part::Thickness", "Thickness");
    auto level = dynamic_cast<App::PropertyInteger*>(thickness->getPropertyByName("_OffsetFixLevel"));
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(level->getValue(), Part::OCCTOffsetFixLevelLatest);
    auto faces = dynamic_cast<App::PropertyLinkSub*>(thickness->getPropertyByName("Faces"));
    auto value = dynamic_cast<App::PropertyQuantity*>(thickness->getPropertyByName("Value"));
    ASSERT_NE(faces, nullptr);
    ASSERT_NE(value, nullptr);
    faces->setValue(_boxes[0], {"Face6"});
    value->setValue(0.1);

    // Act
    _doc->recompute();

    // Assert: made, and the thread back at the level it was at before
    EXPECT_FALSE(thickness->isError());
    EXPECT_EQ(Part::setOCCTOffsetFixLevel(0), 0);
}
