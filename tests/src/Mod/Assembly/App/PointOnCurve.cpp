// SPDX-License-Identifier: LGPL-2.1-or-later

// The PointOnCurve joint (docs/AssemblySolverInterface.md sec 6.18): the
// neutral projector and how it follows the curve parameter (many turns of a
// closed curve, across its seam, never jumping across the curve, an open
// curve's extension), the equations' residuals and analytic Jacobian against
// central differences on a line, a circle, planar and 3D B-splines and a
// periodic B-spline across its seam, their Hessians, the DoF, and a cam
// mechanism solved and simulated on every backend.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Ellipse.hxx>
#include <Geom_Line.hxx>
#include <NCollection_Array1.hxx>
#include <NCollection_HArray1.hxx>
#include <gp_Ax2.hxx>

#include <Base/Interpreter.h>
#include <Base/Placement.h>
#include <Mod/Assembly/App/CamJoint.h>
#include <Mod/Assembly/App/Solver/Curve.h>
#include <Mod/Assembly/App/Solver/OndselBackend.h>
#include <Mod/Assembly/App/Solver/ReferenceBackend.h>
#include <Mod/Assembly/App/Solver/Registry.h>
#include <src/App/InitApplication.h>

using namespace Assembly::Solver;

namespace
{

constexpr double pi = std::numbers::pi;

Base::Rotation rot(double x, double y, double z, double angle)
{
    return Base::Rotation(Base::Vector3d(x, y, z), angle);
}

Base::Placement at(double x, double y, double z, const Base::Rotation& r = Base::Rotation())
{
    return Base::Placement(Base::Vector3d(x, y, z), r);
}

// A model under construction: G grounded at the origin, more bodies free.
struct Builder
{
    Model m;
    Id g = NoId;

    Builder()
    {
        Frame onGround;
        onGround.label = m.addSource("marker-G");
        const Id frameGround = m.addFrame(onGround);
        Body body;
        body.source = m.addSource("G");
        body.grounded = true;
        g = m.addBody(body);
        Frame fix;
        fix.body = g;
        fix.label = m.addSource("FixingMarker");
        const Id frameFix = m.addFrame(fix);
        m.addGround({NoId, m.addSource("G"), frameGround, frameFix});
    }

    Id body(const char* name, const Base::Placement& placement)
    {
        Body b;
        b.source = m.addSource(name);
        b.placement = placement;
        return m.addBody(b);
    }

    Id frame(Id body, const Base::Placement& local)
    {
        Frame f;
        f.body = body;
        f.local = local;
        f.label = m.addSource("F" + std::to_string(m.frames.size()));
        return m.addFrame(f);
    }

    Id joint(JointKind kind, Id fi, Id fj, double value = 0.0)
    {
        Joint j;
        j.kind = kind;
        j.source = m.addSource("J" + std::to_string(m.joints.size()));
        j.frameI = fi;
        j.frameJ = fj;
        j.value = value;
        return m.addJoint(j);
    }

    Id cam(Id fi, Id fj, std::shared_ptr<const CurveGeometry> curve, double offset = 0.0)
    {
        const Id id = joint(JointKind::PointOnCurve, fi, fj, offset);
        Curve c;
        c.source = m.joints[id].source;
        c.geometry = std::move(curve);
        m.joints[id].curve = m.addCurve(c);
        return id;
    }

