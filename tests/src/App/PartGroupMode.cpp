// SPDX-License-Identifier: LGPL-2.1-or-later

// App::Part's GroupMode and its aux groups (docs/AssemblyObjectModel.md
// sec 2 and 3), and what it exports without a view provider.

#include "gtest/gtest.h"

#include <algorithm>

#include "App/Application.h"
#include "App/AuxGroup.h"
#include "App/Document.h"
#include "App/DocumentObject.h"
#include "App/FeatureTest.h"
#include "App/Link.h"
#include "App/Part.h"
#include "Base/Exception.h"
#include <src/App/InitApplication.h>

// NOLINTBEGIN(readability-magic-numbers)

class PartGroupModeTest: public ::testing::Test
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
    }

    void TearDown() override
    {
        for (auto& name : _others)
            App::GetApplication().closeDocument(name.c_str());
        App::GetApplication().closeDocument(_docName.c_str());
    }

    App::Document* doc()
    {
        return _doc;
    }

    App::Document* otherDoc()
    {
        _others.push_back(App::GetApplication().getUniqueDocumentName("other"));
        return App::GetApplication().newDocument(_others.back().c_str(), "testUser");
    }

    App::Part* part(const char* name, long mode = App::Part::GroupModeNormal)
    {
        auto obj = doc()->addObject<App::Part>(name);
        obj->GroupMode.setValue(mode);
        return obj;
    }

    App::FeatureTest* feature(const char* name, App::Document* in = nullptr)
    {
        return static_cast<App::FeatureTest*>((in ? in : doc())->addObject("App::FeatureTest", name));
    }

    static App::Link* asOwnedLink(App::Part* owner, App::DocumentObject* obj)
    {
        auto link = Base::freecad_dynamic_cast<App::Link>(obj);
        if (!link || !owner->isOwnedLink(link))
            return nullptr;
        return link;
    }

private:
    std::string _docName;
    std::vector<std::string> _others;
    App::Document* _doc {};
};

TEST_F(PartGroupModeTest, normalMovesTheObjectOutOfItsPart)
{
    auto a = part("A");
    auto b = part("B");
    auto obj = feature("Obj");
    a->addObject(obj);

    auto added = b->addObject(obj);

    ASSERT_EQ(added.size(), 1u);
    EXPECT_EQ(added[0], obj);
    EXPECT_TRUE(b->hasObject(obj));
    EXPECT_FALSE(a->hasObject(obj));
}

TEST_F(PartGroupModeTest, autoMovesInAFreeObject)
{
    auto p = part("P", App::Part::GroupModeAuto);
    auto obj = feature("Obj");

    auto added = p->addObject(obj);

    ASSERT_EQ(added.size(), 1u);
    EXPECT_EQ(added[0], obj);
    EXPECT_TRUE(p->hasObject(obj));
}

TEST_F(PartGroupModeTest, autoLinksAnObjectAnotherPartHolds)
{
    auto a = part("A");
    auto p = part("P", App::Part::GroupModeAuto);
    auto obj = feature("Obj");
    a->addObject(obj);

    auto added = p->addObject(obj);

    ASSERT_EQ(added.size(), 1u);
    auto link = asOwnedLink(p, added[0]);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->getLinkedObject(false), obj);
    EXPECT_TRUE(a->hasObject(obj));
    EXPECT_TRUE(p->hasObject(link));
    // Named after the object; the document keeps labels unique ("Obj001")
    EXPECT_EQ(link->Label.getStrValue().rfind(obj->Label.getStrValue(), 0), 0u);
}

TEST_F(PartGroupModeTest, autoLinksAnObjectOfAnotherDocument)
{
    auto p = part("P", App::Part::GroupModeAuto);
    auto obj = feature("Obj", otherDoc());

    auto added = p->addObject(obj);

    ASSERT_EQ(added.size(), 1u);
    auto link = asOwnedLink(p, added[0]);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->getLinkedObject(false), obj);
}

TEST_F(PartGroupModeTest, autoLinksASecondInstance)
{
    auto p = part("P", App::Part::GroupModeAuto);
    auto obj = feature("Obj");
    p->addObject(obj);

    auto added = p->addObject(obj);

    ASSERT_EQ(added.size(), 1u);
    auto link = asOwnedLink(p, added[0]);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->getLinkedObject(false), obj);
    EXPECT_TRUE(p->hasObject(obj));
}

TEST_F(PartGroupModeTest, autoLinksWhenADependencyBelongsElsewhere)
{
    // Moving Obj in would drag Dep out of A
    auto a = part("A");
    auto p = part("P", App::Part::GroupModeAuto);
    auto dep = feature("Dep");
    a->addObject(dep);
    auto obj = feature("Obj");
    obj->Link.setValue(dep);

    auto added = p->addObject(obj);

    ASSERT_EQ(added.size(), 1u);
    EXPECT_NE(asOwnedLink(p, added[0]), nullptr);
    EXPECT_TRUE(a->hasObject(dep));
    EXPECT_FALSE(p->hasObject(obj));
}

