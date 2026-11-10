// SPDX-License-Identifier: LGPL-2.1-or-later

// The Chrono backend's flexible bodies (docs/AssemblyDynamics.md, D4b): a
// steel bar of 10-node tetrahedra held at one end, reduced as a dynamic run
// reduces it (Craig-Bampton, the interface held), its modes against the
// clamped beam's.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <string>
#include <utility>

#include <Mod/Assembly/App/Solver/ChronoBackend.h>
#include <src/App/InitApplication.h>

using namespace Assembly::Solver;

namespace
{

constexpr double pi = std::numbers::pi;

// A box [0, lx] x [0, ly] x [0, lz] of nx * ny * nz cells, each cut into six
// tetrahedra about its diagonal (Kuhn's, conforming across the cells), with
// the nodes of their edges
struct BoxMesh
{
    std::vector<Base::Vector3d> nodes;
    std::vector<std::array<Id, 10>> tetrahedra;

    BoxMesh(double lx, double ly, double lz, int nx, int ny, int nz)
    {
        auto corner = [&](int i, int j, int k) {
            return static_cast<Id>((i * (ny + 1) + j) * (nz + 1) + k);
        };
        for (int i = 0; i <= nx; ++i) {
            for (int j = 0; j <= ny; ++j) {
                for (int k = 0; k <= nz; ++k) {
                    nodes.emplace_back(lx * i / nx, ly * j / ny, lz * k / nz);
                }
            }
        }
        std::map<std::pair<Id, Id>, Id> mids;
        auto mid = [&](Id a, Id b) {
            const std::pair<Id, Id> key(std::min(a, b), std::max(a, b));
            auto it = mids.find(key);
            if (it != mids.end()) {
                return it->second;
            }
            nodes.push_back((nodes[a] + nodes[b]) / 2.0);
            const Id n = static_cast<Id>(nodes.size() - 1);
            mids.emplace(key, n);
            return n;
        };
        const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        for (int i = 0; i < nx; ++i) {
            for (int j = 0; j < ny; ++j) {
                for (int k = 0; k < nz; ++k) {
                    for (const auto& p : perms) {
                        int at[3] = {0, 0, 0};
                        std::array<Id, 4> c {};
                        c[0] = corner(i, j, k);
                        for (int s = 0; s < 3; ++s) {
                            at[p[s]] = 1;
                            c[s + 1] = corner(i + at[0], j + at[1], k + at[2]);
                        }
                        const Base::Vector3d a = nodes[c[1]] - nodes[c[0]];
                        const Base::Vector3d b = nodes[c[2]] - nodes[c[0]];
                        const Base::Vector3d d = nodes[c[3]] - nodes[c[0]];
                        if (a.Dot(b.Cross(d)) < 0) {
                            std::swap(c[1], c[2]);
                        }
                        tetrahedra.push_back({c[0], c[1], c[2], c[3], mid(c[0], c[1]),
                                              mid(c[1], c[2]), mid(c[2], c[0]), mid(c[0], c[3]),
                                              mid(c[1], c[3]), mid(c[2], c[3])});
                    }
                }
            }
        }
    }
};

// A steel body of the box's mesh, its interfaces to come
Flexible steel(const BoxMesh& box, Id body, Id source, int modes)
{
    Flexible flexible;
    flexible.source = source;
    flexible.body = body;
    flexible.nodes = box.nodes;
    flexible.tetrahedra = box.tetrahedra;
    flexible.youngs = 2.1e8;     // mN/mm^2, 210 GPa
    flexible.poisson = 0.3;
    flexible.density = 7.85e-6;  // kg/mm^3
    flexible.modes = modes;
    return flexible;
}

// The box's nodes on its face at x
std::vector<Id> faceAt(const BoxMesh& box, double x)
{
    std::vector<Id> nodes;
    for (Id n = 0; n < box.nodes.size(); ++n) {
        if (std::abs(box.nodes[n].x - x) < 1e-9) {
            nodes.push_back(n);
        }
    }
    return nodes;
}

struct RunRecorder: TrajectorySink
{
    std::vector<double> times;
    std::vector<std::vector<BodyPose>> frames;
    std::vector<Channel> seen;
    std::vector<std::vector<double>> rows;
    bool frame(double time, const std::vector<BodyPose>& poses) override
    {
        times.push_back(time);
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
    std::vector<ModeShapes> shapes;
    void modeShapes(const ModeShapes& s) override
    {
        shapes.push_back(s);
    }
    // The offset in a row of the channel of that quantity
    std::size_t offsetOf(const std::string& quantity) const
    {
        std::size_t at = 0;
        for (const Channel& channel : seen) {
            if (channel.quantity == quantity) {
                return at;
            }
            at += static_cast<std::size_t>(channel.width);
        }
        return static_cast<std::size_t>(-1);
    }
};

class FlexibleBody: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

}  // namespace

// A steel bar 200 x 10 x 10 mm held at its end x = 0: the clamped beam's
// bending, f = (beta L)^2 / (2 pi L^2) sqrt(E I / rho A), beta L 1.8751 and
// 4.6941, the same about both axes of the square section
TEST_F(FlexibleBody, cantileverModesAreTheClampedBeams)  // NOLINT
{
    if (!Chrono::hasFlexibleBodies()) {
        GTEST_SKIP() << "the Chrono backend has no modal module";
    }
    const double length = 200.0;
    const double side = 10.0;
    const BoxMesh box(length, side, side, 20, 2, 2);

    Model model;
    Body body;
    body.source = model.addSource("Bar");
    const Id bar = model.addBody(body);
    Frame end;
    end.body = bar;
    end.local = Base::Placement(Base::Vector3d(0, side / 2, side / 2), Base::Rotation());
    end.label = model.addSource("End");
    const Id frame = model.addFrame(end);

    Flexible flexible;
    flexible.source = model.addSource("Flexible");
    flexible.body = bar;
    flexible.nodes = box.nodes;
    flexible.tetrahedra = box.tetrahedra;
    flexible.youngs = 2.1e8;     // mN/mm^2, 210 GPa
    flexible.poisson = 0.3;
    flexible.density = 7.85e-6;  // kg/mm^3
    flexible.modes = 4;
    Flexible::Interface held;
    held.frame = frame;
    for (Id n = 0; n < box.nodes.size(); ++n) {
        if (box.nodes[n].x == 0.0) {
            held.nodes.push_back(n);
        }
    }
    flexible.interfaces.push_back(held);
    model.dynamics.emplace();
    model.dynamics->flexibles.push_back(flexible);

    std::string error;
    const std::vector<double> hz = Chrono::flexibleFrequencies(model, 0, &error);
    ASSERT_EQ(hz.size(), 4u) << error;

    // SI
    const double e = 2.1e11;
    const double rhoA = 7850.0 * 1e-4;
    const double inertia = std::pow(0.01, 4) / 12.0;
    const double l = 0.2;
    auto beam = [&](double betaL) {
        return betaL * betaL / (2.0 * pi * l * l) * std::sqrt(e * inertia / rhoA);
    };
    const double first = beam(1.8751);
    const double second = beam(4.6941);
    // 209.6 and 1299.2 Hz against 208.9 and 1309: the second lower, by shear
    EXPECT_NEAR(hz[0], first, 0.01 * first);
    EXPECT_NEAR(hz[1], first, 0.01 * first);
    EXPECT_NEAR(hz[2], second, 0.02 * second);
    EXPECT_NEAR(hz[3], second, 0.02 * second);

    // The same with every other tetrahedron's corners turning the other way,
    // as Netgen's do
    for (std::size_t i = 0; i < model.dynamics->flexibles[0].tetrahedra.size(); i += 2) {
        auto& t = model.dynamics->flexibles[0].tetrahedra[i];
        t = {t[0], t[2], t[1], t[3], t[6], t[5], t[4], t[7], t[9], t[8]};
    }
    const std::vector<double> turned = Chrono::flexibleFrequencies(model, 0, &error);
    ASSERT_EQ(turned.size(), 4u) << error;
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(turned[i], hz[i], 1e-9 * hz[i]);
    }
}

