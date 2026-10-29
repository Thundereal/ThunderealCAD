// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <GC_MakeSegment.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <gp_Pln.hxx>

#include <Mod/Surface/App/Geom/Blend.h>

using namespace Surface::Geom;

namespace
{

constexpr double Deg = M_PI / 180;

TopoDS_Face quad(const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c, const gp_Pnt& d)
{
    BRepBuilderAPI_MakePolygon poly(a, b, c, d, true);
    return BRepBuilderAPI_MakeFace(poly.Wire(), true);
}

Handle(Geom_Curve) segment(const gp_Pnt& a, const gp_Pnt& b)
{
    return GC_MakeSegment(a, b).Value();
}

// A flat face on z = 0 up to x = 10, and one tilted up from x = 15, z = 3;
// the second edge given the other way round
struct Gap
{
    TopoDS_Face low = quad(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0), gp_Pnt(10, 10, 0), gp_Pnt(0, 10, 0));
    TopoDS_Face high =
        quad(gp_Pnt(15, 0, 3), gp_Pnt(25, 0, 6), gp_Pnt(25, 10, 6), gp_Pnt(15, 10, 3));
    Handle(Geom_Curve) lowEdge = segment(gp_Pnt(10, 0, 0), gp_Pnt(10, 10, 0));
    Handle(Geom_Curve) highEdge = segment(gp_Pnt(15, 10, 3), gp_Pnt(15, 0, 3));

    BlendSide side(bool first, int order, double weight = 1) const
    {
        BlendSide s;
        s.curve = first ? lowEdge : highEdge;
        s.condition.order = order;
        s.condition.support = first ? low : high;
        s.condition.weight = weight;
        return s;
    }
};

gp_Dir normalAt(const Handle(Geom_BSplineSurface) & s, const gp_Pnt& p)
{
    GeomAPI_ProjectPointOnSurf pr(p, s);
    double u, v;
    pr.LowerDistanceParameters(u, v);
    gp_Pnt q;
    gp_Vec du, dv;
    s->D1(u, v, q, du, dv);
    return gp_Dir(du.Crossed(dv));
}

// The worst angle between the blend and a plane of normal `n` along x = `x`
double worstAngle(const Handle(Geom_BSplineSurface) & s, double x, double z, const gp_Dir& n)
{
    double worst = 0;
    for (int k = 0; k <= 10; ++k) {
        gp_Dir m = normalAt(s, gp_Pnt(x, 10.0 * k / 10, z));
        double a = m.Angle(n);
        worst = std::max(worst, std::min(a, M_PI - a));
    }
    return worst;
}

const gp_Dir up(0, 0, 1);
const gp_Dir tilted = gp_Dir(gp_Vec(-3, 0, 10));

}  // namespace

TEST(Blend, g1OnBothSides)
{
    Gap g;
    auto r = blendSurface(g.side(true, 1), g.side(false, 1));
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_TRUE(r.reversed);
    EXPECT_LT(worstAngle(r.surface, 10, 0, up), 0.1 * Deg);
    EXPECT_LT(worstAngle(r.surface, 15, 3, tilted), 0.1 * Deg);
    EXPECT_TRUE(r.reports[0].achieved);
    EXPECT_TRUE(r.reports[1].achieved);
    // it bridges: it leaves each face away from it, never folding back over
    // the low one -- not even at the ends of its edges, where a step aside
    // lands on the face's next edge
    double u1, u2, v1, v2, back = 0;
    r.surface->Bounds(u1, u2, v1, v2);
    for (int i = 0; i <= 20; ++i) {
        for (int j = 0; j <= 20; ++j) {
            gp_Pnt p = r.surface->Value(u1 + (u2 - u1) * i / 20, v1 + (v2 - v1) * j / 20);
            back = std::max(back, 10 - p.X());
        }
    }
    EXPECT_LT(back, 1e-6);
}

TEST(Blend, g2OnBothSides)
{
    Gap g;
    auto r = blendSurface(g.side(true, 2), g.side(false, 2));
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_LT(worstAngle(r.surface, 10, 0, up), 0.1 * Deg);
    EXPECT_LT(worstAngle(r.surface, 15, 3, tilted), 0.1 * Deg);
    // flat supports: no curvature across at either edge
    EXPECT_LT(r.reports[0].maxCurvatureGap, 1e-3);
    EXPECT_LT(r.reports[1].maxCurvatureGap, 1e-3);
    EXPECT_TRUE(r.reports[0].achieved && r.reports[1].achieved);
}

TEST(Blend, aWeightShapesIt)
{
    Gap g;
    auto one = blendSurface(g.side(true, 1), g.side(false, 1));
    auto two = blendSurface(g.side(true, 1, 2.5), g.side(false, 1));
    ASSERT_TRUE(one.isDone() && two.isDone());
    EXPECT_LT(worstAngle(two.surface, 10, 0, up), 0.1 * Deg);
    double u1, u2, v1, v2;
    one.surface->Bounds(u1, u2, v1, v2);
    gp_Pnt a = one.surface->Value((u1 + u2) / 2, (v1 + v2) / 2);
    two.surface->Bounds(u1, u2, v1, v2);
    gp_Pnt b = two.surface->Value((u1 + u2) / 2, (v1 + v2) / 2);
    EXPECT_GT(a.Distance(b), 0.1);
}

TEST(Blend, positionAloneOnOneSide)
{
    Gap g;
    auto r = blendSurface(g.side(true, 1), g.side(false, 0));
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_LT(worstAngle(r.surface, 10, 0, up), 0.1 * Deg);
    GeomAPI_ProjectPointOnSurf pr(gp_Pnt(15, 5, 3), r.surface);
    EXPECT_LT(pr.LowerDistance(), 1e-6);
}

TEST(Blend, edgesThatTouchAreRefused)
{
    Gap g;
    BlendSide b = g.side(false, 1);
    b.curve = segment(gp_Pnt(10, 0, 0), gp_Pnt(15, 10, 3));
    auto r = blendSurface(g.side(true, 1), b);
    EXPECT_FALSE(r.isDone());
    EXPECT_FALSE(r.error.empty());
}
