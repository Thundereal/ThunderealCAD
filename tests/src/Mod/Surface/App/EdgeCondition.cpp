// SPDX-License-Identifier: LGPL-2.1-or-later

// Surface::Geom::matchBoundaries (docs/Surface.md sec 4.3 and 9, U1): G1 and
// G2 to a known neighbour within the stated tolerance, measured here apart
// from the function's own report; the weight; a boundary off its support; and
// the corners -- two conditions that agree at a corner are both met, a corner
// that cannot be is reported.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <array>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SurfaceOfLinearExtrusion.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array2OfPnt.hxx>

#include <Mod/Surface/App/Geom/EdgeCondition.h>

using namespace Surface::Geom;

namespace
{

constexpr double Deg = M_PI / 180;

// z(x, y) = 10 + s(x) y - 0.05 y^2 + h(x) y (10 - y) + lift over x in [0, 20],
// y in [0, 10], written down exactly: s and h are cubic B-splines of five
// poles in x (u), and the rest a Bezier of degree 3 in y (v). Where s and h
// are 0 the surface meets the cylinder below to G2 (z = 10 - y^2 / 20 is its
// top's own curvature); s tilts it across y = 0, h across x = 0.
Handle(Geom_BSplineSurface) field(const std::array<double, 5>& sp,
                                  const std::array<double, 5>& hp = {},
                                  double lift = 0)
{
    const double a = -0.05;
    // x poles at the Greville points, so x is linear in u
    const double xs[5] = {0, 20.0 / 6, 10, 20.0 * 5 / 6, 20};
    // the blossoms of 1, y, y^2 and y (10 - y) at the Bezier's poles
    const double ys[4] = {0, 10.0 / 3, 20.0 / 3, 10};
    const double y2[4] = {0, 0, 100.0 / 3, 100};
    TColgp_Array2OfPnt poles(1, 5, 1, 4);
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 4; ++j) {
            double z = 10 + lift + sp[i] * ys[j] + a * y2[j] + hp[i] * (10 * ys[j] - y2[j]);
            poles.SetValue(i + 1, j + 1, gp_Pnt(xs[i], ys[j], z));
        }
    }
    TColStd_Array1OfReal uk(1, 3), vk(1, 2);
    TColStd_Array1OfInteger um(1, 3), vm(1, 2);
    uk.SetValue(1, 0), uk.SetValue(2, 0.5), uk.SetValue(3, 1);
    um.SetValue(1, 4), um.SetValue(2, 1), um.SetValue(3, 4);
    vk.SetValue(1, 0), vk.SetValue(2, 1);
    vm.SetValue(1, 4), vm.SetValue(2, 4);
    return new Geom_BSplineSurface(poles, uk, vk, um, vm, 3, 3);
}

// Tilted across y = 0 by up to 0.3 inside, level at x = 0 and x = 20 where the
// end curves meet the cylinder
const std::array<double, 5> sloped = {0, -0.2, -0.3, -0.2, 0};

// The neighbour along y = 0: a cylinder of radius 10 round the X axis, whose
// top line is z = 10
TopoDS_Face cylinder()
{
    Handle(Geom_CylindricalSurface) c =
        new Geom_CylindricalSurface(gp_Ax3(gp_Pnt(0, 0, 0), gp::DX(), gp::DY()), 10);
    return BRepBuilderAPI_MakeFace(c, 0, 2 * M_PI, -5, 25, 1e-7).Face();
}

double surfaceNormalCurvature(const Handle(Geom_Surface) & s,
                              double u,
                              double v,
                              const gp_Dir& d,
                              gp_Dir& normal)
{
    GeomLProp_SLProps p(s, u, v, 2, 1e-9);
    normal = p.Normal();
    if (p.IsUmbilic()) {
        return p.MaxCurvature();
    }
    gp_Dir dmax, dmin;
    p.CurvatureDirections(dmax, dmin);
    double c = d.Dot(dmax), sn = d.Dot(dmin);
    return p.MaxCurvature() * c * c + p.MinCurvature() * sn * sn;
}

struct Measure
{
    double angle = 0;
    double curvature = 0;
};