// The modes a reduction found are taken by the next of the same part, and
// not by a part changed: the bar of the test above, twice as stiff, makes
// its own, its frequencies sqrt(2) times. A part moved and turned takes
// them too: its floating frame's axes are its own, not its principal axes
// of inertia (the square section's two as the rounding had them). Reduced
// again, or moved, its whole reduction is kept; damped more, only its modes
TEST_F(FlexibleBody, aPartsModesAreKeptForItsNextReduction)  // NOLINT
{
    if (!Chrono::hasFlexibleBodies()) {
        GTEST_SKIP() << "the Chrono backend has no modal module";
    }
    const double side = 10.0;
    const BoxMesh box(200.0, side, side, 20, 2, 2);
    Model model;
    Body body;
    body.source = model.addSource("Bar");
    const Id bar = model.addBody(body);
    Frame end;
    end.body = bar;
    end.local = Base::Placement(Base::Vector3d(0, side / 2, side / 2), Base::Rotation());
    end.label = model.addSource("End");
    const Id frame = model.addFrame(end);
    Flexible flexible = steel(box, bar, model.addSource("Flexible"), 5);
    flexible.interfaces.push_back({frame, faceAt(box, 0.0)});
    model.dynamics.emplace();
    model.dynamics->flexibles.push_back(flexible);

    std::string error;
    const std::vector<double> hz = Chrono::flexibleFrequencies(model, 0, &error);
    ASSERT_EQ(hz.size(), 5u) << error;
    const std::size_t kept = Chrono::flexibleModesKept();
    const std::vector<double> again = Chrono::flexibleFrequencies(model, 0, &error);
    ASSERT_EQ(again.size(), 5u) << error;
    EXPECT_EQ(Chrono::flexibleModesKept(), kept + 1);
    EXPECT_EQ(again, hz);

    model.bodies[bar].placement = Base::Placement(Base::Vector3d(30, -40, 70),
                                                  Base::Rotation(Base::Vector3d(1, 2, 3), 0.7));
    // its interface's frame and nodes on it as a turned part's rounding finds
    // them, an ulp off
    model.frames[frame].local.setPosition(Base::Vector3d(1e-12, side / 2, side / 2));
    model.dynamics->flexibles[0].nodes[1].x += 1e-12;
    const std::vector<double> moved = Chrono::flexibleFrequencies(model, 0, &error);
    ASSERT_EQ(moved.size(), 5u) << error;
    EXPECT_EQ(Chrono::flexibleModesKept(), kept + 2);
    for (std::size_t i = 0; i < hz.size(); ++i) {
        EXPECT_NEAR(moved[i], hz[i], 1e-9 * hz[i]);
    }

    // Damped more: a reduction of its own, its modes the kept ones
    model.dynamics->flexibles[0].damping *= 2.0;
    const std::vector<double> damped = Chrono::flexibleFrequencies(model, 0, &error);
    ASSERT_EQ(damped.size(), 5u) << error;
    EXPECT_EQ(Chrono::flexibleModesKept(), kept + 3);
    EXPECT_EQ(damped, hz);
    model.dynamics->flexibles[0].damping /= 2.0;

    const std::size_t before = Chrono::flexibleModesKept();
    model.dynamics->flexibles[0].youngs *= 2.0;
    const std::vector<double> stiffer = Chrono::flexibleFrequencies(model, 0, &error);
    ASSERT_EQ(stiffer.size(), 5u) << error;
    EXPECT_EQ(Chrono::flexibleModesKept(), before);
    for (std::size_t i = 0; i < hz.size(); ++i) {
        EXPECT_NEAR(stiffer[i], std::sqrt(2.0) * hz[i], 1e-6 * hz[i]);
    }
}

