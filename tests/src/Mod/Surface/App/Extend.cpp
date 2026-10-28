// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>

#include <Geom_BSplineSurface.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array2OfReal.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <gp_Lin.hxx>

#include <Mod/Surface/App/Geom/Extend.h>

using namespace Surface::Geom;

namespace
{

using BSurface = Handle(Geom_BSplineSurface);

// A Bezier patch of degree 3 x 3 over [0, 1]^2, as a B-spline
BSurface bezier(TColgp_Array2OfPnt& poles)
{
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double x = 10.0 * i / 3, y = 8.0 * j / 3;
            double z = 0.6 * std::sin(1.3 * i + 0.4) * std::cos(0.9 * j) + 0.2 * i * j;
            poles.SetValue(i + 1, j + 1, gp_Pnt(x, y, z));
        }
    }
    TColStd_Array1OfReal k(1, 2);
    TColStd_Array1OfInteger m(1, 2);
    k.SetValue(1, 0), k.SetValue(2, 1);
    m.SetValue(1, 4), m.SetValue(2, 4);
    return new Geom_BSplineSurface(poles, k, k, m, m, 3, 3);
}

// The patch's polynomial, evaluated anywhere: the Bernstein form, written out
gp_Pnt bernstein(const TColgp_Array2OfPnt& poles, double u, double v)
{
    auto b = [](int i, double t) {
        const double c[4] = {1, 3, 3, 1};
        return c[i] * std::pow(t, i) * std::pow(1 - t, 3 - i);
    };
    gp_XYZ p;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            p += poles.Value(i + 1, j + 1).XYZ() * (b(i, u) * b(j, v));
        }
    }
    return gp_Pnt(p);
}

// Several spans each way, cubic, a smooth wave of 7 x 6 poles
BSurface multiSpan(bool rational = false)
{
    TColgp_Array2OfPnt poles(1, 7, 1, 6);
    TColStd_Array2OfReal weights(1, 7, 1, 6);
    for (int i = 0; i < 7; ++i) {
        for (int j = 0; j < 6; ++j) {
            double z = 1.5 * std::sin(0.8 * i) * std::cos(0.7 * j) + 0.1 * i;
            poles.SetValue(i + 1, j + 1, gp_Pnt(3.0 * i, 2.5 * j + 0.3 * i, z));
            weights.SetValue(i + 1, j + 1, rational ? 1 + 0.2 * std::sin(i + 2.0 * j) : 1.0);
        }
    }
    TColStd_Array1OfReal uk(1, 5), vk(1, 4);
    TColStd_Array1OfInteger um(1, 5), vm(1, 4);
    const double us[5] = {0, 0.2, 0.5, 0.7, 1}, vs[4] = {0, 0.3, 0.6, 1};
    for (int i = 0; i < 5; ++i) {
        uk.SetValue(i + 1, us[i]), um.SetValue(i + 1, i == 0 || i == 4 ? 4 : 1);
    }
    for (int i = 0; i < 4; ++i) {
        vk.SetValue(i + 1, vs[i]), vm.SetValue(i + 1, i == 0 || i == 3 ? 4 : 1);
    }
    return new Geom_BSplineSurface(poles, weights, uk, vk, um, vm, 3, 3);
}

bool inU(Boundary b)
{
    return b == Boundary::UMin || b == Boundary::UMax;
}

bool atEnd(Boundary b)
{
    return b == Boundary::UMax || b == Boundary::VMax;
}

// (u, v) for the parameter `t` along the boundary and `s` across it
// (0 on the boundary, growing outwards), on surface `s`
gp_Pnt2d at(const BSurface& sf, Boundary b, double t, double s)
{
    double u1, u2, v1, v2;
    sf->Bounds(u1, u2, v1, v2);
    if (inU(b)) {
        double v = v1 + (v2 - v1) * t;
        return gp_Pnt2d(atEnd(b) ? u2 + s : u1 - s, v);
    }
    double u = u1 + (u2 - u1) * t;
    return gp_Pnt2d(u, atEnd(b) ? v2 + s : v1 - s);
}