// Along the boundary of s at v = v0 (or u = u0), against the support face:
// the angle between the normals, and the normal curvatures across. `from`
// and `to` limit it to a part of the boundary, as fractions.
Measure measure(const Handle(Geom_BSplineSurface) & s,
                Boundary b,
                const TopoDS_Face& support,
                double from = 0,
                double to = 1)
{
    double u0, u1, v0, v1;
    s->Bounds(u0, u1, v0, v1);
    Handle(Geom_Surface) sup = BRep_Tool::Surface(support);
    Measure m;
    for (int k = 0; k <= 60; ++k) {
        double t = from + (to - from) * k / 60.0;
        double u = 0, v = 0;
        switch (b) {
            case Boundary::VMin:
                u = u0 + (u1 - u0) * t, v = v0;
                break;
            case Boundary::VMax:
                u = u0 + (u1 - u0) * t, v = v1;
                break;
            case Boundary::UMin:
                u = u0, v = v0 + (v1 - v0) * t;
                break;
            case Boundary::UMax:
                u = u1, v = v0 + (v1 - v0) * t;
                break;
        }
        gp_Pnt p;
        gp_Vec du, dv;
        s->D1(u, v, p, du, dv);
        gp_Vec along = (b == Boundary::VMin || b == Boundary::VMax) ? du : dv;
        GeomLProp_SLProps sp(s, u, v, 1, 1e-9);
        gp_Dir across = sp.Normal().Crossed(gp_Dir(along));
        gp_Dir n1, n2;
        double k1 = surfaceNormalCurvature(s, u, v, across, n1);
        GeomAPI_ProjectPointOnSurf pr(p, sup);
        double su, sv;
        pr.LowerDistanceParameters(su, sv);
        double k2 = surfaceNormalCurvature(sup, su, sv, across, n2);
        if (n1.Dot(n2) < 0) {
            k2 = -k2;
        }
        m.angle = std::max(m.angle, std::acos(std::min(1.0, std::abs(n1.Dot(n2)))));
        m.curvature = std::max(m.curvature, std::abs(k1 - k2));
    }
    return m;
}

double boundaryMoved(const Handle(Geom_BSplineSurface) & before,
                     const Handle(Geom_BSplineSurface) & after,
                     Boundary b)
{
    Handle(Geom_Curve) c0, c1;
    double u0, u1, v0, v1;
    before->Bounds(u0, u1, v0, v1);
    switch (b) {
        case Boundary::VMin:
            c0 = before->VIso(v0), c1 = after->VIso(v0);
            break;
        case Boundary::VMax:
            c0 = before->VIso(v1), c1 = after->VIso(v1);
            break;
        case Boundary::UMin:
            c0 = before->UIso(u0), c1 = after->UIso(u0);
            break;
        case Boundary::UMax:
            c0 = before->UIso(u1), c1 = after->UIso(u1);
            break;
    }
    double worst = 0, a = c0->FirstParameter(), z = c0->LastParameter();
    for (int k = 0; k <= 50; ++k) {
        double t = a + (z - a) * k / 50;
        worst = std::max(worst, c0->Value(t).Distance(c1->Value(t)));
    }
    return worst;
}

Handle(Geom_BSplineSurface) copy(const Handle(Geom_BSplineSurface) & s)
{
    return Handle(Geom_BSplineSurface)::DownCast(s->Copy());
}

}  // namespace

TEST(EdgeCondition, g1ToACylinder)
{
    auto s = field(sloped);
    auto before = copy(s);
    ASSERT_GT(measure(s, Boundary::VMin, cylinder()).angle, 10 * Deg) << "not tangent to begin";
    MatchOptions opt;
    auto r = matchBoundary(s, Boundary::VMin, {1, cylinder(), 1.0}, opt);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_TRUE(r.achieved);
    auto m = measure(s, Boundary::VMin, cylinder());
    EXPECT_LT(m.angle, opt.angularTolerance);
    EXPECT_NEAR(r.maxAngle, m.angle, 0.05 * Deg) << "the report is what is measured";
    for (auto b : {Boundary::VMin, Boundary::VMax, Boundary::UMin, Boundary::UMax}) {
        EXPECT_LT(boundaryMoved(before, s, b), 1e-9) << "boundary " << int(b);
    }
}

TEST(EdgeCondition, g2ToACylinder)
{
    auto s = field(sloped);
    MatchOptions opt;
    auto r = matchBoundary(s, Boundary::VMin, {2, cylinder(), 1.0}, opt);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_TRUE(r.achieved);
    auto m = measure(s, Boundary::VMin, cylinder());
    EXPECT_LT(m.angle, opt.angularTolerance);
    EXPECT_LT(m.curvature, opt.curvatureTolerance);
    EXPECT_NEAR(r.maxCurvatureGap, m.curvature, 1e-4);
}

