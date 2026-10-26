// SPDX-License-Identifier: LGPL-2.1-or-later

// The 3D layer of PlaneGCS (docs/Sketch3D.md, phase S0): every constraint's gradient
// against finite differences, the DOF count of canonical cases, the diagnosis of
// redundant and conflicting sets, convergence from perturbed starts, and a timing case
// against the 2D solver.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <numbers>
#include <random>

#include <gtest/gtest.h>

#include "Mod/Sketcher/App/planegcs/Constraints3D.h"
#include "Mod/Sketcher/App/planegcs/GCS.h"

namespace
{

constexpr double pi = std::numbers::pi;

// Parameters with stable addresses, and the list of those the solver may move.
class Params
{
public:
    double* add(double v, bool unknown = true)
    {
        values.push_back(v);
        double* p = &values.back();
        if (unknown) {
            unknowns.push_back(p);
        }
        return p;
    }
    GCS::Point3 point(double x, double y, double z, bool unknown = true)
    {
        return {add(x, unknown), add(y, unknown), add(z, unknown)};
    }
    // A workplane through o whose normal is the frame's Z rotated by angle about axis.
    GCS::Plane3
    plane(GCS::Point3 o, double ax, double ay, double az, double angle, bool unknown = false)
    {
        double l = std::sqrt(ax * ax + ay * ay + az * az);
        double s = std::sin(angle / 2) / l;
        GCS::Plane3 pl;
        pl.origin = o;
        pl.qw = add(std::cos(angle / 2), unknown);
        pl.qx = add(ax * s, unknown);
        pl.qy = add(ay * s, unknown);
        pl.qz = add(az * s, unknown);
        return pl;
    }

