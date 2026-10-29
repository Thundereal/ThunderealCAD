// SPDX-License-Identifier: LGPL-2.1-or-later

// The Chrono backend (docs/AssemblySolverInterface.md sec 6.15, 6.17): each
// joint kind solved from a perturbed start to a pose checked on the geometry
// and by the neutral checker, Chrono's own links or the neutral equations'
// (NeutralLink) as nativeLink() says, the NeutralLink's Jacobian in Chrono's
// coordinates against differences, the couplers it holds following drags
// and kinematic runs with their turns counted, OndselSolver's branches kept,
// the rank and DOF of hand-counted models against the Reference's, a gear,
// limits, drags, the motors' functions against the evaluator, the
// kinematic run against the formulas and the Reference, and a dynamic run's
// redundant rows taken out.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <string>

#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Line.hxx>
#include <NCollection_HArray1.hxx>
#include <gp_Ax2.hxx>

#include <Base/Interpreter.h>
#include <Base/Placement.h>
#include <Mod/Assembly/App/CamJoint.h>
#include <Mod/Assembly/App/Solver/ChronoBackend.h>
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

    Id joint(JointKind kind, Id fi, Id fj, double value = 0.0, double value2 = 0.0, Id fk = NoId)
    {
        Joint j;
        j.kind = kind;
        j.source = m.addSource("J" + std::to_string(m.joints.size()));
        j.frameI = fi;
        j.frameJ = fj;
        j.frameK = fk;
        j.value = value;
        j.value2 = value2;
        return m.addJoint(j);
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

// The pose of frame J relative to frame I.
Base::Placement relative(const Model& m, const std::vector<BodyPose>& poses, Id fi, Id fj)
{
    auto global = [&](Id f) {
        const Frame& frame = m.frames[f];
        if (frame.body == NoId) {
            return frame.local;
        }
        return poses[frame.body].placement * frame.local;
    };
    return global(fi).inverse() * global(fj);
}

Base::Vector3d zOf(const Base::Placement& p)
{
    return p.getRotation().multVec(Base::Vector3d(0, 0, 1));
}

double thez(const Base::Placement& p)
{
    const Base::Vector3d x = p.getRotation().multVec(Base::Vector3d(1, 0, 0));
    return std::atan2(x.y, x.x);
}

// The metric of a solve: |dx|^2 + |dtheta|^2 / 4.
double moved(const Base::Placement& a, const Base::Placement& b)
{
    Base::Vector3d axis;
    double angle = 0.0;
    (a.getRotation().inverse() * b.getRotation()).getValue(axis, angle);
    angle = std::abs(std::remainder(angle, 2 * pi));
    return (a.getPosition() - b.getPosition()).Sqr() + 0.25 * angle * angle;
}

struct KindCase
{
    JointKind kind;
    const char* name;
    double value;
    Base::Placement solution;  // J relative to I, satisfying the joint
    int dof;                   // of one body hung from the ground by this joint alone
};

// Every kind with one equation set (the Reference's cases, and a right
// angle): Chrono's own links, and the neutral equations for an angle other
// than a right one and the two distances.
std::vector<KindCase> kindCases()
{
    return {
        {JointKind::Fixed, "Fixed", 0.0, at(0, 0, 0), 0},
        {JointKind::Revolute, "Revolute", 0.0, at(0, 0, 0, rot(0, 0, 1, 0.5)), 1},
        {JointKind::Cylindrical, "Cylindrical", 0.0, at(0, 0, 5, rot(0, 0, 1, 0.5)), 2},
        {JointKind::Prismatic, "Prismatic", 0.0, at(0, 0, 5), 1},
        {JointKind::Spherical, "Spherical", 0.0, at(0, 0, 0, rot(1, 2, 3, 0.4)), 3},
        {JointKind::Planar, "Planar", 2.0, at(3, 4, 2, rot(0, 0, 1, 0.7)), 3},
        {JointKind::ParallelZ, "ParallelZ", 0.0, at(3, 4, 2, rot(0, 0, 1, 0.7)), 4},
        {JointKind::PerpendicularZ, "PerpendicularZ", 0.0, at(3, 4, 2, rot(1, 0, 0, pi / 2)), 5},
        {JointKind::AngleZ, "AngleZ(90)", pi / 2, at(3, 4, 2, rot(1, 0, 0, pi / 2)), 5},
        {JointKind::PointInPlane, "PointInPlane", 2.0, at(3, 4, 2, rot(1, 1, 0, 0.6)), 5},
        {JointKind::LineInPlane, "LineInPlane", 2.0, at(3, 4, 2, rot(1, 0, 0, pi / 2)), 4},
        {JointKind::PointOnLine, "PointOnLine", 0.0, at(0, 0, 7, rot(1, 1, 0, 0.6)), 4},
        {JointKind::PointPointDistance,
         "PointPointDistance",
         5.0,
         at(3, 4, 0, rot(1, 1, 0, 0.6)),
         5},
        {JointKind::AngleZ, "AngleZ(0.5)", 0.5, at(3, 4, 2, rot(1, 0, 0, 0.5)), 5},
        {JointKind::PointLineDistance, "PointLineDistance", 5.0, at(3, 4, 9, rot(1, 1, 0, 0.6)), 5},
        {JointKind::LineLineDistance, "LineLineDistance", 5.0, at(3, 4, 9, rot(0, 0, 1, 0.6)), 3},
    };
}

// The joint's defining property on the relative pose T of J in I.
double geometricError(JointKind kind, double value, const Base::Placement& T)
{
    const Base::Vector3d p = T.getPosition();
    const Base::Vector3d z = zOf(T);
    const double parallel = std::hypot(z.x, z.y);  // 0 when zJ is along zI
    Base::Vector3d axis;
    double angle = 0.0;
    T.getRotation().getValue(axis, angle);
    switch (kind) {
        case JointKind::Fixed:
            return std::max(p.Length(), std::abs(std::remainder(angle, 2 * pi)));
        case JointKind::Revolute:
            return std::max(p.Length(), parallel);
        case JointKind::Cylindrical:
            return std::max({std::abs(p.x), std::abs(p.y), parallel});
        case JointKind::Prismatic:
            return std::max(
                {std::abs(p.x), std::abs(p.y), std::abs(std::remainder(angle, 2 * pi))});
        case JointKind::Spherical:
            return p.Length();
        case JointKind::Planar:
            return std::max(std::abs(p.z - value), parallel);
        case JointKind::ParallelZ:
            return parallel;
        case JointKind::PerpendicularZ:
            return std::abs(z.z);
        case JointKind::AngleZ:
            return std::abs(z.z - std::cos(value));
        case JointKind::PointInPlane:
            return std::abs(p.z - value);
        case JointKind::LineInPlane:
            return std::max(std::abs(p.z - value), std::abs(z.z));
        case JointKind::PointOnLine:
            return std::max(std::abs(p.x), std::abs(p.y));
        case JointKind::PointPointDistance:
            return std::abs(p.Length() - value);
        case JointKind::PointLineDistance:
            return std::abs(std::hypot(p.x, p.y) - value);
        case JointKind::LineLineDistance:
            return std::max(std::abs(std::hypot(p.x, p.y) - value), parallel);
        default:
            return 0.0;
    }
}

struct Single
{
    Builder b;
    Id body = NoId;
    Id fi = NoId;
    Id fj = NoId;
    Id joint = NoId;
    Base::Placement solved;  // B's pose at the known solution
};

Single single(const KindCase& c, const Base::Placement& perturbation)
{
    Single s;
    const Base::Placement onG = at(1, 2, 3, rot(1, 1, 0, 0.3));
    const Base::Placement onB = at(0.5, -1, 2, rot(0, 1, 1, 0.7));
    s.solved = onG * c.solution * onB.inverse();
    s.body = s.b.body("B", perturbation * s.solved);
    s.fi = s.b.frame(s.b.g, onG);
    s.fj = s.b.frame(s.body, onB);
    s.joint = s.b.joint(c.kind, s.fi, s.fj, c.value);
    return s;
}

}  // namespace