// The weight scales the derivative across: twice the weight, twice as long
TEST(EdgeCondition, weight)
{
    auto s1 = field(sloped), s2 = field(sloped);
    ASSERT_TRUE(matchBoundary(s1, Boundary::VMin, {1, cylinder(), 1.0}).achieved);
    ASSERT_TRUE(matchBoundary(s2, Boundary::VMin, {1, cylinder(), 2.0}).achieved);
    double u0, u1, v0, v1;
    s1->Bounds(u0, u1, v0, v1);
    for (double t : {0.3, 0.5, 0.7}) {
        gp_Pnt p;
        gp_Vec du, dv1, dv2;
        s1->D1(u0 + (u1 - u0) * t, v0, p, du, dv1);
        s2->D1(u0 + (u1 - u0) * t, v0, p, du, dv2);
        EXPECT_NEAR(dv2.Magnitude() / dv1.Magnitude(), 2.0, 0.02) << t;
    }
}

TEST(EdgeCondition, boundaryOffItsSupport)
{
    auto s = field(sloped, {}, 1);
    auto r = matchBoundary(s, Boundary::VMin, {1, cylinder(), 1.0});
    EXPECT_FALSE(r.achieved);
    EXPECT_NE(r.error.find("not on"), std::string::npos) << r.error;
}

// The end curve at x = 20 crosses the cylinder's top line at a slope of 0.3:
// the corner cannot be tangent, and is reported; away from it the boundary is
TEST(EdgeCondition, aCornerThatCannotBeMet)
{
    auto s = field({0, -0.1, -0.2, -0.3, -0.3});
    MatchOptions opt;
    auto r = matchBoundary(s, Boundary::VMin, {1, cylinder(), 1.0}, opt);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_FALSE(r.achieved);
    EXPECT_FALSE(r.firstCorner);
    EXPECT_TRUE(r.lastCorner);
    EXPECT_GT(r.maxAngle, 10 * Deg);
    EXPECT_LT(measure(s, Boundary::VMin, cylinder(), 0, 0.85).angle, opt.angularTolerance);
}

// Two boundaries meeting at a corner, each G1 to its own neighbour: y = 0 to
// the cylinder, x = 0 to the surface the end curve sweeps along X. At the
// corner each boundary's curve lies in the other's support, so both are met.
TEST(EdgeCondition, twoBoundariesAtACorner)
{
    // s level at x = 0 (its first two poles 0), h tilting x = 0 between the
    // corners and 0 at them
    auto s = field({0, 0, -0.3, 0, 0}, {0, 0.03, 0, 0, 0});
    double u0, u1, v0, v1;
    s->Bounds(u0, u1, v0, v1);
    Handle(Geom_SurfaceOfLinearExtrusion) swept =
        new Geom_SurfaceOfLinearExtrusion(s->UIso(u0), gp::DX());
    double c0 = s->UIso(u0)->FirstParameter(), c1 = s->UIso(u0)->LastParameter();
    TopoDS_Face side = BRepBuilderAPI_MakeFace(swept, c0, c1, -5, 0, 1e-7).Face();
    ASSERT_GT(measure(s, Boundary::UMin, side).angle, 0.5 * Deg) << "not tangent to begin";

    Conditions c;
    c[int(Boundary::VMin)] = {1, cylinder(), 1.0};
    c[int(Boundary::UMin)] = {1, side, 1.0};
    MatchOptions opt;
    auto r = matchBoundaries(s, c, opt);
    for (auto b : {Boundary::VMin, Boundary::UMin}) {
        ASSERT_TRUE(r[int(b)].error.empty()) << r[int(b)].error;
        EXPECT_TRUE(r[int(b)].achieved) << int(b);
    }
    EXPECT_LT(measure(s, Boundary::VMin, cylinder()).angle, opt.angularTolerance);
    EXPECT_LT(measure(s, Boundary::UMin, side).angle, opt.angularTolerance);
    EXPECT_FALSE(r[int(Boundary::VMax)].achieved || !r[int(Boundary::VMax)].error.empty())
        << "a boundary with no condition reports nothing";
}

// Refinement is bounded: the poles across the boundary grow, not explode
TEST(EdgeCondition, poleCountBounded)
{
    auto s = field(sloped);
    int before = s->NbUPoles();
    matchBoundary(s, Boundary::VMin, {2, cylinder(), 1.0});
    EXPECT_LE(s->NbUPoles(), 4 * before);
}

