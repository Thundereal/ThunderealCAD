// SPDX-License-Identifier: LGPL-2.1-or-later

// Surface::Geom::networkSurface (docs/Surface.md sec 9, U1): a network of
// curves spanned at degree 3, C2 inside, with a bounded pole count -- where
// OCCT's GeomFill_Gordon gives degree 9, C0 and 2004 x 2004 poles on the
// 12 x 12 network (sec 3.4, scripts/occt-gordon-probe.cpp, whose networks
// these are).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>

#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TColgp_HArray1OfPnt.hxx>

#include <Mod/Surface/App/Geom/NetworkSurface.h>

using Surface::Geom::NetworkOptions;
using Surface::Geom::networkSurface;
using Curve = Handle(Geom_Curve);
using Curves = std::vector<Curve>;
using Fn = std::function<gp_Pnt(double, double)>;

namespace
{

Curve interp(const std::vector<gp_Pnt>& pts)
{
    Handle(TColgp_HArray1OfPnt) a = new TColgp_HArray1OfPnt(1, (int)pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        a->SetValue((int)i + 1, pts[i]);
    }
    GeomAPI_Interpolate it(a, false, 1e-9);
    it.Perform();
    return it.Curve();
}

// A curve along u at fixed v (a profile), or along v at fixed u (a guide),
// through n points of f
Curve along(const Fn& f, bool alongU, double fixed, int n)
{
    std::vector<gp_Pnt> pts;
    for (int i = 0; i < n; ++i) {
        double t = double(i) / (n - 1);
        pts.push_back(alongU ? f(t, fixed) : f(fixed, t));
    }
    return interp(pts);
}

// A network curve through f at every crossing parameter and at `extra`
// points between each pair, so that profiles and guides meet exactly
Curve through(const Fn& f, bool alongU, double fixed, const std::vector<double>& cross, int extra)
{
    std::vector<gp_Pnt> pts;
    for (size_t i = 0; i < cross.size(); ++i) {
        double t = cross[i];
        pts.push_back(alongU ? f(t, fixed) : f(fixed, t));
        if (i + 1 < cross.size()) {
            for (int k = 1; k <= extra; ++k) {
                double s = t + (cross[i + 1] - t) * k / (extra + 1);
                pts.push_back(alongU ? f(s, fixed) : f(fixed, s));
            }
        }
    }
    return interp(pts);
}

double distance(const Handle(Geom_BSplineSurface) & s, const gp_Pnt& p)
{
    GeomAPI_ProjectPointOnSurf pr(p, s);
    return pr.NbPoints() > 0 ? pr.LowerDistance() : 1e9;
}

// The worst distance from points along the curves to the surface
double deviation(const Handle(Geom_BSplineSurface) & s, const Curves& curves)
{
    double worst = 0;
    for (const auto& c : curves) {
        double a = c->FirstParameter(), b = c->LastParameter();
        for (int i = 0; i <= 40; ++i) {
            worst = std::max(worst, distance(s, c->Value(a + (b - a) * i / 40.0)));
        }
    }
    return worst;
}

int maxInteriorMultiplicity(const Handle(Geom_BSplineSurface) & s)
{
    int m = 0;
    for (int i = 2; i < s->NbUKnots(); ++i) {
        m = std::max(m, s->UMultiplicity(i));
    }
    for (int i = 2; i < s->NbVKnots(); ++i) {
        m = std::max(m, s->VMultiplicity(i));
    }
    return m;
}

const Fn saddle = [](double u, double v) {
    double x = 10 * u, y = 10 * v;
    return gp_Pnt(x, y, 0.1 * (x - 5) * (y - 5));
};

const Fn wave = [](double u, double v) {
    double x = 20 * u, y = 12 * v;
    return gp_Pnt(x, y + 1.5 * std::sin(u * 3), 2 * std::sin(x / 4) * std::cos(y / 5));
};

// 5 x 4 on the wave, unevenly spaced, a different point count per curve
void unevenWave(Curves& p, Curves& g)
{
    std::vector<double> vs = {0.0, 0.3, 0.55, 0.8, 1.0}, us = {0.0, 0.35, 0.7, 1.0};
    int k = 0;
    for (double v : vs) {
        p.push_back(through(wave, true, v, us, 1 + (k++ % 3)));
    }
    for (double u : us) {
        g.push_back(through(wave, false, u, vs, 1 + (k++ % 2)));
    }
}

Curve reversed(const Curve& c)
{
    return Handle(Geom_Curve)::DownCast(c->Reversed());
}

}  // namespace

