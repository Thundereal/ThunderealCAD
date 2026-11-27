// SPDX-License-Identifier: LGPL-2.1-or-later

// The fluid workbench's solver-neutral model, its registry, the backend of
// test data and the results' archive (docs/FluidSimulation.md secs 11, 12).

#include <numbers>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>

#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Mod/Fluid/App/ResultsArchive.h>
#include <Mod/Fluid/App/Solver/FreeFallBackend.h>
#include <Mod/Fluid/App/Solver/Registry.h>

using namespace Fluid::Solver;

namespace
{

// A box as twelve triangles, each face split along a diagonal.
Surface boxSurface(const Vec3& min, const Vec3& max)
{
    Surface s;
    for (int i = 0; i < 8; ++i) {
        s.points.push_back({(i & 1) ? max[0] : min[0],
                            (i & 2) ? max[1] : min[1],
                            (i & 4) ? max[2] : min[2]});
    }
    s.triangles = {{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}, {0, 1, 5}, {0, 5, 4},
                   {2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    return s;
}

// A flat rectangle in the plane x = at, y from y0 to y1 and z from z0 to z1,
// its two triangles facing +x.
Surface faceSurface(double at, double y0, double y1, double z0, double z1)
{
    Surface s;
    s.points = {{at, y0, z0}, {at, y1, z0}, {at, y1, z1}, {at, y0, z1}};
    s.triangles = {{0, 1, 2}, {0, 2, 3}};
    return s;
}

// A jet in the open, no gravity: a face 0.04 x 0.04 at x = 0 sending water
// along +x at 2 m/s, in a space 0.2 long open all round.
Model jetModel()
{
    Model model;
    Inlet inlet;
    inlet.source = "Doc#Inlet";
    inlet.surface = faceSurface(0.0, -0.02, 0.02, -0.02, 0.02);
    inlet.direction = {1.0, 0.0, 0.0};
    inlet.speed = 2.0;
    model.inlets.push_back(inlet);
    model.space.min = {0.0, -0.1, -0.1};
    model.space.max = {0.2, 0.1, 0.1};
    model.space.open = {true, true, true, true, true, true};
    model.gravity = {0.0, 0.0, 0.0};
    model.resolution = 0.01;
    model.timeEnd = 0.1;
    model.outputInterval = 0.01;
    return model;
}

// A block of water 0.1 x 0.1 x 0.2 standing 0.3 above the floor of a space
// 0.4 x 0.2 x 0.5.
Model blockModel()
{
    Model model;
    model.fluids.push_back({"Doc#Water", boxSurface({0.0, 0.05, 0.3}, {0.1, 0.15, 0.5}), {}});
    model.space.min = {0.0, 0.0, 0.0};
    model.space.max = {0.4, 0.2, 0.5};
    model.resolution = 0.02;
    model.timeEnd = 0.4;
    model.outputInterval = 0.05;
    model.monitors.push_back({"Doc#Height", MonitorKind::Height, {0.05, 0.1, 0.0}});
    return model;
}

struct Kept: public FrameSink
{
    void frame(const Frame& frame) override
    {
        frames.push_back(frame);
    }
    void sample(double time, const std::vector<double>& values) override
    {
        times.push_back(time);
        samples.push_back(values);
    }
    bool cancelled() const override
    {
        return stopAfter >= 0 && static_cast<int>(frames.size()) >= stopAfter;
    }
    std::vector<Frame> frames;
    std::vector<double> times;
    std::vector<std::vector<double>> samples;
    int stopAfter = -1;
};

class NamedBackend: public Backend
{
public:
    NamedBackend(std::string n, bool test, bool here, int tag = 0, bool steady = false)
        : n(std::move(n))
        , test(test)
        , here(here)
        , tag(tag)
        , steady(steady)
    {}
    std::string name() const override
    {
        return n;
    }
    std::string label() const override
    {
        return n + std::to_string(tag);
    }
    Capabilities capabilities() const override
    {
        Capabilities can;
        can.testData = test;
        can.steady = steady;
        return can;
    }
    Availability availability() const override
    {
        return {here, here ? "" : "not installed", "1"};
    }
    std::unique_ptr<Session> createSession() const override
    {
        return nullptr;
    }

private:
    std::string n;
    bool test;
    bool here;
    int tag;
    bool steady;
};

}  // namespace

TEST(FluidModel, insideAClosedSurface)
{
    const Surface box = boxSurface({0.0, 0.0, 0.0}, {1.0, 2.0, 3.0});
    EXPECT_TRUE(inside(box, {0.5, 1.0, 1.5}));
    // on the faces' diagonals, where a line along z meets an edge
    EXPECT_TRUE(inside(box, {0.5, 1.0, 0.1}));
    EXPECT_TRUE(inside(box, {0.25, 0.5, 2.9}));
    EXPECT_FALSE(inside(box, {0.5, 1.0, 3.5}));
    EXPECT_FALSE(inside(box, {0.5, 1.0, -0.5}));
    EXPECT_FALSE(inside(box, {1.5, 1.0, 1.5}));
    EXPECT_FALSE(inside(box, {0.5, -0.1, 1.5}));
    EXPECT_FALSE(inside(Surface {}, {0.0, 0.0, 0.0}));
}

TEST(FluidModel, sampledFillsABoxExactly)
{
    const auto points = sampled(boxSurface({0.0, 0.05, 0.3}, {0.1, 0.15, 0.5}), 0.02);
    ASSERT_EQ(points.size(), 5U * 5U * 10U);
    Vec3 low = points.front();
    Vec3 high = points.front();
    for (const auto& p : points) {
        for (std::size_t i = 0; i < 3; ++i) {
            low[i] = std::min(low[i], p[i]);
            high[i] = std::max(high[i], p[i]);
        }
    }
    EXPECT_NEAR(low[0], 0.01, 1e-12);
    EXPECT_NEAR(low[1], 0.06, 1e-12);
    EXPECT_NEAR(low[2], 0.31, 1e-12);
    EXPECT_NEAR(high[0], 0.09, 1e-12);
    EXPECT_NEAR(high[1], 0.14, 1e-12);
    EXPECT_NEAR(high[2], 0.49, 1e-12);
    // a spacing the box is no whole number of: 0.1 / 0.03 rounds to 3
    EXPECT_EQ(sampled(boxSurface({0.0, 0.0, 0.0}, {0.1, 0.1, 0.1}), 0.03).size(), 27U);
    EXPECT_TRUE(sampled(boxSurface({0.0, 0.0, 0.0}, {0.1, 0.1, 0.1}), 0.0).empty());
}

TEST(FluidModel, sampledLeavesACavityOut)
{
    // a box with a closed cavity in its middle: a line through it crosses
    // the surface four times, and what is between the second and the third
    // is outside
    Surface s = boxSurface({0.0, 0.0, 0.0}, {0.3, 0.3, 0.3});
    const Surface cavity = boxSurface({0.1, 0.1, 0.1}, {0.2, 0.2, 0.2});
    const int offset = static_cast<int>(s.points.size());
    s.points.insert(s.points.end(), cavity.points.begin(), cavity.points.end());
    for (auto t : cavity.triangles) {
        s.triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
    }
    EXPECT_EQ(sampled(s, 0.1).size(), 26U);
    EXPECT_FALSE(inside(s, {0.15, 0.15, 0.15}));
    EXPECT_TRUE(inside(s, {0.15, 0.15, 0.05}));
}

TEST(FluidModel, signatureFollowsTheModel)
{
    const Model model = blockModel();
    const auto base = signature(model);
    EXPECT_EQ(base, signature(blockModel()));
    Model other = model;
    other.resolution = 0.01;
    EXPECT_NE(base, signature(other));
    other = model;
    other.fluids[0].surface.points[3][2] += 1e-9;
    EXPECT_NE(base, signature(other));
    other = model;
    other.fluids[0].material.density = 900.0;
    EXPECT_NE(base, signature(other));
    other = model;
    other.monitors[0].kind = MonitorKind::Pressure;
    EXPECT_NE(base, signature(other));
    other = model;
    other.space.open[5] = true;
    EXPECT_NE(base, signature(other));
    other = model;
    other.gravity[2] = -0.0;
    Model zero = model;
    zero.gravity[2] = 0.0;
    EXPECT_EQ(signature(other), signature(zero));
}

TEST(FluidModel, wallsTheFluidMoves)
{
    Model model = blockModel();
    model.walls.push_back({"Doc#Floor", boxSurface({0.0, 0.0, -0.01}, {0.4, 0.2, 0.0}), {}, {}});
    model.walls.push_back({"Doc#Float", boxSurface({0.2, 0.05, 0.1}, {0.3, 0.15, 0.2}), {}, {}});
    EXPECT_TRUE(freeWalls(model).empty());
    const auto fixed = signature(model);
    model.walls[1].motion = WallMotion::Free;
    model.walls[1].mass = {0.5, {0.25, 0.1, 0.15}, {1e-3, 1e-3, 1e-3, 0.0, 0.0, 0.0}};
    ASSERT_EQ(freeWalls(model).size(), 1U);
    EXPECT_EQ(freeWalls(model)[0], 1U);
    // what it weighs is of the model: a kept result is of that weight
    const auto free = signature(model);
    EXPECT_NE(fixed, free);
    Model other = model;
    other.walls[1].mass.mass = 0.6;
    EXPECT_NE(free, signature(other));
    other = model;
    other.walls[1].mass.inertia[3] = 1e-4;
    EXPECT_NE(free, signature(other));
    // a fixed wall's is nothing to it
    other = model;
    other.walls[0].mass.mass = 3.0;
    EXPECT_EQ(free, signature(other));
}

TEST(FluidModel, inletsAndForces)
{
    Model model = jetModel();
    double area = 0.0;
    double slant = 1.0;
    Vec3 middle {};
    ASSERT_TRUE(faceOf(model.inlets[0], area, middle, slant));
    EXPECT_NEAR(area, 0.0016, 1e-15);
    EXPECT_NEAR(middle[0], 0.0, 1e-15);
    EXPECT_NEAR(middle[1], 0.0, 1e-15);
    EXPECT_NEAR(slant, 0.0, 1e-12);
    // not square to the face: the sine of the angle between
    Inlet askew = model.inlets[0];
    askew.direction = {std::sqrt(0.5), std::sqrt(0.5), 0.0};
    ASSERT_TRUE(faceOf(askew, area, middle, slant));
    EXPECT_NEAR(slant, std::sqrt(0.5), 1e-12);
    Inlet none = model.inlets[0];
    none.surface.triangles.clear();
    EXPECT_FALSE(faceOf(none, area, middle, slant));

    // as wide as a round face of its area; the Reynolds number by that
    const double width = std::sqrt(4.0 * 0.0016 / std::numbers::pi);
    EXPECT_NEAR(widthOf(model, 0), width, 1e-12);
    Model water = model;
    water.inlets[0].material = {1000.0, 1.0e-3};
    EXPECT_NEAR(reynolds(water, 0), 1000.0 * 2.0 * width / 1.0e-3, 1e-6);
    EXPECT_GT(reynolds(water, 0), 90000.0);
    // of a steady flow the inlet is its part of the fluid's space: the face
    // picked may be wider than the opening
    Model steady = water;
    steady.analysis = Analysis::Steady;
    steady.domain = faceSurface(0.0, -0.01, 0.01, -0.01, 0.01);
    steady.domain.patches.assign(steady.domain.triangles.size(), 1);
    EXPECT_NEAR(widthOf(steady, 0), width / 2.0, 1e-12);
    EXPECT_NEAR(reynolds(steady, 0), reynolds(water, 0) / 2.0, 1e-6);
    // no such inlet, no face, no viscosity
    EXPECT_EQ(widthOf(model, 1), 0.0);
    EXPECT_EQ(reynolds(model, 1), 0.0);
    water.inlets[0].material.viscosity = 0.0;
    EXPECT_EQ(reynolds(water, 0), 0.0);
    water.inlets[0] = none;
    EXPECT_EQ(widthOf(water, 0), 0.0);

    // the liquid is the first volume's, or with none the first inlet's
    Material liquid;
    model.inlets[0].material = {800.0, 0.05};
    ASSERT_TRUE(liquidOf(model, liquid));
    EXPECT_EQ(liquid.density, 800.0);
    Model both = model;
    both.fluids.push_back({"Doc#Water", boxSurface({0.0, 0.0, 0.0}, {0.1, 0.1, 0.1}), {}});
    ASSERT_TRUE(liquidOf(both, liquid));
    EXPECT_EQ(liquid.density, 1000.0);
    EXPECT_FALSE(liquidOf(Model(), liquid));

    // everything of an inlet and of a force monitor is in the signature
    const std::uint64_t base = signature(model);
    Model changed = model;
    changed.inlets[0].speed = 3.0;
    EXPECT_NE(signature(changed), base);
    changed = model;
    changed.inlets[0].direction = {-1.0, 0.0, 0.0};
    EXPECT_NE(signature(changed), base);
    changed = model;
    changed.inlets[0].end = 0.05;
    EXPECT_NE(signature(changed), base);
    changed = model;
    changed.inlets[0].surface.points[2][1] = 0.03;
    EXPECT_NE(signature(changed), base);
    changed = model;
    changed.inlets.clear();
    EXPECT_NE(signature(changed), base);
    model.walls.push_back({"Doc#Plate", boxSurface({0.1, -0.1, -0.1}, {0.11, 0.1, 0.1}), {}, {}});
    model.walls.push_back({"Doc#Other", boxSurface({0.15, -0.1, -0.1}, {0.16, 0.1, 0.1}), {}, {}});
    model.monitors.push_back({"Doc#Force", MonitorKind::Force, {}, 0, {1.0, 0.0, 0.0}});
    const std::uint64_t forced = signature(model);
    changed = model;
    changed.monitors[0].wall = 1;
    EXPECT_NE(signature(changed), forced);
    changed = model;
    changed.monitors[0].direction = {0.0, 1.0, 0.0};
    EXPECT_NE(signature(changed), forced);
}

TEST(FluidModel, aSteadyFlowsParts)
{
    const Model model = blockModel();
    const auto base = signature(model);
    // a model of a free surface is told from the same one as a steady flow
    Model steady = model;
    steady.analysis = Analysis::Steady;
    const auto first = signature(steady);
    EXPECT_NE(base, first);
    Model other = steady;
    other.iterations = 100;
    EXPECT_NE(first, signature(other));
    other = steady;
    other.turbulence = Turbulence::KOmegaSST;
    EXPECT_NE(first, signature(other));
    other = steady;
    other.outlets.push_back({"Outlet", {}, 0.0});
    const auto withOutlet = signature(other);
    EXPECT_NE(first, withOutlet);
    other.outlets[0].pressure = 10.0;
    EXPECT_NE(withOutlet, signature(other));
    other = steady;
    other.domain = model.fluids[0].surface;
    const auto withDomain = signature(other);
    EXPECT_NE(first, withDomain);
    other.domain.patches.assign(other.domain.triangles.size(), 0);
    other.domain.patches[0] = 1;
    EXPECT_NE(withDomain, signature(other));
    // and none of it tells in a free surface's
    other = model;
    other.iterations = 100;
    other.turbulence = Turbulence::KOmegaSST;
    other.outlets.push_back({"Outlet", {}, 0.0});
    EXPECT_EQ(base, signature(other));

    // the block of the model is 0.2 x 0.1 x 0.3 m
    const Surface& block = model.fluids[0].surface;
    Vec3 low {};
    Vec3 high {};
    ASSERT_TRUE(boxOf(block, low, high));
    const Vec3 size {high[0] - low[0], high[1] - low[1], high[2] - low[2]};
    EXPECT_NEAR(volumeOf(block), size[0] * size[1] * size[2], 1e-15);
    Surface patched = block;
    patched.patches.assign(patched.triangles.size(), 0);
    patched.patches[0] = 2;
    const auto areas = patchAreas(patched, 3);
    ASSERT_EQ(areas.size(), 3U);
    const double all = 2.0 * (size[0] * size[1] + size[1] * size[2] + size[0] * size[2]);
    EXPECT_NEAR(areas[0] + areas[2], all, 1e-15);
    EXPECT_EQ(areas[1], 0.0);
    EXPECT_GT(areas[2], 0.0);
    // without patches it is all the first
    EXPECT_NEAR(patchAreas(block, 2)[0], all, 1e-15);
}

TEST(FluidRegistry, addFindRemoveSelect)
{
    auto& registry = Registry::instance();
    registry.add(std::make_unique<NamedBackend>("T-test", true, true));
    registry.add(std::make_unique<NamedBackend>("T-away", false, false));
    registry.add(std::make_unique<NamedBackend>("T-here", false, true, 1));
    ASSERT_NE(registry.find("T-here"), nullptr);
    EXPECT_EQ(registry.find("T-here")->label(), "T-here1");
    // the same name replaces
    registry.add(std::make_unique<NamedBackend>("T-here", false, true, 2));
    EXPECT_EQ(registry.find("T-here")->label(), "T-here2");
    // by name: that one, whatever it is, and never another in its place
    EXPECT_EQ(registry.select("T-test"), registry.find("T-test"));
    EXPECT_EQ(registry.select("T-away"), registry.find("T-away"));
    EXPECT_EQ(registry.select("T-nobody"), nullptr);
    // unnamed: the first that is no test data and can run here
    EXPECT_EQ(registry.select(""), registry.find("T-here"));
    // unnamed, for a steady flow: the first of them that runs one
    EXPECT_EQ(registry.select("", Analysis::Steady), nullptr);
    registry.add(std::make_unique<NamedBackend>("T-steady", false, true, 3, true));
    EXPECT_EQ(registry.select("", Analysis::Steady), registry.find("T-steady"));
    EXPECT_EQ(registry.select(""), registry.find("T-here"));
    // by name it is that one still, and says itself what it cannot
    EXPECT_EQ(registry.select("T-here", Analysis::Steady), registry.find("T-here"));
    EXPECT_TRUE(registry.remove("T-steady"));
    EXPECT_TRUE(registry.remove("T-here"));
    EXPECT_FALSE(registry.remove("T-here"));
    EXPECT_EQ(registry.select(""), nullptr);
    EXPECT_TRUE(registry.remove("T-test"));
    EXPECT_TRUE(registry.remove("T-away"));
}

TEST(FluidFreeFall, framesHaveTheClosedForm)
{
    const FreeFallBackend backend;
    EXPECT_TRUE(backend.capabilities().testData);
    auto session = backend.createSession();
    std::string why;
    const Model model = blockModel();
    ASSERT_EQ(session->prepare(model, why), Status::Ok) << why;
    Kept kept;
    ASSERT_EQ(session->run(kept, why), Status::Ok) << why;
    ASSERT_EQ(kept.frames.size(), 9U);
    const double g = 9.80665;
    for (std::size_t k = 0; k < kept.frames.size(); ++k) {
        const Frame& frame = kept.frames[k];
        const double t = 0.05 * static_cast<double>(k);
        EXPECT_NEAR(frame.time, t, 1e-12);
        ASSERT_EQ(frame.points.size(), 250U);
        const double fall = 0.5 * g * t * t;
        double top = 0.0;
        for (std::size_t i = 0; i < frame.points.size(); ++i) {
            const Vec3& now = frame.points[i];
            const Vec3& was = kept.frames[0].points[i];
            EXPECT_NEAR(now[0], was[0], 1e-12);
            EXPECT_NEAR(now[2], std::max(was[2] - fall, 0.01), 1e-12);
            top = std::max(top, now[2]);
        }
        // the monitors at each frame's time
        ASSERT_EQ(kept.samples.size(), kept.frames.size());
        EXPECT_NEAR(kept.times[k], t, 1e-12);
        ASSERT_EQ(kept.samples[k].size(), 1U);
        EXPECT_NEAR(kept.samples[k][0], top + 0.01, 1e-12);
    }
}

TEST(FluidFreeFall, aFreeWallFallsToTheFloor)
{
    const FreeFallBackend backend;
    EXPECT_TRUE(backend.capabilities().freeBodies);
    Model model = blockModel();
    model.walls.push_back({"Doc#Fixed", boxSurface({0.3, 0.0, 0.0}, {0.4, 0.2, 0.05}), {}, {}});
    // 0.1 above the space's floor
    model.walls.push_back({"Doc#Float", boxSurface({0.2, 0.05, 0.1}, {0.3, 0.15, 0.2}), {}, {}});
    model.walls[1].motion = WallMotion::Free;
    model.walls[1].mass = {0.5, {0.25, 0.1, 0.15}, {1e-3, 1e-3, 1e-3, 0.0, 0.0, 0.0}};
    auto session = backend.createSession();
    std::string why;
    ASSERT_EQ(session->prepare(model, why), Status::Ok) << why;
    Kept kept;
    ASSERT_EQ(session->run(kept, why), Status::Ok) << why;
    ASSERT_EQ(kept.frames.size(), 9U);
    for (std::size_t k = 0; k < kept.frames.size(); ++k) {
        const Frame& frame = kept.frames[k];
        // one per wall the fluid moves, and none for the fixed one
        ASSERT_EQ(frame.bodies.size(), 1U);
        const Motion& motion = frame.bodies[0];
        const double t = 0.05 * static_cast<double>(k);
        EXPECT_NEAR(motion.translation[0], 0.0, 1e-12);
        EXPECT_NEAR(motion.translation[2], -std::min(0.5 * 9.80665 * t * t, 0.1), 1e-12);
        EXPECT_NEAR(motion.rotation[3], 1.0, 1e-12);
    }
}

TEST(FluidFreeFall, openSideLetsTheFluidOut)
{
    Model model = blockModel();
    model.space.open[4] = true;  // -z
    auto session = FreeFallBackend().createSession();
    std::string why;
    ASSERT_EQ(session->prepare(model, why), Status::Ok);
    Kept kept;
    ASSERT_EQ(session->run(kept, why), Status::Ok);
    EXPECT_EQ(kept.frames.front().points.size(), 250U);
    EXPECT_TRUE(kept.frames.back().points.empty());
    EXPECT_EQ(kept.samples.back()[0], 0.0);
}

TEST(FluidFreeFall, anInletSendsLayers)
{
    const FreeFallBackend backend;
    EXPECT_TRUE(backend.capabilities().inlets);
    EXPECT_TRUE(backend.capabilities().forces);
    Model model = jetModel();
    model.monitors.push_back({"Doc#Along", MonitorKind::Force, {}, -1, {1.0, 0.0, 0.0}});
    model.monitors.push_back({"Doc#Across", MonitorKind::Force, {}, -1, {0.0, 1.0, 0.0}});
    auto session = backend.createSession();
    std::string why;
    ASSERT_EQ(session->prepare(model, why), Status::Ok) << why;
    Kept kept;
    ASSERT_EQ(session->run(kept, why), Status::Ok) << why;
    ASSERT_EQ(kept.frames.size(), 11U);
    // A layer of 4 x 4 at the start and each 5 ms after, a spacing at 2
    // m/s; the first is out of the space's far side, 0.2 on, at 0.1 s.
    for (std::size_t k = 0; k < kept.frames.size(); ++k) {
        const std::size_t layers = 2 * k + 1;
        const std::size_t gone = k == 10 ? 1 : 0;
        EXPECT_EQ(kept.frames[k].points.size(), 16U * (layers - gone)) << k;
        double front = 0.0;
        for (const Vec3& p : kept.frames[k].points) {
            front = std::max(front, p[0]);
            EXPECT_LE(std::fabs(p[1]), 0.015 + 1e-12);
        }
        // no further than the fluid has gone, and the last layer still
        // at the face (held half a spacing inside the space)
        EXPECT_NEAR(front, std::max(std::min(0.02 * static_cast<double>(k), 0.19), 0.005), 1e-9)
            << k;
        // the momentum a second: 1000 x 0.0016 x 2 x 2
        EXPECT_NEAR(kept.samples[k][0], 6.4, 1e-9);
        EXPECT_NEAR(kept.samples[k][1], 0.0, 1e-9);
    }
    // an inlet that stops: nothing after its end
    model.inlets[0].start = 0.02;
    model.inlets[0].end = 0.04;
    ASSERT_EQ(session->prepare(model, why), Status::Ok) << why;
    Kept short_;
    ASSERT_EQ(session->run(short_, why), Status::Ok) << why;
    EXPECT_TRUE(short_.frames[1].points.empty());
    EXPECT_EQ(short_.frames[2].points.size(), 16U);
    EXPECT_EQ(short_.frames[4].points.size(), 5U * 16U);
    EXPECT_EQ(short_.frames[6].points.size(), 5U * 16U);
    EXPECT_EQ(short_.samples[1][0], 0.0);
    EXPECT_NEAR(short_.samples[3][0], 6.4, 1e-9);
    EXPECT_EQ(short_.samples[6][0], 0.0);
    // no fluid and no inlet is nothing to simulate
    model.inlets.clear();
    EXPECT_EQ(session->prepare(model, why), Status::Empty);
}

TEST(FluidFreeFall, emptyAndCancelled)
{
    auto session = FreeFallBackend().createSession();
    std::string why;
    Model model = blockModel();
    model.resolution = 1.0;  // coarser than the water
    EXPECT_EQ(session->prepare(model, why), Status::Empty);
    EXPECT_FALSE(why.empty());
    model.resolution = 0.0;
    EXPECT_EQ(session->prepare(model, why), Status::Unsupported);

    model = blockModel();
    ASSERT_EQ(session->prepare(model, why), Status::Ok);
    Kept kept;
    kept.stopAfter = 3;
    EXPECT_EQ(session->run(kept, why), Status::Cancelled);
    EXPECT_EQ(kept.frames.size(), 3U);
}

TEST(FluidArchive, framesReadBackOneAtATime)
{
    const std::string path = Base::FileInfo::getTempFileName("FluidArchive");
    Fluid::ResultsIndex index;
    index.backend = "FreeFall";
    index.version = "1";
    index.kind = ResultKind::Surface;
    index.signature = 18446744073709551557ULL;  // above 2^63: kept as text
    index.monitors = {{"Doc#Height", MonitorKind::Height}, {"Doc#P", MonitorKind::Pressure}};
    index.bodies = {"Doc#Float", "Doc#Other"};
    {
        Fluid::ArchiveWriter writer(path);
        for (int k = 0; k < 3; ++k) {
            Frame frame;
            frame.time = 0.1 * k;
            for (int i = 0; i <= k; ++i) {
                frame.points.push_back({0.001 * i, 0.002 * k, 0.5});
            }
            frame.triangles.push_back({0, k, 0});
            // the first slides along x, the second turns about z
            frame.bodies.resize(2);
            frame.bodies[0].translation = {0.01 * k, 0.0, -0.002};
            frame.bodies[1].rotation = {0.0, 0.0, std::sin(0.1 * k), std::cos(0.1 * k)};
            writer.frame(frame);
            // the monitors twice as often as the frames
            writer.sample(0.1 * k, {0.25 + k, 1000.0 * k});
            writer.sample(0.1 * k + 0.05, {0.5 + k, std::nan("")});
            // the fluid's energy with each frame
            Energy energy;
            energy.time = 0.1 * k;
            energy.kinetic = 0.5 * k;
            energy.potential = 3.0 - k;
            energy.work = 0.25 * k;
            energy.created = 0.001 * k;
            energy.exchanged = 0.1 * k;
            writer.energy(energy);
        }
        EXPECT_EQ(writer.frames(), 3U);
        EXPECT_NEAR(writer.lastTime(), 0.2, 1e-12);
        writer.close(index);
    }
    {
        const Fluid::ArchiveReader reader(path);
        const auto& read = reader.index();
        EXPECT_EQ(read.backend, "FreeFall");
        EXPECT_EQ(read.kind, ResultKind::Surface);
        EXPECT_EQ(read.signature, 18446744073709551557ULL);
        ASSERT_EQ(read.times.size(), 3U);
        EXPECT_NEAR(read.times[2], 0.2, 1e-12);
        ASSERT_EQ(read.monitors.size(), 2U);
        EXPECT_EQ(read.monitors[1].kind, MonitorKind::Pressure);
        ASSERT_EQ(read.samples.size(), 6U);
        EXPECT_NEAR(read.samples[5], 0.25, 1e-12);
        ASSERT_EQ(read.values.size(), 12U);
        // Height in mm, pressure in Pa; a value there was none of stays one
        EXPECT_NEAR(read.values[4 * 2 + 0], 2250.0, 1e-9);
        EXPECT_NEAR(read.values[4 * 2 + 1], 2000.0, 1e-9);
        EXPECT_NEAR(read.values[5 * 2 + 0], 2500.0, 1e-9);
        EXPECT_TRUE(std::isnan(read.values[5 * 2 + 1]));
        const auto frame = reader.frame(2);
        ASSERT_EQ(frame.points.size(), 9U);
        EXPECT_FLOAT_EQ(frame.points[6], 2.0F);  // mm
        EXPECT_FLOAT_EQ(frame.points[7], 4.0F);
        EXPECT_FLOAT_EQ(frame.points[8], 500.0F);
        ASSERT_EQ(frame.triangles.size(), 3U);
        EXPECT_EQ(frame.triangles[1], 2);
        // where the moved walls are: the lengths in mm, as the points
        ASSERT_EQ(read.bodies.size(), 2U);
        EXPECT_EQ(read.bodies[1], "Doc#Other");
        ASSERT_EQ(frame.bodies.size(), 2U);
        EXPECT_NEAR(frame.bodies[0].translation[0], 20.0, 1e-9);
        EXPECT_NEAR(frame.bodies[0].translation[2], -2.0, 1e-9);
        EXPECT_NEAR(frame.bodies[0].rotation[3], 1.0, 1e-12);
        EXPECT_NEAR(frame.bodies[1].rotation[2], std::sin(0.2), 1e-12);
        EXPECT_NEAR(frame.bodies[1].rotation[3], std::cos(0.2), 1e-12);
        EXPECT_EQ(reader.frame(0).points.size(), 3U);
        EXPECT_THROW(reader.frame(3), Base::IndexError);
        ASSERT_EQ(read.energy.size(), 3U);
        EXPECT_NEAR(read.energy[2].time, 0.2, 1e-12);
        EXPECT_NEAR(read.energy[2].kinetic, 1.0, 1e-12);
        EXPECT_NEAR(read.energy[2].potential, 1.0, 1e-12);
        EXPECT_NEAR(read.energy[2].work, 0.5, 1e-12);
        EXPECT_NEAR(read.energy[2].created, 0.002, 1e-12);
        EXPECT_NEAR(read.energy[2].exchanged, 0.2, 1e-12);
    }
    Base::FileInfo(path).deleteFile();
    EXPECT_THROW(Fluid::ArchiveReader missing(path), Base::Exception);
    // a run with no wall moved keeps nothing of them
    {
        Fluid::ArchiveWriter writer(path);
        Frame frame;
        frame.points.push_back({0.0, 0.0, 0.0});
        writer.frame(frame);
        writer.close({});
    }
    {
        const Fluid::ArchiveReader reader(path);
        EXPECT_TRUE(reader.index().bodies.empty());
        EXPECT_TRUE(reader.frame(0).bodies.empty());
        // nor of an energy it was told none of: results made before it was kept
        EXPECT_TRUE(reader.index().energy.empty());
    }
    Base::FileInfo(path).deleteFile();
}

TEST(FluidArchive, aFieldOnFacesAndTheOpenings)
{
    const std::string path = Base::FileInfo::getTempFileName("FluidArchive");
    Fluid::ResultsIndex index;
    index.backend = "OpenFOAM";
    index.kind = ResultKind::Fields;
    index.steady = true;
    index.settled = true;
    index.openings = {{"Doc#Inlet", false}, {"Doc#Outlet", true}};
    index.turbulence = "KOmegaSST";
    index.reynolds = 30000.0;
    {
        Fluid::ArchiveWriter writer(path);
        for (int k = 0; k < 2; ++k) {
            Frame frame;
            frame.time = 50.0 * k;
            frame.points = {{0.0, 0.0, 0.0}, {0.001, 0.0, 0.0}, {0.0, 0.001, 0.0}};
            frame.triangles.push_back({0, 1, 2});
            frame.values = {100.0 * k, 50.0 * k, 0.25};
            frame.openings = {{160.0 * k, 7.5e-6 * k}, {0.0, 7.5e-6 * k}};
            if (k == 1) {
                // the fluid inside, of the last frame only
                frame.centres = {{0.001, 0.002, 0.003}, {0.004, 0.005, 0.006}};
                frame.velocities = {{0.1, 0.0, 0.0}, {0.2, -0.1, 0.05}};
                frame.pressures = {12.5, 3.0};
                frame.wallUnits = {12.0, 79.0, 53.0};
            }
            writer.frame(frame);
        }
        writer.close(index);
    }
    {
        const Fluid::ArchiveReader reader(path);
        const auto& read = reader.index();
        EXPECT_EQ(read.kind, ResultKind::Fields);
        EXPECT_TRUE(read.steady);
        EXPECT_TRUE(read.settled);
        ASSERT_EQ(read.openings.size(), 2U);
        EXPECT_EQ(read.openings[0].source, "Doc#Inlet");
        EXPECT_FALSE(read.openings[0].outlet);
        EXPECT_TRUE(read.openings[1].outlet);
        // frames x openings x the mean pressure, Pa, and the flow, m^3/s
        ASSERT_EQ(read.flows.size(), 8U);
        EXPECT_NEAR(read.flows[4], 160.0, 1e-12);
        EXPECT_NEAR(read.flows[5], 7.5e-6, 1e-18);
        EXPECT_NEAR(read.flows[6], 0.0, 1e-12);
        const auto frame = reader.frame(1);
        ASSERT_EQ(frame.triangles.size(), 3U);
        ASSERT_EQ(frame.values.size(), 3U);
        EXPECT_FLOAT_EQ(frame.values[0], 100.0F);
        EXPECT_FLOAT_EQ(frame.values[2], 0.25F);
        // a cell a row: its centre in mm, its velocity, its pressure
        EXPECT_EQ(read.inside, (std::vector<std::size_t> {1}));
        EXPECT_EQ(frame.inside,
                  (std::vector<float> {1.0F, 2.0F, 3.0F, 0.1F, 0.0F, 0.0F, 12.5F,
                                       4.0F, 5.0F, 6.0F, 0.2F, -0.1F, 0.05F, 3.0F}));
        EXPECT_TRUE(reader.frame(0).inside.empty());
        // how the eddies were had, and what says whether to believe it
        EXPECT_EQ(read.turbulence, "KOmegaSST");
        EXPECT_EQ(read.reynolds, 30000.0);
        EXPECT_EQ(read.wallUnits, (std::array<double, 3> {12.0, 79.0, 53.0}));
    }
    Base::FileInfo(path).deleteFile();
    // results of a free surface say nothing of it
    {
        Fluid::ArchiveWriter writer(path);
        Frame frame;
        frame.points.push_back({0.0, 0.0, 0.0});
        writer.frame(frame);
        writer.close({});
    }
    {
        const Fluid::ArchiveReader reader(path);
        EXPECT_FALSE(reader.index().steady);
        EXPECT_TRUE(reader.index().openings.empty());
        EXPECT_TRUE(reader.frame(0).values.empty());
        EXPECT_TRUE(reader.index().inside.empty());
    }
    Base::FileInfo(path).deleteFile();
}

namespace
{
// A run's account at a time: the fluid's energy, and what its mechanism's
// hand-over made and passed.
Energy accountAt(double time, double fluid, double created = 0.0, double exchanged = 0.0)
{
    Energy energy;
    energy.time = time;
    energy.kinetic = 0.01;
    energy.potential = fluid - 0.01;
    energy.created = created;
    energy.exchanged = exchanged;
    return energy;
}
}  // namespace

TEST(FluidEnergy, aRunThatOnlyLosesIsBelieved)
{
    Fluid::ResultsIndex index;
    // no account kept: nothing to say
    EXPECT_FALSE(Fluid::judge(index).kept);
    // the fluid's energy falling all the way, as a solver's does
    for (int k = 0; k <= 20; ++k) {
        index.energy.push_back(accountAt(0.1 * k, 1.0 - 0.001 * k));
    }
    auto verdict = Fluid::judge(index);
    EXPECT_TRUE(verdict.kept);
    EXPECT_TRUE(verdict.budget);
    EXPECT_FALSE(verdict.rose);
    EXPECT_NEAR(verdict.rise, 0.0, 1e-12);
    // no mechanism passed anything
    EXPECT_FALSE(verdict.handOver);
    EXPECT_FALSE(verdict.lagged);
}

TEST(FluidEnergy, theStartIsNotHeldAgainstARun)
{
    // Seeded against a part, the fluid is pressed apart in the first
    // steps and has energy it was not given (measured: the bar put where
    // it rests, as much again as its kinetic energy ever is). The first
    // quarter of a run is its start.
    Fluid::ResultsIndex index;
    for (int k = 0; k <= 20; ++k) {
        index.energy.push_back(accountAt(0.1 * k, k == 1 ? 1.01 : 1.0 - 0.001 * k));
    }
    EXPECT_FALSE(Fluid::judge(index).rose);
}

TEST(FluidEnergy, aRiseLaterIsNot)
{
    Fluid::ResultsIndex index;
    for (int k = 0; k <= 20; ++k) {
        // from 1.5 s on it gains a fifth of its kinetic energy
        const double fluid = k < 15 ? 1.0 - 0.001 * k : 0.986 + 0.0004 * (k - 15);
        index.energy.push_back(accountAt(0.1 * k, fluid));
    }
    auto verdict = Fluid::judge(index);
    EXPECT_TRUE(verdict.rose);
    EXPECT_NEAR(verdict.rise, 0.2, 1e-9);
    // a run that was fed has no such budget: its inlets bring energy
    index.fed = true;
    verdict = Fluid::judge(index);
    EXPECT_FALSE(verdict.budget);
    EXPECT_FALSE(verdict.rose);
}

TEST(FluidEnergy, whatAMechanismsLagMakes)
{
    // The hinged bar as measured at 2.5 mm: on the whole step the lag made
    // three tenths of what went over, and went on making it; on a quarter
    // of the step a fiftieth, all of it at the start.
    Fluid::ResultsIndex swinging;
    Fluid::ResultsIndex settled;
    for (int k = 0; k <= 20; ++k) {
        const double t = 0.1 * k;
        swinging.energy.push_back(accountAt(t, 1.0 - 0.001 * k, 0.003 * t, 0.01 * t));
        settled.energy.push_back(
            accountAt(t, 1.0 - 0.001 * k, k == 0 ? 0.0 : 0.00015, 0.006 + 0.001 * t));
    }
    auto verdict = Fluid::judge(swinging);
    EXPECT_TRUE(verdict.handOver);
    EXPECT_NEAR(verdict.made, 0.3, 1e-9);
    EXPECT_TRUE(verdict.lagged);
    // the fluid's own budget says nothing of it
    EXPECT_FALSE(verdict.rose);
    verdict = Fluid::judge(settled);
    EXPECT_TRUE(verdict.handOver);
    EXPECT_NEAR(verdict.made, 0.0, 1e-9);
    EXPECT_FALSE(verdict.lagged);
    // a lag that takes energy out damps, and is nothing to warn of
    for (auto& at : swinging.energy) {
        at.created = -at.created;
    }
    EXPECT_FALSE(Fluid::judge(swinging).lagged);
    // a mechanism that stands still passes nothing: no share of it
    Fluid::ResultsIndex still;
    for (int k = 0; k <= 20; ++k) {
        still.energy.push_back(accountAt(0.1 * k, 1.0, k == 0 ? 0.0 : 1e-9, k == 0 ? 0.0 : 2e-9));
    }
    verdict = Fluid::judge(still);
    EXPECT_FALSE(verdict.lagged);
}
