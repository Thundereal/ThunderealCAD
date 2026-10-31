// SPDX-License-Identifier: LGPL-2.1-or-later

// Sheet metal's geometry layer, M0 (docs/SheetMetal.md sec 4.1, 4.6 and 9):
// the flat length of a bend by every allowance method against worked values,
// the tables interpolated the way the addon's is not past its second row; and
// the cylindrical bend's fold map -- the bend's start where it was, the
// neutral fibre's length kept, a round trip back, and the far end of an L
// where the hand sum of its legs and the allowance puts it.

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <Base/Exception.h>
#include <Mod/PartDesign/App/SheetMetal/BendAllowance.h>
#include <Mod/PartDesign/App/SheetMetal/Fold.h>

using namespace PartDesign::SheetMetal;

namespace
{

constexpr double pi = std::numbers::pi;

double deg(double a)
{
    return a * pi / 180;
}

Allowance kFactor(double k)
{
    Allowance a;
    a.method = Allowance::Method::KFactor;
    a.k = k;
    return a;
}

}  // namespace

// The K-factor's arc on the neutral fibre: BA = A (R + K T)
TEST(SheetMetalAllowance, kFactor)
{
    const Bend bend {2, 2, deg(90)};
    EXPECT_NEAR(bendAllowance(bend, kFactor(0.44)), pi / 2 * (2 + 0.44 * 2), 1e-12);
    EXPECT_NEAR(bendAllowance({1.5, 3, deg(30)}, kFactor(0.33)), deg(30) * (3 + 0.33 * 1.5), 1e-12);
    EXPECT_NEAR(bendAllowance({2, 2, 0}, kFactor(0.44)), 0, 1e-12);
}

// DIN's K is measured on half the thickness: 0.88 DIN is 0.44 ANSI
TEST(SheetMetalAllowance, kFactorDIN)
{
    Allowance din;
    din.method = Allowance::Method::KFactorDIN;
    din.k = 0.88;
    const Bend bend {2, 2, deg(90)};
    EXPECT_NEAR(bendAllowance(bend, din), bendAllowance(bend, kFactor(0.44)), 1e-12);
}

// DIN 6935: k = 0.65 + 0.5 lg(R/T) below R/T = 5, 1 from there, never below
// 0; the neutral fibre at R + k T / 2
TEST(SheetMetalAllowance, din6935)
{
    EXPECT_NEAR(din6935K(2, 2), 0.65 / 2, 1e-12);
    EXPECT_NEAR(din6935K(2, 4), (0.65 + 0.5 * std::log10(2.0)) / 2, 1e-12);
    EXPECT_NEAR(din6935K(2, 10), 0.5, 1e-12);
    EXPECT_NEAR(din6935K(2, 40), 0.5, 1e-12);
    EXPECT_NEAR(din6935K(2, 0.02), 0, 1e-12);
    Allowance din;
    din.method = Allowance::Method::DIN6935;
    EXPECT_NEAR(bendAllowance({2, 2, deg(90)}, din), pi / 2 * (2 + 0.325 * 2), 1e-12);
}

// Creo's Y-factor is K in another form: K = 2 Y / pi
TEST(SheetMetalAllowance, yFactor)
{
    EXPECT_NEAR(kFromY(0.5), 1 / pi, 1e-12);
    EXPECT_NEAR(kFromY(pi / 4), 0.5, 1e-12);
}

// Outside setback and bend deduction: OSSB = (R + T) tan(A / 2), BD = 2 OSSB - BA
TEST(SheetMetalAllowance, deduction)
{
    const Bend bend {2, 2, deg(90)};
    EXPECT_NEAR(outsideSetback(bend), 4, 1e-12);
    EXPECT_NEAR(bendDeduction(bend, kFactor(0.44)), 8 - pi / 2 * 2.88, 1e-12);
    const Bend open {2, 2, deg(60)};
    EXPECT_NEAR(outsideSetback(open), 4 * std::tan(deg(30)), 1e-12);
    // A hem has no setback to speak of: a deduction there is refused
    EXPECT_THROW(outsideSetback({2, 2, deg(180)}), Base::ValueError);
}

// K by R/T, the addon's material sheet with the README's table: linear
// between rows, past the second one too (the addon gives 1.605 at 50, survey
// B2); outside the table the end row's value, and said so
TEST(SheetMetalAllowance, kTable)
{
    Allowance a;
    a.method = Allowance::Method::KTable;
    a.table.axes = {Table::Axis::RadiusOverThickness};
    a.table.keys = {{1, 3, 99}};
    a.table.values = {0.38, 0.43, 0.5};
    bool inside = false;
    EXPECT_NEAR(kOf({2, 100, deg(90)}, a, &inside), 0.43 + (50.0 - 3) / (99 - 3) * 0.07, 1e-12);
    EXPECT_TRUE(inside);
    EXPECT_NEAR(kOf({2, 4, deg(90)}, a, &inside), 0.405, 1e-12);
    EXPECT_TRUE(inside);
    EXPECT_NEAR(kOf({2, 1, deg(90)}, a, &inside), 0.38, 1e-12);
    EXPECT_FALSE(inside);
    EXPECT_NEAR(kOf({2, 400, deg(90)}, a, &inside), 0.5, 1e-12);
    EXPECT_FALSE(inside);
    const Bend bend {2, 100, deg(90)};
    EXPECT_NEAR(bendAllowance(bend, a), pi / 2 * (100 + kOf(bend, a) * 2), 1e-12);
}