// A network of iso-curves of a known surface gives that surface back
TEST(NetworkSurface, isoNetworkReproducesTheSurface)
{
    Curves p, g;
    for (double v : {0.0, 0.5, 1.0}) {
        p.push_back(along(saddle, true, v, 7));
    }
    for (double u : {0.0, 0.5, 1.0}) {
        g.push_back(along(saddle, false, u, 7));
    }
    auto r = networkSurface(p, g);
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_EQ(r.surface->UDegree(), 3);
    EXPECT_EQ(r.surface->VDegree(), 3);
    double worst = 0;
    for (int i = 0; i <= 10; ++i) {
        for (int j = 0; j <= 10; ++j) {
            worst = std::max(worst, distance(r.surface, saddle(i / 10.0, j / 10.0)));
        }
    }
    EXPECT_LT(worst, 1e-4);
}

// Crossings at uneven parameters: degree 3, simple knots, not rational
TEST(NetworkSurface, unevenNetworkStaysCubicAndSmooth)
{
    Curves p, g;
    unevenWave(p, g);
    NetworkOptions opt;
    auto r = networkSurface(p, g, opt);
    ASSERT_TRUE(r.isDone()) << r.error;
    const auto& s = r.surface;
    RecordProperty("poles",
                   std::to_string(s->NbUPoles()) + " x " + std::to_string(s->NbVPoles()));
    EXPECT_EQ(s->UDegree(), 3);
    EXPECT_EQ(s->VDegree(), 3);
    EXPECT_LE(maxInteriorMultiplicity(s), 1) << "C2 inside";
    EXPECT_FALSE(s->IsURational() || s->IsVRational());
    EXPECT_LE(s->NbUPoles(), 60);
    EXPECT_LE(s->NbVPoles(), 60);
    EXPECT_LT(deviation(s, p), 2 * opt.fitTolerance);
    EXPECT_LT(deviation(s, g), 2 * opt.fitTolerance);
    EXPECT_LE(r.maxFitDeviation, opt.fitTolerance);
    EXPECT_LT(r.maxGap, 1e-6);
}

// The network that made 2004 x 2004 poles in 14 to 20 s
TEST(NetworkSurface, largeNetworkIsBounded)
{
    std::vector<double> c;
    for (int i = 0; i < 12; ++i) {
        c.push_back(i / 11.0 + (i % 2 ? 0.02 : 0) * (i < 11));
    }
    Curves p, g;
    for (double v : c) {
        p.push_back(through(wave, true, v, c, 2));
    }
    for (double u : c) {
        g.push_back(through(wave, false, u, c, 2));
    }
    NetworkOptions opt;
    auto t0 = std::chrono::steady_clock::now();
    auto r = networkSurface(p, g, opt);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(r.isDone()) << r.error;
    RecordProperty("poles", std::to_string(r.surface->NbUPoles()) + " x "
                                + std::to_string(r.surface->NbVPoles()));
    RecordProperty("ms", std::to_string(int(s * 1000)));
    RecordProperty("fit", std::to_string(r.maxFitDeviation));
    EXPECT_LT(r.surface->NbUPoles(), 100);
    EXPECT_LT(r.surface->NbVPoles(), 100);
    EXPECT_LT(s, 1.0);
    EXPECT_LT(deviation(r.surface, p), 2 * opt.fitTolerance);
    EXPECT_LT(deviation(r.surface, g), 2 * opt.fitTolerance);
}