// The worst distance between `a` and `b` at the same parameters, over the
// parameter range of `a`
double sameOver(const BSurface& a, const BSurface& b)
{
    double u1, u2, v1, v2, worst = 0;
    a->Bounds(u1, u2, v1, v2);
    for (int i = 0; i <= 12; ++i) {
        for (int j = 0; j <= 12; ++j) {
            double u = u1 + (u2 - u1) * i / 12, v = v1 + (v2 - v1) * j / 12;
            worst = std::max(worst, a->Value(u, v).Distance(b->Value(u, v)));
        }
    }
    return worst;
}

// The mean distance from the old boundary to the new one, along the isos
double meanLength(const BSurface& before, const BSurface& after, Boundary b)
{
    double sum = 0;
    for (int k = 0; k <= 10; ++k) {
        gp_Pnt2d p = at(before, b, k / 10.0, 0);
        gp_Pnt2d q = at(after, b, k / 10.0, 0);
        sum += after->Value(p.X(), p.Y()).Distance(after->Value(q.X(), q.Y()));
    }
    return sum / 11;
}

// The normal curvature across the boundary of `ref`, at `s` across it, on
// `sf`
double curvatureAcross(const BSurface& sf, const BSurface& ref, Boundary b, double t, double s)
{
    gp_Pnt2d p = at(ref, b, t, s);
    gp_Pnt pt;
    gp_Vec du, dv, duu, dvv, duv;
    sf->D2(p.X(), p.Y(), pt, du, dv, duu, dvv, duv);
    gp_Vec n = du.Crossed(dv).Normalized();
    // along an iso: the second fundamental form over the first
    return inU(b) ? duu.Dot(n) / du.SquareMagnitude() : dvv.Dot(n) / dv.SquareMagnitude();
}

}  // namespace

// The natural extension is the patch's own polynomial, further on
TEST(Extend, naturalIsTheSamePolynomial)
{
    TColgp_Array2OfPnt poles(1, 4, 1, 4);
    BSurface original = bezier(poles);
    BSurface s = BSurface::DownCast(original->Copy());
    ASSERT_EQ(extendBoundary(s, Boundary::UMax, 4.0, Extension::Natural), "");
    double u1, u2, v1, v2;
    s->Bounds(u1, u2, v1, v2);
    EXPECT_GT(u2, 1.0);
    EXPECT_DOUBLE_EQ(u1, 0.0);
    double worst = 0;
    for (int i = 0; i <= 10; ++i) {
        for (int j = 0; j <= 10; ++j) {
            double u = u1 + (u2 - u1) * i / 10, v = v1 + (v2 - v1) * j / 10;
            worst = std::max(worst, s->Value(u, v).Distance(bernstein(poles, u, v)));
        }
    }
    EXPECT_LT(worst, 1e-9);
    EXPECT_EQ(s->UDegree(), 3);
    EXPECT_NEAR(meanLength(original, s, Boundary::UMax), 4.0, 1e-6);
}

// On several spans, rational: what was there stays, the length is met, and
// the curvature across the old boundary is the same on both sides of it
TEST(Extend, naturalIsG2WithItself)
{
    for (bool rational : {false, true}) {
        for (Boundary b : {Boundary::UMin, Boundary::UMax, Boundary::VMin, Boundary::VMax}) {
            SCOPED_TRACE(testing::Message() << "rational " << rational << " boundary " << int(b));
            BSurface original = multiSpan(rational);
            BSurface s = BSurface::DownCast(original->Copy());
            ASSERT_EQ(extendBoundary(s, b, 2.5, Extension::Natural), "");
            EXPECT_LT(sameOver(original, s), 1e-9);
            EXPECT_NEAR(meanLength(original, s, b), 2.5, 1e-6);
            for (double t : {0.1, 0.5, 0.9}) {
                double inside = curvatureAcross(s, original, b, t, -1e-5);
                double outside = curvatureAcross(s, original, b, t, 1e-5);
                EXPECT_NEAR(inside, outside, 1e-4) << "at " << t;
            }
            EXPECT_EQ(s->UDegree(), 3);
            EXPECT_EQ(s->VDegree(), 3);
            // no knot of full multiplicity left behind at the old boundary
            for (int i = 2; i < s->NbUKnots(); ++i) {
                EXPECT_LT(s->UMultiplicity(i), 3);
            }
            for (int i = 2; i < s->NbVKnots(); ++i) {
                EXPECT_LT(s->VMultiplicity(i), 3);
            }
        }
    }
}

