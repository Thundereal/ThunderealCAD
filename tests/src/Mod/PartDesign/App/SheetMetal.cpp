// SPDX-License-Identifier: LGPL-2.1-or-later

// Sheet metal's geometry layer, M0 (docs/SheetMetal.md sec 4.1, 4.6 and 9):
// the flat length of a bend by every allowance method against worked values,
// the tables interpolated the way the addon's is not past its second row; and
// the cylindrical bend's fold map -- the bend's start where it was, the
// neutral fibre's length kept, a round trip back, and the far end of an L
// where the hand sum of its legs and the allowance puts it. The sheet model:
// which wall or bend a point is in, and a U laid flat from either end.

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <Base/Exception.h>
#include <Mod/PartDesign/App/SheetMetal/BendAllowance.h>
#include <Mod/PartDesign/App/SheetMetal/Fold.h>
#include <Mod/PartDesign/App/SheetMetal/Model.h>

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

namespace
{

/// A U of three walls and two bends, both turning the same way (sec 4.1):
/// the bottom (wall 0) in z <= 0 running along +X, up a bend about
/// (0, y, R) to the side (wall 1) in R <= x <= R + T, H high, and over a
/// bend about (0, y, R + H) to the top (wall 2), its inner skin z = 2 R + H,
/// running back along -X
struct U
{
    double R = 2, T = 1.5, K = 0.4, H = 30;
    Model model;
    int bottom, side, top, up, over;

    U()
    {
        model.thickness = T;
        bottom = model.addWall("Wall1", gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, -1)));
        side = model.addWall("Wall2", gp_Ax3(gp_Pnt(R, 0, R), gp_Dir(1, 0, 0)));
        top = model.addWall("Wall3", gp_Ax3(gp_Pnt(0, 0, 2 * R + H), gp_Dir(0, 0, 1)));
        CylBend b = upBend(R, T, deg(90), K);
        up = model.addBend("Bend1", b, bottom, side);
        b.frame = gp_Ax3(gp_Pnt(0, 0, R + H), gp_Dir(0, -1, 0), gp_Dir(1, 0, 0));
        over = model.addBend("Bend2", b, side, top);
    }

    /// A point of the second bend at angle a into it, depth d
    gp_Pnt inOver(double a, double d) const
    {
        const double r = R + d;
        return {r * std::cos(a), 0, R + H + r * std::sin(a)};
    }
};

}  // namespace

// The highest numbers a sheet's names hold, what a feature numbers on from:
// not its counts, once a wall or a bend keeps its number
TEST(SheetMetalModel, numbers)
{
    using PartDesign::SheetMetal::numberOf;
    EXPECT_EQ(numberOf("Wall12", "Wall"), 12);
    EXPECT_EQ(numberOf("Bend3", "Bend"), 3);
    EXPECT_EQ(numberOf("Bend3", "Wall"), 0);
    EXPECT_EQ(numberOf("Wall", "Wall"), 0);
    EXPECT_EQ(numberOf("Wall2Skin1", "Wall"), 0);
    EXPECT_EQ(numberOf("Bend2Part2", "Bend"), 0);
    EXPECT_EQ(numberOf("BaseWall", "Wall"), 0);

    int walls = -1, bends = -1;
    Model().numbers(walls, bends);
    EXPECT_EQ(walls, 0);
    EXPECT_EQ(bends, 0);

    U u;
    u.model.numbers(walls, bends);
    EXPECT_EQ(walls, 3);
    EXPECT_EQ(bends, 2);
    u.model.elements[u.side].name = "Wall7";
    u.model.elements[u.up].name = "Bend5";
    u.model.numbers(walls, bends);
    EXPECT_EQ(walls, 7);
    EXPECT_EQ(bends, 5);
}

