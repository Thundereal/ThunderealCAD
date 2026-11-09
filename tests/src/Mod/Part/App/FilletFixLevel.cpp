// SPDX-License-Identifier: LGPL-2.1-or-later

#include "gtest/gtest.h"

#include <App/Document.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include "Mod/Part/App/PropertyTopoShape.h"
#include "Mod/Part/App/OCCTFork.h"
#include <src/App/InitApplication.h>

#include "PartTestHelpers.h"

// The OCCT fork's fillet fix level (ChFi3d_SetFixLevel, OCCTFork.h): a
// fillet or chamfer feature holds its _FilletFixLevel on the thread while it
// recomputes, and puts back the level it found; what each level makes is
// src/Mod/Part/parttests/FilletFixLevelTest.py.
class FilletFixLevelTest: public ::testing::Test, public PartTestHelpers::PartTestHelperClass
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

TEST_F(FilletFixLevelTest, recomputePutsTheLevelBack)
{
    if (Part::setOCCTFilletFixLevel(0) < 0) {
        GTEST_SKIP() << "needs the OCCT fork's ChFi3d_SetFixLevel (TKFillet)";
    }
    // Arrange
    auto fillet = _doc->addObject("Part::Fillet", "Fillet");
    auto level = dynamic_cast<App::PropertyInteger*>(fillet->getPropertyByName("_FilletFixLevel"));
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(level->getValue(), Part::OCCTFilletFixLevelLatest);
    auto base = dynamic_cast<App::PropertyLink*>(fillet->getPropertyByName("Base"));
    auto edges = dynamic_cast<Part::PropertyFilletEdges*>(fillet->getPropertyByName("Edges"));
    ASSERT_NE(base, nullptr);
    ASSERT_NE(edges, nullptr);
    base->setValue(_boxes[0]);
    edges->setValue(1, 0.1, 0.1);

    // Act
    _doc->recompute();

    // Assert: made, and the thread back at the level it was at before
    EXPECT_FALSE(fillet->isError());
    EXPECT_EQ(Part::setOCCTFilletFixLevel(0), 0);
}
