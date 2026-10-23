// SPDX-License-Identifier: LGPL-2.1-or-later

// The reference backend (docs/AssemblySolverInterface.md sec 6.13): each joint
// kind solved from a perturbed start to a pose checked on the geometry, its
// analytic Jacobian against central differences, the rank and DOF of
// hand-counted models, couplers, limits, drags, and the position-solve
// simulation against OndselSolver's kinematic run.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <string>

#include <Base/Interpreter.h>
#include <Base/Placement.h>
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
        // Unique: the Ondsel adapter names its markers by the label.
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

// The angle of J's x about I's z (I the identity here), as thez measures it.
double thez(const Base::Placement& p)
{
    const Base::Vector3d x = p.getRotation().multVec(Base::Vector3d(1, 0, 0));
    return std::atan2(x.y, x.x);
}

struct KindCase
{
    JointKind kind;
    const char* name;
    double value;
    Base::Placement solution;  // J relative to I, satisfying the joint
    int dof;  // of one body hung from the ground by this joint alone
};

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
        {JointKind::AngleZ, "AngleZ", 0.5, at(3, 4, 2, rot(1, 0, 0, 0.5)), 5},
        {JointKind::PointInPlane, "PointInPlane", 2.0, at(3, 4, 2, rot(1, 1, 0, 0.6)), 5},
        {JointKind::LineInPlane, "LineInPlane", 2.0, at(3, 4, 2, rot(1, 0, 0, pi / 2)), 4},
        {JointKind::PointOnLine, "PointOnLine", 0.0, at(0, 0, 7, rot(1, 1, 0, 0.6)), 4},
        {JointKind::PointPointDistance,
         "PointPointDistance",
         5.0,
         at(3, 4, 0, rot(1, 1, 0, 0.6)),
         5},
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
                {std::abs(p.x), std::abs(p.y), std::abs(std::remainder(angle, 2 * pi))}
            );
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

// G grounded, B hung from it by one joint whose solution puts B at
// 'solution'; B starts perturbed from there.
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
    // B * onB = onG * solution
    s.solved = onG * c.solution * onB.inverse();
    s.body = s.b.body("B", perturbation * s.solved);
    s.fi = s.b.frame(s.b.g, onG);
    s.fj = s.b.frame(s.body, onB);
    s.joint = s.b.joint(c.kind, s.fi, s.fj, c.value);
    return s;
}

}  // namespace

// Every joint kind: zero residual at the known solution, and the solver gets
// to a pose with the joint's property from a perturbed start.
TEST(ReferenceBackend, eachKindSolvesFromAPerturbedStart)  // NOLINT
{
    const Base::Placement perturbation = at(0.3, -0.2, 0.25, rot(1, -2, 0.5, 0.05));
    for (const auto& c : kindCases()) {
        SCOPED_TRACE(c.name);
        Single s = single(c, perturbation);
        ReferenceSession session;
        ASSERT_EQ(session.load(s.b.m), Status::Ok);

        // Zero at the known solution.
        for (double r : session.residualsAt({Base::Placement(), s.solved})) {
            EXPECT_NEAR(r, 0.0, 1e-12);
        }
        // Not at the start.
        double worstAtStart = 0.0;
        for (double r : session.residualsAt(placements(session.poses()))) {
            worstAtStart = std::max(worstAtStart, std::abs(r));
        }
        EXPECT_GT(worstAtStart, 1e-3);

        Result r = session.solve({});
        ASSERT_EQ(r.status, Status::Ok) << r.message;
        const auto poses = session.poses();
        EXPECT_TRUE(poses[s.b.g].placement.isSame(Base::Placement(), 1e-12));
        const Base::Placement T = relative(s.b.m, poses, s.fi, s.fj);
        EXPECT_LT(geometricError(c.kind, c.value, T), 1e-9);
        const auto residuals = Reference::jointResiduals(s.b.m, placements(poses));
        ASSERT_EQ(residuals.size(), 1u);
        EXPECT_LT(residuals[0], 1e-9);
        // The start does not satisfy the joint (apart from what the
        // perturbation happens to keep), so the body moved, but not far.
        const double moved = (poses[s.body].placement.getPosition()
                              - s.b.m.bodies[s.body].placement.getPosition())
                                 .Length();
        EXPECT_LT(moved, 2.0);

        Diagnostics d = session.analyse();
        // 6 x (1 + 2 bodies) - 6 (the ground) - the joint's: OndselSession's count.
        EXPECT_EQ(d.dof, 6 + c.dof);
        EXPECT_TRUE(d.redundant.empty());
        EXPECT_TRUE(d.conflicting.empty());
        // The ground body is pinned, not a variable: the rank is the joint's.
        EXPECT_EQ(session.rank(), 6 - c.dof);
    }
}