namespace
{

// The bar of the test above as a dynamic run's: fixed to the ground at its
// end x = 0, a block of 'tip' kg fixed to its end x = 200, gravity along -z;
// damped at 'damping' of critical at 'hz'
Model cantilever(double tip, double damping, double hz)
{
    const BoxMesh box(200.0, 10.0, 10.0, 20, 2, 2);
    Model m;
    Frame onGround;
    onGround.label = m.addSource("marker-G");
    const Id groundMarker = m.addFrame(onGround);
    Body ground;
    ground.source = m.addSource("G");
    ground.grounded = true;
    const Id g = m.addBody(ground);
    Frame fix;
    fix.body = g;
    fix.label = m.addSource("FixingMarker");
    const Id fixFrame = m.addFrame(fix);
    m.addGround({NoId, m.addSource("G"), groundMarker, fixFrame});

    Body barBody;
    barBody.source = m.addSource("Bar");
    const Id bar = m.addBody(barBody);
    Body tipBody;
    tipBody.source = m.addSource("Tip");
    tipBody.placement = Base::Placement(Base::Vector3d(200, 5, 5), Base::Rotation());
    const Id block = m.addBody(tipBody);

    auto frame = [&](Id body, const Base::Vector3d& at, const char* label) {
        Frame f;
        f.body = body;
        f.local = Base::Placement(at, Base::Rotation());
        f.label = m.addSource(label);
        return m.addFrame(f);
    };
    auto fixed = [&](Id fi, Id fj, const char* name) {
        Joint j;
        j.source = m.addSource(name);
        j.kind = JointKind::Fixed;
        j.frameI = fi;
        j.frameJ = fj;
        m.addJoint(j);
    };
    const Id root = frame(bar, Base::Vector3d(0, 5, 5), "Root");
    fixed(frame(g, Base::Vector3d(0, 5, 5), "Wall"), root, "Clamp");
    const Id end = frame(bar, Base::Vector3d(200, 5, 5), "End");
    fixed(end, frame(block, Base::Vector3d(0, 0, 0), "Mount"), "Weld");

    Dynamics dynamics;
    dynamics.gravity = Base::Vector3d(0, 0, -9806.65);
    // The bar's rigid inertia, its centre the mesh's: a flexible body's mass
    // is its mesh's, the record placing its frame at the centre
    Inertia barInertia;
    barInertia.body = bar;
    barInertia.mass = 0.157;
    barInertia.com = Base::Vector3d(100, 5, 5);
    barInertia.inertia = {0.157 * 200 / 12, 0.157 * 40000 / 12, 0.157 * 40000 / 12, 0, 0, 0};
    dynamics.inertias.push_back(barInertia);
    Inertia tipInertia;
    tipInertia.body = block;
    tipInertia.mass = tip;
    tipInertia.inertia = {tip * 8 / 12, tip * 8 / 12, tip * 8 / 12, 0, 0, 0};
    dynamics.inertias.push_back(tipInertia);

    Flexible flexible = steel(box, bar, m.addSource("Flexible"), 6);
    flexible.damping = damping;
    flexible.dampingFrequency = hz;
    flexible.interfaces.push_back({root, faceAt(box, 0.0)});
    flexible.interfaces.push_back({end, faceAt(box, 200.0)});
    dynamics.flexibles.push_back(flexible);
    m.dynamics = dynamics;
    return m;
}

Status run(const Model& m, RunRecorder& out, double end, double step)
{
    SimulationSpec spec;
    spec.analysis = Analysis::Dynamic;
    spec.tEnd = end;
    spec.tOutput = step;
    spec.maxStep = step;
    ChronoSession session;
    if (session.load(m) != Status::Ok) {
        return Status::Failed;
    }
    return session.simulate(spec, out);
}

}  // namespace