TEST_F(PartGroupModeTest, linkModeAlwaysLinksAndHidesATopLevelOriginal)
{
    auto p = part("P", App::Part::GroupModeLink);
    auto obj = feature("Obj");

    auto added = p->addObject(obj);

    ASSERT_EQ(added.size(), 1u);
    auto link = asOwnedLink(p, added[0]);
    ASSERT_NE(link, nullptr);
    EXPECT_FALSE(p->hasObject(obj));
    EXPECT_FALSE(obj->Visibility.getValue());
}

TEST_F(PartGroupModeTest, aNewLinkStartsWhereTheObjectIs)
{
    auto a = part("A");
    a->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    auto p = part("P", App::Part::GroupModeAuto);
    p->Placement.setValue(Base::Placement(Base::Vector3d(0, 0, 3), Base::Rotation()));
    auto obj = feature("Obj");
    obj->Placement.setValue(Base::Placement(Base::Vector3d(0, 5, 0), Base::Rotation()));
    a->addObject(obj);

    auto added = p->addObject(obj);

    auto link = asOwnedLink(p, added.at(0));
    ASSERT_NE(link, nullptr);
    auto pos = link->Placement.getValue().getPosition();
    EXPECT_DOUBLE_EQ(pos.x, 10.0);
    EXPECT_DOUBLE_EQ(pos.y, 5.0);
    EXPECT_DOUBLE_EQ(pos.z, -3.0);
}

TEST_F(PartGroupModeTest, removingAnOwnedLinkDeletesIt)
{
    auto p = part("P", App::Part::GroupModeLink);
    auto obj = feature("Obj");
    auto link = p->addObject(obj).at(0);
    std::string name = link->getNameInDocument();

    p->removeObject(link);

    EXPECT_EQ(doc()->getObject(name.c_str()), nullptr);
    EXPECT_NE(doc()->getObject("Obj"), nullptr);
}

TEST_F(PartGroupModeTest, removingADirectChildMovesItOut)
{
    auto p = part("P", App::Part::GroupModeAuto);
    auto obj = feature("Obj");
    p->addObject(obj);

    p->removeObject(obj);

    EXPECT_FALSE(p->hasObject(obj));
    EXPECT_EQ(doc()->getObject("Obj"), obj);
}

TEST_F(PartGroupModeTest, anOwnedLinkMovedToAnotherPartIsKeptAndDisowned)
{
    auto p = part("P", App::Part::GroupModeLink);
    auto b = part("B");
    auto obj = feature("Obj");
    auto link = Base::freecad_dynamic_cast<App::Link>(p->addObject(obj).at(0));
    ASSERT_NE(link, nullptr);
    std::string name = link->getNameInDocument();

    b->addObject(link);

    ASSERT_EQ(doc()->getObject(name.c_str()), link);
    EXPECT_TRUE(b->hasObject(link));
    EXPECT_FALSE(p->hasObject(link));
    EXPECT_EQ(link->_LinkOwner.getValue(), 0);
}

TEST_F(PartGroupModeTest, anAuxGroupIsNeverLinked)
{
    auto p = part("P", App::Part::GroupModeLink);
    auto group = static_cast<App::AuxGroup*>(doc()->addObject("App::PartAuxGroup", "Parts"));

    auto added = p->addObject(group);

    ASSERT_EQ(added.size(), 1u);
    EXPECT_EQ(added[0], group);
}

TEST_F(PartGroupModeTest, thePartsGroupListsTheComponents)
{
    auto p = part("P");
    auto obj = feature("Obj");
    auto obj2 = feature("Obj2");
    p->addObject(obj);
    auto group = static_cast<App::AuxGroup*>(doc()->addObject("App::PartAuxGroup", "Parts"));
    group->Role.setValue(App::Part::PartsRole);
    auto children = p->Group.getValues();
    children.push_back(group);
    p->Group.setValues(children);
    group->setOwner(p);

    EXPECT_EQ(group->Group.getValues(), std::vector<App::DocumentObject*>({obj}));
    EXPECT_EQ(p->getAuxGroup(App::Part::PartsRole), group);

    p->addObject(obj2);
    EXPECT_EQ(group->Group.getValues(), std::vector<App::DocumentObject*>({obj, obj2}));
}

// The exported children, without a view provider to build the list
static std::vector<App::DocumentObject*> exported(App::Part* p)
{
    return p->_ExportChildren.getValues();
}

