// Does OCCT 8.0.1's GeomFill_Gordon build an exact surface from the curve
// networks a surfacing user draws, how fast, and how big? Deviation is
// measured here, independently of the builder's own report: points sampled on
// each input curve are projected onto the result (skipped above 40000 poles,
// where the projection itself takes minutes). docs/Surface.md sec 3.4 has the
// results of 2026-10-27.
//
// Build and run against the dev stack's OCCT:
//   O=~/works/sw/occt/install/conda-relwithdebinfo-801
//   .conda/run.sh bash -c "\$CXX -std=c++17 -O1 -I$O/include/opencascade \
//     scripts/occt-gordon-probe.cpp -L$O/lib -Wl,-rpath,$O/lib -lTKGeomAlgo \
//     -lTKGeomBase -lTKG3d -lTKG2d -lTKMath -lTKernel -o /tmp/gordon-probe"
//   /tmp/gordon-probe
#include <GeomFill_Gordon.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <GeomConvert.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using Curve = occ::handle<Geom_Curve>;
using Fn = std::function<gp_Pnt(double, double)>;

static Curve interp(const std::vector<gp_Pnt>& pts)
{
    occ::handle<TColgp_HArray1OfPnt> a = new TColgp_HArray1OfPnt(1, (int)pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        a->SetValue((int)i + 1, pts[i]);
    GeomAPI_Interpolate it(a, false, 1e-9);
    it.Perform();
    return it.Curve();
}

// A curve along u at fixed v (profile), or along v at fixed u (guide), through
// n points of f; `warp` bends the parameter so curves are not iso-spaced
static Curve along(const Fn& f, bool alongU, double fixed, int n, double warp = 0)
{
    std::vector<gp_Pnt> pts;
    for (int i = 0; i < n; ++i) {
        double t = double(i) / (n - 1);
        t = t + warp * t * (1 - t);
        pts.push_back(alongU ? f(t, fixed) : f(fixed, t));
    }
    return interp(pts);
}

// A network curve: through f at every crossing parameter `cross` and at
// `extra` points between each pair, so profiles and guides meet exactly
static Curve through(const Fn& f, bool alongU, double fixed, const std::vector<double>& cross,
                     int extra)
{
    std::vector<gp_Pnt> pts;
    for (size_t i = 0; i < cross.size(); ++i) {
        double t = cross[i];
        pts.push_back(alongU ? f(t, fixed) : f(fixed, t));
        if (i + 1 < cross.size())
            for (int k = 1; k <= extra; ++k) {
                double s = t + (cross[i + 1] - t) * k / (extra + 1);
                pts.push_back(alongU ? f(s, fixed) : f(fixed, s));
            }
    }
    return interp(pts);
}

static double deviation(const occ::handle<Geom_BSplineSurface>& s, const Curve& c)
{
    double worst = 0;
    double a = c->FirstParameter(), b = c->LastParameter();
    for (int i = 0; i <= 50; ++i) {
        gp_Pnt p = c->Value(a + (b - a) * i / 50.0);
        GeomAPI_ProjectPointOnSurf pr(p, s);
        if (pr.NbPoints() == 0)
            return 1e9;
        worst = std::max(worst, pr.LowerDistance());
    }
    return worst;
}

static void run(const char* name, const std::vector<Curve>& prof, const std::vector<Curve>& guide,
                double tol = 1e-4)
{
    NCollection_Array1<Curve> P(1, (int)prof.size()), G(1, (int)guide.size());
    for (size_t i = 0; i < prof.size(); ++i)
        P.SetValue((int)i + 1, prof[i]);
    for (size_t i = 0; i < guide.size(); ++i)
        G.SetValue((int)i + 1, guide[i]);
    GeomFill_Gordon g;
    auto t0 = std::chrono::steady_clock::now();
    try {
        g.Init(P, G, tol);
        g.Perform();
    }
    catch (Standard_Failure& e) {
        printf("%-34s THROW %s\n", name, e.what());
        return;
    }
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    if (!g.IsDone()) {
        fflush(stdout);
        printf("%-34s FAIL status=%d stage=%d (%.1f ms)\n", name, (int)g.Status(),
               (int)g.Report().FailedStage, ms);
        return;
    }
    const auto& s = g.Surface();
    printf("%-34s built %6.1f ms poles=%dx%d deg=%d/%d knots=%dx%d\n", name, ms, s->NbUPoles(),
           s->NbVPoles(), s->UDegree(), s->VDegree(), s->NbUKnots(), s->NbVKnots());
    {
        int mu = 0, mv = 0;
        for (int i = 2; i < s->NbUKnots(); ++i)
            mu = std::max(mu, s->UMultiplicity(i));
        for (int i = 2; i < s->NbVKnots(); ++i)
            mv = std::max(mv, s->VMultiplicity(i));
        double wmin = 1e300, wmax = -1e300;
        for (int i = 1; i <= s->NbUPoles(); ++i)
            for (int j = 1; j <= s->NbVPoles(); ++j) {
                wmin = std::min(wmin, s->Weight(i, j));
                wmax = std::max(wmax, s->Weight(i, j));
            }
        printf("%-34s interior mult max %d/%d weights %.4g..%.4g\n", name, mu, mv, wmin, wmax);
    }
    fflush(stdout);
    double dev = 0;
    if (s->NbUPoles() * s->NbVPoles() > 40000)
        dev = -1; // too big to project onto in reasonable time
    else
    {
        for (auto& c : prof)
            dev = std::max(dev, deviation(s, c));
        for (auto& c : guide)
            dev = std::max(dev, deviation(s, c));
    }
    const auto& r = g.Report();
    printf("%-34s OK %6.1f ms approx=%d poles=%dx%d deg=%d/%d rational=%d measured=%.2e "
           "report(prof=%.1e guide=%.1e gap=%.1e)\n",
           name, ms, (int)g.IsApproximate(), s->NbUPoles(), s->NbVPoles(), s->UDegree(),
           s->VDegree(), (int)(s->IsURational() || s->IsVRational()), dev,
           r.MaxProfileDeviation, r.MaxGuideDeviation, r.MaxContactGap);
}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Fn saddle = [](double u, double v) {
        double x = 10 * u, y = 10 * v;
        return gp_Pnt(x, y, 0.1 * (x - 5) * (y - 5));
    };
    Fn wave = [](double u, double v) {
        double x = 20 * u, y = 12 * v;
        return gp_Pnt(x, y + 1.5 * std::sin(u * 3), 2 * std::sin(x / 4) * std::cos(y / 5));
    };

    // 1. 3 x 3 iso network on a saddle
    {
        std::vector<Curve> p, g;
        for (double v : {0.0, 0.5, 1.0})
            p.push_back(along(saddle, true, v, 7));
        for (double u : {0.0, 0.5, 1.0})
            g.push_back(along(saddle, false, u, 7));
        run("saddle 3x3 iso", p, g);
    }
    // 2. 5 x 4 on a wave, uneven spacing, different point counts per curve
    {
        std::vector<double> vs = {0.0, 0.3, 0.55, 0.8, 1.0}, us = {0.0, 0.35, 0.7, 1.0};
        std::vector<Curve> p, g;
        int k = 0;
        for (double v : vs)
            p.push_back(through(wave, true, v, us, 1 + (k++ % 3)));
        for (double u : us)
            g.push_back(through(wave, false, u, vs, 1 + (k++ % 2)));
        run("wave 5x4 uneven", p, g);
    }
    // 3. a 4 x 6 network whose profiles are rational arcs (a duct)
    {
        std::vector<Curve> p, g;
        std::vector<double> zs = {0, 4, 8, 12};
        auto r = [](double z) { return 3 + 0.15 * z; };
        for (double z : zs) {
            double R = r(z);
            p.push_back(GC_MakeArcOfCircle(gp_Pnt(-R, 0, z), gp_Pnt(0, R, z), gp_Pnt(R, 0, z))
                            .Value());
        }
        for (int j = 0; j < 6; ++j) {
            double a = M_PI - M_PI * j / 5;
            std::vector<gp_Pnt> pts;
            for (double z = 0; z <= 12.0001; z += 2)
                pts.push_back(gp_Pnt(r(z) * std::cos(a), r(z) * std::sin(a), z));
            g.push_back(interp(pts));
        }
        run("duct 4x6 rational arcs", p, g);
    }
    // 4. the saddle network with a profile that misses one guide by 0.01
    {
        std::vector<Curve> p, g;
        for (double v : {0.0, 0.5, 1.0})
            p.push_back(along(saddle, true, v, 7));
        Fn lifted = [&](double u, double v) {
            gp_Pnt q = saddle(u, v);
            return gp_Pnt(q.X(), q.Y(), q.Z() + 0.01);
        };
        p[1] = along(lifted, true, 0.5, 7);
        for (double u : {0.0, 0.5, 1.0})
            g.push_back(along(saddle, false, u, 7));
        run("saddle, profile 0.01 off", p, g);
    }
    // 4b. the same at a tolerance that admits the gap
    {
        std::vector<Curve> p, g;
        for (double v : {0.0, 0.5, 1.0})
            p.push_back(along(saddle, true, v, 7));
        Fn lifted = [&](double u, double v) {
            gp_Pnt q = saddle(u, v);
            return gp_Pnt(q.X(), q.Y(), q.Z() + 0.01);
        };
        p[1] = along(lifted, true, 0.5, 7);
        for (double u : {0.0, 0.5, 1.0})
            g.push_back(along(saddle, false, u, 7));
        run("saddle, 0.01 off, tol 0.02", p, g, 0.02);
    }
    // 5. a large network: 12 x 12 on the wave
    {
        std::vector<double> c;
        for (int i = 0; i < 12; ++i)
            c.push_back(i / 11.0 + (i % 2 ? 0.02 : 0) * (i < 11));
        std::vector<Curve> p, g;
        for (double v : c)
            p.push_back(through(wave, true, v, c, 2));
        for (double u : c)
            g.push_back(through(wave, false, u, c, 2));
        run("wave 12x12", p, g);
    }
    // 6. a closed network: rings around a bulging tube, guides along it
    {
        Fn tube = [](double u, double v) {
            double a = 2 * M_PI * u, z = 20 * v, R = 4 + std::sin(v * M_PI) * 2;
            return gp_Pnt(R * std::cos(a), R * std::sin(a), z);
        };
        std::vector<double> vs = {0, 0.25, 0.5, 0.75, 1}, us = {0, 0.25, 0.5, 0.75, 1};
        std::vector<Curve> p, g;
        for (double v : vs)
            p.push_back(through(tube, true, v, us, 2));
        for (double u : {0.0, 0.25, 0.5, 0.75})
            g.push_back(through(tube, false, u, vs, 2));
        g.push_back(g.front());
        run("tube 5x4 closed (seam repeated)", p, g);
    }
    return 0;
}