// The bar as a flexible body in a dynamic run: held at one end, a gram at
// the other, under gravity. It rings at the cantilever's frequency with
// its tip mass, 1 + cos b cosh b + mu b (cos b sinh b - sin b cosh b) = 0,
// decaying at the ratio given at that frequency; damped hard, it settles
// where the beam does, q L^4 / 8 E I + P L^3 / 3 E I, 8.95 um. And the run
// reports its modal coordinates.
TEST_F(FlexibleBody, cantileverBendsAndRingsInARun)  // NOLINT
{
    if (!Chrono::hasFlexibleBodies()) {
        GTEST_SKIP() << "the Chrono backend has no modal module";
    }
    const double tip = 1e-3;
    const double e = 2.1e11;
    const double inertia = std::pow(0.01, 4) / 12.0;
    const double l = 0.2;
    const double rhoA = 7850.0 * 1e-4;
    const double g = 9.80665;
    const double sag = (rhoA * g * std::pow(l, 4) / (8 * e * inertia)
                        + tip * g * std::pow(l, 3) / (3 * e * inertia))
        * 1e3;  // mm

    // The tip mass' frequency equation, by bisection
    const double mu = tip / (rhoA * l);
    auto equation = [mu](double b) {
        return 1 + std::cos(b) * std::cosh(b)
            + mu * b * (std::cos(b) * std::sinh(b) - std::sin(b) * std::cosh(b));
    };
    double lo = 1.0;
    double hi = 1.8751;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        (equation(lo) * equation(mid) <= 0 ? hi : lo) = mid;
    }
    const double b = 0.5 * (lo + hi);
    const double expected = b * b / (2 * pi * l * l) * std::sqrt(e * inertia / rhoA);

    RunRecorder settled;
    ASSERT_EQ(run(cantilever(tip, 0.5, expected), settled, 0.05, 1e-4), Status::Ok);
    ASSERT_FALSE(settled.frames.empty());
    const double rest = settled.frames.back()[2].placement.getPosition().z - 5.0;
    // 8.87 um against 8.95: the mesh a little stiffer, as its modes say
    EXPECT_NEAR(rest, -sag, 0.02 * sag) << "sag " << sag << " mm";
    // Its modal coordinates, of the six modes, after its two interfaces'
    // deformation; then its inertia loads, the twelve residual modes'
    bool found = false;
    bool loads = false;
    for (const Channel& channel : settled.seen) {
        if (channel.quantity == "ModalCoordinates") {
            found = true;
            EXPECT_EQ(channel.source, "Flexible");
            EXPECT_EQ(channel.width, 6);
        }
        if (channel.quantity == "InterfaceDeformation") {
            EXPECT_EQ(channel.width, 12);
        }
        if (channel.quantity == "InertialLoad") {
            loads = true;
            EXPECT_EQ(channel.width, 12);
        }
    }
    EXPECT_TRUE(found);
    EXPECT_TRUE(loads);
    // Its mode shapes: the node at the middle of its end face, made of them
    // and the coordinates, where the gram welded to that face is
    ASSERT_EQ(settled.shapes.size(), 1u);
    const ModeShapes& shapes = settled.shapes[0];
    EXPECT_EQ(shapes.source, "Flexible");
    ASSERT_EQ(shapes.columns, 30u);
    const std::size_t from = settled.offsetOf("InterfaceDeformation");
    ASSERT_EQ(settled.offsetOf("ModalCoordinates"), from + 12);
    ASSERT_EQ(settled.offsetOf("InertialLoad"), from + 18);
    // At rest, held still: the gravity's alone, in SI
    EXPECT_NEAR(settled.rows.back()[from + 18 + 2], -g, 1e-3 * g);
    // A node on the bar's axis at x, where the run leaves it
    auto nodeAt = [&](int i) {
        const std::size_t node = (i * 3 + 1) * 3 + 1;  // (10 i, 5, 5)
        Base::Vector3d at(10.0 * i, 5, 5);
        for (int k = 0; k < 3; ++k) {
            double moved = 0.0;
            for (std::size_t c = 0; c < shapes.columns; ++c) {
                moved += shapes.values[(3 * node + k) * shapes.columns + c]
                    * settled.rows.back()[from + c];
            }
            at[k] += moved;
        }
        Base::Vector3d placed;
        settled.frames.back()[1].placement.multVec(at, placed);
        return placed;
    };
    const Base::Vector3d end = nodeAt(20);
    const Base::Vector3d mount = settled.frames.back()[2].placement.getPosition();
    EXPECT_LT((end - mount).Length(), 1e-3 * sag) << "the node " << end.z << ", the gram "
                                                   << mount.z;
    // Mid-span, the beam's q x^2 (6 L^2 - 4 L x + x^2) / 24 E I + P x^2 (3 L
    // - x) / 6 E I
    const double x = l / 2;
    const double mid = (rhoA * g * x * x * (6 * l * l - 4 * l * x + x * x) / (24 * e * inertia)
                        + tip * g * x * x * (3 * l - x) / (6 * e * inertia))
        * 1e3;
    EXPECT_NEAR(nodeAt(10).z - 5.0, -mid, 0.02 * mid);

    const double ratio = 0.02;
    RunRecorder ringing;
    ASSERT_EQ(run(cantilever(tip, ratio, expected), ringing, 0.1, 1e-4), Status::Ok);
    // Its crossings of where it settles, upward, and how far it swings
    // below it between them
    std::vector<double> up;
    std::vector<double> swing;
    double deepest = 0.0;
    for (std::size_t f = 1; f < ringing.frames.size(); ++f) {
        const double z0 = ringing.frames[f - 1][2].placement.getPosition().z - 5.0 - rest;
        const double z1 = ringing.frames[f][2].placement.getPosition().z - 5.0 - rest;
        deepest = std::max(deepest, -z1);
        if (z0 < 0 && z1 >= 0) {
            const double t0 = ringing.times[f - 1];
            const double t1 = ringing.times[f];
            up.push_back(t0 + (t1 - t0) * (-z0) / (z1 - z0));
            swing.push_back(deepest);
            deepest = 0.0;
        }
    }
    ASSERT_GE(up.size(), 10u);
    const double measured = (up.size() - 1) / (up.back() - up.front());
    EXPECT_NEAR(measured, expected, 0.005 * expected) << "expected " << expected << " Hz";
    // The logarithmic decrement over the swings, its first from rest
    const double decrement = std::log(swing[1] / swing.back()) / (swing.size() - 2);
    const double decay = decrement / std::sqrt(4 * pi * pi + decrement * decrement);
    // 206.60 Hz against 206.27, 0.0200 of critical
    EXPECT_NEAR(decay, ratio, 0.05 * ratio);
}