// The analytic Jacobian of every equation against central differences, at
// poses where nothing is singular.
TEST(ReferenceBackend, jacobianMatchesDifferences)  // NOLINT
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    auto randomPose = [&](double spread) {
        return at(spread * u(rng),
                  spread * u(rng),
                  spread * u(rng),
                  rot(u(rng), u(rng), u(rng) + 1.5, 2.0 * u(rng)));
    };
    auto check = [&](const Model& m, const char* name) {
        SCOPED_TRACE(name);
        ReferenceSession session;
        ASSERT_EQ(session.load(m), Status::Ok);
        std::vector<Base::Placement> poses;
        for (const auto& body : m.bodies) {
            poses.push_back(body.placement);
        }
        const auto J = session.jacobianAt(poses);
        const double h = 1e-6;
        for (std::size_t b = 0; b < poses.size(); ++b) {
            // A body a Ground pins is not a variable: no gradient.
            const bool pinned = std::any_of(
                m.grounds.begin(),
                m.grounds.end(),
                [&](const Ground& g) { return m.frames[g.frameJ].body == b; }
            );
            for (int c = 0; c < 6; ++c) {
                if (pinned) {
                    for (const auto& row : J) {
                        EXPECT_EQ(row[6 * b + c], 0.0);
                    }
                    continue;
                }
                auto plus = poses;
                auto minus = poses;
                if (c < 3) {
                    Base::Vector3d dx;
                    dx[c] = h;
                    plus[b].setPosition(plus[b].getPosition() + dx);
                    minus[b].setPosition(minus[b].getPosition() - dx);
                }
                else {
                    Base::Vector3d w;
                    w[c - 3] = 1.0;
                    plus[b].setRotation(Base::Rotation(w, h) * plus[b].getRotation());
                    minus[b].setRotation(Base::Rotation(w, -h) * minus[b].getRotation());
                }
                const auto rp = session.residualsAt(plus);
                const auto rm = session.residualsAt(minus);
                ASSERT_EQ(rp.size(), J.size());
                for (std::size_t r = 0; r < J.size(); ++r) {
                    const double numeric = (rp[r] - rm[r]) / (2 * h);
                    const double analytic = J[r][6 * b + c];
                    EXPECT_NEAR(analytic, numeric, 1e-6 * std::max(1.0, std::abs(numeric)))
                        << "row " << r << " body " << b << " column " << c;
                }
            }
        }
    };

    for (const auto& c : kindCases()) {
        Builder b;
        const Id body = b.body("B", randomPose(10.0));
        // The grounded body is somewhere else than the origin.
        b.m.bodies[b.g].placement = randomPose(3.0);
        const Id fi = b.frame(b.g, randomPose(5.0));
        b.joint(c.kind, fi, b.frame(body, randomPose(5.0)), c.value + 1.0);
        check(b.m, c.name);
    }

    // The couplers, on three free bodies (the carrier K for the coaxial gear).
    struct Coupler
    {
        JointKind kind;
        const char* name;
        double value, value2;
        bool coaxial;
    };
    for (const Coupler& c : {Coupler {JointKind::Gear, "Gear", 10.0, 20.0, false},
                             Coupler {JointKind::Gear, "Belt", 10.0, -20.0, false},
                             Coupler {JointKind::CoaxialGear, "CoaxialGear", 10.0, 5.0, true},
                             Coupler {JointKind::RackPinion, "RackPinion", 7.0, 0.0, false},
                             Coupler {JointKind::Screw, "Screw", 3.0, 0.0, false}}) {
        Builder b;
        const Id a = b.body("A", randomPose(10.0));
        const Id bb = b.body("B", randomPose(10.0));
        const Id k = b.body("K", randomPose(10.0));
        Base::Placement onA = randomPose(4.0);
        Base::Placement onB = randomPose(4.0);
        if (c.coaxial) {
            // Frames I and J on one axis at the start: B placed so.
            b.m.bodies[bb].placement = b.m.bodies[a].placement * onA
                * at(0, 0, 3, rot(0, 0, 1, 0.8))
                * onB.inverse();
        }
        const Id fk = b.frame(k, randomPose(4.0));
        b.joint(c.kind, b.frame(a, onA), b.frame(bb, onB), c.value, c.value2, fk);
        check(b.m, c.name);
    }

    // A driver of each kind, held at a constant.
    Builder b;
    const Id body = b.body("B", randomPose(10.0));
    const Id fi = b.frame(b.g, randomPose(5.0));
    const Id fj = b.frame(body, randomPose(5.0));
    Driver dr;
    dr.frameI = fi;
    dr.frameJ = fj;
    dr.kind = DriverKind::Both;
    dr.rotation.text = "0.4";
    dr.translation.text = "2.5";
    b.m.addDriver(dr);
    check(b.m, "drivers");
}