    Base::Placement global(Id frame, const std::vector<Base::Placement>& poses) const
    {
        const Frame& f = m.frames[frame];
        return f.body == NoId ? f.local : poses[f.body] * f.local;
    }
};

std::vector<Base::Placement> placements(const std::vector<BodyPose>& poses)
{
    std::vector<Base::Placement> out;
    for (const auto& p : poses) {
        out.push_back(p.placement);
    }
    return out;
}

struct Recorder: TrajectorySink
{
    std::vector<double> times;
    std::vector<std::vector<BodyPose>> frames;
    bool frame(double time, const std::vector<BodyPose>& poses) override
    {
        times.push_back(time);
        frames.push_back(poses);
        return true;
    }
};

// ---------------------------------------------------------------- curves

std::shared_ptr<const CurveGeometry> circle(double cx, double cy, double r)
{
    Handle(Geom_Circle) c = new Geom_Circle(gp_Ax2(gp_Pnt(cx, cy, 0), gp_Dir(0, 0, 1)), r);
    return Assembly::makeCurveGeometry(c, 0.0, 2 * pi);
}

std::shared_ptr<const CurveGeometry> line()
{
    Handle(Geom_Line) l = new Geom_Line(gp_Pnt(1, 2, 0), gp_Dir(1, 0.5, 0.25));
    return Assembly::makeCurveGeometry(l, -10.0, 30.0);
}

std::shared_ptr<const CurveGeometry> interpolated(const std::vector<gp_Pnt>& points, bool periodic)
{
    const int count = static_cast<int>(points.size());
    Handle(NCollection_HArray1<gp_Pnt>) pts = new NCollection_HArray1<gp_Pnt>(1, count);
    for (std::size_t i = 0; i < points.size(); ++i) {
        pts->SetValue(static_cast<int>(i + 1), points[i]);
    }
    GeomAPI_Interpolate interpolate(pts, periodic, 1e-9);
    interpolate.Perform();
    Handle(Geom_BSplineCurve) curve = interpolate.Curve();
    return Assembly::makeCurveGeometry(curve, curve->FirstParameter(), curve->LastParameter());
}

// A wavy planar spline in z = 0.
std::shared_ptr<const CurveGeometry> planarSpline()
{
    std::vector<gp_Pnt> p;
    for (int i = 0; i <= 8; ++i) {
        p.emplace_back(5.0 * i, 3.0 * std::sin(0.9 * i), 0.0);
    }
    return interpolated(p, false);
}

// A helix-like spline: not planar.
std::shared_ptr<const CurveGeometry> spatialSpline()
{
    std::vector<gp_Pnt> p;
    for (int i = 0; i <= 10; ++i) {
        const double a = 0.6 * i;
        p.emplace_back(10.0 * std::cos(a), 10.0 * std::sin(a), 1.5 * i);
    }
    return interpolated(p, false);
}

// A closed, periodic spline round an egg-shaped cam: a uniform periodic
// cubic B-spline, smooth (C2) across its seam as everywhere. (OCCT's periodic
// interpolation is only C1 at its seam -- a knot of multiplicity 2 -- where
// the curvature, and with it the equations' gradient off the curve, jumps.)
std::shared_ptr<const CurveGeometry> periodicSpline()
{
    const int n = 12;
    NCollection_Array1<gp_Pnt> poles(1, n);
    NCollection_Array1<double> knots(1, n + 1);
    NCollection_Array1<int> mults(1, n + 1);
    for (int i = 0; i < n; ++i) {
        const double a = 2 * pi * i / n;
        const double r = 22.0 + 4.0 * std::cos(a) + 1.5 * std::sin(2 * a);
        poles.SetValue(i + 1, gp_Pnt(r * std::cos(a), r * std::sin(a), 0.0));
    }
    for (int i = 0; i <= n; ++i) {
        knots.SetValue(i + 1, 3.0 * i);
        mults.SetValue(i + 1, 1);
    }
    Handle(Geom_BSplineCurve) curve = new Geom_BSplineCurve(poles, knots, mults, 3, Standard_True);
    return Assembly::makeCurveGeometry(curve, curve->FirstParameter(), curve->LastParameter());
}

double distanceToCurveGlobally(const CurveGeometry& c, const Base::Vector3d& x)
{
    return projectOnCurveGlobally(c, x).distance;
}

}  // namespace

// ---------------------------------------------------------------- the projector