namespace
{

// The bar hinged at its end x = 0 about y, let go level under gravity
// along -z: as a flexible body, or rigid with its inertia
Model pendulum(bool flexible)
{
    const BoxMesh box(200.0, 10.0, 10.0, 20, 2, 2);
    Model m;
    Frame onGround;
    onGround.label = m.addSource("marker-G");
    const Id groundMarker = m.addFrame(onGround);
    Body ground;
    ground.source = m.addSource("G");
    ground.grounded = true;
    const Id g = m.addBody(ground);
    Frame fix;
    fix.body = g;
    fix.label = m.addSource("FixingMarker");
    const Id fixFrame = m.addFrame(fix);
    m.addGround({NoId, m.addSource("G"), groundMarker, fixFrame});
    Body barBody;
    barBody.source = m.addSource("Bar");
    const Id bar = m.addBody(barBody);

    // z along y
    const Base::Rotation axis(Base::Vector3d(1, 0, 0), -pi / 2);
    auto frame = [&](Id body, const char* label) {
        Frame f;
        f.body = body;
        f.local = Base::Placement(Base::Vector3d(0, 5, 5), axis);
        f.label = m.addSource(label);
        return m.addFrame(f);
    };
    const Id hingeOnBar = frame(bar, "Hinge");
    Joint hinge;
    hinge.source = m.addSource("Pivot");
    hinge.kind = JointKind::Revolute;
    hinge.frameI = frame(g, "Post");
    hinge.frameJ = hingeOnBar;
    m.addJoint(hinge);

    Dynamics dynamics;
    dynamics.gravity = Base::Vector3d(0, 0, -9806.65);
    const double mass = 7.85e-6 * 200 * 10 * 10;
    Inertia barInertia;
    barInertia.body = bar;
    barInertia.mass = mass;
    barInertia.com = Base::Vector3d(100, 5, 5);
    barInertia.inertia = {mass * 200 / 12, mass * 40100 / 12, mass * 40100 / 12, 0, 0, 0};
    dynamics.inertias.push_back(barInertia);
    if (flexible) {
        Flexible record = steel(box, bar, m.addSource("Flexible"), 6);
        record.interfaces.push_back({hingeOnBar, faceAt(box, 0.0)});
        dynamics.flexibles.push_back(record);
    }
    m.dynamics = dynamics;
    return m;
}

}  // namespace