TEST_F(PartGroupModeTest, thePartsGroupLeavesTheComponentsExported)
{
    auto p = part("P");
    auto obj = feature("Obj");
    p->addObject(obj);
    auto group = p->addPartsGroup();
    ASSERT_TRUE(group);
    // A view only refers to the components
    EXPECT_EQ(group->Group.getScope(), App::LinkScope::Global);

    // A change of Group after the group came builds the list again
    auto obj2 = feature("Obj2");
    p->addObject(obj2);

    ASSERT_EQ(group->Group.getValues(), std::vector<App::DocumentObject*>({obj, obj2}));
    EXPECT_EQ(exported(p), std::vector<App::DocumentObject*>({obj, group, obj2}));
    auto subs = p->getSubObjects();
    EXPECT_NE(std::find(subs.begin(), subs.end(), "Obj."), subs.end());
    EXPECT_NE(std::find(subs.begin(), subs.end(), "Obj2."), subs.end());
}

TEST_F(PartGroupModeTest, theElementsGroupOwnsItsChildren)
{
    auto p = part("P");
    auto elements = p->getElementsGroup(true);
    ASSERT_TRUE(elements);
    ASSERT_TRUE(elements->isOwning());
    EXPECT_EQ(elements->Group.getScope(), App::LinkScope::Local);
}

TEST_F(PartGroupModeTest, aChildTakenInThroughALocalLinkIsNotExported)
{
    auto p = part("P");
    auto obj = feature("Obj");
    auto base = feature("Base");
    obj->Link.setValue(base);
    p->addObjects({obj, base});

    EXPECT_EQ(exported(p), std::vector<App::DocumentObject*>({obj}));
}

TEST_F(PartGroupModeTest, aChildAGlobalLinkRefersToIsExported)
{
    auto p = part("P");
    auto obj = feature("Obj");
    auto other = feature("Other");
    auto ref = static_cast<App::PropertyLinkGlobal*>(
        obj->addDynamicProperty("App::PropertyLinkGlobal", "Ref"));
    ref->setValue(other);
    p->addObjects({obj, other});

    EXPECT_EQ(exported(p), std::vector<App::DocumentObject*>({obj, other}));
}

// A change of Group checks only the members it adds, unless one left: the
// checks must still catch both
TEST_F(PartGroupModeTest, aMemberAddedLaterLinkingOutOfThePartIsTakenOut)
{
    auto p = part("P");
    auto q = part("Q");
    auto a = feature("A");
    auto b = feature("B");
    auto c = feature("C");
    q->addObject(b);
    p->Group.setValues({a});
    c->Link.setValue(b);

    EXPECT_THROW(p->Group.setValues({a, c}), Base::RuntimeError);

    EXPECT_EQ(p->Group.getValues(), std::vector<App::DocumentObject*>({a}));
}

TEST_F(PartGroupModeTest, aMemberLeftLinkingToOneThatLeftIsTakenOut)
{
    auto p = part("P");
    auto x = feature("X");
    auto y = feature("Y");
    y->Link.setValue(x);
    p->Group.setValues({x, y});
    ASSERT_EQ(p->Group.getValues(), std::vector<App::DocumentObject*>({x, y}));

    EXPECT_THROW(p->Group.setValues({y}), Base::RuntimeError);

    EXPECT_TRUE(p->Group.getValues().empty());
}

TEST_F(PartGroupModeTest, aLinkSetAfterIsFollowedOnRecompute)
{
    auto p = part("P");
    auto obj = feature("Obj");
    auto base = feature("Base");
    p->addObjects({obj, base});
    ASSERT_EQ(exported(p), std::vector<App::DocumentObject*>({obj, base}));

    obj->Link.setValue(base);
    doc()->recompute();

    EXPECT_EQ(exported(p), std::vector<App::DocumentObject*>({obj}));
}

TEST_F(PartGroupModeTest, thePartsGroupRanksLast)
{
    auto parts = static_cast<App::AuxGroup*>(doc()->addObject("App::PartAuxGroup", "Parts"));
    parts->Role.setValue(App::Part::PartsRole);
    auto user = static_cast<App::AuxGroup*>(doc()->addObject("App::PartAuxGroup", "Group"));
    auto views = static_cast<App::AuxGroup*>(doc()->addObject("App::PartAuxGroup", "Views"));
    views->Role.setValue("Views");
    auto elements = static_cast<App::AuxGroup*>(doc()->addObject("App::PartAuxGroup", "Elements"));
    elements->Role.setValue("Elements");

    EXPECT_LT(App::Part::getAuxGroupRank(elements), App::Part::getAuxGroupRank(views));
    EXPECT_LT(App::Part::getAuxGroupRank(views), App::Part::getAuxGroupRank(user));
    EXPECT_LT(App::Part::getAuxGroupRank(user), App::Part::getAuxGroupRank(parts));
}

// NOLINTEND(readability-magic-numbers)