// Curves in any order and either way round are sorted and turned; the
// surface is the one the ordered network gives
TEST(NetworkSurface, curvesAreSortedAndTurned)
{
    Curves p, g;
    unevenWave(p, g);
    auto ordered = networkSurface(p, g);
    ASSERT_TRUE(ordered.isDone()) << ordered.error;

    // input k is profile perm[k] of the ordered network
    const std::vector<int> perm = {2, 0, 4, 1, 3};
    Curves ps;
    for (int k : perm) {
        ps.push_back(p[k]);
    }
    ps[1] = reversed(ps[1]);
    ps[3] = reversed(ps[3]);
    Curves gs = g;
    gs[2] = reversed(gs[2]);
    auto r = networkSurface(ps, gs);
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_EQ(r.profileOrder, (std::vector<int> {1, 3, 0, 4, 2}));
    EXPECT_EQ(r.guideOrder, (std::vector<int> {0, 1, 2, 3}));
    EXPECT_EQ(r.profileReversed, (std::vector<bool> {false, true, false, true, false}));
    EXPECT_EQ(r.guideReversed, (std::vector<bool> {false, false, true, false}));
    double worst = 0;
    for (int i = 0; i <= 8; ++i) {
        for (int j = 0; j <= 8; ++j) {
            double u = i / 8.0, v = j / 8.0;
            worst = std::max(worst,
                             ordered.surface->Value(u, v).Distance(r.surface->Value(u, v)));
        }
    }
    EXPECT_LT(worst, 1e-6);
}

// A profile that passes a guide 0.01 away crosses it at a tolerance of 0.02,
// not at the default
TEST(NetworkSurface, gapAgainstTolerance)
{
    Curves p, g;
    for (double v : {0.0, 0.5, 1.0}) {
        p.push_back(along(saddle, true, v, 7));
    }
    Fn lifted = [](double u, double v) {
        gp_Pnt q = saddle(u, v);
        return gp_Pnt(q.X(), q.Y(), q.Z() + 0.01);
    };
    p[1] = along(lifted, true, 0.5, 7);
    for (double u : {0.0, 0.5, 1.0}) {
        g.push_back(along(saddle, false, u, 7));
    }
    auto r = networkSurface(p, g);
    EXPECT_FALSE(r.isDone());
    EXPECT_NE(r.error.find("do not meet"), std::string::npos) << r.error;

    NetworkOptions opt;
    opt.tolerance = 0.02;
    r = networkSurface(p, g, opt);
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_NEAR(r.maxGap, 0.01, 1e-3);
}

// Rational input -- arcs -- gives a polynomial cubic, not a rational of high
// degree
TEST(NetworkSurface, rationalProfilesGivePolynomial)
{
    Curves p, g;
    auto radius = [](double z) {
        return 3 + 0.15 * z;
    };
    for (double z : {0.0, 4.0, 8.0, 12.0}) {
        double R = radius(z);
        p.push_back(
            GC_MakeArcOfCircle(gp_Pnt(-R, 0, z), gp_Pnt(0, R, z), gp_Pnt(R, 0, z)).Value());
    }
    for (int j = 0; j < 6; ++j) {
        double a = M_PI - M_PI * j / 5;
        std::vector<gp_Pnt> pts;
        for (double z = 0; z <= 12.0001; z += 2) {
            pts.emplace_back(radius(z) * std::cos(a), radius(z) * std::sin(a), z);
        }
        g.push_back(interp(pts));
    }
    NetworkOptions opt;
    auto r = networkSurface(p, g, opt);
    ASSERT_TRUE(r.isDone()) << r.error;
    EXPECT_EQ(r.surface->UDegree(), 3);
    EXPECT_EQ(r.surface->VDegree(), 3);
    EXPECT_FALSE(r.surface->IsURational() || r.surface->IsVRational());
    EXPECT_LT(deviation(r.surface, p), 2 * opt.fitTolerance);
    EXPECT_LT(deviation(r.surface, g), 2 * opt.fitTolerance);
}

TEST(NetworkSurface, tooFewCurves)
{
    Curves p, g;
    unevenWave(p, g);
    p.resize(1);
    auto r = networkSurface(p, g);
    EXPECT_FALSE(r.isDone());
    EXPECT_FALSE(r.error.empty());
}