// A flexible pendulum, the bar hinged at one end and let go level: its
// floating frame turned through the swing, the steel bar bending some
// microns, it swings as the rigid bar does
TEST_F(FlexibleBody, pendulumSwingsAsTheRigidBar)  // NOLINT
{
    if (!Chrono::hasFlexibleBodies()) {
        GTEST_SKIP() << "the Chrono backend has no modal module";
    }
    RunRecorder rigid;
    RunRecorder flexible;
    ASSERT_EQ(run(pendulum(false), rigid, 0.2, 1e-4), Status::Ok);
    ASSERT_EQ(run(pendulum(true), flexible, 0.2, 1e-4), Status::Ok);
    ASSERT_EQ(rigid.frames.size(), flexible.frames.size());
    auto centre = [](const RunRecorder& r, std::size_t f) {
        Base::Vector3d c;
        r.frames[f][1].placement.multVec(Base::Vector3d(100, 5, 5), c);
        return c;
    };
    double apart = 0.0;
    for (std::size_t f = 0; f < rigid.frames.size(); ++f) {
        apart = std::max(apart, (centre(rigid, f) - centre(flexible, f)).Length());
    }
    // It swung: its centre below the hinge by the end
    const Base::Vector3d end = centre(rigid, rigid.frames.size() - 1);
    EXPECT_LT(end.z, -60.0);
    // 2.4 um apart, the centre 93 mm down by the end
    EXPECT_LT(apart, 0.01) << "mm";
}

