// SPDX-License-Identifier: LGPL-2.1-or-later

// A "$$Name." subname step names the object held by the link property Name
// (DocumentObject::resolvePropertyStep()).

#include "gtest/gtest.h"

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentObject.h"
#include "App/DocumentObjectGroup.h"
#include "App/DocumentObserver.h"
#include "App/Link.h"
#include "App/Part.h"
#include "App/PropertyLinks.h"
#include "Base/Matrix.h"
#include <src/App/InitApplication.h>

// NOLINTBEGIN(readability-magic-numbers)

class PropertyStepTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("test");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");

        // Part holds its Elements group through a property, not in Group
        part = _doc->addObject<App::Part>("Part");
        elements = _doc->addObject<App::DocumentObjectGroup>("Elements001");
        elements->Label.setValue("Elements");
        pinEnd = _doc->addObject<App::DocumentObjectGroup>("Feature");
        pinEnd->Label.setValue("PinEnd");
        elements->addObject(pinEnd);
        auto prop = Base::freecad_dynamic_cast<App::PropertyLink>(
            part->addDynamicProperty("App::PropertyLink", "Elements"));
        ASSERT_NE(prop, nullptr);
        prop->setValue(elements);
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_docName.c_str());
    }

    App::Document* doc()
    {
        return _doc;
    }

    App::Part* part {};
    App::DocumentObjectGroup* elements {};
    App::DocumentObjectGroup* pinEnd {};

private:
    std::string _docName;
    App::Document* _doc {};
};

TEST_F(PropertyStepTest, stepResolvesThroughTheProperty)
{
    EXPECT_EQ(part->getSubObject("$$Elements."), elements);
    EXPECT_EQ(part->getSubObject("$$Elements.$PinEnd."), pinEnd);
    EXPECT_EQ(part->getSubObject("$$Elements.Feature."), pinEnd);
}

TEST_F(PropertyStepTest, stepIsNotALabel)
{
    // The group's label is free, and its old label no longer reaches it
    elements->Label.setValue("Renamed");
    EXPECT_EQ(part->getSubObject("$$Elements.$PinEnd."), pinEnd);
    EXPECT_EQ(part->getSubObject("$Renamed.$PinEnd."), nullptr);
}

TEST_F(PropertyStepTest, emptyPropertyGivesNothing)
{
    auto prop = Base::freecad_dynamic_cast<App::PropertyLink>(
        part->getPropertyByName("Elements"));
    prop->setValue(nullptr);
    EXPECT_EQ(part->getSubObject("$$Elements."), nullptr);
}

TEST_F(PropertyStepTest, withoutPropertyStepIsTheOldLabel)
{
    // A file saved before "$$" had a meaning could only mean the label "$X"
    auto legacy = doc()->addObject<App::DocumentObjectGroup>("Legacy");
    legacy->Label.setValue("$Legacy");
    part->addObject(legacy);
    EXPECT_EQ(part->getSubObject("$$Legacy."), legacy);
}

TEST_F(PropertyStepTest, stepTakesTheOwnersPlacement)
{
    part->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    Base::Matrix4D mat;
    EXPECT_EQ(part->getSubObject("$$Elements.", nullptr, &mat), elements);
    EXPECT_DOUBLE_EQ(mat[0][3], 10.0);

    Base::Matrix4D noTransform;
    part->getSubObject("$$Elements.", nullptr, &noTransform, false);
    EXPECT_DOUBLE_EQ(noTransform[0][3], 0.0);
}

TEST_F(PropertyStepTest, linkPassesTheStepToItsLinkedObject)
{
    auto link = doc()->addObject<App::Link>("Link");
    link->LinkedObject.setValue(part);
    EXPECT_EQ(link->getSubObject("$$Elements.$PinEnd."), pinEnd);
    // A link's own property
    EXPECT_EQ(link->getSubObject("$$LinkedObject."), part);
}

TEST_F(PropertyStepTest, subObjectListWalksTheStep)
{
    std::vector<int> sizes;
    auto objs = part->getSubObjectList("$$Elements.$PinEnd.", &sizes);
    ASSERT_EQ(objs.size(), 3u);
    EXPECT_EQ(objs[1], elements);
    EXPECT_EQ(objs[2], pinEnd);
}

TEST_F(PropertyStepTest, normalizeKeepsTheStep)
{
    App::SubObjectT sobjT(part, "$$Elements.$PinEnd.");
    auto res = sobjT.normalized();
    EXPECT_EQ(res.getSubName(), "$$Elements.Feature.");
}

TEST_F(PropertyStepTest, relabelLeavesTheStepAlone)
{
    // Relabelling the group "Elements" must not rewrite "$$Elements."
    EXPECT_EQ(App::PropertyLinkBase::updateLabelReference(part,
                                                         "$$Elements.$PinEnd.",
                                                         elements,
                                                         "$Elements.",
                                                         "Renamed"),
              "");
}

TEST_F(PropertyStepTest, relabelUpdatesTheLabelAfterTheStep)
{
    auto owner = doc()->addObject<App::DocumentObjectGroup>("Owner");
    auto ref = Base::freecad_dynamic_cast<App::PropertyLinkSub>(
        owner->addDynamicProperty("App::PropertyLinkSub", "Ref"));
    ASSERT_NE(ref, nullptr);
    ref->setValue(part, std::vector<std::string> {"$$Elements.$PinEnd."});

    pinEnd->Label.setValue("End2");
    ASSERT_EQ(ref->getSubValues().size(), 1u);
    EXPECT_EQ(ref->getSubValues()[0], "$$Elements.$End2.");

    elements->Label.setValue("Renamed");
    EXPECT_EQ(ref->getSubValues()[0], "$$Elements.$End2.");
}

// NOLINTEND(readability-magic-numbers)