// Which element a point is in, and none for a point outside the sheet
TEST(SheetMetalModel, owner)
{
    const U u;
    const Model& m = u.model;
    EXPECT_EQ(m.find("Wall2"), u.side);
    EXPECT_EQ(m.find("Bend2"), u.over);
    EXPECT_EQ(m.find("Bend9"), -1);
    EXPECT_EQ(m.owner(gp_Pnt(-5, 3, -1)), u.bottom);
    EXPECT_EQ(m.owner(inBend(m.elements[u.up].bend, deg(45), 1, 2)), u.up);
    EXPECT_EQ(m.owner(gp_Pnt(u.R + 1, 0, u.R + 5)), u.side);
    EXPECT_EQ(m.owner(u.inOver(deg(45), 1)), u.over);
    EXPECT_EQ(m.owner(gp_Pnt(-5, 0, 2 * u.R + u.H + 1)), u.top);
    // on the skins, within the tolerance
    EXPECT_EQ(m.owner(gp_Pnt(-5, 0, 1e-9)), u.bottom);
    EXPECT_EQ(m.owner(gp_Pnt(-5, 0, -u.T - 1e-9)), u.bottom);
    // inside the U, outside it, beyond the side's skin
    EXPECT_EQ(m.owner(gp_Pnt(-5, 0, 5)), -1);
    EXPECT_EQ(m.owner(gp_Pnt(-5, 0, -u.T - 1e-3)), -1);
    EXPECT_EQ(m.owner(gp_Pnt(u.R + u.T + 1, 0, u.R + 5)), -1);
    EXPECT_FALSE(m.contains(u.bottom, gp_Pnt(u.R + 1, 0, u.R + 5)));
}

// A teardrop (docs/SheetMetal.md sec 5.1): a bend past 180 deg, its leg
// heading back down toward the plate it came from. The leg runs through 270
// deg about the axis, where angles taken from -90 deg turned it into the
// plate's side of the bend; the plate under it and far behind is the plate's.
TEST(SheetMetalModel, pastHalfTurn)
{
    const double R = 1, T = 1, A = deg(225);
    Model m;
    m.thickness = T;
    const int plate = m.addWall("Wall1", gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, -1)));
    const CylBend b = upBend(R, T, A, 0.5);
    // the leg's inner skin from the bend's end, the material out from the axis
    const gp_Dir out(std::sin(A), 0, -std::cos(A));
    const gp_Dir legAlong(std::cos(A), 0, std::sin(A));
    const gp_Pnt legStart = gp_Pnt(0, 0, R).Translated(gp_Vec(out) * R);
    const int leg = m.addWall("Wall2", gp_Ax3(legStart, out));
    const int bend = m.addBend("Bend1", b, plate, leg);

    for (double s : {0.5, 1.0, 2.0, 2.5}) {
        const gp_Pnt p = legStart.Translated(gp_Vec(out) * 0.5 + gp_Vec(legAlong) * s);
        EXPECT_EQ(m.owner(p), leg) << s;
    }
    EXPECT_EQ(m.owner(inBend(b, deg(200), 0.5, 0)), bend);
    EXPECT_EQ(m.owner(gp_Pnt(-2.5, 0, -0.5)), plate);
    EXPECT_EQ(m.owner(gp_Pnt(-50, 0, -0.5)), plate);
    EXPECT_EQ(m.owner(gp_Pnt(-0.2, 0, -0.5)), plate);
    // flat, the leg's inner skin goes on from the bend's arc along X
    const gp_Pnt tip = legStart.Translated(gp_Vec(legAlong) * 2);
    const gp_Pnt flat = m.toFlat(leg, tip, plate);
    EXPECT_NEAR(flat.X(), A * (R + 0.5 * T) + 2, 1e-12);
    EXPECT_NEAR(flat.Z(), 0, 1e-12);
}

