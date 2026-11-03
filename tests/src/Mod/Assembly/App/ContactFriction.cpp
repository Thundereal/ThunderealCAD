// SPDX-License-Identifier: LGPL-2.1-or-later

// The friction table of materials' pairs (Mod/Assembly/ContactFriction.json,
// docs/AssemblyDynamics.md P6): which family FreeCAD's own cards fall in, by
// their name, KindOfMaterial and tags, and the pairs read either way round

#include <gtest/gtest.h>

#include <algorithm>

#include <Mod/Assembly/App/ContactFriction.h>
#include <src/App/InitApplication.h>

using Assembly::FrictionTable;

class ContactFriction: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

// The cards of Mod/Material's Standard library, as they say themselves
TEST_F(ContactFriction, standardCardsFallInTheirFamilies)
{
    const FrictionTable& table = FrictionTable::instance();
    ASSERT_FALSE(table.families().empty());
    struct Card
    {
        const char* name;
        const char* kind;
        std::vector<std::string> tags;
        const char* family;
    };
    const std::vector<Card> cards {
        {"Steel Generic", "", {}, "Steel"},
        {"1C45", "Heat-treatable steel", {}, "Steel"},
        {"Iron Generic", "Iron", {}, "Steel"},
        {"X5CrNi18-10", "Stainless steel", {}, "Stainless steel"},
        {"EN-GJL-250", "Cast iron", {}, "Cast iron"},
        {"Aluminum-6061-T6", "Aluminium", {}, "Aluminium"},
        {"AlMg3F24", "Aluminium", {}, "Aluminium"},
        // The copper alloys by their tags: a red brass, a phosphor bronze,
        // an aluminium bronze -- a bronze, not aluminium
        {"Copper-230", "Annealed", {"C23000", "Brasses", "Red Brass"}, "Brass"},
        {"Copper-510", "Cold drawn", {"C51000", "Phosphor Bronze"}, "Bronze"},
        {"Copper-614", "", {"C61400", "Aluminum Bronze"}, "Bronze"},
        {"Copper-102", "", {"C10200", "Coppers"}, "Copper"},
        {"Ti-6Al-4V (Grade 5)", "Titanium", {}, "Titanium"},
        {"PTFE", "", {}, "PTFE"},
        {"Polyamide 6", "", {}, "Nylon"},
        // An acrylic glass is acrylic, not glass
        {"Acrylic Glass", "", {}, "Acrylic"},
        {"PMMA-Generic", "", {}, "Acrylic"},
        {"Glass", "", {}, "Glass"},
        {"Graphite", "Solid", {}, "Graphite"},
        {"Wood", "", {}, "Wood"},
        // In no family: their pairs run on a set's own coefficients
        {"PC-Molded", "", {}, ""},
        {"ABS", "", {}, ""},
    };
    for (const Card& card : cards) {
        EXPECT_EQ(table.familyOf(card.name, card.kind, card.tags), card.family) << card.name;
    }
}

TEST_F(ContactFriction, pairsReadEitherWayRound)
{
    const FrictionTable& table = FrictionTable::instance();
    const auto* steel = table.pairOf("Steel", "Steel");
    ASSERT_NE(steel, nullptr);
    EXPECT_DOUBLE_EQ(steel->muStatic, 0.30);
    EXPECT_DOUBLE_EQ(steel->muDynamic, 0.25);
    const auto* one = table.pairOf("PTFE", "Steel");
    const auto* other = table.pairOf("Steel", "PTFE");
    ASSERT_NE(one, nullptr);
    EXPECT_EQ(one, other);
    EXPECT_EQ(table.pairOf("Steel", "Wood"), nullptr);
}

// Every pair of families the table has, each coefficient as a law takes it:
// the dynamic no more than the static
TEST_F(ContactFriction, pairsAreOfItsFamilies)
{
    const FrictionTable& table = FrictionTable::instance();
    auto known = [&](const std::string& name) {
        return std::ranges::any_of(table.families(), [&](const auto& f) {
            return f.name == name;
        });
    };
    for (const auto& pair : table.pairs()) {
        EXPECT_TRUE(known(pair.a)) << pair.a;
        EXPECT_TRUE(known(pair.b)) << pair.b;
        EXPECT_GT(pair.muStatic, 0.0) << pair.a << " " << pair.b;
        EXPECT_LE(pair.muDynamic, pair.muStatic) << pair.a << " " << pair.b;
        EXPECT_FALSE(pair.source.empty()) << pair.a << " " << pair.b;
    }
}

TEST_F(ContactFriction, aMissingFileAddsNothing)
{
    const FrictionTable table = FrictionTable::read({"no/such/ContactFriction.json"});
    EXPECT_TRUE(table.families().empty());
    EXPECT_TRUE(table.pairs().empty());
}