// Every kind: solved from a perturbed start to the joint's property,
// satisfied by the neutral checker, with the DOF the Reference counts and
// nothing redundant; by Chrono's link or the neutral equations, as
// nativeLink() says.
TEST(ChronoBackend, eachKindSolvesFromAPerturbedStart)  // NOLINT
{
    const Base::Placement perturbation = at(0.3, -0.2, 0.25, rot(1, -2, 0.5, 0.05));
    for (const auto& c : kindCases()) {
        SCOPED_TRACE(c.name);
        Single s = single(c, perturbation);
        ChronoSession session;
        ASSERT_EQ(session.load(s.b.m), Status::Ok);
        EXPECT_TRUE(session.refusalMessage().empty()) << session.refusalMessage();
        EXPECT_EQ(session.neutralLinkCount(), Chrono::nativeLink(s.b.m.joints[0]) ? 0 : 1);
        const double startError = Reference::jointResiduals(s.b.m, placements(session.poses()))[0];
        EXPECT_GT(startError, 1e-3);

        Result r = session.solve({});
        ASSERT_EQ(r.status, Status::Ok) << r.message;
        const auto poses = session.poses();
        EXPECT_TRUE(poses[s.b.g].placement.isSame(Base::Placement(), 1e-12));
        const Base::Placement T = relative(s.b.m, poses, s.fi, s.fj);
        EXPECT_LT(geometricError(c.kind, c.value, T), 1e-9);
        const auto residuals = Reference::jointResiduals(s.b.m, placements(poses));
        ASSERT_EQ(residuals.size(), 1u);
        EXPECT_LT(residuals[0], 1e-9);
        EXPECT_LT(
            (poses[s.body].placement.getPosition() - s.b.m.bodies[s.body].placement.getPosition())
                .Length(),
            2.0);

        Diagnostics d = session.analyse();
        EXPECT_EQ(d.dof, 6 + c.dof);
        EXPECT_TRUE(d.redundant.empty());
        EXPECT_TRUE(d.conflicting.empty());
        EXPECT_TRUE(d.unsupported.empty());
        EXPECT_EQ(session.rank(), 6 - c.dof);

        // The Reference, from the same start, to compare closeness: Chrono's
        // solution is no farther (in the metric both minimise).
        ReferenceSession reference;
        ASSERT_EQ(reference.load(s.b.m), Status::Ok);
        ASSERT_EQ(reference.solve({}).status, Status::Ok);
        const Base::Placement start = s.b.m.bodies[s.body].placement;
        const double mine = moved(start, poses[s.body].placement);
        const double theirs = moved(start, reference.poses()[s.body].placement);
        EXPECT_LE(mine, theirs * (1.0 + 1e-6) + 1e-12) << mine << " " << theirs;
    }
}

// Since sec 6.17 the backend holds every kind: Chrono's own link where it has
// one, else the neutral equations. Refused: a gear without a ratio (a zero
// second radius), and a gear whose frames are coaxial without a carrier --
// the Reference fails both. A refused model loads, every solve answers
// Unsupported with the joint and the reason, and analyse() names the joint.
TEST(ChronoBackend, refusalsAndTheLinkEachKindGets)  // NOLINT
{
    const JointKindSet kinds = Chrono::supportedKinds();
    EXPECT_EQ(kinds.count(), static_cast<std::size_t>(JointKind::Count_));

    auto joint = [](JointKind kind, double value, double value2 = 0.0) {
        Joint j;
        j.kind = kind;
        j.value = value;
        j.value2 = value2;
        return j;
    };
    // Chrono's own links: the lower pairs, the orientations, a right angle,
    // the incidences, a point's distance, a gear between positive radii.
    for (JointKind k : {JointKind::Fixed,
                        JointKind::Revolute,
                        JointKind::Cylindrical,
                        JointKind::Prismatic,
                        JointKind::Spherical,
                        JointKind::Planar,
                        JointKind::ParallelZ,
                        JointKind::PerpendicularZ,
                        JointKind::PointInPlane,
                        JointKind::LineInPlane,
                        JointKind::PointOnLine,
                        JointKind::PointPointDistance}) {
        EXPECT_TRUE(Chrono::nativeLink(joint(k, 1.0))) << static_cast<int>(k);
    }
    EXPECT_TRUE(Chrono::nativeLink(joint(JointKind::AngleZ, pi / 2)));
    EXPECT_TRUE(Chrono::nativeLink(joint(JointKind::Gear, 10.0, 20.0)));
    // The neutral equations.
    EXPECT_FALSE(Chrono::nativeLink(joint(JointKind::AngleZ, 0.5)));
    EXPECT_FALSE(Chrono::nativeLink(joint(JointKind::PointLineDistance, 5.0)));
    EXPECT_FALSE(Chrono::nativeLink(joint(JointKind::LineLineDistance, 5.0)));
    EXPECT_FALSE(Chrono::nativeLink(joint(JointKind::Gear, 10.0, -20.0)));
    EXPECT_FALSE(Chrono::nativeLink(joint(JointKind::CoaxialGear, 10.0, 5.0)));
    EXPECT_FALSE(Chrono::nativeLink(joint(JointKind::RackPinion, 7.0)));
    EXPECT_FALSE(Chrono::nativeLink(joint(JointKind::Screw, 3.0)));
    EXPECT_FALSE(Chrono::nativeLink(joint(JointKind::PointOnCurve, 2.0)));
    for (JointKind k : {JointKind::AngleZ,
                        JointKind::PointLineDistance,
                        JointKind::LineLineDistance,
                        JointKind::CoaxialGear,
                        JointKind::RackPinion,
                        JointKind::Screw,
                        JointKind::PointOnCurve}) {
        EXPECT_TRUE(Chrono::refusal(joint(k, 1.0, 1.0)).empty()) << static_cast<int>(k);
    }
    EXPECT_TRUE(Chrono::refusal(joint(JointKind::Gear, 10.0, -20.0)).empty());
    EXPECT_NE(Chrono::refusal(joint(JointKind::Gear, 10.0, 0.0)).find("radius is zero"),
              std::string::npos);

    struct Refused
    {
        JointKind kind;
        double value2;
        bool coaxial;
        const char* why;
    };
    for (const Refused& c : {Refused {JointKind::Gear, 0.0, false, "radius is zero"},
                             Refused {JointKind::Gear, 5.0, true, "carrier"},
                             Refused {JointKind::CoaxialGear, 5.0, true, "carrier"}}) {
        SCOPED_TRACE(c.why);
        Builder b;
        const Id body = b.body("B", at(1, 2, 3));
        b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
        // On one axis when coaxial, else beside it; no carrier frame.
        const Id refused = b.joint(c.kind,
                                   b.frame(b.g, c.coaxial ? at(1, 2, 0) : at(5, 0, 10)),
                                   b.frame(body, at(0, 0, 0)),
                                   10.0,
                                   c.value2);
        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        const std::string message = session.refusalMessage();
        EXPECT_NE(message.find("J1"), std::string::npos) << message;
        EXPECT_NE(message.find(c.why), std::string::npos) << message;
        Result r = session.solve({});
        EXPECT_EQ(r.status, Status::Unsupported);
        EXPECT_EQ(r.message, message);
        EXPECT_TRUE(session.poses()[body].placement.isSame(at(1, 2, 3), 0.0));
        Diagnostics d = session.analyse();
        EXPECT_EQ(d.known, Diagnostics::KnownUnsupported);
        EXPECT_EQ(d.unsupported, (std::vector<Id> {refused}));
        EXPECT_EQ(session.beginDrag({body}, {}), Status::Unsupported);
        EXPECT_EQ(session.dragStep({{body, at(0, 0, 10)}}).status, Status::Unsupported);
    }
}

namespace
{

std::mt19937& rng()
{
    static std::mt19937 engine(11);
    return engine;
}

Base::Placement randomPose(double spread)
{
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    auto& e = rng();
    const double x = spread * u(e);
    const double y = spread * u(e);
    const double z = spread * u(e);
    const double ax = u(e);
    const double ay = u(e);
    const double az = u(e) + 1.5;
    const double angle = 2.0 * u(e);
    return at(x, y, z, rot(ax, ay, az, angle));
}

}  // namespace