namespace
{

// Two revolutes between G and B. 'gap' moves the second along the common
// axis; 'tilt' turns its axis.
Model twoRevolutes(double gap, double tilt, Id& body)
{
    Builder b;
    body = b.body("B", at(0.2, -0.1, 10.3, rot(1, 0, 0, 0.02)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
    b.joint(
        JointKind::Revolute,
        b.frame(b.g, at(0, 0, 10 + gap, rot(1, 0, 0, tilt))),
        b.frame(body, at(0, 0, gap, rot(1, 0, 0, tilt)))
    );
    return b.m;
}

}  // namespace

// DOF and redundancy on models counted by hand.
TEST(ReferenceBackend, rankOnHandCountedModels)  // NOLINT
{
    Id body = NoId;
    {
        // One revolute: DOF 1, nothing redundant.
        Builder b;
        body = b.body("B", at(1, 2, 3));
        b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
        ReferenceSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        Diagnostics d = session.analyse();
        EXPECT_EQ(d.dof, 6 + 1);  // OndselSession's count: the origin's 6 too
        EXPECT_TRUE(d.redundant.empty());
        EXPECT_EQ(d.known & Diagnostics::KnownDof, Diagnostics::KnownDof);
    }
    {
        // Two revolutes on a common axis: still DOF 1. The second's axial
        // displacement and both joints' parallelism repeat what is already
        // there: five equations dependent, nothing conflicting.
        Model m = twoRevolutes(4.0, 0.0, body);
        ReferenceSession session;
        ASSERT_EQ(session.load(m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        Diagnostics d = session.analyse();
        EXPECT_EQ(d.dof, 6 + 1);
        EXPECT_EQ(d.redundant, (std::vector<Id> {0, 1}));
        EXPECT_EQ(d.partiallyRedundant, (std::vector<Id> {0, 1}));
        EXPECT_TRUE(d.conflicting.empty());
        int dependent = 0;
        for (const auto& e : session.equations()) {
            if (e.owner == Reference::EquationInfo::Owner::Joint) {
                dependent += e.independent ? 0 : 1;
            }
        }
        EXPECT_EQ(dependent, 5);
        EXPECT_EQ(session.rank(), 5);
    }
    {
        // Two revolutes through one point on crossing axes: B cannot turn.
        Model m = twoRevolutes(0.0, 0.5, body);
        ReferenceSession session;
        ASSERT_EQ(session.load(m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        Diagnostics d = session.analyse();
        EXPECT_EQ(d.dof, 6 + 0);
        EXPECT_EQ(d.redundant, (std::vector<Id> {1}));  // its at-point equations
        EXPECT_TRUE(d.conflicting.empty());
    }
    {
        // Two fixed joints that disagree: the second conflicts, and the first
        // is satisfied.
        Builder b;
        body = b.body("B", at(1, 2, 3));
        const Id f0 = b.frame(b.g, at(0, 0, 10));
        const Id f1 = b.frame(body, at(0, 0, 0));
        b.joint(JointKind::Fixed, f0, f1);
        b.joint(JointKind::Fixed, b.frame(b.g, at(0, 0, 12)), b.frame(body, at(0, 0, 0)));
        ReferenceSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        Diagnostics d = session.analyse();
        EXPECT_EQ(d.dof, 6 + 0);
        EXPECT_EQ(d.conflicting, (std::vector<Id> {1}));
        EXPECT_EQ(d.redundant, (std::vector<Id> {1}));
        EXPECT_TRUE(d.partiallyRedundant.empty());
        const auto residuals = Reference::jointResiduals(b.m, placements(session.poses()));
        EXPECT_LT(residuals[0], 1e-9);
        EXPECT_NEAR(residuals[1], 2.0, 1e-9);
    }
    {
        // A free body nobody holds: 6; the grounded one alone: 0.
        Builder b;
        b.body("Free", at(1, 2, 3));
        ReferenceSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        EXPECT_EQ(session.analyse().dof, 6 + 6);
    }
}

namespace
{

// G with two gears A (radius 10) and B (radius 'rb') on revolutes at
// (0, 0, 10) and (30, 0, 10), coupled by 'kind'.
Model gearPair(JointKind kind, double rb, Id& a, Id& bb)
{
    Builder b;
    a = b.body("A", at(0, 0, 10));
    bb = b.body("B", at(30, 0, 10, rot(0, 0, 1, 0.3)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(a, at(0, 0, 0)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(30, 0, 10)), b.frame(bb, at(0, 0, 0)));
    b.joint(kind, b.frame(a, at(0, 0, 0)), b.frame(bb, at(0, 0, 0)), 10.0, rb);
    return b.m;
}

double turnAboutZ(const Base::Placement& p)
{
    return thez(p);
}

}  // namespace

// A dragged gear turns the other by the ratio, oppositely for gears, alike for a belt.
TEST(ReferenceBackend, gearsFollowADrag)  // NOLINT
{
    for (double rb : {20.0, -20.0}) {
        SCOPED_TRACE(rb);
        Id a = NoId;
        Id bb = NoId;
        Model m = gearPair(JointKind::Gear, rb, a, bb);
        ReferenceSession session;
        ASSERT_EQ(session.load(m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        EXPECT_EQ(session.analyse().dof, 6 + 1);
        const double b0 = turnAboutZ(session.poses()[bb].placement);
        ASSERT_EQ(session.beginDrag({a}, {}), Status::Ok);
        const double turn = 20.0 * pi / 180.0;
        Result r = session.dragStep({{a, at(0, 0, 10, rot(0, 0, 1, turn))}});
        ASSERT_EQ(r.status, Status::Ok) << r.message;
        const auto poses = session.poses();
        // B, held where it was with the same weight, gives way: A gets part
        // of the turn, and orbit(J, I) + (rA / rB) orbit(I, J) is held, so B
        // turns by -(rA / rB) of A's turn.
        const double turned = turnAboutZ(poses[a].placement);
        EXPECT_GT(turned, 0.5 * turn);
        EXPECT_LE(turned, turn + 1e-12);
        EXPECT_NEAR(turnAboutZ(poses[bb].placement) - b0, -(10.0 / rb) * turned, 1e-9);
        EXPECT_EQ(session.endDrag().status, Status::Ok);
    }
}

// Two gears on one axis turn against a carrier frame K.
TEST(ReferenceBackend, coaxialGearFollowsADrag)  // NOLINT
{
    Builder b;
    const Id a = b.body("A", at(0, 0, 20));
    const Id bb = b.body("B", at(0, 0, 25, rot(0, 0, 1, 0.2)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 20)), b.frame(a, at(0, 0, 0)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 25)), b.frame(bb, at(0, 0, 0)));
    const Id carrier = b.frame(b.g, at(0, 0, 20));
    b.joint(
        JointKind::CoaxialGear,
        b.frame(a, at(0, 0, 0)),
        b.frame(bb, at(0, 0, 0)),
        10.0,
        5.0,
        carrier
    );
    ReferenceSession session;
    ASSERT_EQ(session.load(b.m), Status::Ok);
    ASSERT_EQ(session.solve({}).status, Status::Ok);
    EXPECT_EQ(session.analyse().dof, 6 + 1);
    const double b0 = turnAboutZ(session.poses()[bb].placement);
    ASSERT_EQ(session.beginDrag({a}, {}), Status::Ok);
    const double turn = 0.4;
    ASSERT_EQ(session.dragStep({{a, at(0, 0, 20, rot(0, 0, 1, turn))}}).status, Status::Ok);
    // angleKJ + (rA / rB) angleKI held.
    // B, turning twice as far, resists with the same weight.
    const double turned = turnAboutZ(session.poses()[a].placement);
    EXPECT_GT(turned, 0.05);
    EXPECT_NEAR(turnAboutZ(session.poses()[bb].placement) - b0, -2.0 * turned, 1e-9);

    // Without the carrier it cannot be solved.
    Model noCarrier = b.m;
    noCarrier.joints[2].frameK = NoId;
    ReferenceSession other;
    ASSERT_EQ(other.load(noCarrier), Status::Ok);
    EXPECT_NE(other.solve({}).status, Status::Ok);
}

// A rack sliding along x under a pinion, and a screw: the slide follows the
// turn by the pitch radius, and by the pitch per turn.
TEST(ReferenceBackend, rackPinionAndScrewFollowADrag)  // NOLINT
{
    for (JointKind kind : {JointKind::RackPinion, JointKind::Screw}) {
        const bool rack = kind == JointKind::RackPinion;
        SCOPED_TRACE(rack ? "RackPinion" : "Screw");
        Builder b;
        // The sliding body S: along x for the rack, along z for the screw
        // (its slide axis z of the joint frame).
        const Id s = b.body("S", at(0, 0, 10));
        const Id p = b.body("P", at(0, 20, 10, rot(0, 0, 1, 0.1)));
        const Base::Rotation slideAxis = rack ? rot(0, 1, 0, pi / 2) : Base::Rotation();
        b.joint(
            JointKind::Prismatic,
            b.frame(b.g, at(0, 0, 10, slideAxis)),
            b.frame(s, at(0, 0, 0, slideAxis))
        );
        b.joint(JointKind::Revolute, b.frame(b.g, at(0, 20, 10)), b.frame(p, at(0, 0, 0)));
        // I on the sliding body, J on the turning one.
        const Id fi = b.frame(s, at(0, 20, 0));
        const Id fj = b.frame(p, at(0, 0, 0));
        b.joint(kind, fi, fj, rack ? 7.0 : 3.0);
        ReferenceSession session;
        ASSERT_EQ(session.load(b.m), Status::Ok);
        ASSERT_EQ(session.solve({}).status, Status::Ok);
        EXPECT_EQ(session.analyse().dof, 6 + 1);
        auto measure = [&](const std::vector<BodyPose>& poses) {
            const Base::Placement T = relative(b.m, poses, fi, fj);
            const double turn = thez(T);
            const Base::Vector3d d = T.getPosition();
            // x + r thez, or 2 pi z - p thez: constant.
            return rack ? d.x + 7.0 * turn : 2 * pi * d.z - 3.0 * turn;
        };
        const double before = measure(session.poses());
        // The pinion heavy, so it follows the mouse and the slide gives way.
        Model heavy = b.m;
        heavy.bodies[p].dragWeight = 1e4;
        ASSERT_EQ(session.update(heavy), Status::Ok);
        ASSERT_EQ(session.beginDrag({p}, {}), Status::Ok);
        Result r = session.dragStep({{p, at(0, 20, 10, rot(0, 0, 1, 0.6))}});
        ASSERT_EQ(r.status, Status::Ok) << r.message;
        const auto poses = session.poses();
        EXPECT_GT(thez(poses[p].placement), 0.1 + 0.4);
        EXPECT_LE(thez(poses[p].placement), 0.6 + 1e-12);
        EXPECT_NEAR(measure(poses), before, 1e-9);
        EXPECT_GT((poses[s].placement.getPosition() - Base::Vector3d(0, 0, 10)).Length(), 0.01);
    }
}

// Limits are inequalities: a static solve from beyond one ends on it, a drag
// beyond one stops on it, a drag within them is free.
TEST(ReferenceBackend, limits)  // NOLINT
{
    Builder b;
    const Id body = b.body("B", at(0, 0, 40));
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

    ReferenceSession session;
    ASSERT_EQ(session.load(b.m), Status::Ok);
    ASSERT_EQ(session.solve({}).status, Status::Ok);
    // Started at 30 along the axis: the closest pose within the limits is at 20.
    EXPECT_NEAR(session.poses()[body].placement.getPosition().z, 30.0, 1e-9);
    // The limits are not equations of the diagnostics.
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

// A pendulum dragged a quarter turn follows; a drag weight alone is a warm update.
TEST(ReferenceBackend, dragAndUpdate)  // NOLINT
{
    Builder b;
    const Id body = b.body("B", at(3, 1, 2, rot(1, 2, 3, 0.2)));
    b.joint(JointKind::Revolute, b.frame(b.g, at(0, 0, 10)), b.frame(body, at(0, 0, 0)));
    ReferenceSession session;
    ASSERT_EQ(session.load(b.m), Status::Ok);
    ASSERT_EQ(session.solve({}).status, Status::Ok);
    const Base::Vector3d onAxis = session.poses()[body].placement.getPosition();
    EXPECT_NEAR((onAxis - Base::Vector3d(0, 0, 10)).Length(), 0.0, 1e-9);

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
    // Dragged off the axis: the closest pose on it.
    r = session.dragStep({{body, at(4, 0, 10, rot(0, 0, 1, pi / 2))}});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    EXPECT_TRUE(session.poses()[body].placement.isSame(target, 1e-9));
    EXPECT_EQ(session.endDrag().status, Status::Ok);
    EXPECT_TRUE(session.poses()[body].placement.isSame(target, 1e-9));
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

// OndselAdapter's gateSimulation: a revolute driven in angle, a slider in
// length, a cylindrical in both (with the angular driver repeated).
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

}  // namespace

// The position-solve simulation against OndselSolver's kinematic run, with
// output steps small enough that no driver turns a quarter turn between two.
TEST(ReferenceBackend, simulationFollowsTheKinematicRun)  // NOLINT
{
    const Model m = gateSimulation();
    SimulationSpec spec;
    spec.tStart = 0.0;
    spec.tEnd = 1.0;
    spec.tOutput = 0.01;

    auto ondsel = std::make_unique<OndselSession>(false);
    ASSERT_EQ(ondsel->load(m), Status::Ok);
    Recorder expected;
    ASSERT_EQ(ondsel->simulate(spec, expected), Status::Ok);
    // runKINEMATIC reports the start twice.
    ASSERT_EQ(expected.frames.size(), 102u);
    expected.times.erase(expected.times.begin());
    expected.frames.erase(expected.frames.begin());

    ReferenceSession session;
    ASSERT_EQ(session.load(m), Status::Ok);
    Recorder actual;
    ASSERT_EQ(session.simulate(spec, actual), Status::Ok);
    ASSERT_EQ(actual.frames.size(), expected.frames.size());
    double worst = 0.0;
    for (std::size_t k = 0; k < actual.frames.size(); ++k) {
        SCOPED_TRACE(k);
        EXPECT_NEAR(actual.times[k], expected.times[k], 1e-12);
        for (std::size_t i = 0; i < actual.frames[k].size(); ++i) {
            const Base::Placement& a = actual.frames[k][i].placement;
            const Base::Placement& e = expected.frames[k][i].placement;
            Base::Vector3d axis;
            double angle = 0.0;
            (a.getRotation().inverse() * e.getRotation()).getValue(axis, angle);
            const double dp = (a.getPosition() - e.getPosition()).Length();
            const double da = std::abs(std::remainder(angle, 2 * pi));
            worst = std::max({worst, dp, da});
            EXPECT_LT(dp, 1e-7) << "body " << i;
            EXPECT_LT(da, 1e-7) << "body " << i;
        }
    }
    RecordProperty("worst", std::to_string(worst));
}

// Drivers that turn more than a quarter turn between output times (the gate's
// 36*time and 90*time at 0.1 s: 3.6 and 9 rad). The Reference holds the angle
// itself and follows the formula. OndselSolver's angular motion holds only
// its sine (ZRotation: the direction cosine of I's y and J's x), which is zero
// at phi and at phi + pi, and from the previous frame it lands on the root
// nearest to it: off by half a turn at every other frame. Asserted, so that a
// fix shows.
TEST(ReferenceBackend, drivenAnglesFollowTheFormula)  // NOLINT
{
    const Model m = gateSimulation();
    SimulationSpec spec;
    spec.tStart = 0.0;
    spec.tEnd = 1.0;
    spec.tOutput = 0.1;
    auto turn = [](const Base::Placement& p) {
        double x, y, z, w;
        p.getRotation().getValue(x, y, z, w);
        return 2.0 * std::atan2(z, w);
    };

    ReferenceSession session;
    ASSERT_EQ(session.load(m), Status::Ok);
    Recorder actual;
    ASSERT_EQ(session.simulate(spec, actual), Status::Ok);
    ASSERT_EQ(actual.frames.size(), 11u);
    for (std::size_t k = 0; k < actual.frames.size(); ++k) {
        SCOPED_TRACE(k);
        const double t = actual.times[k];
        const auto& f = actual.frames[k];
        // A on the revolute at 36*time, B on the slider at 5*time, C on the
        // cylindrical at 90*time and 2*sin(time).
        EXPECT_NEAR(std::remainder(turn(f[1].placement) - 36.0 * t, 2 * pi), 0.0, 1e-9);
        EXPECT_NEAR(f[2].placement.getPosition().z - 20.0, 5.0 * t, 1e-9);
        EXPECT_NEAR(std::remainder(turn(f[3].placement) - 90.0 * t, 2 * pi), 0.0, 1e-9);
        EXPECT_NEAR(f[3].placement.getPosition().z - 20.0, 2.0 * std::sin(t), 1e-9);
    }

    auto ondsel = std::make_unique<OndselSession>(false);
    ASSERT_EQ(ondsel->load(m), Status::Ok);
    Recorder expected;
    ASSERT_EQ(ondsel->simulate(spec, expected), Status::Ok);
    ASSERT_EQ(expected.frames.size(), 12u);
    for (std::size_t k = 1; k < expected.frames.size(); ++k) {
        SCOPED_TRACE(k);
        const double t = expected.times[k];
        const auto& f = expected.frames[k];
        const double offA = std::remainder(turn(f[1].placement) - 36.0 * t, 2 * pi);
        const double offC = std::remainder(turn(f[3].placement) - 90.0 * t, 2 * pi);
        const double flipped = (k - 1) % 2 == 1 ? pi : 0.0;
        EXPECT_NEAR(std::abs(offA), flipped, 1e-6);
        EXPECT_NEAR(std::abs(offC), flipped, 1e-6);
    }
}

// Registered as "Reference" beside "Ondsel", with what sec 5.3 says it does.
TEST(ReferenceBackend, registered)  // NOLINT
{
    tests::initApplication();
    Base::Interpreter().runString("import AssemblyApp");
    Backend* backend = Registry::instance().find("Reference");
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->name(), "Reference");
    const Capabilities caps = backend->capabilities();
    EXPECT_TRUE(caps.joints.all());
    EXPECT_TRUE(caps.limits);
    EXPECT_TRUE(caps.drivers);
    EXPECT_TRUE(caps.drag);
    EXPECT_TRUE(caps.redundancy);
    EXPECT_TRUE(caps.conflicts);
    EXPECT_FALSE(caps.warmUpdate);
    EXPECT_FALSE(caps.dynamics);
    auto session = backend->createSession();
    ASSERT_TRUE(session);
    EXPECT_NE(dynamic_cast<ReferenceSession*>(session.get()), nullptr);
}

// Four grounded bodies (pinned, not variables) and a box far from the plane
// it must touch, with a lever arm: the restoration does not try to turn the
// box through radians to get there (the case the gate's PointPlane found).
TEST(ReferenceBackend, farStartWithManyGroundedBodies)  // NOLINT
{
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
    b.joint(JointKind::PointInPlane, fi, fj, 3.0);
    ReferenceSession session;
    ASSERT_EQ(session.load(b.m), Status::Ok);
    Result r = session.solve({});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    const auto poses = session.poses();
    EXPECT_NEAR(relative(b.m, poses, fi, fj).getPosition().z, 3.0, 1e-9);
    // Every grounded body exactly where it was.
    for (Id k = 0; k < 5; ++k) {
        EXPECT_TRUE(poses[k].placement.isSame(b.m.bodies[k].placement, 0.0));
    }
    // 6 x (1 + 6 bodies) - 6 x 5 grounded - 1.
    EXPECT_EQ(session.analyse().dof, 6 + 5);
}

// A point far from the cylinder of radius 3 it must lie on: the pose closest
// to the start is the radial projection, though the manifold curves within
// the distance to it (where the projected gradient alone crawls).
TEST(ReferenceBackend, closestPoseOnACurvedManifold)  // NOLINT
{
    Builder b;
    const Base::Placement start = at(40, 30, 7, rot(1, 2, 3, 0.4));
    const Id m = b.body("M", start);
    const Id fi = b.frame(b.g, at(0, 0, 0));
    const Id fj = b.frame(m, at(0, 0, 0));
    b.joint(JointKind::PointLineDistance, fi, fj, 3.0);
    ReferenceSession session;
    ASSERT_EQ(session.load(b.m), Status::Ok);
    Result r = session.solve({});
    ASSERT_EQ(r.status, Status::Ok) << r.message;
    const Base::Placement solved = session.poses()[m].placement;
    // (40, 30) is 50 from the axis: (40, 30) * 3 / 50, z and the turn kept.
    EXPECT_NEAR((solved.getPosition() - Base::Vector3d(2.4, 1.8, 7)).Length(), 0.0, 1e-9);
    EXPECT_TRUE(solved.getRotation().isSame(start.getRotation(), 1e-12));
}