// An allowance table in thickness, radius and angle: interpolated linearly in
// each, so a table of a function linear in each is exact anywhere inside
TEST(SheetMetalAllowance, allowanceTable)
{
    auto f = [](double t, double r, double a) {
        return 1 + 2 * t + 3 * r + 4 * a + 0.5 * t * r * a;
    };
    Allowance al;
    al.method = Allowance::Method::AllowanceTable;
    al.table.axes = {Table::Axis::Thickness, Table::Axis::Radius, Table::Axis::Angle};
    al.table.keys = {{1, 2, 4}, {1, 3}, {deg(45), deg(90), deg(135)}};
    for (double t : al.table.keys[0]) {
        for (double r : al.table.keys[1]) {
            for (double a : al.table.keys[2]) {
                al.table.values.push_back(f(t, r, a));
            }
        }
    }
    bool inside = false;
    const Bend bend {3, 2.5, deg(100)};
    EXPECT_NEAR(bendAllowance(bend, al, &inside), f(3, 2.5, deg(100)), 1e-12);
    EXPECT_TRUE(inside);
    // on a row exactly
    EXPECT_NEAR(bendAllowance({2, 3, deg(90)}, al), f(2, 3, deg(90)), 1e-12);
}

// A deduction table gives the allowance back through the setback
TEST(SheetMetalAllowance, deductionTable)
{
    Allowance bd;
    bd.method = Allowance::Method::DeductionTable;
    bd.table.axes = {Table::Axis::Angle};
    bd.table.keys = {{deg(45), deg(90)}};
    bd.table.values = {1.0, 3.5};
    const Bend bend {2, 2, deg(90)};
    EXPECT_NEAR(bendAllowance(bend, bd), 2 * outsideSetback(bend) - 3.5, 1e-12);
    EXPECT_NEAR(bendDeduction(bend, bd), 3.5, 1e-12);
}

// A table that does not say what it means is refused, not guessed at
TEST(SheetMetalAllowance, badTables)
{
    Allowance a;
    a.method = Allowance::Method::AllowanceTable;
    EXPECT_THROW(bendAllowance({2, 2, deg(90)}, a), Base::ValueError);  // empty
    a.table.axes = {Table::Axis::Radius};
    a.table.keys = {{3, 1}};
    a.table.values = {1, 2};
    EXPECT_THROW(bendAllowance({2, 2, deg(90)}, a), Base::ValueError);  // not ascending
    a.table.keys = {{1, 3}};
    a.table.values = {1, 2, 3};
    EXPECT_THROW(bendAllowance({2, 2, deg(90)}, a), Base::ValueError);  // size
    a.table.axes = {Table::Axis::Radius, Table::Axis::Radius};
    a.table.keys = {{1, 3}, {1, 3}};
    a.table.values = {1, 2, 3, 4};
    EXPECT_THROW(bendAllowance({2, 2, deg(90)}, a), Base::ValueError);  // an axis twice
}

// A card's bend table is rows in any order, (thickness, radius, angle,
// value); made a table, an axis with one key is kept, and the rows have to
// fill the grid
TEST(SheetMetalAllowance, tableFromRows)
{
    std::vector<Table::Row> rows;
    for (double t : {2.0, 1.0}) {
        for (double r : {1.0, 3.0}) {
            for (double a : {deg(90), deg(45)}) {
                rows.push_back({t, r, a, 10 * t + r + a});
            }
        }
    }
    const Table table = Table::fromRows(rows);
    ASSERT_EQ(table.axes.size(), 3u);
    EXPECT_EQ(table.keys[0], (std::vector<double>{1, 2}));
    EXPECT_EQ(table.keys[1], (std::vector<double>{1, 3}));
    EXPECT_EQ(table.keys[2], (std::vector<double>{deg(45), deg(90)}));
    for (const auto& row : rows) {
        EXPECT_NEAR(table.at({row.thickness, row.radius, row.angle}), row.value, 1e-12);
    }
    EXPECT_NEAR(table.at({1.5, 2, deg(67.5)}), 15 + 2 + deg(67.5), 1e-12);

    // one thickness only: its axis has the one key
    std::vector<Table::Row> one(rows.begin() + 4, rows.end());
    EXPECT_EQ(Table::fromRows(one).keys[0], (std::vector<double>{1}));

    // a corner missing, a corner twice, no rows
    std::vector<Table::Row> missing(rows.begin(), rows.end() - 1);
    EXPECT_THROW(Table::fromRows(missing), Base::ValueError);
    std::vector<Table::Row> twice = rows;
    twice.back() = twice.front();
    EXPECT_THROW(Table::fromRows(twice), Base::ValueError);
    EXPECT_THROW(Table::fromRows({}), Base::ValueError);
}