// The NeutralLink's Jacobian is Chrono's: per body d/dx and d/dw, w the
// angular velocity in the body's own frame (ChBody::IntStateIncrement turns q
// into q exp(dw)). Against central differences of its constraint values under
// exactly those increments, on free bodies at random poses; and against the
// Reference's gradient (spatial rotation vector) turned into the body frame,
// g R, with the same values.
TEST(ChronoBackend, neutralJacobianInChronosCoordinates)  // NOLINT
{
    struct Case
    {
        JointKind kind;
        const char* name;
        double value, value2;
        bool coaxial;
    };
    const Case cases[] = {
        {JointKind::AngleZ, "AngleZ", 0.7, 0.0, false},
        {JointKind::PointLineDistance, "PointLineDistance", 4.0, 0.0, false},
        {JointKind::LineLineDistance, "LineLineDistance", 4.0, 0.0, false},
        {JointKind::Gear, "Belt", 10.0, -20.0, false},
        {JointKind::CoaxialGear, "CoaxialGear", 10.0, 5.0, true},
        {JointKind::RackPinion, "RackPinion", 7.0, 0.0, false},
        {JointKind::Screw, "Screw", 3.0, 0.0, false},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        Builder b;
        const Id a = b.body("A", randomPose(10.0));
        const Id bb = b.body("B", randomPose(10.0));
        const Id k = b.body("K", randomPose(10.0));
        const Base::Placement onA = randomPose(4.0);
        const Base::Placement onB = randomPose(4.0);
        if (c.coaxial) {
            // Frames I and J on one axis at the start.
            b.m.bodies[bb].placement =
                b.m.bodies[a].placement * onA * at(0, 0, 3, rot(0, 0, 1, 0.8)) * onB.inverse();
        }
        const Id fk = b.frame(k, randomPose(4.0));
        b.joint(c.kind, b.frame(a, onA), b.frame(bb, onB), c.value, c.value2, fk);

        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_TRUE(session.refusalMessage().empty()) << session.refusalMessage();
        ASSERT_EQ(session.neutralLinkCount(), 1);
        std::vector<Base::Placement> poses;
        for (const auto& body : b.m.bodies) {
            poses.push_back(body.placement);
        }
        const auto C = session.constraintsAt(poses);
        const auto J = session.jacobianAt(poses);
        ASSERT_EQ(J.size(), C.size());
        ASSERT_FALSE(C.empty());

        // The Reference's equations: its rows after the ground's six.
        ReferenceSession reference;
        ASSERT_EQ(reference.load(b.m), Status::Ok);
        const auto rc = reference.residualsAt(poses);
        const auto rj = reference.jacobianAt(poses);
        ASSERT_EQ(rc.size(), 6 + C.size());

        const double h = 1e-6;
        for (std::size_t r = 0; r < C.size(); ++r) {
            EXPECT_NEAR(C[r], rc[6 + r], 1e-12 * std::max(1.0, std::abs(C[r])));
        }
        for (std::size_t body = 0; body < poses.size(); ++body) {
            const bool fixed = body == b.g;
            const Base::Rotation R = poses[body].getRotation();
            for (int col = 0; col < 6; ++col) {
                auto plus = poses;
                auto minus = poses;
                Base::Vector3d e;
                e[col % 3] = 1.0;
                if (col < 3) {
                    plus[body].setPosition(plus[body].getPosition() + e * h);
                    minus[body].setPosition(minus[body].getPosition() - e * h);
                }
                else {
                    // Chrono's increment: in the body's own frame.
                    plus[body].setRotation(R * Base::Rotation(e, h));
                    minus[body].setRotation(R * Base::Rotation(e, -h));
                }
                const auto cp = session.constraintsAt(plus);
                const auto cm = session.constraintsAt(minus);
                for (std::size_t r = 0; r < C.size(); ++r) {
                    const double analytic = J[r][6 * body + col];
                    if (fixed) {
                        EXPECT_EQ(analytic, 0.0);
                        continue;
                    }
                    const double numeric = (cp[r] - cm[r]) / (2 * h);
                    EXPECT_NEAR(analytic, numeric, 1e-6 * std::max(1.0, std::abs(numeric)))
                        << "row " << r << " body " << body << " column " << col;
                }
            }
            if (fixed) {
                continue;
            }
            // The Reference's spatial gradient g turned into the body frame: g R.
            for (std::size_t r = 0; r < C.size(); ++r) {
                const auto& g = rj[6 + r];
                for (int col = 0; col < 3; ++col) {
                    EXPECT_NEAR(J[r][6 * body + col], g[6 * body + col], 1e-12);
                    Base::Vector3d e;
                    e[col] = 1.0;
                    const Base::Vector3d axis = R.multVec(e);  // R's column col
                    const double local = g[6 * body + 3] * axis.x + g[6 * body + 4] * axis.y
                        + g[6 * body + 5] * axis.z;
                    EXPECT_NEAR(J[r][6 * body + 3 + col], local, 1e-12 * std::max(1.0, std::abs(local)));
                }
            }
        }
        // The probes left the session as it was.
        EXPECT_EQ(session.constraintsAt(poses), C);
    }
}

// A PointOnCurve (sec 6.18) held by the NeutralLink: its rows equal the
// Reference's, its Jacobian in Chrono's coordinates against differences
// under Chrono's increments; a circle with a roller, and a 3D spline; and a
// model whose curve is missing is refused with the reason.
TEST(ChronoBackend, neutralPointOnCurveInChronosCoordinates)  // NOLINT
{
    Handle(Geom_Circle) circle = new Geom_Circle(gp_Ax2(gp_Pnt(2, 1, 0), gp_Dir(0, 0, 1)), 12.0);
    Handle(NCollection_HArray1<gp_Pnt>) pts = new NCollection_HArray1<gp_Pnt>(1, 9);
    for (int i = 0; i < 9; ++i) {
        const double a = 0.7 * i;
        pts->SetValue(i + 1, gp_Pnt(10.0 * std::cos(a), 10.0 * std::sin(a), 1.5 * i));
    }
    GeomAPI_Interpolate interpolate(pts, Standard_False, 1e-9);
    interpolate.Perform();
    Handle(Geom_BSplineCurve) spline = interpolate.Curve();
    struct Case
    {
        const char* name;
        std::shared_ptr<const CurveGeometry> curve;
        double offset;
    };
    const Case cases[] = {
        {"circle, roller", Assembly::makeCurveGeometry(circle, 0.0, 2 * pi), 2.5},
        {"3D spline",
         Assembly::makeCurveGeometry(spline, spline->FirstParameter(), spline->LastParameter()),
         0.0},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        Builder b;
        const Id a = b.body("A", randomPose(10.0));
        const Id bb = b.body("B", randomPose(10.0));
        const Base::Placement onA = randomPose(4.0);
        const Base::Placement onB = randomPose(4.0);
        // Frame J near the curve: 1.3 mm from its point at a third of the range.
        const CurvePoint p =
            c.curve->evaluate(c.curve->first() + (c.curve->last() - c.curve->first()) / 3.0);
        const Base::Vector3d x = p.p + Base::Vector3d(0.6, -1.1, 0.4);
        b.m.bodies[bb].placement =
            b.m.bodies[a].placement * onA * at(x.x, x.y, x.z, rot(1, 2, 3, 0.5)) * onB.inverse();
        const Id id = b.joint(JointKind::PointOnCurve, b.frame(a, onA), b.frame(bb, onB), c.offset);
        Curve record;
        record.source = b.m.joints[id].source;
        record.geometry = c.curve;
        b.m.joints[id].curve = b.m.addCurve(record);

        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_TRUE(session.refusalMessage().empty()) << session.refusalMessage();
        ASSERT_EQ(session.neutralLinkCount(), 1);
        std::vector<Base::Placement> poses;
        for (const auto& body : b.m.bodies) {
            poses.push_back(body.placement);
        }
        const auto C = session.constraintsAt(poses);
        const auto J = session.jacobianAt(poses);
        ASSERT_EQ(C.size(), 2u);
        ReferenceSession reference;
        ASSERT_EQ(reference.load(b.m), Status::Ok);
        const auto rc = reference.residualsAt(poses);
        ASSERT_EQ(rc.size(), 6u + 2u);
        for (std::size_t r = 0; r < C.size(); ++r) {
            EXPECT_NEAR(C[r], rc[6 + r], 1e-12 * std::max(1.0, std::abs(C[r])));
        }
        const double h = 1e-6;
        for (std::size_t body = 1; body < poses.size(); ++body) {
            const Base::Rotation R = poses[body].getRotation();
            for (int col = 0; col < 6; ++col) {
                auto plus = poses;
                auto minus = poses;
                Base::Vector3d e;
                e[col % 3] = 1.0;
                if (col < 3) {
                    plus[body].setPosition(plus[body].getPosition() + e * h);
                    minus[body].setPosition(minus[body].getPosition() - e * h);
                }
                else {
                    plus[body].setRotation(R * Base::Rotation(e, h));
                    minus[body].setRotation(R * Base::Rotation(e, -h));
                }
                const auto cp = session.constraintsAt(plus);
                const auto cm = session.constraintsAt(minus);
                for (std::size_t r = 0; r < C.size(); ++r) {
                    const double numeric = (cp[r] - cm[r]) / (2 * h);
                    EXPECT_NEAR(J[r][6 * body + col], numeric, 1e-6 * std::max(1.0, std::abs(numeric)))
                        << "row " << r << " body " << body << " column " << col;
                }
            }
        }
        EXPECT_EQ(session.constraintsAt(poses), C);

        // Without its curve the joint cannot be held.
        Model bad = b.m;
        bad.joints[id].curve = NoId;
        ChronoSession refused;
        ASSERT_EQ(refused.load(bad), Status::Ok);
        EXPECT_NE(refused.refusalMessage().find("curve"), std::string::npos)
            << refused.refusalMessage();
    }
}