// The same corner with the boundary at x = 20 free -- no curve of the user's
// there, as at the side of a loft -- is met: the free side's poles move
TEST(EdgeCondition, aFreeSideLetsTheCornerBeMet)
{
    auto s = field({0, -0.1, -0.2, -0.3, -0.3});
    auto before = copy(s);
    Conditions c;
    c[int(Boundary::VMin)] = {1, cylinder(), 1.0};
    c[int(Boundary::UMax)].free = true;
    MatchOptions opt;
    auto r = matchBoundaries(s, c, opt);
    ASSERT_TRUE(r[int(Boundary::VMin)].error.empty()) << r[int(Boundary::VMin)].error;
    EXPECT_TRUE(r[int(Boundary::VMin)].achieved);
    EXPECT_LT(measure(s, Boundary::VMin, cylinder()).angle, opt.angularTolerance);
    EXPECT_LT(boundaryMoved(before, s, Boundary::VMin), 1e-9);
    EXPECT_LT(boundaryMoved(before, s, Boundary::UMin), 1e-9) << "a side that is not free stays";
    EXPECT_GT(boundaryMoved(before, s, Boundary::UMax), 1e-3) << "the free side moved";
}

// A reach keeps a condition local: the surface beyond that share of the way
// across does not move -- the rest of a loft's sections stay on it
TEST(EdgeCondition, reach)
{
    auto s = field(sloped);
    auto before = copy(s);
    EdgeCondition c {1, cylinder(), 1.0};
    c.reach = 0.3;
    MatchOptions opt;
    auto r = matchBoundary(s, Boundary::VMin, c, opt);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_TRUE(r.achieved);
    EXPECT_LT(measure(s, Boundary::VMin, cylinder()).angle, opt.angularTolerance);
    double u0, u1, v0, v1;
    before->Bounds(u0, u1, v0, v1);
    double moved = 0, near = 0;
    for (int i = 0; i <= 10; ++i) {
        for (int j = 0; j <= 10; ++j) {
            double u = u0 + (u1 - u0) * i / 10, v = v0 + (v1 - v0) * j / 10;
            double d = before->Value(u, v).Distance(s->Value(u, v));
            (v - v0 >= 0.3 * (v1 - v0) ? moved : near) =
                std::max(v - v0 >= 0.3 * (v1 - v0) ? moved : near, d);
        }
    }
    EXPECT_LT(moved, 1e-9) << "beyond the reach";
    EXPECT_GT(near, 1e-3) << "within it";
}

// A boundary's marker: its middle, the way into the surface square to it and
// in its tangent plane, and its length
TEST(EdgeCondition, boundaryMarker)
{
    Handle(Geom_BSplineSurface) s = field(sloped);
    double u0, u1, v0, v1;
    s->Bounds(u0, u1, v0, v1);
    // along y = 0 the surface is level at z = 10, from x = 0 to 20
    BoundaryMarker m = boundaryMarker(s, Boundary::VMin);
    EXPECT_LT(m.middle.Distance(gp_Pnt(10, 0, 10)), 1e-9);
    EXPECT_NEAR(m.length, 20, 1e-6);
    EXPECT_GT(m.inward.Y(), 0.9);
    EXPECT_LT(std::abs(m.inward.X()), 1e-9);
    for (Boundary b : {Boundary::UMin, Boundary::UMax, Boundary::VMin, Boundary::VMax}) {
        BoundaryMarker k = boundaryMarker(s, b);
        GeomAPI_ProjectPointOnSurf pr(k.middle, s);
        double u, v;
        pr.LowerDistanceParameters(u, v);
        gp_Pnt p;
        gp_Vec du, dv;
        s->D1(u, v, p, du, dv);
        bool alongU = b == Boundary::VMin || b == Boundary::VMax;
        gp_Vec along = alongU ? du : dv;
        EXPECT_LT(std::abs(gp_Vec(k.inward).Dot(along.Normalized())), 1e-9);
        EXPECT_LT(std::abs(gp_Vec(k.inward).Dot(du.Crossed(dv).Normalized())), 1e-9);
        // a step along it lands inside the surface, near it
        GeomAPI_ProjectPointOnSurf in(k.middle.Translated(gp_Vec(k.inward) * 0.5), s);
        in.LowerDistanceParameters(u, v);
        EXPECT_LT(in.LowerDistance(), 0.05);
        double inside = b == Boundary::UMin ? u - u0
            : b == Boundary::UMax           ? u1 - u
            : b == Boundary::VMin           ? v - v0
                                            : v1 - v;
        EXPECT_GT(inside, 1e-3) << int(b);
    }
}