// The pendulum run again: its flexible bar's whole reduction is the first
// run's, kept (its matrices, modes and static modes not made again), and it
// swings the same -- each frame, each channel, the mode shapes
TEST_F(FlexibleBody, aRunAgainTakesTheKeptReduction)  // NOLINT
{
    if (!Chrono::hasFlexibleBodies()) {
        GTEST_SKIP() << "the Chrono backend has no modal module";
    }
    RunRecorder first;
    ASSERT_EQ(run(pendulum(true), first, 0.05, 1e-4), Status::Ok);
    const std::size_t kept = Chrono::flexibleModesKept();
    RunRecorder again;
    ASSERT_EQ(run(pendulum(true), again, 0.05, 1e-4), Status::Ok);
    EXPECT_EQ(Chrono::flexibleModesKept(), kept + 1);
    ASSERT_EQ(first.frames.size(), again.frames.size());
    ASSERT_EQ(first.rows.size(), again.rows.size());
    double apart = 0.0;
    for (std::size_t f = 0; f < first.frames.size(); ++f) {
        for (std::size_t b = 0; b < first.frames[f].size(); ++b) {
            apart = std::max(apart,
                             (first.frames[f][b].placement.getPosition()
                              - again.frames[f][b].placement.getPosition())
                                 .Length());
        }
        ASSERT_EQ(first.rows[f].size(), again.rows[f].size());
        for (std::size_t c = 0; c < first.rows[f].size(); ++c) {
            EXPECT_NEAR(again.rows[f][c], first.rows[f][c], 1e-9 * (1 + std::abs(first.rows[f][c])))
                << "frame " << f << ", column " << c;
        }
    }
    EXPECT_LT(apart, 1e-9) << "mm";
    ASSERT_EQ(first.shapes.size(), 1u);
    ASSERT_EQ(again.shapes.size(), 1u);
    EXPECT_EQ(again.shapes[0].values, first.shapes[0].values);
}