// OndselSolver's direction cosines hold zJ = -zI as well as zJ = zI, and
// four rotations for a fixed joint. Chrono holds the branch the start is
// nearest, where OndselSolver would converge: a revolute starting with its
// axes nearly opposed stays opposed, a fixed joint near a half turn about y
// stays at that half turn.
TEST(ChronoBackend, keepsTheBranchTheStartIsNearest)  // NOLINT
{
    {
        Builder b;
        const Id body = b.body("B", at(0.1, 0.2, 10.1, rot(1, 0, 0, pi - 0.05)));
        const Id fi = b.frame(b.g, at(0, 0, 10));
        const Id fj = b.frame(body, at(0, 0, 0));
        b.joint(JointKind::Revolute, fi, fj);
        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        const Base::Placement T = relative(b.m, session.poses(), fi, fj);
        EXPECT_NEAR(zOf(T).z, -1.0, 1e-12);
        EXPECT_LT(T.getPosition().Length(), 1e-9);
        EXPECT_LT(Reference::jointResiduals(b.m, placements(session.poses()))[0], 1e-9);
    }
    {
        Builder b;
        const Id body = b.body("B", at(3, 0, 4, rot(0.05, 1, 0, pi)));
        const Id fi = b.frame(b.g, at(3, 0, 4));
        const Id fj = b.frame(body, at(0, 0, 0));
        b.joint(JointKind::Fixed, fi, fj);
        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        const Base::Placement T = relative(b.m, session.poses(), fi, fj);
        EXPECT_TRUE(T.getRotation().isSame(rot(0, 1, 0, pi), 1e-12));
        EXPECT_LT(Reference::jointResiduals(b.m, placements(session.poses()))[0], 1e-9);
        EXPECT_EQ(session.analyse().dof, 6);
    }
}