namespace
{

/// A plate in z <= 0 (its inner skin z = 0 for a bend up), running along +X
/// into a bend about an axis along -Y at (0, 0, R): the sheet turns up
CylBend upBend(double radius, double thickness, double angle, double k)
{
    CylBend bend;
    bend.frame = gp_Ax3(gp_Pnt(0, 0, radius), gp_Dir(0, -1, 0), gp_Dir(0, 0, -1));
    bend.radius = radius;
    bend.thickness = thickness;
    bend.angle = angle;
    bend.k = k;
    return bend;
}

/// A point of the bend's material: at angle a into it, depth d below the
/// inner skin, y along the axis
gp_Pnt inBend(const CylBend& b, double a, double d, double y)
{
    const double r = b.radius + d;
    return {r * std::sin(a), y, b.radius - r * std::cos(a)};
}

}  // namespace

// Where the bend starts, the flat and the fold agree
TEST(SheetMetalFold, startStays)
{
    const CylBend b = upBend(2, 2, deg(90), 0.44);
    for (double d : {0.0, 1.0, 2.0}) {
        const gp_Pnt p = inBend(b, 0, d, 3);
        EXPECT_LT(b.toFlat(p).Distance(p), 1e-12);
    }
    // the material behind the bend, the start wall, stays where it is too
    EXPECT_TRUE(b.side(gp_Pnt(-5, 0, -1)) < 0);
}

// Every fibre lies flat at the neutral fibre's length: A (R + K T)
TEST(SheetMetalFold, neutralLength)
{
    const CylBend b = upBend(2, 2, deg(90), 0.44);
    const double neutral = 2 + 0.44 * 2;
    for (double d : {0.0, 0.88, 2.0}) {
        const gp_Pnt s = b.toFlat(inBend(b, deg(20), d, 0));
        const gp_Pnt e = b.toFlat(inBend(b, deg(70), d, 0));
        EXPECT_NEAR(s.Distance(e), deg(50) * neutral, 1e-12) << d;
        // and at its depth below the inner skin
        EXPECT_NEAR(s.Z(), -d, 1e-12);
    }
}

// Flat and back again
TEST(SheetMetalFold, roundTrip)
{
    const CylBend b = upBend(1.5, 3, deg(120), 0.3);
    for (double a : {0.0, 0.3, 1.0, 2.0}) {
        for (double d : {0.0, 1.2, 3.0}) {
            const gp_Pnt p = inBend(b, a, d, -4);
            EXPECT_LT(b.toFolded(b.toFlat(p)).Distance(p), 1e-12) << a << " " << d;
        }
    }
}

// The wall after the bend moves rigidly, continuing the bend's last fibres:
// an L with legs 10 and 20 (inside lengths, from the bend's tangent lines)
// lies flat 10 + BA + 20 long on its inner skin
TEST(SheetMetalFold, anL)
{
    const double R = 2, T = 2, K = 0.44, L1 = 10, L2 = 20;
    const CylBend b = upBend(R, T, deg(90), K);
    const gp_Trsf unbend = b.unbend();
    // the end of the bend, on its inner and outer skin, against the map
    for (double d : {0.0, T}) {
        const gp_Pnt end = inBend(b, deg(90), d, 1);
        EXPECT_LT(end.Transformed(unbend).Distance(b.toFlat(end)), 1e-12) << d;
    }
    // the second leg's far end, inner skin: up at x = R, z = R + L2
    const gp_Pnt tip = gp_Pnt(R, 0, R + L2).Transformed(unbend);
    const double ba = bendAllowance({T, R, deg(90)}, kFactor(K));
    EXPECT_NEAR(tip.X() - (-L1), L1 + ba + L2, 1e-12);
    EXPECT_NEAR(tip.Z(), 0, 1e-12);
    // folding it back is the inverse
    EXPECT_LT(tip.Transformed(b.refold()).Distance(gp_Pnt(R, 0, R + L2)), 1e-12);
    // which side of the bend a point is: before it, in it, after it
    EXPECT_LT(b.side(gp_Pnt(-1, 0, -1)), 0);
    EXPECT_EQ(b.side(inBend(b, deg(45), 1, 0)), 0);
    EXPECT_GT(b.side(gp_Pnt(R + 1, 0, R + 5)), 0);
}