    std::deque<double> values;
    GCS::VEC_pD unknowns;
};

class SystemTest: public GCS::System
{
public:
    size_t getNumberOfConstraints(int tagID = -1)
    {
        return _getNumberOfConstraints(tagID);
    }
};

// The DOF count and the diagnosis, as the 2D facade sets them up.
int diagnose(SystemTest& sys, Params& ps)
{
    GCS::VEC_pD driven;
    sys.declareUnknowns(ps.unknowns);
    sys.declareDrivenParams(driven);
    sys.initSolution();
    return sys.dofsNumber();
}

GCS::SolveStatus solve(SystemTest& sys, Params& ps)
{
    diagnose(sys, ps);
    auto status = sys.solve(GCS::DogLeg);
    if (status == GCS::SolveStatus::Success) {
        sys.applySolution();
    }
    return status;
}

double dist(const GCS::Point3& a, const GCS::Point3& b)
{
    double dx = *b.x - *a.x, dy = *b.y - *a.y, dz = *b.z - *a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The gradient by every parameter against a central difference of the error.
void expectGradientMatches(GCS::Constraint& c, const char* what)
{
    GCS::VEC_pD params = c.params();
    std::sort(params.begin(), params.end());
    params.erase(std::unique(params.begin(), params.end()), params.end());
    for (double* p : params) {
        double v = *p;
        double h = 1e-6 * std::max(1.0, std::fabs(v));
        *p = v + h;
        double ep = c.error();
        *p = v - h;
        double em = c.error();
        *p = v;
        double numeric = (ep - em) / (2 * h);
        double analytic = c.grad(p);
        EXPECT_NEAR(analytic, numeric, 1e-6 * std::max(1.0, std::fabs(numeric)))
            << what << ": parameter at offset " << c.findParamInPvec(p);
    }
}

// A helix, as a model edge would be given.
class Helix: public GCS::FixedCurve3
{
public:
    double r = 5, pitch = 2;
    void value(double u, double p[3], double d1[3]) const override
    {
        p[0] = r * std::cos(u);
        p[1] = r * std::sin(u);
        p[2] = pitch * u;
        d1[0] = -r * std::sin(u);
        d1[1] = r * std::cos(u);
        d1[2] = pitch;
    }
};

// A rational cubic with five poles, clamped.
GCS::BSpline3 makeSpline(Params& ps, bool unknown = true)
{
    GCS::BSpline3 bsp;
    bsp.degree = 3;
    const double poles[5][3] = {{0, 0, 0}, {10, 2, 1}, {20, -3, 6}, {30, 4, 2}, {40, 0, -1}};
    const double weights[5] = {1, 1.5, 0.8, 1.2, 1};
    for (int i = 0; i < 5; ++i) {
        bsp.poles.push_back(ps.point(poles[i][0], poles[i][1], poles[i][2], unknown));
    }
    for (double w : weights) {
        bsp.weights.push_back(ps.add(w, unknown));
    }
    bsp.flatknots = {0, 0, 0, 0, 1, 2, 2, 2, 2};
    return bsp;
}

}  // namespace

// --------------------------------------------------------
// Gradients

TEST(Constraints3DGradient, everyConstraint)  // NOLINT
{
    Params ps;
    auto P = ps.point(1.3, -2.1, 0.7);
    auto Q = ps.point(-0.4, 3.2, 2.5);
    auto A = ps.point(0.2, 0.5, -1.1);
    auto B = ps.point(4.1, 1.7, 2.2);
    auto C = ps.point(-2.0, 1.0, 3.0);
    auto D = ps.point(1.0, -1.5, 0.5);
    auto N = ps.point(0.3, 0.4, 0.8);
    double* t = ps.add(0.37);
    double* s = ps.add(-1.7);
    double* d = ps.add(2.5);
    auto pl = ps.plane(ps.point(0.5, -0.2, 0.3), 1, 2, 0.5, 0.7, true);
    auto bsp = makeSpline(ps);
    double* u = ps.add(1.3);
    auto helix = std::make_shared<Helix>();

    using namespace GCS;
    std::vector<std::pair<const char*, std::unique_ptr<Constraint>>> cs;
    for (int k = 0; k < 3; ++k) {
        cs.emplace_back("P3OnLine", std::make_unique<ConstraintP3OnLine>(P, A, B, t, k));
        cs.emplace_back("P3OnBSpline", std::make_unique<ConstraintP3OnBSpline>(P, bsp, u, k));
        cs.emplace_back("P3OnFixedCurve",
                        std::make_unique<ConstraintP3OnFixedCurve>(P, helix, u, k));
        cs.emplace_back("P3Midpoint", std::make_unique<ConstraintP3Midpoint>(P, A, B, k));
        cs.emplace_back(
            "Dir3Parallel line-line",
            std::make_unique<ConstraintDir3Parallel>(Dir3::diff(A, B), Dir3::diff(C, D), s, k));
        cs.emplace_back("Dir3Parallel arc tangent-plane normal",
                        std::make_unique<ConstraintDir3Parallel>(Dir3::crossDiff(N, C, D),
                                                                 Dir3::planeNormal(pl),
                                                                 s,
                                                                 k));
    }
    cs.emplace_back("P3PlaneDistance", std::make_unique<ConstraintP3PlaneDistance>(P, pl, d));
    cs.emplace_back("P3OnPlane", std::make_unique<ConstraintP3PlaneDistance>(P, pl, nullptr));
    cs.emplace_back("Dir3Dot",
                    std::make_unique<ConstraintDir3Dot>(Dir3::diff(A, B), Dir3::vector(N)));
    cs.emplace_back(
        "Dir3Dot crossDiff",
        std::make_unique<ConstraintDir3Dot>(Dir3::crossDiff(N, A, B), Dir3::diff(C, D)));
    cs.emplace_back("Dir3Angle",
                    std::make_unique<ConstraintDir3Angle>(Dir3::diff(A, B), Dir3::diff(C, D), d));
    cs.emplace_back("P3Distance", std::make_unique<ConstraintP3Distance>(P, Q, d));
    cs.emplace_back("P3EqualDistance", std::make_unique<ConstraintP3EqualDistance>(A, B, C, D));
    cs.emplace_back("P3LineDistance", std::make_unique<ConstraintP3LineDistance>(P, A, B, d));
    cs.emplace_back("Vec3Unit", std::make_unique<ConstraintVec3Unit>(N));
    cs.emplace_back("QuatUnit", std::make_unique<ConstraintQuatUnit>(pl));

    for (auto& [what, c] : cs) {
        expectGradientMatches(*c, what);
    }
}

TEST(Constraints3DGradient, bsplineTangent)  // NOLINT
{
    // The tangent used for spline tangency, against differences of the value.
    Params ps;
    auto bsp = makeSpline(ps);
    GCS::VEC_pD pvec;
    bsp.push(pvec);
    GCS::BSpline3View view(pvec, 0, bsp.poles.size(), bsp.flatknots, bsp.degree);
    for (double u : {0.0, 0.4, 1.0, 1.6, 2.0}) {
        double h = 1e-6;
        double u0 = std::max(0.0, u - h), u1 = std::min(2.0, u + h);
        auto a = view.value(u0, 0, nullptr), b = view.value(u1, 0, nullptr);
        auto tg = view.tangent(u, nullptr);
        // One-sided at the ends, so first order there: relative tolerance.
        EXPECT_NEAR(tg.x, (b.x - a.x) / (u1 - u0), 1e-5 * std::max(1.0, std::fabs(tg.x)))
            << "u=" << u;
        EXPECT_NEAR(tg.y, (b.y - a.y) / (u1 - u0), 1e-5 * std::max(1.0, std::fabs(tg.y)))
            << "u=" << u;
        EXPECT_NEAR(tg.z, (b.z - a.z) / (u1 - u0), 1e-5 * std::max(1.0, std::fabs(tg.z)))
            << "u=" << u;
    }
    // Clamped: the ends are the end poles.
    auto s = view.value(0, 0, nullptr), e = view.value(2, 0, nullptr);
    EXPECT_DOUBLE_EQ(s.x, 0);
    EXPECT_DOUBLE_EQ(e.x, 40);
    EXPECT_DOUBLE_EQ(e.z, -1);
}

// --------------------------------------------------------
// DOF counts

TEST(Constraints3DDof, freeEntities)  // NOLINT
{
    {
        // Two free points held at a distance: 6 - 1.
        SystemTest sys;
        Params ps;
        auto p = ps.point(1, 2, 3);
        auto q = ps.point(4, 5, 6);
        GCS::addConstraintP3Distance(sys, p, q, ps.add(10, false), 1);
        EXPECT_EQ(diagnose(sys, ps), 5);
    }
    {
        // A line along Z: 6 - 2.
        SystemTest sys;
        Params ps;
        auto a = ps.point(0, 0, 0);
        auto b = ps.point(0.1, -0.2, 10);
        GCS::addConstraintP3AlongAxis(sys, a, b, 2, 1);
        EXPECT_EQ(diagnose(sys, ps), 4);
    }
    {
        // A circle: 7 parameters, a unit normal.
        SystemTest sys;
        Params ps;
        GCS::Circle3 c {ps.point(1, 2, 3), ps.point(0.1, 0.2, 0.9), ps.add(5)};
        GCS::addConstraintCircle3Rules(sys, c, 1);
        EXPECT_EQ(diagnose(sys, ps), 6);
    }
    {
        // An arc: 12 parameters, four internal equations.
        SystemTest sys;
        Params ps;
        GCS::Arc3 a {ps.point(0, 0, 0), ps.point(0, 0, 1), ps.point(5, 0, 0), ps.point(0, 5, 0)};
        GCS::addConstraintArc3Rules(sys, a, 1);
        EXPECT_EQ(diagnose(sys, ps), 8);
    }
}

TEST(Constraints3DDof, constraintsRemoveWhatTheTableSays)  // NOLINT
{
    // Two free lines (12 DOF) and each constraint alone, at a configuration near
    // satisfying it: the drop must be the table's (docs/Sketch3D.md 4.4), with no
    // partial redundancy. An auxiliary curve parameter (t) is one more unknown and one
    // more equation, so it does not change the count.
    struct Case
    {
        const char* what;
        int removes;
        std::function<
            void(SystemTest&, Params&, GCS::Point3&, GCS::Point3&, GCS::Point3&, GCS::Point3&)>
            add;
    };
    std::vector<Case> cases = {
        {"coincident",
         3,
         [](auto& s, auto&, auto& a, auto&, auto& c, auto&) {
             GCS::addConstraintP3Coincident(s, a, c, 1);
         }},
        {"along X",
         2,
         [](auto& s, auto&, auto& a, auto& b, auto&, auto&) {
             GCS::addConstraintP3AlongAxis(s, a, b, 0, 1);
         }},
        {"point on line",
         2,
         [](auto& s, auto& ps, auto& a, auto& b, auto& c, auto&) {
             GCS::addConstraintP3OnLine(s, c, a, b, ps.add(0.3), 1);
         }},
        {"parallel",
         2,
         [](auto& s, auto& ps, auto& a, auto& b, auto& c, auto& d) {
             GCS::addConstraintParallel3(s,
                                         GCS::Dir3::diff(a, b),
                                         GCS::Dir3::diff(c, d),
                                         ps.add(0.5),
                                         1);
         }},
        {"perpendicular",
         1,
         [](auto& s, auto&, auto& a, auto& b, auto& c, auto& d) {
             GCS::addConstraintPerpendicular3(s, GCS::Dir3::diff(a, b), GCS::Dir3::diff(c, d), 1);
         }},
        {"collinear",
         4,
         [](auto& s, auto& ps, auto& a, auto& b, auto& c, auto& d) {
             GCS::addConstraintCollinear3(s, a, b, c, d, ps.add(0.2), ps.add(0.8), 1);
         }},
        {"equal length",
         1,
         [](auto& s, auto&, auto& a, auto& b, auto& c, auto& d) {
             GCS::addConstraintEqualLength3(s, a, b, c, d, 1);
         }},
        {"midpoint",
         3,
         [](auto& s, auto&, auto& a, auto& b, auto& c, auto&) {
             GCS::addConstraintP3Midpoint(s, c, a, b, 1);
         }},
        {"distance",
         1,
         [](auto& s, auto& ps, auto& a, auto& b, auto&, auto&) {
             GCS::addConstraintP3Distance(s, a, b, ps.add(10, false), 1);
         }},
        {"distance along Y",
         1,
         [](auto& s, auto& ps, auto& a, auto& b, auto&, auto&) {
             GCS::addConstraintP3AxisDistance(s, a, b, 1, ps.add(3, false), 1);
         }},
        {"point-line distance",
         1,
         [](auto& s, auto& ps, auto& a, auto& b, auto& c, auto&) {
             GCS::addConstraintP3LineDistance(s, c, a, b, ps.add(2, false), 1);
         }},
        {"angle",
         1,
         [](auto& s, auto& ps, auto& a, auto& b, auto& c, auto& d) {
             GCS::addConstraintAngle3(s,
                                      GCS::Dir3::diff(a, b),
                                      GCS::Dir3::diff(c, d),
                                      ps.add(0.5, false),
                                      1);
         }},
    };
    for (auto& cs : cases) {
        SystemTest sys;
        Params ps;
        // Line 1 near the X axis, line 2 near it too but skew; the point C near line 1.
        auto a = ps.point(0, 0, 0);
        auto b = ps.point(10, 0.3, -0.2);
        auto c = ps.point(3, 0.2, 0.1);
        auto d = ps.point(8, 0.6, 0.4);
        cs.add(sys, ps, a, b, c, d);
        EXPECT_EQ(diagnose(sys, ps), 12 - cs.removes) << cs.what;
        EXPECT_FALSE(sys.hasPartiallyRedundant()) << cs.what;
        EXPECT_FALSE(sys.hasRedundant()) << cs.what;
    }
}

TEST(Constraints3DDof, rectangleOnAWorkplane)  // NOLINT
{
    // Four points on a fixed tilted workplane (12 - 4 = 8), four sides, three right
    // angles: a planar rectangle's 5 DOF. A fourth right angle is implied: redundant.
    for (bool fourth : {false, true}) {
        SystemTest sys;
        Params ps;
        auto pl = ps.plane(ps.point(0, 0, 40, false), 1, 0, 0, pi / 6);
        GCS::Point3 p[4] = {ps.point(0, 0, 40),
                            ps.point(20, 0, 40),
                            ps.point(20, 8.7, 45),
                            ps.point(0, 8.7, 45)};
        int tag = 1;
        for (auto& q : p) {
            GCS::addConstraintP3OnPlane(sys, q, pl, tag++);
        }
        for (int i = 0; i < (fourth ? 4 : 3); ++i) {
            GCS::addConstraintPerpendicular3(sys,
                                             GCS::Dir3::diff(p[i], p[(i + 1) % 4]),
                                             GCS::Dir3::diff(p[(i + 1) % 4], p[(i + 2) % 4]),
                                             tag++);
        }
        EXPECT_EQ(diagnose(sys, ps), 5) << (fourth ? "four right angles" : "three");
        EXPECT_EQ(sys.hasRedundant(), fourth);
    }
}

TEST(Constraints3DDof, parallelStaysRankExactOffAxis)  // NOLINT
{
    // Parallel is d2 = s d1 with s unknown: lines along each axis and along a diagonal
    // must all lose exactly 2.
    const double dirs[4][3] = {{1, 0.01, 0.02}, {0.01, 1, -0.02}, {0.02, -0.01, 1}, {1, 1, 1}};
    for (const auto& dir : dirs) {
        SystemTest sys;
        Params ps;
        auto a = ps.point(0, 0, 0);
        auto b = ps.point(dir[0] * 10, dir[1] * 10, dir[2] * 10);
        auto c = ps.point(1, 2, 3);
        auto d = ps.point(1 + dir[0] * 5, 2 + dir[1] * 5.1, 3 + dir[2] * 4.9);
        auto d1 = GCS::Dir3::diff(a, b), d2 = GCS::Dir3::diff(c, d);
        GCS::addConstraintParallel3(sys, d1, d2, ps.add(GCS::initialParallelRatio(d1, d2)), 1);
        EXPECT_EQ(diagnose(sys, ps), 10);
        EXPECT_FALSE(sys.hasPartiallyRedundant());
    }
}

TEST(Constraints3DSolve, parallelSwingsOntoAnotherAxis)  // NOLINT
{
    // Two lines drawn along Y, one then held along Z: the other must follow it round
    // in one solve, keeping its length (the case that broke a parallel built from two
    // cross product components picked by the directions as they were drawn).
    SystemTest sys;
    Params ps;
    auto a = ps.point(0, 0, 0, false);
    auto b = ps.point(0, 10, 0);
    auto c = ps.point(5, 0, 0);
    auto d = ps.point(5, 10, 0.5);
    auto d1 = GCS::Dir3::diff(a, b), d2 = GCS::Dir3::diff(c, d);
    int tag = 1;
    GCS::addConstraintP3AlongAxis(sys, a, b, 2, tag++);
    GCS::addConstraintP3Distance(sys, a, b, ps.add(10, false), tag++);
    GCS::addConstraintParallel3(sys, d1, d2, ps.add(GCS::initialParallelRatio(d1, d2)), tag++);
    GCS::addConstraintP3Distance(sys, c, d, ps.add(10, false), tag++);
    ASSERT_EQ(solve(sys, ps), GCS::SolveStatus::Success);
    EXPECT_NEAR(std::fabs(*b.z), 10, 1e-9);
    EXPECT_NEAR(std::fabs(*d.z - *c.z), 10, 1e-9);
    EXPECT_NEAR(*d.x - *c.x, 0, 1e-9);
    EXPECT_NEAR(*d.y - *c.y, 0, 1e-9);
}

// --------------------------------------------------------
// Diagnosis

TEST(Constraints3DDiagnosis, redundantAndConflicting)  // NOLINT
{
    {
        // The same point on the same plane twice.
        SystemTest sys;
        Params ps;
        auto pl = ps.plane(ps.point(0, 0, 0, false), 0, 1, 0, 0.3);
        auto p = ps.point(1, 2, 0.5);
        GCS::addConstraintP3OnPlane(sys, p, pl, 1);
        GCS::addConstraintP3OnPlane(sys, p, pl, 2);
        diagnose(sys, ps);
        EXPECT_TRUE(sys.hasRedundant());
        EXPECT_FALSE(sys.hasConflicting());
    }
    {
        // Two different distances between the same points.
        SystemTest sys;
        Params ps;
        auto a = ps.point(0, 0, 0);
        auto b = ps.point(3, 4, 5);
        GCS::addConstraintP3Distance(sys, a, b, ps.add(10, false), 1);
        GCS::addConstraintP3Distance(sys, a, b, ps.add(20, false), 2);
        diagnose(sys, ps);
        EXPECT_TRUE(sys.hasConflicting());
        GCS::VEC_I conflicting;
        sys.getConflicting(conflicting);
        EXPECT_EQ(conflicting.size(), 2u);
    }
}

// --------------------------------------------------------
// Convergence

TEST(Constraints3DSolve, routedPath)  // NOLINT
{
    // A pipe route from a fixed start: 10 along X, 20 along Z, then 15 perpendicular to
    // both. Fully constrained (the last leg's sense is a branch, not a DOF); solved from
    // starts perturbed by up to 30%, keeping the branch the start was on.
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jitter(-0.3, 0.3);
    for (int run = 0; run < 20; ++run) {
        SystemTest sys;
        Params ps;
        auto p0 = ps.point(0, 0, 0, false);
        auto p1 = ps.point(10 * (1 + jitter(rng)), jitter(rng), jitter(rng));
        auto p2 = ps.point(10 + jitter(rng), jitter(rng), 20 * (1 + jitter(rng)));
        auto p3 = ps.point(10 + jitter(rng), 15 * (1 + jitter(rng)), 20 + jitter(rng));
        int tag = 1;
        GCS::addConstraintP3AlongAxis(sys, p0, p1, 0, tag++);
        GCS::addConstraintP3Distance(sys, p0, p1, ps.add(10, false), tag++);
        GCS::addConstraintP3AlongAxis(sys, p1, p2, 2, tag++);
        GCS::addConstraintP3Distance(sys, p1, p2, ps.add(20, false), tag++);
        GCS::addConstraintPerpendicular3(sys,
                                         GCS::Dir3::diff(p0, p1),
                                         GCS::Dir3::diff(p2, p3),
                                         tag++);
        GCS::addConstraintPerpendicular3(sys,
                                         GCS::Dir3::diff(p1, p2),
                                         GCS::Dir3::diff(p2, p3),
                                         tag++);
        GCS::addConstraintP3Distance(sys, p2, p3, ps.add(15, false), tag++);
        ASSERT_EQ(solve(sys, ps), GCS::SolveStatus::Success) << "run " << run;
        EXPECT_EQ(sys.dofsNumber(), 0);
        EXPECT_NEAR(*p1.x, 10, 1e-9);
        EXPECT_NEAR(*p2.z, 20, 1e-9);
        EXPECT_NEAR(std::fabs(*p3.y), 15, 1e-9);
        EXPECT_NEAR(*p3.x, 10, 1e-9);
        EXPECT_NEAR(*p3.z, 20, 1e-9);
        // The branch the start was on is kept.
        EXPECT_GT(*p3.y, 0);
    }
}

TEST(Constraints3DSolve, curvesAndPlanes)  // NOLINT
{
    // A point on the helix model edge and on a tilted workplane through it; a point on
    // a spline at a distance from it; a line from there at an angle to X and a distance
    // from the first point. The dimensions are read from a configuration known to
    // satisfy them, and the solve starts from it perturbed.
    auto helix = std::make_shared<Helix>();
    const double u0 = 2.0, v0 = 0.6;
    double h[3], hd[3];
    helix->value(u0, h, hd);

    std::mt19937 rng(3);
    std::uniform_real_distribution<double> jitter(-1, 1);
    for (int run = 0; run < 10; ++run) {
        SystemTest sys;
        Params ps;
        auto pl = ps.plane(ps.point(h[0], h[1], h[2], false), 1, 0, 0, 0.2);
        auto bsp = makeSpline(ps, false);
        GCS::VEC_pD pvec;
        bsp.push(pvec);
        GCS::BSpline3View view(pvec, 0, 5, bsp.flatknots, 3);
        auto s0 = view.value(v0, 0, nullptr);
        const double r0[3] = {s0.x + 6, s0.y + 9, s0.z - 4};

        // The dimensions of that configuration.
        auto x0 = ps.point(0, 0, 0, false);
        auto x1 = ps.point(1, 0, 0, false);
        auto pT = ps.point(h[0], h[1], h[2], false);
        auto qT = ps.point(s0.x, s0.y, s0.z, false);
        auto rT = ps.point(r0[0], r0[1], r0[2], false);
        double* distance = ps.add(dist(pT, qT), false);
        double* angle = ps.add(0, false);
        double* lineDistance = ps.add(0, false);
        GCS::ConstraintDir3Angle(GCS::Dir3::diff(x0, x1), GCS::Dir3::diff(qT, rT), angle)
            .evaluate();
        GCS::ConstraintP3LineDistance(pT, qT, rT, lineDistance).evaluate();

        // The perturbed start.
        auto p =
            ps.point(h[0] + 0.5 * jitter(rng), h[1] + 0.5 * jitter(rng), h[2] + 0.5 * jitter(rng));
        double* u = ps.add(u0 + 0.1 * jitter(rng));
        auto q = ps.point(s0.x + jitter(rng), s0.y + jitter(rng), s0.z + jitter(rng));
        double* v = ps.add(v0 + 0.05 * jitter(rng));
        auto r = ps.point(r0[0] + jitter(rng), r0[1] + jitter(rng), r0[2] + jitter(rng));

        GCS::addConstraintP3OnFixedCurve(sys, p, helix, u, 1);
        GCS::addConstraintP3OnPlane(sys, p, pl, 2);
        GCS::addConstraintP3OnBSpline(sys, q, bsp, v, 3);
        GCS::addConstraintP3Distance(sys, p, q, distance, 4);
        GCS::addConstraintAngle3(sys, GCS::Dir3::diff(x0, x1), GCS::Dir3::diff(q, r), angle, 5);
        GCS::addConstraintP3LineDistance(sys, p, q, r, lineDistance, 6);

        ASSERT_EQ(solve(sys, ps), GCS::SolveStatus::Success) << "run " << run;
        // r keeps one DOF: the line can swing about the cone the two dimensions leave.
        EXPECT_EQ(sys.dofsNumber(), 1);
        double c[3], d1[3];
        helix->value(*u, c, d1);
        EXPECT_NEAR(*p.x, c[0], 1e-9);
        EXPECT_NEAR(*p.y, c[1], 1e-9);
        EXPECT_NEAR(*p.z, c[2], 1e-9);
        auto at = view.value(*v, 0, nullptr);
        EXPECT_NEAR(*q.x, at.x, 1e-9);
        EXPECT_NEAR(*q.y, at.y, 1e-9);
        EXPECT_NEAR(*q.z, at.z, 1e-9);
        EXPECT_NEAR(dist(p, q), *distance, 1e-9);
        for (int t = 1; t <= 6; ++t) {
            EXPECT_NEAR(sys.calculateConstraintErrorByTag(t), 0, 1e-9) << "tag " << t;
        }
    }
}

TEST(Constraints3DSolve, arcTangentToLines)  // NOLINT
{
    // The routing bend: an arc tangent to two lines at its ends, radius 5. The lines
    // meet at right angles in 3D (not in a frame plane).
    SystemTest sys;
    Params ps;
    auto a = ps.point(0, 0, 0, false);
    auto b = ps.point(20, 2, 1);
    auto c = ps.point(25, 15, 6);
    auto d = ps.point(26, 30, 12);
    GCS::Arc3 arc {ps.point(20, 7, 2),
                   ps.point(0.1, 0.1, 1),
                   ps.point(20, 2, 1),
                   ps.point(25, 7, 3)};
    int tag = 1;
    GCS::addConstraintArc3Rules(sys, arc, 0);
    GCS::addConstraintP3Coincident(sys, b, arc.start, tag++);
    GCS::addConstraintP3Coincident(sys, c, arc.end, tag++);
    GCS::addConstraintTangentLineArc3(sys, a, b, arc, arc.start, tag++);
    GCS::addConstraintTangentLineArc3(sys, c, d, arc, arc.end, tag++);
    GCS::addConstraintArc3Radius(sys, arc, ps.add(5, false), tag++);
    GCS::addConstraintPerpendicular3(sys, GCS::Dir3::diff(a, b), GCS::Dir3::diff(c, d), tag++);
    ASSERT_EQ(solve(sys, ps), GCS::SolveStatus::Success);
    EXPECT_NEAR(dist(arc.center, arc.start), 5, 1e-9);
    EXPECT_NEAR(dist(arc.center, arc.end), 5, 1e-9);
    EXPECT_FALSE(sys.hasConflicting());
    EXPECT_FALSE(sys.hasRedundant());
}

// --------------------------------------------------------
// Timing against the 2D solver

TEST(Constraints3DSolve, chainTimedAgainst2D)  // NOLINT
{
    // A chain of lines, each joined to the last, with a length and an axis direction
    // (3D) or horizontal / vertical (2D), solved from a perturbed start, diagnosis
    // included. Reports both times; it asserts only that both solve.
    // FC_S0_CHAIN overrides the length for a one-off measurement (docs/Sketch3D.md
    // asks for 1000); the suite runs a size that keeps it under a second.
    const char* env = std::getenv("FC_S0_CHAIN");
    const int n = env ? std::atoi(env) : 300;
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(-0.2, 0.2);

    auto time3d = [&]() {
        SystemTest sys;
        Params ps;
        std::vector<GCS::Point3> pts;
        pts.push_back(ps.point(0, 0, 0, false));
        double x = 0, y = 0, z = 0;
        for (int i = 0; i < n; ++i) {
            int axis = i % 3;
            (axis == 0 ? x : axis == 1 ? y : z) += 10;
            pts.push_back(ps.point(x + jitter(rng), y + jitter(rng), z + jitter(rng)));
            GCS::addConstraintP3AlongAxis(sys, pts[i], pts[i + 1], axis, i + 1);
            GCS::addConstraintP3Distance(sys, pts[i], pts[i + 1], ps.add(10, false), i + 1);
        }
        auto t0 = std::chrono::steady_clock::now();
        diagnose(sys, ps);
        auto t1 = std::chrono::steady_clock::now();
        auto status = sys.solve(GCS::DogLeg);
        auto t2 = std::chrono::steady_clock::now();
        EXPECT_EQ(status, GCS::SolveStatus::Success);
        EXPECT_EQ(sys.dofsNumber(), 0);
        return std::make_pair(std::chrono::duration<double, std::milli>(t1 - t0).count(),
                              std::chrono::duration<double, std::milli>(t2 - t1).count());
    };
    auto time2d = [&]() {
        SystemTest sys;
        std::deque<double> values;
        GCS::VEC_pD unknowns;
        auto add = [&](double v, bool unknown) {
            values.push_back(v);
            if (unknown) {
                unknowns.push_back(&values.back());
            }
            return &values.back();
        };
        std::vector<GCS::Point> pts;
        pts.emplace_back(add(0, false), add(0, false));
        double x = 0, y = 0;
        for (int i = 0; i < n; ++i) {
            (i % 2 == 0 ? x : y) += 10;
            pts.emplace_back(add(x + jitter(rng), true), add(y + jitter(rng), true));
            if (i % 2 == 0) {
                sys.addConstraintHorizontal(pts[i], pts[i + 1], i + 1);
            }
            else {
                sys.addConstraintVertical(pts[i], pts[i + 1], i + 1);
            }
            sys.addConstraintP2PDistance(pts[i], pts[i + 1], add(10, false), i + 1);
        }
        GCS::VEC_pD driven;
        auto t0 = std::chrono::steady_clock::now();
        sys.declareUnknowns(unknowns);
        sys.declareDrivenParams(driven);
        sys.initSolution();
        auto t1 = std::chrono::steady_clock::now();
        auto status = sys.solve(GCS::DogLeg);
        auto t2 = std::chrono::steady_clock::now();
        EXPECT_EQ(status, GCS::SolveStatus::Success);
        return std::make_pair(std::chrono::duration<double, std::milli>(t1 - t0).count(),
                              std::chrono::duration<double, std::milli>(t2 - t1).count());
    };
    auto [d3, s3] = time3d();
    auto [d2, s2] = time2d();
    std::cout << "chain of " << n << " lines, diagnose + solve: 3D " << d3 << " + " << s3
              << " ms, 2D " << d2 << " + " << s2 << " ms\n";
    RecordProperty("ms3d", std::to_string(d3 + s3));
    RecordProperty("ms2d", std::to_string(d2 + s2));
}
