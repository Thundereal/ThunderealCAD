// SPDX-License-Identifier: LGPL-2.1-or-later

// The neutral model's dynamics block (docs/AssemblyDynamics.md sec 6.1):
// inertias moved between frames and added, and a bundle of bodies held by a
// Fixed joint weighing its members.

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <numbers>

#include <Base/Placement.h>
#include <Base/Quantity.h>
#include <Mod/Assembly/App/Solver/Backend.h>
#include <Mod/Assembly/App/Solver/Model.h>
#include <Mod/Assembly/App/Solver/RigidGroups.h>
#include <Mod/Assembly/App/Solver/Units.h>

using namespace Assembly::Solver;

namespace
{

constexpr double pi = std::numbers::pi;

// A box of mass m and edges a, b, c along its frame's x, y, z, its centre at com
Inertia box(Id body, double m, double a, double b, double c, const Base::Vector3d& com)
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

void expectSame(const Inertia& got, const Inertia& want)
{
    EXPECT_NEAR(got.mass, want.mass, 1e-12);
    EXPECT_NEAR(got.com.x, want.com.x, 1e-9);
    EXPECT_NEAR(got.com.y, want.com.y, 1e-9);
    EXPECT_NEAR(got.com.z, want.com.z, 1e-9);
    for (std::size_t i = 0; i < 6; ++i) {
        EXPECT_NEAR(got.inertia[i], want.inertia[i], 1e-9) << "entry " << i;
    }
}

// A session that keeps the model it is given, and does nothing else
class Keeper: public Session
{
public:
    explicit Keeper(Model& seen)
        : seen(seen)
    {}
    Status load(const Model& model) override
    {
        seen = model;
        return Status::Ok;
    }
    Result solve(const SolveOptions&) override
    {
        return {};
    }
    Status beginDrag(const std::vector<Id>&, const DragOptions&) override
    {
        return Status::Ok;
    }
    Result dragStep(const std::vector<BodyPose>&) override
    {
        return {};
    }
    Result endDrag() override
    {
        return {};
    }
    std::vector<BodyPose> poses() const override
    {
        return {};
    }
    Diagnostics analyse() override
    {
        return {};
    }

private:
    Model& seen;
};

}  // namespace

// Turned a quarter about z and moved: the centre with it, x and y swapped
TEST(DynamicsModel, movedTurnsAndShifts)  // NOLINT
{
    Inertia inertia = box(0, 2.0, 10.0, 20.0, 30.0, Base::Vector3d(1, 2, 3));
    inertia.inertia[3] = 4.0;  // a product of inertia, xy
    const Base::Placement to(Base::Vector3d(100, 0, 0),
                             Base::Rotation(Base::Vector3d(0, 0, 1), pi / 2));
    const Inertia got = moved(inertia, to);
    Inertia want = inertia;
    want.com = Base::Vector3d(98, 1, 3);
    want.inertia = {inertia.inertia[1], inertia.inertia[0], inertia.inertia[2], -4.0, 0.0, 0.0};
    expectSame(got, want);
}

// Two equal cubes 10 either side of the origin along x
TEST(DynamicsModel, addByParallelAxis)  // NOLINT
{
    const double m = 3.0;
    Inertia a = box(0, m, 10, 10, 10, Base::Vector3d(-10, 0, 0));
    const Inertia b = box(0, m, 10, 10, 10, Base::Vector3d(10, 0, 0));
    const double own = m * 200 / 12;
    add(a, b);
    Inertia want = box(0, 2 * m, 10, 10, 10, Base::Vector3d());
    want.inertia = {2 * own, 2 * (own + m * 100), 2 * (own + m * 100), 0, 0, 0};
    expectSame(a, want);
}

// The origin: assumed if either is, else overridden if either is
TEST(DynamicsModel, addKeepsTheWeakestOrigin)  // NOLINT
{
    Inertia a = box(0, 1.0, 1, 1, 1, Base::Vector3d());
    Inertia b = box(0, 1.0, 1, 1, 1, Base::Vector3d());
    b.origin = Inertia::Origin::Override;
    add(a, b);
    EXPECT_EQ(a.origin, Inertia::Origin::Override);
    b.origin = Inertia::Origin::AssumedDensity;
    add(a, b);
    EXPECT_EQ(a.origin, Inertia::Origin::AssumedDensity);
    b.origin = Inertia::Origin::Shape;
    add(a, b);
    EXPECT_EQ(a.origin, Inertia::Origin::AssumedDensity);
}