// A roll of 300 deg: its end comes round below the axis, past where angles
// taken from -90 deg turn; the bend's material there is the bend's, and lies
// flat at its angle's length
TEST(SheetMetalFold, roll)
{
    const CylBend b = upBend(1, 1, deg(300), 0.5);
    for (double a : {deg(10), deg(150), deg(275), deg(299)}) {
        const gp_Pnt p = inBend(b, a, 0.5, 0);
        EXPECT_TRUE(b.contains(p)) << a;
        EXPECT_NEAR(b.toFlat(p).Distance(b.toFlat(inBend(b, 0, 0.5, 0))), a * 1.5, 1e-12) << a;
    }
    EXPECT_FALSE(b.contains(inBend(b, deg(-10), 0.5, 0)));
    EXPECT_FALSE(b.contains(inBend(b, deg(320), 0.5, 0)));
}

// Flat from the bottom: the top's far end on the inner skin is the hand sum
// of the legs and the two allowances along X; from the top, the same sheet
// moved, the top staying where it is
TEST(SheetMetalModel, flatU)
{
    const U u;
    const Model& m = u.model;
    const double ba = bendAllowance({u.T, u.R, deg(90)}, kFactor(u.K));
    const double L = 12;
    const gp_Pnt start(-L, 0, 0), end(-L, 0, 2 * u.R + u.H);
    EXPECT_LT(m.toFlat(u.bottom, start, u.bottom).Distance(start), 1e-12);
    const gp_Pnt far = m.toFlat(u.top, end, u.bottom);
    EXPECT_NEAR(far.X(), ba + u.H + ba + L, 1e-12);
    EXPECT_NEAR(far.Y(), 0, 1e-12);
    EXPECT_NEAR(far.Z(), 0, 1e-12);
    // the outer skin a thickness below
    const gp_Pnt outer = m.toFlat(u.top, end.Translated(gp_Vec(0, 0, u.T)), u.bottom);
    EXPECT_NEAR(outer.Z(), -u.T, 1e-12);

    EXPECT_LT(m.toFlat(u.top, end, u.top).Distance(end), 1e-12);
    const gp_Pnt near = m.toFlat(u.bottom, start, u.top);
    EXPECT_NEAR(near.Distance(end), L + ba + u.H + ba + L, 1e-12);
    // every point keeps its distance to every other: the two flats differ
    // by one rigid move
    const gp_Pnt mid(u.R + 0.5, 1, u.R + 10);
    EXPECT_NEAR(m.toFlat(u.side, mid, u.top).Distance(near),
                m.toFlat(u.side, mid, u.bottom).Distance(start),
                1e-12);
}

// A bend meets its walls in the flat: its last fibres where the wall after
// it is laid, whichever end is the root
TEST(SheetMetalModel, flatBendMeetsWalls)
{
    const U u;
    const Model& m = u.model;
    for (int root : {u.bottom, u.top}) {
        for (double d : {0.0, 0.7, u.T}) {
            const gp_Pnt end = u.inOver(deg(90), d);
            EXPECT_LT(m.toFlat(u.over, end, root).Distance(m.toFlat(u.top, end, root)), 1e-12)
                << root << " " << d;
            const gp_Pnt start = u.inOver(0, d);
            EXPECT_LT(m.toFlat(u.over, start, root).Distance(m.toFlat(u.side, start, root)),
                      1e-12)
                << root << " " << d;
        }
    }
}

TEST(SheetMetalModel, errors)
{
    U u;
    Model& m = u.model;
    EXPECT_THROW(m.addBend("Bend3", m.elements[u.up].bend, u.bottom, 9), Base::IndexError);
    EXPECT_THROW(m.addBend("Bend3", m.elements[u.up].bend, u.up, u.top), Base::IndexError);
    EXPECT_THROW(m.placement(u.top, u.up), Base::ValueError);
    const int lone = m.addWall("Wall4", gp_Ax3());
    EXPECT_THROW(m.placement(lone, u.bottom), Base::IndexError);
    EXPECT_THROW(m.placement(42, u.bottom), Base::IndexError);
}