// The linear extension is straight, tangent to the surface, and as long
TEST(Extend, linearIsStraightAndTangent)
{
    for (Boundary b : {Boundary::UMin, Boundary::UMax, Boundary::VMin, Boundary::VMax}) {
        SCOPED_TRACE(testing::Message() << "boundary " << int(b));
        BSurface original = multiSpan();
        BSurface s = BSurface::DownCast(original->Copy());
        ASSERT_EQ(extendBoundary(s, b, 3.0, Extension::Linear), "");
        EXPECT_LT(sameOver(original, s), 1e-9);
        EXPECT_NEAR(meanLength(original, s, b), 3.0, 1e-6);
        for (double t : {0.0, 0.3, 0.7, 1.0}) {
            gp_Pnt2d p0 = at(original, b, t, 0);
            gp_Vec du, dv;
            gp_Pnt base;
            original->D1(p0.X(), p0.Y(), base, du, dv);
            gp_Vec across = inU(b) ? du : dv;
            if (!atEnd(b)) {
                across.Reverse();
            }
            gp_Lin line(base, gp_Dir(across));
            // from the old boundary to the new one along the iso
            gp_Pnt2d q = at(s, b, t, 0);
            double off = 0;
            for (int k = 1; k <= 8; ++k) {
                double f = k / 8.0;
                gp_Pnt2d r(p0.X() + (q.X() - p0.X()) * f, p0.Y() + (q.Y() - p0.Y()) * f);
                off = std::max(off, line.Distance(s->Value(r.X(), r.Y())));
            }
            EXPECT_LT(off, 1e-9) << "at " << t;
            // tangent: the derivative across, just inside and just outside
            // the old boundary, points the same way
            gp_Vec d[2];
            for (int side = 0; side < 2; ++side) {
                gp_Pnt2d r = at(original, b, t, side ? 1e-7 : -1e-7);
                gp_Vec ru, rv;
                gp_Pnt pt;
                s->D1(r.X(), r.Y(), pt, ru, rv);
                d[side] = inU(b) ? ru : rv;
            }
            EXPECT_LT(d[0].Angle(d[1]), 1e-5) << "at " << t;
        }
    }
}

TEST(Extend, refusesWhatItCannot)
{
    BSurface s = multiSpan();
    EXPECT_NE(extendBoundary(s, Boundary::UMax, 0.0, Extension::Natural), "");
    EXPECT_NE(extendBoundary(s, Boundary::UMax, -1.0, Extension::Linear), "");
    // periodic across: nothing to extend
    TColgp_Array2OfPnt poles(1, 6, 1, 4);
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 4; ++j) {
            double a = i * M_PI / 3;
            poles.SetValue(i + 1, j + 1, gp_Pnt(5 * std::cos(a), 5 * std::sin(a), 2.0 * j));
        }
    }
    TColStd_Array1OfReal uk(1, 7), vk(1, 2);
    TColStd_Array1OfInteger um(1, 7), vm(1, 2);
    for (int i = 0; i < 7; ++i) {
        uk.SetValue(i + 1, i), um.SetValue(i + 1, 1);
    }
    vk.SetValue(1, 0), vk.SetValue(2, 1), vm.SetValue(1, 4), vm.SetValue(2, 4);
    BSurface p = new Geom_BSplineSurface(poles, uk, vk, um, vm, 3, 3, true, false);
    ASSERT_TRUE(p->IsUPeriodic());
    EXPECT_NE(extendBoundary(p, Boundary::UMax, 1.0, Extension::Natural), "");
    EXPECT_EQ(extendBoundary(p, Boundary::VMax, 1.0, Extension::Natural), "");
    // a rational surface whose weights vary across the boundary has no
    // straight extension of the same form
    BSurface r = multiSpan(true);
    EXPECT_NE(extendBoundary(r, Boundary::UMax, 1.0, Extension::Linear), "");
}