// A and B held by a Fixed joint are one body in the solve: its inertia, A's
// with B's moved into A's frame and added
TEST(DynamicsModel, fixedBundleWeighsItsMembers)  // NOLINT
{
    Model model;
    auto body = [&](const char* name, const Base::Placement& placement) {
        Body b;
        b.source = model.addSource(name);
        b.placement = placement;
        return model.addBody(b);
    };
    auto frame = [&](Id on, const Base::Placement& local) {
        Frame f;
        f.body = on;
        f.local = local;
        f.label = model.addSource("F" + std::to_string(model.frames.size()));
        return model.addFrame(f);
    };
    const Base::Placement bInA(Base::Vector3d(20, 0, 0),
                               Base::Rotation(Base::Vector3d(0, 0, 1), pi / 2));
    const Id a = body("A", Base::Placement());
    const Id b = body("B", bInA);
    Joint fixed;
    fixed.kind = JointKind::Fixed;
    fixed.source = model.addSource("Fixed");
    fixed.frameI = frame(a, bInA);
    fixed.frameJ = frame(b, Base::Placement());
    model.addJoint(fixed);
    Dynamics dynamics;
    dynamics.inertias.push_back(box(a, 1.0, 10, 10, 10, Base::Vector3d(5, 5, 5)));
    dynamics.inertias.push_back(box(b, 2.0, 10, 20, 30, Base::Vector3d(5, 10, 15)));
    dynamics.inertias[1].origin = Inertia::Origin::Override;
    model.dynamics = dynamics;

    Model seen;
    auto session = groupedRigidly(std::make_unique<Keeper>(seen));
    ASSERT_EQ(session->load(model), Status::Ok);
    ASSERT_TRUE(seen.dynamics.has_value());
    ASSERT_EQ(seen.bodies.size(), 1U);
    ASSERT_EQ(seen.dynamics->inertias.size(), 1U);
    Inertia want = dynamics.inertias[0];
    add(want, moved(dynamics.inertias[1], bInA));
    want.body = 0;
    expectSame(seen.dynamics->inertias[0], want);
    EXPECT_EQ(seen.dynamics->inertias[0].origin, Inertia::Origin::Override);
    EXPECT_NEAR(seen.dynamics->inertias[0].mass, 3.0, 1e-12);
}

// A kinematic build has no dynamics block, and a model with one is not the
// same as one without
TEST(DynamicsModel, sameNeedsTheSameBlock)  // NOLINT
{
    Model a;
    Model b;
    EXPECT_TRUE(sameExceptDragWeights(a, b));
    b.dynamics = Dynamics();
    EXPECT_FALSE(sameExceptDragWeights(a, b));
    a.dynamics = Dynamics();
    EXPECT_TRUE(sameExceptDragWeights(a, b));
    a.dynamics->inertias.push_back(box(0, 1.0, 1, 1, 1, Base::Vector3d()));
    EXPECT_FALSE(sameExceptDragWeights(a, b));
}

// The unit contract (sec 8.1), against FreeCAD's units: one SI unit, parsed,
// is the factor's inverse in internal units
TEST(DynamicsModel, unitFactorsAreFreeCADs)  // NOLINT
{
    auto internal = [](const char* text) {
        return Base::Quantity::parse(text).getValue();
    };
    const struct
    {
        const char* unit;
        double factor;
    } checks[] = {
        {"1 m", SI::length},
        {"1 kg", SI::mass},
        {"1 s", SI::time},
        {"1 m/s", SI::velocity},
        {"1 m/s^2", SI::acceleration},
        {"1 N", SI::force},
        {"1 N*m", SI::torque},
        {"1 J", SI::energy},
        {"1 W", SI::power},
        {"1 N/m", SI::linearStiffness},
        {"1 N*s/m", SI::linearDamping},
        {"1 kg*m^2", SI::inertia},
        {"1 kg/m^3", SI::density},
        {"1 Pa", SI::pressure},
    };
    for (const auto& check : checks) {
        EXPECT_NEAR(internal(check.unit) * check.factor, 1.0, 1e-12) << check.unit;
    }
    // The torsional ones: a torque per radian, a torque per radian per second
    EXPECT_NEAR(internal("1 N*m") * SI::torsionalStiffness, 1.0, 1e-12);
    EXPECT_NEAR(internal("1 N*m*s") * SI::torsionalDamping, 1.0, 1e-12);
    // The impact stiffness: N/m for an exponent of 1, N/m^2 for 2, and the
    // 1.5 of a Hertzian contact between the two
    EXPECT_NEAR(internal("1 N/m") * SI::impactStiffness(1.0), 1.0, 1e-12);
    EXPECT_NEAR(internal("1 N/m^2") * SI::impactStiffness(2.0), 1.0, 1e-12);
    EXPECT_NEAR(SI::impactStiffness(1.5), 1e-3 / std::pow(1e-3, 1.5), 1e-6);
    EXPECT_NEAR(SI::impactStiffness(1.5), std::sqrt(1e3), 1e-9);
    EXPECT_NEAR(internal("1 N*s/m") * SI::impactDamping, 1.0, 1e-12);
}