// The curves as the factory makes them: closed or open, planar or not.
TEST(PointOnCurve, curveGeometry)  // NOLINT
{
    EXPECT_TRUE(circle(0, 0, 10)->closed());
    EXPECT_NEAR(circle(0, 0, 10)->planeNormal().z, 1.0, 1e-12);
    EXPECT_FALSE(line()->closed());
    EXPECT_LT(line()->planeNormal().Length(), 0.5);
    EXPECT_FALSE(planarSpline()->closed());
    EXPECT_NEAR(std::abs(planarSpline()->planeNormal().z), 1.0, 1e-9);
    EXPECT_LT(spatialSpline()->planeNormal().Length(), 0.5);
    EXPECT_TRUE(periodicSpline()->closed());
    // The same geometry compares the same; another does not.
    EXPECT_TRUE(circle(0, 0, 10)->same(*circle(0, 0, 10)));
    EXPECT_FALSE(circle(0, 0, 10)->same(*circle(0, 0, 10.5)));
    // The reference of a non-planar curve is far from every tangent.
    double worst = 1.0;
    curveReference(*spatialSpline(), &worst);
    EXPECT_LT(worst, 0.9);
    // Moved into a frame: the curve is where the placement puts it.
    Handle(Geom_Circle) c = new Geom_Circle(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 5.0);
    const Base::Placement plc = at(1, 2, 3, rot(1, 0, 0, pi / 2));
    auto moved = Assembly::makeCurveGeometry(c, 0.0, 2 * pi, plc);
    EXPECT_LT((moved->evaluate(0.0).p - plc.getPosition() - Base::Vector3d(5, 0, 0)).Length(), 1e-12);
    EXPECT_NEAR(std::abs(moved->planeNormal().y), 1.0, 1e-12);
}

// A point going round a circle many times, in steps of 0.3 rad and of 1.9
// rad (more than a quarter turn), both ways: the parameter followed from the
// last one is the point's angle with its turns counted, across the seam every
// turn.
TEST(PointOnCurve, followsAClosedCurveForManyTurns)  // NOLINT
{
    const auto c = circle(2, -1, 10);
    for (double step : {0.3, 1.9, -0.3, -1.9}) {
        SCOPED_TRACE(step);
        const int n = static_cast<int>(std::ceil(12 * 2 * pi / std::abs(step)));
        double u = projectOnCurveGlobally(*c, Base::Vector3d(2 + 13, -1, 0.5)).u;
        EXPECT_NEAR(u, 0.0, 1e-12);
        for (int k = 1; k <= n; ++k) {
            const double theta = step * k;
            const Base::Vector3d x(2 + 13 * std::cos(theta), -1 + 13 * std::sin(theta), 0.5);
            const CurveProjection p = projectOnCurve(*c, x, u);
            ASSERT_TRUE(p.converged) << k;
            ASSERT_NEAR(p.u, theta, 1e-12) << k;
            EXPECT_NEAR(p.distance, std::hypot(3.0, 0.5), 1e-12);
            u = p.u;
        }
        // Twelve turns and more, counted.
        EXPECT_GT(std::abs(u), 12 * 2 * pi);
    }

    // A periodic B-spline: the parameter grows by a period per turn, and the
    // closest point is the one over the whole curve.
    const auto s = periodicSpline();
    double u = projectOnCurveGlobally(*s, Base::Vector3d(28, 0, -0.25)).u;
    const double u0 = u;
    const int n = 10 * 9;
    for (int k = 1; k <= n; ++k) {
        const double a = 2 * pi * k / 9.0;
        const double r = 27.0 + std::cos(3 * a);
        const Base::Vector3d x(r * std::cos(a), r * std::sin(a), -0.25);
        const CurveProjection p = projectOnCurve(*s, x, u);
        ASSERT_TRUE(p.converged) << k;
        ASSERT_GT(p.u, u) << k;
        EXPECT_NEAR(p.distance, distanceToCurveGlobally(*s, x), 1e-10) << k;
        u = p.u;
    }
    EXPECT_NEAR((u - u0) / s->period(), 10.0, 1e-9);
}