namespace
{

Model twoRevolutes(double gap, double tilt, Id& body)
{
    Builder b;
    body = b.body("B", at(0.2, -0.1, 10.3, rot(1, 0, 0, 0.02)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
    b.joint(JointKind::Revolute,
            b.frame(b.g, at(0, 0, 10 + gap, rot(1, 0, 0, tilt))),
            b.frame(body, at(0, 0, gap, rot(1, 0, 0, tilt))));
    return b.m;
}

void expectSameDiagnostics(const Model& m)
{
    ChronoSession chrono;
    ASSERT_EQ(chrono.load(m), Status::Ok);
    ASSERT_EQ(chrono.solve({}).status, Status::Ok);
    ReferenceSession reference;
    ASSERT_EQ(reference.load(m), Status::Ok);
    ASSERT_EQ(reference.solve({}).status, Status::Ok);
    const Diagnostics c = chrono.analyse();
    const Diagnostics r = reference.analyse();
    EXPECT_EQ(c.dof, r.dof);
    EXPECT_EQ(c.redundant, r.redundant);
    EXPECT_EQ(c.partiallyRedundant, r.partiallyRedundant);
    EXPECT_EQ(c.conflicting, r.conflicting);
}

}  // namespace

// DOF and redundancy on the Reference's hand-counted models, equal to its.
TEST(ChronoBackend, rankOnHandCountedModels)  // NOLINT
{
    Id body = NoId;
    {
        SCOPED_TRACE("one revolute");
        Builder b;
        body = b.body("B", at(1, 2, 3));
        b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
        expectSameDiagnostics(b.m);
        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        EXPECT_EQ(session.analyse().dof, 6 + 1);
    }
    {
        SCOPED_TRACE("two coaxial revolutes");
        Model m = twoRevolutes(4.0, 0.0, body);
        expectSameDiagnostics(m);
        ChronoSession session;
        ASSERT_EQ(session.load(m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        const Diagnostics d = session.analyse();
        EXPECT_EQ(d.dof, 6 + 1);
        EXPECT_EQ(d.redundant, (std::vector<Id> {0, 1}));
        EXPECT_TRUE(d.conflicting.empty());
        EXPECT_EQ(session.rank(), 5);
    }
    {
        SCOPED_TRACE("crossing revolutes");
        Model m = twoRevolutes(0.0, 0.5, body);
        expectSameDiagnostics(m);
    }
    {
        SCOPED_TRACE("two fixed joints that disagree");
        Builder b;
        body = b.body("B", at(1, 2, 3));
        b.joint(JointKind::Fixed, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
        b.joint(JointKind::Fixed, b.frame(b.g, at(0, 0, 12)), b.frame(body, at(0, 0, 0)));
        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        // Nothing satisfies both, and Chrono's Newton iteration does not set
        // equations aside as OndselSolver and the Reference do: the solve
        // fails and leaves the bodies where they were. The analysis there
        // names the conflict as the Reference does after its solve.
        Result r = session.solve({});
        EXPECT_EQ(r.status, Status::NotConverged);
        EXPECT_TRUE(session.poses()[body].placement.isSame(at(1, 2, 3), 0.0));
        const Diagnostics d = session.analyse();
        EXPECT_EQ(d.dof, 6 + 0);
        EXPECT_EQ(d.conflicting, (std::vector<Id> {1}));
        EXPECT_EQ(d.redundant, (std::vector<Id> {1}));
        EXPECT_TRUE(d.partiallyRedundant.empty());
    }
    {
        SCOPED_TRACE("a free body");
        Builder b;
        b.body("Free", at(1, 2, 3));
        expectSameDiagnostics(b.m);
    }
}

namespace
{

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

Model gearPair(double rb, Id& a, Id& bb)
{
    Builder b;
    a = b.body("A", at(0, 0, 10));
    bb = b.body("B", at(30, 0, 10, rot(0, 0, 1, 0.3)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(a, at(0, 0, 0)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(30, 0, 10)), b.frame(bb, at(0, 0, 0)));
    b.joint(JointKind::Gear, b.frame(a, at(0, 0, 0)), b.frame(bb, at(0, 0, 0)), 10.0, rb);
    return b.m;
}

}  // namespace

// ChLinkLockGear with its phase enforced holds OndselSolver's relation at
// position level: a dragged gear turns the other by the ratio.
TEST(ChronoBackend, gearFollowsADrag)  // NOLINT
{
    Id a = NoId;
    Id bb = NoId;
    Model m = gearPair(20.0, a, bb);
    ChronoSession session;
    ASSERT_EQ(session.load(m), Status::Ok);
    ASSERT_EQ(session.solve({}).status, Status::Ok);
    EXPECT_EQ(session.analyse().dof, 6 + 1);
    const double b0 = thez(session.poses()[bb].placement);
    ASSERT_EQ(session.beginDrag({a}, {}), Status::Ok);
    const double turn = 20.0 * pi / 180.0;
    Result r = session.dragStep({{a, at(0, 0, 10, rot(0, 0, 1, turn))}});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    const auto poses = session.poses();
    const double turned = thez(poses[a].placement);
    EXPECT_GT(turned, 0.5 * turn);
    EXPECT_LE(turned, turn + 1e-12);
    EXPECT_NEAR(thez(poses[bb].placement) - b0, -(10.0 / 20.0) * turned, 1e-9);
    const auto residuals = Reference::jointResiduals(m, placements(poses));
    EXPECT_LT(*std::max_element(residuals.begin(), residuals.end()), 1e-9);
    EXPECT_EQ(session.endDrag().status, Status::Ok);
}

// The couplers the neutral equations hold follow a drag as the Reference's do:
// a belt turns B with A, two gears on one axis turn against their carrier,
// a rack slides by the pitch radius per radian of its pinion and a screw by
// the pitch per turn. Each relation is held at position level (the neutral
// checker), and the poses are the Reference's.
TEST(ChronoBackend, neutralCouplersFollowADrag)  // NOLINT
{
    auto sameAsReference = [](const Model& m, const std::vector<Id>& dragged,
                              const std::vector<BodyPose>& targets, ChronoSession& session) {
        ReferenceSession reference;
        ASSERT_EQ(reference.load(m), Status::Ok);
        ASSERT_EQ(reference.solve({}).status, Status::Ok);
        ASSERT_EQ(reference.beginDrag(dragged, {}), Status::Ok);
        ASSERT_EQ(reference.dragStep(targets).status, Status::Ok);
        const auto mine = session.poses();
        const auto theirs = reference.poses();
        for (std::size_t k = 0; k < mine.size(); ++k) {
            EXPECT_LT(std::sqrt(moved(mine[k].placement, theirs[k].placement)), 1e-7)
                << "body " << k;
        }
    };
    {
        SCOPED_TRACE("Belt");
        Id a = NoId;
        Id bb = NoId;
        Model m = gearPair(-20.0, a, bb);
        ChronoSession session;
        ASSERT_EQ(session.load(m), Status::Ok);
        EXPECT_EQ(session.neutralLinkCount(), 1);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        EXPECT_EQ(session.analyse().dof, 6 + 1);
        const double b0 = thez(session.poses()[bb].placement);
        ASSERT_EQ(session.beginDrag({a}, {}), Status::Ok);
        const double turn = 20.0 * pi / 180.0;
        const std::vector<BodyPose> targets {{a, at(0, 0, 10, rot(0, 0, 1, turn))}};
        Result r = session.dragStep(targets);
        ASSERT_EQ(r.status, Status::Ok) << r.message;
        const auto poses = session.poses();
        const double turned = thez(poses[a].placement);
        EXPECT_GT(turned, 0.5 * turn);
        // A belt: B turns with A, by half.
        EXPECT_NEAR(thez(poses[bb].placement) - b0, 0.5 * turned, 1e-9);
        const auto residuals = Reference::jointResiduals(m, placements(poses));
        EXPECT_LT(*std::max_element(residuals.begin(), residuals.end()), 1e-9);
        sameAsReference(m, {a}, targets, session);
        EXPECT_EQ(session.endDrag().status, Status::Ok);
    }
    {
        SCOPED_TRACE("CoaxialGear");
        Builder b;
        const Id a = b.body("A", at(0, 0, 20));
        const Id bb = b.body("B", at(0, 0, 25, rot(0, 0, 1, 0.2)));
        b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 20)), b.frame(a, at(0, 0, 0)));
        b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 25)), b.frame(bb, at(0, 0, 0)));
        const Id carrier = b.frame(b.g, at(0, 0, 20));
        b.joint(JointKind::CoaxialGear,
                b.frame(a, at(0, 0, 0)),
                b.frame(bb, at(0, 0, 0)),
                10.0,
                5.0,
                carrier);
        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        EXPECT_EQ(session.neutralLinkCount(), 1);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        EXPECT_EQ(session.analyse().dof, 6 + 1);
        const double b0 = thez(session.poses()[bb].placement);
        ASSERT_EQ(session.beginDrag({a}, {}), Status::Ok);
        const std::vector<BodyPose> targets {{a, at(0, 0, 20, rot(0, 0, 1, 0.4))}};
        ASSERT_EQ(session.dragStep(targets).status, Status::Ok);
        const double turned = thez(session.poses()[a].placement);
        EXPECT_GT(turned, 0.05);
        EXPECT_NEAR(thez(session.poses()[bb].placement) - b0, -2.0 * turned, 1e-9);
        sameAsReference(b.m, {a}, targets, session);
    }
    for (JointKind kind : {JointKind::RackPinion, JointKind::Screw}) {
        const bool rack = kind == JointKind::RackPinion;
        SCOPED_TRACE(rack ? "RackPinion" : "Screw");
        Builder b;
        const Id s = b.body("S", at(0, 0, 10));
        const Id p = b.body("P", at(0, 20, 10, rot(0, 0, 1, 0.1)));
        const Base::Rotation slideAxis = rack ? rot(0, 1, 0, pi / 2) : Base::Rotation();
        b.joint(JointKind::Prismatic,
                b.frame(b.g, at(0, 0, 10, slideAxis)),
                b.frame(s, at(0, 0, 0, slideAxis)));
        b.joint(JointKind::Revolute, b.frame(b.g, at(0, 20, 10)), b.frame(p, at(0, 0, 0)));
        const Id fi = b.frame(s, at(0, 20, 0));
        const Id fj = b.frame(p, at(0, 0, 0));
        b.joint(kind, fi, fj, rack ? 7.0 : 3.0);
        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        EXPECT_EQ(session.neutralLinkCount(), 1);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        EXPECT_EQ(session.analyse().dof, 6 + 1);
        auto measure = [&](const std::vector<BodyPose>& poses) {
            const Base::Placement T = relative(b.m, poses, fi, fj);
            const double turn = thez(T);
            const Base::Vector3d d = T.getPosition();
            return rack ? d.x + 7.0 * turn : 2 * pi * d.z - 3.0 * turn;
        };
        const double before = measure(session.poses());
        Model heavy = b.m;
        heavy.bodies[p].dragWeight = 1e4;
        ASSERT_EQ(session.update(heavy), Status::Ok);
        ASSERT_EQ(session.beginDrag({p}, {}), Status::Ok);
        const std::vector<BodyPose> targets {{p, at(0, 20, 10, rot(0, 0, 1, 0.6))}};
        Result r = session.dragStep(targets);
        ASSERT_EQ(r.status, Status::Ok) << r.message;
        const auto poses = session.poses();
        EXPECT_GT(thez(poses[p].placement), 0.1 + 0.4);
        EXPECT_LE(thez(poses[p].placement), 0.6 + 1e-12);
        EXPECT_NEAR(measure(poses), before, 1e-9);
        sameAsReference(heavy, {p}, targets, session);
    }
}

