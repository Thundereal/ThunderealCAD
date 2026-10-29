// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>

#include <GC_MakeArcOfCircle.hxx>
#include <GC_MakeSegment.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_TrimmedCurve.hxx>

#include <Mod/Surface/App/Geom/Sweep.h>

using namespace Surface::Geom;

namespace
{

using Curve = Handle(Geom_Curve);

Curve segment(const gp_Pnt& a, const gp_Pnt& b)
{
    return GC_MakeSegment(a, b).Value();
}

// A half circle standing on the chord from a to b, rising to `height` (a
// circle's arc through the chord's ends and the point above its middle)
Curve arch(const gp_Pnt& a, const gp_Pnt& b, double height)
{
    gp_Pnt mid((a.XYZ() + b.XYZ()) / 2 + gp_XYZ(0, 0, height));
    return GC_MakeArcOfCircle(a, mid, b).Value();
}

double distance(const Handle(Geom_BSplineSurface) & s, const gp_Pnt& p)
{
    GeomAPI_ProjectPointOnSurf pr(p, s);
    return pr.NbPoints() > 0 ? pr.LowerDistance() : 1e9;
}

double worst(const Handle(Geom_BSplineSurface) & s, const Curve& c)
{
    double w = 0;
    for (int k = 0; k <= 10; ++k) {
        double t = c->FirstParameter() + (c->LastParameter() - c->FirstParameter()) * k / 10;
        w = std::max(w, distance(s, c->Value(t)));
    }
    return w;
}

}  // namespace

// One section, scaled by rails that part: a half circle on the chord stays a
// half circle on the chord, as high as half of it
TEST(Sweep, oneSectionScaledByTheRails)
{
    Curve r1 = segment(gp_Pnt(0, 0, 0), gp_Pnt(20, 0, 0));
    Curve r2 = segment(gp_Pnt(0, 10, 0), gp_Pnt(20, 16, 0));
    Curve section = arch(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0), 5);
    auto r = twoRailSweep(r1, r2, {section});
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_LT(worst(r.surface, r1), 2e-4);
    EXPECT_LT(worst(r.surface, r2), 2e-4);
    EXPECT_LT(worst(r.surface, section), 2e-4);
    // at the far end the chord is (20, 0, 0)-(20, 16, 0): its half circle
    // tops out at z = 8 over y = 8
    EXPECT_LT(distance(r.surface, gp_Pnt(20, 8, 8)), 2e-3);
    EXPECT_LT(distance(r.surface, gp_Pnt(10, 6.5, 6.5)), 2e-3);
}

// Two sections: the sweep goes from the one to the other between them
TEST(Sweep, twoSectionsBlended)
{
    Curve r1 = segment(gp_Pnt(0, 0, 0), gp_Pnt(20, 0, 0));
    Curve r2 = segment(gp_Pnt(0, 10, 0), gp_Pnt(20, 10, 0));
    Curve high = arch(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0), 5);
    Curve low = arch(gp_Pnt(20, 10, 0), gp_Pnt(20, 0, 0), 2);  // the other way round
    auto r = twoRailSweep(r1, r2, {low, high});
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_LT(worst(r.surface, high), 2e-4);
    EXPECT_LT(worst(r.surface, low), 2e-4);
    // half way, half way between the two heights
    EXPECT_LT(distance(r.surface, gp_Pnt(10, 5, 3.5)), 2e-2);
}

// A rail given the other way round runs with the first
TEST(Sweep, aRailTheOtherWayRound)
{
    Curve r1 = segment(gp_Pnt(0, 0, 0), gp_Pnt(20, 0, 0));
    Curve r2 = segment(gp_Pnt(20, 10, 0), gp_Pnt(0, 10, 0));
    auto r = twoRailSweep(r1, r2, {arch(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0), 5)});
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_LT(distance(r.surface, gp_Pnt(10, 5, 5)), 2e-3);
}

TEST(Sweep, aSectionOffTheRailsIsNamed)
{
    Curve r1 = segment(gp_Pnt(0, 0, 0), gp_Pnt(20, 0, 0));
    Curve r2 = segment(gp_Pnt(0, 10, 0), gp_Pnt(20, 10, 0));
    auto r = twoRailSweep(r1, r2, {arch(gp_Pnt(0, 0, 1), gp_Pnt(0, 10, 0), 5)});
    EXPECT_FALSE(r.isDone());
    EXPECT_NE(r.error.find("section 1"), std::string::npos) << r.error;
}