// The projection follows the stretch of the curve it starts on: inside a
// long ellipse, a point nearer its top projects onto the bottom when it starts
// there, and onto the top only from the top or from a global search.
TEST(PointOnCurve, neverJumpsAcrossTheCurve)  // NOLINT
{
    Handle(Geom_Ellipse) e = new Geom_Ellipse(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 20.0, 2.0);
    const auto c = Assembly::makeCurveGeometry(e, 0.0, 2 * pi);
    const Base::Vector3d x(0.0, 1.5, 0.0);
    const CurveProjection bottom = projectOnCurve(*c, x, 1.5 * pi);
    EXPECT_NEAR(std::remainder(bottom.u - 1.5 * pi, 2 * pi), 0.0, 1e-9);
    EXPECT_NEAR(bottom.distance, 3.5, 1e-9);
    const CurveProjection top = projectOnCurve(*c, x, 0.5 * pi);
    EXPECT_NEAR(top.distance, 0.5, 1e-9);
    EXPECT_NEAR(projectOnCurveGlobally(*c, x).distance, 0.5, 1e-9);
}

// An open curve goes on along its end tangents: a point past an end projects
// onto the extension, its parameter beyond the range.
TEST(PointOnCurve, openCurveExtendsAlongItsTangents)  // NOLINT
{
    const auto c = planarSpline();
    const CurvePoint end = c->evaluate(c->last());
    const Base::Vector3d t = end.d1 / end.d1.Length();
    const Base::Vector3d n(-t.y, t.x, 0.0);
    const Base::Vector3d x = end.p + t * 7.0 + n * 2.0;
    const CurveProjection p = projectOnCurve(*c, x, c->last() - 0.1);
    EXPECT_TRUE(p.converged);
    EXPECT_GT(p.u, c->last());
    EXPECT_NEAR(p.distance, 2.0, 1e-9);
    EXPECT_NEAR((p.at.p - (end.p + t * 7.0)).Length(), 0.0, 1e-9);
}

// ---------------------------------------------------------------- the equations

namespace
{

struct CurveCase
{
    const char* name;
    std::shared_ptr<const CurveGeometry> curve;
    double u0;  // where the follower is put near
    double offset;  // a roller's (planar curves)
};

std::vector<CurveCase> curveCases()
{
    const auto s = periodicSpline();
    return {
        {"line", line(), 4.0, 0.0},
        {"circle", circle(3, 1, 12), 1.0, 0.0},
        {"circle, roller", circle(3, 1, 12), 2.5, 2.0},
        {"circle across its seam", circle(3, 1, 12), 1e-9, 0.0},
        {"planar spline", planarSpline(), 0.4, 0.0},
        {"planar spline, roller", planarSpline(), 0.6, -1.5},
        {"3D spline", spatialSpline(), 0.5, 0.0},
        {"periodic spline across its seam", s, s->first() + 1e-9, 0.0},
        {"periodic spline, roller", s, s->first() + 0.3 * s->period(), 3.0},
    };
}

// The normal frame (N1, N2) at a parameter, as the equations build it.
void normals(const CurveGeometry& c, double u, Base::Vector3d& n1, Base::Vector3d& n2)
{
    const CurvePoint p = evaluateCurve(c, u);
    const Base::Vector3d t = p.d1 / p.d1.Length();
    const Base::Vector3d a = c.reference();
    n1 = a - t * (a * t);
    n1.Normalize();
    n2 = t % n1;
}

}  // namespace