// The neutral couplers count their turns as the Reference does, across drag
// steps and across the steps of a kinematic run: a pinion turned through two
// and a half turns in drag steps of 0.9 rad moves its rack by 7 mm per radian
// of the whole angle (a lost turn is 44 mm), and a pinion driven at 90*time (9 rad per output
// step) moves it by 630 mm in a second, frame for frame the Reference's.
TEST(ChronoBackend, neutralCouplersCountTheirTurns)  // NOLINT
{
    Builder b;
    const Id s = b.body("Rack", at(0, 0, 10));
    const Id p = b.body("Pinion", at(0, 20, 10));
    const Base::Rotation alongX = rot(0, 1, 0, pi / 2);
    b.joint(JointKind::Prismatic, b.frame(b.g, at(0, 0, 10, alongX)), b.frame(s, at(0, 0, 0, alongX)));
    const Id ri = b.frame(b.g, at(0, 20, 10));
    const Id rj = b.frame(p, at(0, 0, 0));
    b.joint(JointKind::Revolute, ri, rj);
    b.joint(JointKind::RackPinion, b.frame(s, at(0, 20, 0)), b.frame(p, at(0, 0, 0)), 7.0);
    {
        SCOPED_TRACE("drag");
        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        Model heavy = b.m;
        heavy.bodies[p].dragWeight = 1e6;
        ASSERT_EQ(session.update(heavy), Status::Ok);
        ASSERT_EQ(session.beginDrag({p}, {}), Status::Ok);
        double total = 0.0;
        for (int k = 1; k <= 18; ++k) {
            total = 0.9 * k;
            Result r = session.dragStep({{p, at(0, 20, 10, rot(0, 0, 1, total))}});
            ASSERT_EQ(r.status, Status::Ok) << k << ": " << r.message;
        }
        ASSERT_EQ(session.endDrag().status, Status::Ok);
        const auto poses = session.poses();
        // The pinion nearly where the mouse put it (the rack resists a
        // little), the rack moved by 7 times the whole angle the pinion
        // turned through: -x + 7 thez is held at its start, 0.
        const double turn = thez(poses[p].placement);
        EXPECT_NEAR(std::remainder(turn - total, 2 * pi), 0.0, 1e-3);
        const double accumulated = total + std::remainder(turn - total, 2 * pi);
        EXPECT_NEAR(poses[s].placement.getPosition().x, 7.0 * accumulated, 1e-6);
    }
    {
        SCOPED_TRACE("simulation");
        Model m = b.m;
        Driver d;
        d.frameI = ri;
        d.frameJ = rj;
        d.kind = DriverKind::RotationZ;
        d.rotation.text = "90*time";
        d.rotation.tree = FormulaTree::parse(d.rotation.text);
        m.addDriver(d);
        SimulationSpec spec;
        spec.tStart = 0.0;
        spec.tEnd = 1.0;
        spec.tOutput = 0.1;
        ChronoSession session;
        ASSERT_EQ(session.load(m), Status::Ok);
        Recorder actual;
        ASSERT_EQ(session.simulate(spec, actual), Status::Ok);
        ReferenceSession reference;
        ASSERT_EQ(reference.load(m), Status::Ok);
        Recorder expected;
        ASSERT_EQ(reference.simulate(spec, expected), Status::Ok);
        ASSERT_EQ(actual.frames.size(), 11u);
        ASSERT_EQ(expected.frames.size(), 11u);
        for (std::size_t k = 0; k < actual.frames.size(); ++k) {
            SCOPED_TRACE(k);
            const double t = actual.times[k];
            const auto& f = actual.frames[k];
            EXPECT_NEAR(std::remainder(thez(f[p].placement) - 90.0 * t, 2 * pi), 0.0, 1e-9);
            EXPECT_NEAR(f[s].placement.getPosition().x, 7.0 * 90.0 * t, 1e-8);
            for (std::size_t i = 0; i < f.size(); ++i) {
                EXPECT_LT(std::sqrt(moved(f[i].placement, expected.frames[k][i].placement)), 1e-7)
                    << "body " << i;
            }
        }
    }
}

// Limits: a drag beyond one stops on it, a drag within them is free.
TEST(ChronoBackend, limits)  // NOLINT
{
    Builder b;
    const Id body = b.body("B", at(0, 0, 25));
    const Id fi = b.frame(b.g, at(0, 0, 10));
    const Id fj = b.frame(body, at(0, 0, 0));
    b.joint(JointKind::Cylindrical, fi, fj);
    Limit slide;
    slide.frameI = fi;
    slide.frameJ = fj;
    slide.kind = LimitKind::TranslationZ;
    slide.min = 0.0;
    slide.max = 20.0;
    b.m.addLimit(slide);
    Limit turn = slide;
    turn.kind = LimitKind::RotationZ;
    turn.min = -15.0 * pi / 180.0;
    turn.max = 30.0 * pi / 180.0;
    b.m.addLimit(turn);

    ChronoSession session;
    ASSERT_EQ(session.load(b.m), Status::Ok);
    ASSERT_EQ(session.solve({}).status, Status::Ok);
    EXPECT_EQ(session.analyse().dof, 6 + 2);

    ASSERT_EQ(session.beginDrag({body}, {}), Status::Ok);
    Result r = session.dragStep({{body, at(0, 0, 25, rot(0, 0, 1, 0.2))}});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    EXPECT_NEAR(session.poses()[body].placement.getPosition().z, 25.0, 1e-9);
    EXPECT_NEAR(thez(session.poses()[body].placement), 0.2, 1e-9);

    r = session.dragStep({{body, at(0, 0, 50, rot(0, 0, 1, 1.0))}});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    EXPECT_NEAR(session.poses()[body].placement.getPosition().z, 30.0, 1e-9);
    EXPECT_NEAR(thez(session.poses()[body].placement), 30.0 * pi / 180.0, 1e-9);

    r = session.dragStep({{body, at(0, 0, 5, rot(0, 0, 1, -1.0))}});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    EXPECT_NEAR(session.poses()[body].placement.getPosition().z, 10.0, 1e-9);
    EXPECT_NEAR(thez(session.poses()[body].placement), -15.0 * pi / 180.0, 1e-9);
}

