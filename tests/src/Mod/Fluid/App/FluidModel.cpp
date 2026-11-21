// SPDX-License-Identifier: LGPL-2.1-or-later

// The fluid workbench's solver-neutral model, its registry, the backend of
// test data and the results' archive (docs/FluidSimulation.md secs 11, 12).

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
    bool cancelled() const override
    {
        return stopAfter >= 0 && static_cast<int>(frames.size()) >= stopAfter;
    }
    std::vector<Frame> frames;
    int stopAfter = -1;
};

class NamedBackend: public Backend
{
public:
    NamedBackend(std::string n, bool test, bool here, int tag = 0)
        : n(std::move(n))
        , test(test)
        , here(here)
        , tag(tag)
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
        ASSERT_EQ(frame.monitors.size(), 1U);
        EXPECT_NEAR(frame.monitors[0], top + 0.01, 1e-12);
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
    EXPECT_EQ(kept.frames.back().monitors[0], 0.0);
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
    {
        Fluid::ArchiveWriter writer(path);
        for (int k = 0; k < 3; ++k) {
            Frame frame;
            frame.time = 0.1 * k;
            for (int i = 0; i <= k; ++i) {
                frame.points.push_back({0.001 * i, 0.002 * k, 0.5});
            }
            frame.triangles.push_back({0, k, 0});
            frame.monitors = {0.25 + k, 1000.0 * k};
            writer.frame(frame);
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
        // Height in mm, pressure in Pa
        EXPECT_NEAR(read.values[2 * 2 + 0], 2250.0, 1e-9);
        EXPECT_NEAR(read.values[2 * 2 + 1], 2000.0, 1e-9);
        const auto frame = reader.frame(2);
        ASSERT_EQ(frame.points.size(), 9U);
        EXPECT_FLOAT_EQ(frame.points[6], 2.0F);  // mm
        EXPECT_FLOAT_EQ(frame.points[7], 4.0F);
        EXPECT_FLOAT_EQ(frame.points[8], 500.0F);
        ASSERT_EQ(frame.triangles.size(), 3U);
        EXPECT_EQ(frame.triangles[1], 2);
        EXPECT_EQ(reader.frame(0).points.size(), 3U);
        EXPECT_THROW(reader.frame(3), Base::IndexError);
    }
    Base::FileInfo(path).deleteFile();
    EXPECT_THROW(Fluid::ArchiveReader missing(path), Base::Exception);
}