// The residuals: zero on the curve (offset), the distance off it otherwise;
// the analytic Jacobian of both rows against central differences, the cam
// and the follower both free, at a point off the curve and across a closed
// curve's seam.
TEST(PointOnCurve, residualAndJacobianByDifferences)  // NOLINT
{
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    auto randomPose = [&](double spread) {
        return at(spread * uni(rng),
                  spread * uni(rng),
                  spread * uni(rng),
                  rot(uni(rng), uni(rng), uni(rng) + 1.5, 2.0 * uni(rng)));
    };
    for (const auto& cc : curveCases()) {
        SCOPED_TRACE(cc.name);
        for (double off : {0.0, 0.7}) {
            SCOPED_TRACE(off);
            Builder b;
            const Id a = b.body("A", randomPose(10.0));
            const Base::Placement onA = randomPose(5.0);
            const Base::Placement onB = randomPose(5.0);
            // B placed so that frame J's origin is the curve's (offset) point
            // at u0, moved off it by 'off' along N1 and N2.
            Base::Vector3d n1, n2;
            normals(*cc.curve, cc.u0, n1, n2);
            const Base::Vector3d x = evaluateCurve(*cc.curve, cc.u0).p + n2 * (cc.offset + off)
                + n1 * (0.5 * off);
            const Base::Placement frameJ = b.m.bodies[a].placement * onA * at(x.x, x.y, x.z)
                * Base::Placement(Base::Vector3d(), rot(1, 2, 3, 0.4));
            const Id body = b.body("B", frameJ * onB.inverse());
            b.cam(b.frame(a, onA), b.frame(body, onB), cc.curve, cc.offset);

            ReferenceSession session;
            ASSERT_EQ(session.load(b.m), Status::Ok);
            std::vector<Base::Placement> poses;
            for (const auto& bd : b.m.bodies) {
                poses.push_back(bd.placement);
            }
            const auto r0 = session.residualsAt(poses);
            ASSERT_EQ(r0.size(), 6u + 2u);  // the ground's six, the joint's two
            EXPECT_NEAR(r0[6], 0.5 * off, 1e-9);
            EXPECT_NEAR(r0[7], off, 1e-9);

            const auto J = session.jacobianAt(poses);
            const double h = 1e-6;
            for (std::size_t bi = 1; bi < poses.size(); ++bi) {
                for (int c = 0; c < 6; ++c) {
                    auto plus = poses;
                    auto minus = poses;
                    if (c < 3) {
                        Base::Vector3d dx;
                        dx[c] = h;
                        plus[bi].setPosition(plus[bi].getPosition() + dx);
                        minus[bi].setPosition(minus[bi].getPosition() - dx);
                    }
                    else {
                        Base::Vector3d w;
                        w[c - 3] = 1.0;
                        plus[bi].setRotation(Base::Rotation(w, h) * plus[bi].getRotation());
                        minus[bi].setRotation(Base::Rotation(w, -h) * minus[bi].getRotation());
                    }
                    const auto rp = session.residualsAt(plus);
                    const auto rm = session.residualsAt(minus);
                    for (std::size_t r = 6; r < J.size(); ++r) {
                        const double numeric = (rp[r] - rm[r]) / (2 * h);
                        const double analytic = J[r][6 * bi + c];
                        EXPECT_NEAR(analytic, numeric, 1e-6 * std::max(1.0, std::abs(numeric)))
                            << "row " << r << " body " << bi << " column " << c;
                    }
                }
            }
        }
    }
}

// The Hessian (OndselSolver's second derivatives) where the equations hold:
// against central differences of the analytic gradient.
TEST(PointOnCurve, hessianByDifferences)  // NOLINT
{
    for (const auto& cc : curveCases()) {
        SCOPED_TRACE(cc.name);
        Base::Vector3d n1, n2;
        normals(*cc.curve, cc.u0, n1, n2);
        const Base::Vector3d x = evaluateCurve(*cc.curve, cc.u0).p + n2 * cc.offset;
        const PointOnCurveRows rows = pointOnCurveRows(*cc.curve, cc.offset, x, cc.u0);
        ASSERT_TRUE(rows.converged);
        EXPECT_NEAR(rows.value[0], 0.0, 1e-10);
        EXPECT_NEAR(rows.value[1], 0.0, 1e-10);
        const double h = 1e-5;
        for (int j = 0; j < 3; ++j) {
            Base::Vector3d dx;
            dx[j] = h;
            const auto plus = pointOnCurveRows(*cc.curve, cc.offset, x + dx, rows.u);
            const auto minus = pointOnCurveRows(*cc.curve, cc.offset, x - dx, rows.u);
            for (int k = 0; k < 2; ++k) {
                for (int i = 0; i < 3; ++i) {
                    const double numeric = (plus.gradient[k][i] - minus.gradient[k][i]) / (2 * h);
                    EXPECT_NEAR(rows.hessian[k][i][j], numeric, 1e-5 * std::max(1.0, std::abs(numeric)))
                        << "row " << k << " (" << i << ", " << j << ")";
                }
            }
        }
    }
}