// A pendulum dragged a quarter turn follows; a drag weight alone is a warm
// update; a target off the axis gives the closest pose on it.
TEST(ChronoBackend, dragAndUpdate)  // NOLINT
{
    Builder b;
    const Id body = b.body("B", at(3, 1, 2, rot(1, 2, 3, 0.2)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
    ChronoSession session;
    ASSERT_EQ(session.load(b.m), Status::Ok);
    ASSERT_EQ(session.solve({}).status, Status::Ok);
    EXPECT_NEAR((session.poses()[body].placement.getPosition() - Base::Vector3d(0, 0, 10)).Length(),
                0.0,
                1e-9);

    Model weighted = b.m;
    weighted.bodies[body].dragWeight = 10.0;
    EXPECT_EQ(session.update(weighted), Status::Ok);
    Model moved = b.m;
    moved.joints[0].value = 1.0;
    EXPECT_EQ(session.update(moved), Status::NeedsReload);

    ASSERT_EQ(session.beginDrag({body}, {}), Status::Ok);
    const Base::Placement target = at(0, 0, 10, rot(0, 0, 1, pi / 2));
    Result r = session.dragStep({{body, target}});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    ASSERT_EQ(r.moved.size(), 1u);
    EXPECT_TRUE(session.poses()[body].placement.isSame(target, 1e-9));
    r = session.dragStep({{body, at(4, 0, 10, rot(0, 0, 1, pi / 2))}});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    EXPECT_TRUE(session.poses()[body].placement.isSame(target, 1e-9));
    EXPECT_EQ(session.endDrag().status, Status::Ok);
    EXPECT_TRUE(session.poses()[body].placement.isSame(target, 1e-9));
}

// The motors' functions are the evaluator: GetVal, GetDer and GetDer2 equal
// its value, rate and acceleration bit for bit (no finite differences).
TEST(ChronoBackend, motionFunctionIsTheEvaluator)  // NOLINT
{
    for (const char* text : {"36*time",
                             "2*sin(time)",
                             "5*time + 0.25",
                             "(time + 1)^time",
                             "2*exp(time/0.8)",
                             "abs(time - 0.45)",
                             "piecewise(time, functions(time, 2*time - 0.3, 0.75 + 0.5*time), "
                             "transitions(0.3, 0.7))"}) {
        SCOPED_TRACE(text);
        Formula formula;
        formula.text = text;
        formula.tree = FormulaTree::parse(text);
        ASSERT_TRUE(formula.tree);
        for (double t = 0.013; t < 1.5; t += 0.097) {
            const FormulaValue want = formula.tree->evaluate(t);
            const FormulaValue got = Chrono::motionFunction(formula, t);
            EXPECT_EQ(got.value, want.value);
            EXPECT_EQ(got.rate, want.rate);
            EXPECT_EQ(got.acceleration, want.acceleration);
        }
    }
    Formula bad;
    bad.text = "2*-time";
    EXPECT_TRUE(std::isnan(Chrono::motionFunction(bad, 0.5).value));
}

namespace
{

// The Reference test's gateSimulation.
Model gateSimulation()
{
    Builder b;
    auto joint = [&](JointKind kind, Id other, Base::Vector3d where) {
        Joint j;
        j.kind = kind;
        j.source = b.m.addSource("J" + std::to_string(b.m.joints.size()));
        j.frameI = b.frame(b.g, Base::Placement(where, Base::Rotation()));
        j.frameJ = b.frame(other, at(5, 5, 0));
        b.m.addJoint(j);
        return j;
    };
    auto driver = [&](const Joint& j, DriverKind kind, const char* rotation, const char* slide) {
        Driver d;
        d.source = j.source;
        d.frameI = j.frameI;
        d.frameJ = j.frameJ;
        d.kind = kind;
        d.rotation.text = rotation;
        d.translation.text = slide;
        d.rotation.tree = FormulaTree::parse(rotation);
        d.translation.tree = FormulaTree::parse(slide);
        b.m.addDriver(d);
    };
    Joint rev = joint(JointKind::Revolute, b.body("A", at(0, 0, 20)), Base::Vector3d(5, 5, 20));
    Joint sli = joint(JointKind::Prismatic, b.body("B", at(30, 0, 20)), Base::Vector3d(15, 5, 20));
    const Id c = b.body("C", at(-30, 0, 20));
    Joint cyl = joint(JointKind::Cylindrical, c, Base::Vector3d(2, 15, 20));
    driver(rev, DriverKind::RotationZ, "36*time", "");
    driver(sli, DriverKind::TranslationZ, "", "5*time + 0");
    driver(cyl, DriverKind::Both, "90*time", "2*sin(time)");
    driver(cyl, DriverKind::RotationZ, "90*time", "");
    return b.m;
}

double turnOf(const Base::Placement& p)
{
    double x, y, z, w;
    p.getRotation().getValue(x, y, z, w);
    return 2.0 * std::atan2(z, w);
}

}  // namespace

// Drivers turning more than a quarter turn per output step (3.6 and 9 rad at
// 0.1 s): every frame where the formula puts it, and a gear the driven one
// turns keeps the ratio with the turns counted.
TEST(ChronoBackend, drivenAnglesFollowTheFormula)  // NOLINT
{
    SimulationSpec spec;
    spec.tStart = 0.0;
    spec.tEnd = 1.0;
    spec.tOutput = 0.1;
    {
        const Model m = gateSimulation();
        ChronoSession session;
        ASSERT_EQ(session.load(m), Status::Ok);
        Recorder actual;
        ASSERT_EQ(session.simulate(spec, actual), Status::Ok);
        ASSERT_EQ(actual.frames.size(), 11u);
        for (std::size_t k = 0; k < actual.frames.size(); ++k) {
            SCOPED_TRACE(k);
            const double t = actual.times[k];
            const auto& f = actual.frames[k];
            EXPECT_NEAR(std::remainder(turnOf(f[1].placement) - 36.0 * t, 2 * pi), 0.0, 1e-9);
            EXPECT_NEAR(f[2].placement.getPosition().z - 20.0, 5.0 * t, 1e-9);
            EXPECT_NEAR(std::remainder(turnOf(f[3].placement) - 90.0 * t, 2 * pi), 0.0, 1e-9);
            EXPECT_NEAR(f[3].placement.getPosition().z - 20.0, 2.0 * std::sin(t), 1e-9);
        }
        // The motors are gone after the run: a static solve has none.
        EXPECT_EQ(session.analyse().dof, 6 + 4);
    }
    {
        // A drives B through a gear of ratio 1/2: B turns -t/2 of A's 90*t,
        // counted over the turns (B's turn is known modulo 2 pi only, so the
        // check is that it agrees with -45*t modulo 2 pi at every frame,
        // which a lost turn of A -- pi of B -- breaks).
        Id a = NoId;
        Id bb = NoId;
        Model m = gearPair(20.0, a, bb);
        Driver d;
        d.source = m.joints[0].source;
        d.frameI = m.joints[0].frameI;
        d.frameJ = m.joints[0].frameJ;
        d.kind = DriverKind::RotationZ;
        d.rotation.text = "90*time";
        d.rotation.tree = FormulaTree::parse(d.rotation.text);
        m.addDriver(d);
        ChronoSession session;
        ASSERT_EQ(session.load(m), Status::Ok);
        Recorder actual;
        ASSERT_EQ(session.simulate(spec, actual), Status::Ok);
        ASSERT_EQ(actual.frames.size(), 11u);
        const double b0 = thez(m.bodies[bb].placement);
        for (std::size_t k = 0; k < actual.frames.size(); ++k) {
            SCOPED_TRACE(k);
            const double t = actual.times[k];
            EXPECT_NEAR(std::remainder(thez(actual.frames[k][a].placement) - 90.0 * t, 2 * pi),
                        0.0,
                        1e-9);
            EXPECT_NEAR(
                std::remainder(thez(actual.frames[k][bb].placement) - b0 + 45.0 * t, 2 * pi),
                0.0,
                1e-8);
        }
    }
}

// The kinematic run against the Reference's on the gate's simulation, at the
// gate's own output step.
TEST(ChronoBackend, simulationMatchesTheReference)  // NOLINT
{
    const Model m = gateSimulation();
    SimulationSpec spec;
    spec.tStart = 0.0;
    spec.tEnd = 1.0;
    spec.tOutput = 0.1;
    ReferenceSession reference;
    ASSERT_EQ(reference.load(m), Status::Ok);
    Recorder expected;
    ASSERT_EQ(reference.simulate(spec, expected), Status::Ok);
    ChronoSession session;
    ASSERT_EQ(session.load(m), Status::Ok);
    Recorder actual;
    ASSERT_EQ(session.simulate(spec, actual), Status::Ok);
    ASSERT_EQ(actual.frames.size(), expected.frames.size());
    for (std::size_t k = 0; k < actual.frames.size(); ++k) {
        SCOPED_TRACE(k);
        for (std::size_t i = 0; i < actual.frames[k].size(); ++i) {
            const Base::Placement& p = actual.frames[k][i].placement;
            const Base::Placement& q = expected.frames[k][i].placement;
            EXPECT_LT(std::sqrt(moved(p, q)), 1e-7) << "body " << i;
        }
    }
}

// Outside a formula's domain the run stops.
TEST(ChronoBackend, simulationStopsOutsideTheDomain)  // NOLINT
{
    Builder b;
    const Id body = b.body("B", at(0, 0, 10));
    const Id fi = b.frame(b.g, at(0, 0, 10));
    const Id fj = b.frame(body, at(0, 0, 0));
    b.joint(JointKind::Prismatic, fi, fj);
    Driver d;
    d.frameI = fi;
    d.frameJ = fj;
    d.kind = DriverKind::TranslationZ;
    d.translation.text = "ln(time - 0.5)";
    d.translation.tree = FormulaTree::parse(d.translation.text);
    b.m.addDriver(d);
    SimulationSpec spec;
    spec.tStart = 0.0;
    spec.tEnd = 1.0;
    spec.tOutput = 0.1;
    ChronoSession session;
    ASSERT_EQ(session.load(b.m), Status::Ok);
    Recorder actual;
    EXPECT_EQ(session.simulate(spec, actual), Status::Failed);
    EXPECT_TRUE(actual.frames.empty());
}

// Registered as "Chrono", with what sec 5.3 says it does.
TEST(ChronoBackend, registered)  // NOLINT
{
    tests::initApplication();
    Base::Interpreter().runString("import AssemblyApp");
    Backend* backend = Registry::instance().find("Chrono");
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->name(), "Chrono");
    const Capabilities caps = backend->capabilities();
    EXPECT_EQ(caps.joints, Chrono::supportedKinds());
    EXPECT_TRUE(caps.limits);
    EXPECT_TRUE(caps.drivers);
    EXPECT_TRUE(caps.drag);
    EXPECT_TRUE(caps.kinematics);
    EXPECT_TRUE(caps.redundancy);
    EXPECT_TRUE(caps.conflicts);
    EXPECT_FALSE(caps.warmUpdate);
    // A dynamic run since phase D1 (docs/AssemblyDynamics.md)
    EXPECT_TRUE(caps.dynamics);
    auto session = backend->createSession();
    ASSERT_TRUE(session);
    EXPECT_NE(dynamic_cast<ChronoSession*>(session.get()), nullptr);
    const auto names = Registry::instance().names();
    EXPECT_NE(std::find(names.begin(), names.end(), "Chrono"), names.end());
}

// A box far from the plane (or the point) it must reach, with a lever arm,
// among grounded bodies: the restoration does not turn it through radians to
// get there (the gate's PointPlane and PointSphere cases), and the solution
// is no farther from the start than the Reference's.
TEST(ChronoBackend, farStartWithManyGroundedBodies)  // NOLINT
{
    for (JointKind kind : {JointKind::PointInPlane, JointKind::PointPointDistance}) {
        SCOPED_TRACE(static_cast<int>(kind));
        Builder b;
        const double xs[4][3] = {{0, 40, 0}, {40, 0, 0}, {-40, 0, 0}, {0, 0, 0}};
        for (int k = 0; k < 4; ++k) {
            Frame onGround;
            onGround.local = at(xs[k][0], xs[k][1], xs[k][2]);
            onGround.label = b.m.addSource("mg" + std::to_string(k));
            const Id fg = b.m.addFrame(onGround);
            Body body;
            body.source = b.m.addSource("g" + std::to_string(k));
            body.grounded = true;
            body.placement = onGround.local;
            const Id g = b.m.addBody(body);
            Frame fix;
            fix.body = g;
            fix.label = b.m.addSource("fix" + std::to_string(k));
            const Id ff = b.m.addFrame(fix);
            b.m.addGround({NoId, b.m.addSource("g"), fg, ff});
        }
        const Base::Placement start = at(7, -23, 41, rot(1, 2, 3, 25.0 * pi / 180.0));
        const Id m = b.body("BoxM", start);
        const Id fi = b.frame(b.g, at(10, 10, 20));
        const Id fj = b.frame(m, at(0, 0, 10));
        b.joint(kind, fi, fj, 3.0);
        ChronoSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        Result r = session.solve({});
        ASSERT_EQ(r.status, Status::Ok) << r.message;
        const auto poses = session.poses();
        EXPECT_LT(Reference::jointResiduals(b.m, placements(poses))[0], 1e-9);
        for (Id k = 0; k < 5; ++k) {
            EXPECT_TRUE(poses[k].placement.isSame(b.m.bodies[k].placement, 0.0));
        }
        EXPECT_EQ(session.analyse().dof, 6 + 5);
        ReferenceSession reference;
        ASSERT_EQ(reference.load(b.m), Status::Ok);
        ASSERT_EQ(reference.solve({}).status, Status::Ok);
        const double mine = moved(start, poses[m].placement);
        const double theirs = moved(start, reference.poses()[m].placement);
        EXPECT_LE(mine, theirs * (1.0 + 1e-6) + 1e-12) << mine << " " << theirs;
    }
}

namespace
{

// A box of mass m and edges a, b, c along its frame's x, y, z, its centre at com
Inertia boxInertia(Id body, double m, double a, double b, double c, const Base::Vector3d& com)
{
    Inertia inertia;
    inertia.body = body;
    inertia.mass = m;
    inertia.com = com;
    inertia.inertia = {m * (b * b + c * c) / 12,
                       m * (a * a + c * c) / 12,
                       m * (a * a + b * b) / 12,
                       0.0,
                       0.0,
                       0.0};
    return inertia;
}

struct RunRecorder: TrajectorySink
{
    std::vector<std::vector<BodyPose>> frames;
    std::vector<Channel> seen;
    std::vector<std::vector<double>> rows;  // values(), per frame
    bool frame(double, const std::vector<BodyPose>& poses) override
    {
        frames.push_back(poses);
        return true;
    }
    void channels(const std::vector<Channel>& channels) override
    {
        seen = channels;
    }
    void values(const std::vector<double>& v) override
    {
        rows.push_back(v);
    }
};

Status runDynamics(const Model& m, RunRecorder& out, double end = 0.1)
{
    SimulationSpec spec;
    spec.analysis = Analysis::Dynamic;
    spec.tEnd = end;
    spec.tOutput = 0.01;
    ChronoSession session;
    if (session.load(m) != Status::Ok) {
        return Status::Failed;
    }
    return session.simulate(spec, out);
}

}  // namespace

// A dynamic run takes out the joint rows that repeat others, in
// OndselSolver's order (docs/AssemblyDynamics.md sec 6.1): a door on two
// coaxial hinges swings as on one, the two hinges' reactions add up to the
// one's and are flagged not unique; a puck on a plane and on a line through
// the plane's origin loses one of the line's two equations (a neutral link
// held partly) and slides on as it was started; two Fixed joints that
// disagree refuse the run.
TEST(ChronoBackend, dynamicRunTakesOutRedundantRows)  // NOLINT
{
    auto door = [](bool twoHinges) {
        Builder b;
        const Id body = b.body("Door", at(0, 0, 10));
        b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
        if (twoHinges) {
            b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 50)), b.frame(body, at(0, 0, 40)));
        }
        Dynamics dynamics;
        dynamics.gravity = Base::Vector3d(0, -9806.65, 0);
        dynamics.inertias.push_back(boxInertia(body, 0.5, 100, 10, 40, Base::Vector3d(50, 0, 20)));
        b.m.dynamics = dynamics;
        return b.m;
    };
    RunRecorder one;
    RunRecorder two;
    ASSERT_EQ(runDynamics(door(false), one), Status::Ok);
    ASSERT_EQ(runDynamics(door(true), two), Status::Ok);
    ASSERT_EQ(one.frames.size(), 11u);
    ASSERT_EQ(two.frames.size(), one.frames.size());
    ASSERT_EQ(one.seen.size(), 2u);
    ASSERT_EQ(two.seen.size(), 4u);
    EXPECT_TRUE(one.seen[0].unique);
    for (const Channel& channel : two.seen) {
        EXPECT_FALSE(channel.unique) << channel.source << " " << channel.quantity;
    }
    double turned = 0.0;
    for (std::size_t f = 0; f < one.frames.size(); ++f) {
        SCOPED_TRACE(f);
        const Base::Placement& a = one.frames[f][1].placement;
        const Base::Placement& b = two.frames[f][1].placement;
        // moved() is squared: within 1e-8 mm
        EXPECT_LT(moved(a, b), 1e-16);
        turned = std::max(turned, std::abs(thez(a)));
        // Forces on the ground (frame I's body): the two hinges' together
        // are the one's
        const Base::Vector3d single(one.rows[f][0], one.rows[f][1], one.rows[f][2]);
        const Base::Vector3d sum(two.rows[f][0] + two.rows[f][6],
                                 two.rows[f][1] + two.rows[f][7],
                                 two.rows[f][2] + two.rows[f][8]);
        EXPECT_LT((single - sum).Length(), 1e-6 * single.Length()) << single.Length();
    }
    // It swings
    EXPECT_GT(turned, 0.05);

    {
        SCOPED_TRACE("a puck on a plane and a line");
        Builder b;
        const Id puck = b.body("Puck", at(0, 0, 0));
        b.joint(JointKind::Planar, b.frame(b.g, at(0, 0, 0)), b.frame(puck, at(0, 0, 0)));
        Handle(Geom_Line) line = new Geom_Line(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0));
        const Id fj = b.frame(puck, at(0, 0, 0));
        const Id id = b.joint(JointKind::PointOnCurve, b.frame(b.g, at(0, 0, 0)), fj);
        Curve record;
        record.source = b.m.joints[id].source;
        record.geometry = Assembly::makeCurveGeometry(line, -1000.0, 1000.0);
        b.m.joints[id].curve = b.m.addCurve(record);
        Dynamics dynamics;
        dynamics.inertias.push_back(boxInertia(puck, 0.2, 20, 20, 5, Base::Vector3d(3, 4, 1)));
        InitialVelocity start;
        start.kind = InitialVelocity::Kind::Body;
        start.frameI = fj;
        start.v = Base::Vector3d(100, 0, 0);
        dynamics.initial.push_back(start);
        b.m.dynamics = dynamics;
        RunRecorder run;
        ASSERT_EQ(runDynamics(b.m, run), Status::Ok);
        ASSERT_EQ(run.frames.size(), 11u);
        ASSERT_EQ(run.seen.size(), 4u);
        for (const Channel& channel : run.seen) {
            EXPECT_FALSE(channel.unique) << channel.source << " " << channel.quantity;
        }
        for (std::size_t f = 0; f < run.frames.size(); ++f) {
            SCOPED_TRACE(f);
            const Base::Placement want = at(100.0 * 0.01 * static_cast<double>(f), 0, 0);
            EXPECT_LT(moved(run.frames[f][1].placement, want), 1e-18);
        }
    }
    {
        SCOPED_TRACE("two Fixed joints that disagree");
        Builder b;
        const Id body = b.body("B", at(0, 0, 10));
        b.joint(JointKind::Fixed, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
        b.joint(JointKind::Fixed, b.frame(b.g, at(0, 0, 12)), b.frame(body, at(0, 0, 0)));
        Dynamics dynamics;
        dynamics.inertias.push_back(boxInertia(body, 1.0, 10, 10, 10, Base::Vector3d()));
        b.m.dynamics = dynamics;
        RunRecorder run;
        EXPECT_EQ(runDynamics(b.m, run), Status::Unsupported);
        EXPECT_TRUE(run.frames.empty());
    }
}