// Two equations: a free body held on a curve keeps four freedoms, in both
// OndselSolver's count and the Reference's; and the start parameter is the
// closest point over the whole curve.
TEST(PointOnCurve, countsTwoEquations)  // NOLINT
{
    for (const auto& cc : curveCases()) {
        SCOPED_TRACE(cc.name);
        Builder b;
        Base::Vector3d n1, n2;
        normals(*cc.curve, cc.u0, n1, n2);
        const Base::Vector3d x = evaluateCurve(*cc.curve, cc.u0).p + n2 * (cc.offset + 0.4);
        const Id body = b.body("B", at(x.x, x.y, x.z, rot(0, 0, 1, 0.3)));
        b.cam(b.frame(b.g, Base::Placement()), b.frame(body, Base::Placement()), cc.curve, cc.offset);

        ReferenceSession reference;
        ASSERT_EQ(reference.load(b.m), Status::Ok);
        Result r = reference.solve({});
        ASSERT_EQ(r.status, Status::Ok) << r.message;
        EXPECT_EQ(reference.analyse().dof, 6 + 4);
        const auto residuals = Reference::jointResiduals(b.m, placements(reference.poses()));
        ASSERT_EQ(residuals.size(), 1u);
        EXPECT_LT(residuals[0], 1e-9);
        // It moved the 0.4 mm to the curve, no farther.
        EXPECT_NEAR((reference.poses()[body].placement.getPosition() - x).Length(), 0.4, 1e-6);

        OndselSession ondsel(false);
        ASSERT_EQ(ondsel.load(b.m), Status::Ok);
        r = ondsel.solve({});
        ASSERT_EQ(r.status, Status::Ok) << r.message;
        EXPECT_EQ(ondsel.analyse().dof, 6 + 4);
        EXPECT_LT(Reference::jointResiduals(b.m, placements(ondsel.poses()))[0], 1e-9);
    }
}

// ---------------------------------------------------------------- a cam

namespace
{

// An eccentric disk cam A, radius R about a centre e from its axis, on a
// revolute about z through the origin, driven at 'rate' rad/s; a follower B on
// a slider along y through the axis, its point (or a roller of radius r) held
// on the cam by a PointOnCurve. The follower's height, analytic:
//   y = e sin(theta) + sqrt((R + r)^2 - e^2 cos(theta)^2).
constexpr double camE = 6.0;
constexpr double camR = 20.0;

double lift(double theta, double r)
{
    const double c = camE * std::cos(theta);
    return camE * std::sin(theta) + std::sqrt((camR + r) * (camR + r) - c * c);
}

Model camMechanism(double roller, double startError, Id& cam, Id& follower)
{
    Builder b;
    cam = b.body("Cam", Base::Placement());
    // The slider's z along global y; the follower starts off the cam by startError.
    const Base::Rotation zToY = rot(1, 0, 0, -pi / 2);
    follower = b.body("Follower", at(0, lift(0.0, roller) + startError, 0));
    const Id rev = b.joint(JointKind::Revolute, b.frame(b.g, Base::Placement()), b.frame(cam, Base::Placement()));
    b.joint(JointKind::Prismatic, b.frame(b.g, at(0, 0, 0, zToY)), b.frame(follower, at(0, 0, 0, zToY)));
    b.cam(b.frame(cam, Base::Placement()), b.frame(follower, Base::Placement()), circle(camE, 0, camR), roller);
    Driver d;
    d.source = b.m.joints[rev].source;
    d.frameI = b.m.joints[rev].frameI;
    d.frameJ = b.m.joints[rev].frameJ;
    d.kind = DriverKind::RotationZ;
    d.rotation.text = "10*time";
    d.rotation.tree = FormulaTree::parse(d.rotation.text);
    d.rotation.function = Formula::functionOf(d.rotation.tree);
    b.m.addDriver(d);
    return b.m;
}

double turnAboutZ(const Base::Placement& p)
{
    double x, y, z, w;
    p.getRotation().getValue(x, y, z, w);
    return 2.0 * std::atan2(z, w);
}

std::vector<std::pair<std::string, std::unique_ptr<Session>>> sessions()
{
    tests::initApplication();
    Base::Interpreter().runString("import AssemblyApp");
    std::vector<std::pair<std::string, std::unique_ptr<Session>>> out;
    for (const auto& name : Registry::instance().names()) {
        out.emplace_back(name, Registry::instance().find(name)->createSession());
    }
    return out;
}

}  // namespace

// A static solve puts the follower on the cam (with the cam where it is, or
// turned), for a point and a roller, on every registered backend.
TEST(PointOnCurve, camSolvesOnEveryBackend)  // NOLINT
{
    for (double roller : {0.0, 3.0}) {
        SCOPED_TRACE(roller);
        Id cam = NoId, follower = NoId;
        Model m = camMechanism(roller, 0.75, cam, follower);
        m.drivers.clear();
        for (auto& [name, session] : sessions()) {
            SCOPED_TRACE(name);
            ASSERT_EQ(session->load(m), Status::Ok);
            Result r = session->solve({});
            ASSERT_EQ(r.status, Status::Ok) << r.message;
            const auto poses = session->poses();
            const double theta = turnAboutZ(poses[cam].placement);
            const Base::Vector3d p = poses[follower].placement.getPosition();
            EXPECT_NEAR(p.x, 0.0, 1e-9);
            EXPECT_NEAR(p.z, 0.0, 1e-9);
            EXPECT_NEAR(p.y, lift(theta, roller), 1e-9);
            EXPECT_LT(Reference::jointResiduals(m, placements(poses))[2], 1e-9);
            // A planar cam with its follower already held in the plane: the
            // second equation (out of the plane) depends on the slider's.
            const Diagnostics d = session->analyse();
            EXPECT_EQ(d.dof, 6 + 1) << name;
        }
    }
}

// Driven at 10 rad/s for 4 s (more than six turns) with an output step of
// 0.2 s (2 rad, more than a quarter turn): the follower's lift is the
// analytic one at every frame, on every backend.
TEST(PointOnCurve, camFollowsTheDriverForManyTurns)  // NOLINT
{
    SimulationSpec spec;
    spec.tStart = 0.0;
    spec.tEnd = 4.0;
    spec.tOutput = 0.2;
    for (double roller : {0.0, 3.0}) {
        SCOPED_TRACE(roller);
        Id cam = NoId, follower = NoId;
        const Model m = camMechanism(roller, 0.0, cam, follower);
        for (auto& [name, session] : sessions()) {
            SCOPED_TRACE(name);
            ASSERT_EQ(session->load(m), Status::Ok);
            Recorder rec;
            ASSERT_EQ(session->simulate(spec, rec), Status::Ok);
            ASSERT_GE(rec.frames.size(), 21u);
            double worst = 0.0;
            for (std::size_t k = 0; k < rec.frames.size(); ++k) {
                const double t = rec.times[k];
                const auto& f = rec.frames[k];
                const double theta = 10.0 * t;
                EXPECT_NEAR(std::remainder(turnAboutZ(f[cam].placement) - theta, 2 * pi), 0.0, 1e-9)
                    << "t " << t;
                const double err = std::abs(f[follower].placement.getPosition().y - lift(theta, roller));
                worst = std::max(worst, err);
                EXPECT_LT(err, 1e-9) << "t " << t;
            }
            RecordProperty(name + (roller > 0 ? "_roller" : "_point"), std::to_string(worst));
        }
    }
}
