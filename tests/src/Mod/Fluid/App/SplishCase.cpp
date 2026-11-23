// SPDX-License-Identifier: LGPL-2.1-or-later

// What the SPlisHSPlasH backend hands its runner: the walls as particles
// behind their faces, the fluid as particles, the case's files, and the
// runner's lines read back (docs/FluidSimulation.md sec 12 S3).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <Mod/Fluid/App/Solver/SplishCase.h>

using namespace Fluid::Solver;

namespace
{

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

double distance(const Vec3& a, const Vec3& b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])
                     + (a[2] - b[2]) * (a[2] - b[2]));
}

// How far a point is outside a box: 0 inside.
double outside(const Vec3& p, const Vec3& min, const Vec3& max)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        const double d = std::max({min[i] - p[i], p[i] - max[i], 0.0});
        sum += d * d;
    }
    return std::sqrt(sum);
}

// How far the wall's particles are from the worst served place on the
// faces of a box open above: its floor and its four sides, gone over in
// steps.
double widestHole(const std::vector<Vec3>& wall, const Vec3& min, const Vec3& max, double step)
{
    double widest = 0.0;
    const auto look = [&](const Vec3& q) {
        double nearest = std::numeric_limits<double>::max();
        for (const Vec3& p : wall) {
            nearest = std::min(nearest, distance(p, q));
        }
        widest = std::max(widest, nearest);
    };
    for (double x = min[0]; x <= max[0] + 1e-12; x += step) {
        for (double y = min[1]; y <= max[1] + 1e-12; y += step) {
            look({x, y, min[2]});
        }
        for (double z = min[2]; z <= max[2] + 1e-12; z += step) {
            look({x, min[1], z});
            look({x, max[1], z});
        }
    }
    for (double y = min[1]; y <= max[1] + 1e-12; y += step) {
        for (double z = min[2]; z <= max[2] + 1e-12; z += step) {
            look({min[0], y, z});
            look({max[0], y, z});
        }
    }
    return widest;
}

// A space 0.4 x 0.2 x 0.3, open above, and water 0.1 x 0.2 x 0.2 in a
// corner of it.
Model spaceModel()
{
    Model model;
    model.fluids.push_back({"Doc#Water", boxSurface({0.0, 0.0, 0.0}, {0.1, 0.2, 0.2}), {}});
    model.space.min = {0.0, 0.0, 0.0};
    model.space.max = {0.4, 0.2, 0.3};
    model.space.open[5] = true;
    model.resolution = 0.02;
    model.timeEnd = 0.5;
    model.outputInterval = 0.05;
    return model;
}

// The same space made of parts: a floor and four walls 0.03 thick that only
// touch one another, and around them a space that is their box.
Model platesModel()
{
    Model model = spaceModel();
    const double t = 0.03;
    model.walls.push_back({"Doc#Floor", boxSurface({-t, -t, -t}, {0.4 + t, 0.2 + t, 0.0}), {}, {}});
    model.walls.push_back({"Doc#Left", boxSurface({-t, -t, 0.0}, {0.0, 0.2 + t, 0.3}), {}, {}});
    model.walls.push_back({"Doc#Right", boxSurface({0.4, -t, 0.0}, {0.4 + t, 0.2 + t, 0.3}), {}, {}});
    model.walls.push_back({"Doc#Front", boxSurface({0.0, -t, 0.0}, {0.4, 0.0, 0.3}), {}, {}});
    model.walls.push_back({"Doc#Back", boxSurface({0.0, 0.2, 0.0}, {0.4, 0.2 + t, 0.3}), {}, {}});
    model.space.min = {-t, -t, -t};
    model.space.max = {0.4 + t, 0.2 + t, 0.3};
    return model;
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

TEST(SplishCase, sizesFollowTheResolution)
{
    const Splish::Sizes sizes = Splish::sizesOf(0.02);
    EXPECT_DOUBLE_EQ(sizes.radius, 0.01);
    // measured: still water stays still with the wall's particles this far
    // behind the wall's face, and is thrown off it with them nearer
    EXPECT_DOUBLE_EQ(sizes.behind, 0.012);
    EXPECT_DOUBLE_EQ(sizes.wallSpacing, 0.015);
}

TEST(SplishCase, closedSidesOfTheSpaceAreWalls)
{
    const Model model = spaceModel();
    const Splish::Sizes sizes = Splish::sizesOf(model.resolution);
    const std::vector<Vec3> wall = Splish::boundaryOf(model);
    ASSERT_FALSE(wall.empty());
    bool corner = false;
    std::size_t floor = 0;
    for (const Vec3& p : wall) {
        // behind a closed side by the distance, never in the space
        EXPECT_GE(outside(p, model.space.min, model.space.max), sizes.behind - 1e-9);
        EXPECT_LE(outside(p, model.space.min, model.space.max), sizes.behind * 1.7321 + 1e-9);
        // none over the open top
        const bool overTheTop = p[2] > model.space.max[2] + 1e-9 && p[0] > 0.0 && p[0] < 0.4
            && p[1] > 0.0 && p[1] < 0.2;
        EXPECT_FALSE(overTheTop);
        corner = corner
            || distance(p, {-sizes.behind, -sizes.behind, -sizes.behind})
                < 1.01 * sizes.behind;
        floor += std::fabs(p[2] + sizes.behind) < 1e-9 ? 1 : 0;
    }
    // the sides meet behind the corner: no gap there
    EXPECT_TRUE(corner);
    // the floor's particles are about its area over the spacing's square
    const double expected = (0.4 + 2 * sizes.behind) * (0.2 + 2 * sizes.behind)
        / (sizes.wallSpacing * sizes.wallSpacing);
    EXPECT_GT(static_cast<double>(floor), 0.85 * expected);
    EXPECT_LT(static_cast<double>(floor), 1.4 * expected);
    // no hole: nowhere on the sides is the nearest particle farther than
    // straight behind and half the grid's diagonal aside
    const double fair = std::sqrt(sizes.behind * sizes.behind
                                  + 0.5 * sizes.wallSpacing * sizes.wallSpacing);
    EXPECT_LT(widestHole(wall, model.space.min, model.space.max, 0.005), 1.05 * fair);
    // no two on one another, and none alone
    for (std::size_t i = 0; i < wall.size(); ++i) {
        double nearest = std::numeric_limits<double>::max();
        for (std::size_t j = 0; j < wall.size(); ++j) {
            if (i != j) {
                nearest = std::min(nearest, distance(wall[i], wall[j]));
            }
        }
        EXPECT_GT(nearest, 0.3 * sizes.wallSpacing);
        EXPECT_LT(nearest, 1.05 * sizes.wallSpacing);
    }
}

TEST(SplishCase, wallsOfPartsThatTouch)
{
    const Model model = platesModel();
    const Splish::Sizes sizes = Splish::sizesOf(model.resolution);
    const std::vector<Vec3> wall = Splish::boundaryOf(model);
    ASSERT_FALSE(wall.empty());
    const Vec3 inMin {0.0, 0.0, 0.0};
    const Vec3 inMax {0.4, 0.2, 0.3};
    std::size_t floor = 0;
    bool corner = false;
    for (const Vec3& p : wall) {
        // Every particle is the distance behind the inside of the parts
        // taken together -- behind the floor, a wall, or both where they
        // meet -- whichever part it is in: never behind a part's outer
        // face, nor behind a face one part touches another by.
        double behind = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
            behind = std::max({behind, inMin[k] - p[k], k == 2 ? 0.0 : p[k] - inMax[k]});
        }
        EXPECT_NEAR(behind, sizes.behind, 1e-9) << p[0] << " " << p[1] << " " << p[2];
        floor += std::fabs(p[2] + sizes.behind) < 1e-9 ? 1 : 0;
        corner = corner
            || distance(p, {-sizes.behind, -sizes.behind, -sizes.behind})
                < 1.01 * sizes.behind;
        // a wall's sheet runs on past its top by no more than the distance
        EXPECT_LT(p[2], 0.3 + sizes.behind + 1e-9);
    }
    // the floor's sheet and the walls' meet behind the corner the parts
    // make, though no part has a corner there
    EXPECT_TRUE(corner);
    const double expected = 0.4 * 0.2 / (sizes.wallSpacing * sizes.wallSpacing);
    EXPECT_GT(static_cast<double>(floor), 0.9 * expected);
    EXPECT_LT(static_cast<double>(floor), 1.5 * expected);
    // and no hole where the parts touch: a dam break in these plates lost
    // two thirds of its water through the corners before this held
    const double fair = std::sqrt(sizes.behind * sizes.behind
                                  + 0.5 * sizes.wallSpacing * sizes.wallSpacing);
    EXPECT_LT(widestHole(wall, inMin, inMax, 0.005), 1.05 * fair);
}

TEST(SplishCase, particlesFillTheWaterAndKeepOutOfParts)
{
    Model model = spaceModel();
    // 5 x 10 x 10 cubes of 0.02
    std::vector<Vec3> water = Splish::particlesOf(model);
    EXPECT_EQ(water.size(), 500U);
    EXPECT_NEAR(water.front()[0], 0.01, 1e-12);
    // a part standing in the water takes its place
    model.walls.push_back({"Doc#Post", boxSurface({0.0, 0.0, 0.0}, {0.04, 0.04, 0.2}), {}, {}});
    water = Splish::particlesOf(model);
    EXPECT_EQ(water.size(), 500U - 2U * 2U * 10U);
    for (const Vec3& p : water) {
        EXPECT_FALSE(p[0] < 0.04 && p[1] < 0.04);
    }
}

TEST(SplishCase, filesOfACase)
{
    Model model = platesModel();
    model.monitors.push_back({"Doc#Height", MonitorKind::Height, {0.05, 0.1, 0.0}});
    // on the right wall's face, which looks along -x; and in the open water
    model.monitors.push_back({"Doc#Pressure", MonitorKind::Pressure, {0.4, 0.1, 0.01}});
    model.monitors.push_back({"Doc#Within", MonitorKind::Pressure, {0.2, 0.1, 0.1}});
    Splish::CaseOptions options;
    options.threads = 4;
    Splish::Case made;
    std::string why;
    ASSERT_TRUE(Splish::makeCase(model, options, made, why)) << why;
    EXPECT_EQ(made.particles, 500U);
    ASSERT_TRUE(made.files.contains("case.txt"));
    ASSERT_TRUE(made.files.contains("fluid.points"));
    ASSERT_TRUE(made.files.contains("boundary.points"));
    // three floats a point
    EXPECT_EQ(made.files.at("fluid.points").size(), 500U * 12U);
    EXPECT_EQ(made.files.at("boundary.points").size(), Splish::boundaryOf(model).size() * 12U);
    float first[3];
    std::memcpy(first, made.files.at("fluid.points").data(), sizeof(first));
    EXPECT_FLOAT_EQ(first[0], 0.01F);
    const std::string& text = made.files.at("case.txt");
    // cohesion is asked for, or the case does not speak of it
    EXPECT_EQ(text.find("cohesion"), std::string::npos);
    Splish::CaseOptions held = options;
    held.cohesion = 0.05;
    Splish::Case holding;
    ASSERT_TRUE(Splish::makeCase(model, held, holding, why)) << why;
    EXPECT_NE(holding.files.at("case.txt").find("threads 4\ncohesion 0.05\nspace "),
              std::string::npos);
    for (const char* line : {"fluidsplish 1\n",
                             "radius 0.01\n",
                             "gravity 0 0 -9.80665\n",
                             "end 0.5\n",
                             "interval 0.05\n",
                             "density 1000\n",
                             "viscosity 1e-06\n",
                             "threads 4\n",
                             "space -0.03 -0.03 -0.03 0.43 0.23 0.3\n",
                             "fluid 500\n",
                             "monitor height 0.05 0.1 0\n",
                             "monitor pressure 0.4 0.1 0.01 -1 0 0\n",
                             "monitor pressure 0.2 0.1 0.1\n"}) {
        EXPECT_NE(text.find(line), std::string::npos) << line;
    }
    EXPECT_NE(text.find("boundary " + std::to_string(Splish::boundaryOf(model).size()) + "\n"),
              std::string::npos);
}

TEST(SplishCase, aWallTheFluidMoves)
{
    // a block 0.1 x 0.1 x 0.06 standing on the floor of the plates' tank,
    // beside the water, no face of it where a particle of the grid is
    Model model = platesModel();
    const Vec3 low {0.2, 0.06, 0.0};
    const Vec3 high {0.3, 0.16, 0.06};
    const std::size_t without = Splish::boundaryOf(model).size();
    model.walls.push_back({"Doc#Float", boxSurface(low, high), {}, {}});
    // held, it covers the floor under it and has its own faces among the
    // walls
    EXPECT_NE(Splish::boundaryOf(model).size(), without);
    model.walls.back().motion = WallMotion::Free;
    model.walls.back().mass = {0.3, {0.25, 0.11, 0.03}, {4e-4, 5e-4, 6e-4, 1e-5, 2e-5, 3e-5}};
    const Splish::Sizes sizes = Splish::sizesOf(model.resolution);

    // The fixed walls are as though it were not there: the floor under it
    // has its sheet, which a fixed block would have covered.
    const std::vector<Vec3> wall = Splish::boundaryOf(model);
    EXPECT_EQ(wall.size(), without);
    std::size_t under = 0;
    for (const Vec3& p : wall) {
        // nothing of the block's own among the walls
        EXPECT_GT(outside(p, low, high), 0.0);
        if (p[2] < 0.0 && p[0] > 0.22 && p[0] < 0.28 && p[1] > 0.08 && p[1] < 0.14) {
            ++under;
        }
    }
    EXPECT_GT(under, 4U);

    // Its own sheet: behind every face of it, the floor's side too, and
    // nowhere a hole a particle of fluid gets through.
    const std::vector<Vec3> body = Splish::bodyOf(model, model.walls.size() - 1);
    ASSERT_FALSE(body.empty());
    for (const Vec3& p : body) {
        EXPECT_LT(outside(p, low, high), 1e-9);
        double depth = 1.0;
        for (std::size_t k = 0; k < 3; ++k) {
            depth = std::min({depth, p[k] - low[k], high[k] - p[k]});
        }
        // as deep as a sheet is, every one: the corners' too
        EXPECT_NEAR(depth, sizes.behind, 1e-9);
    }
    const Vec3 in {low[0] + sizes.behind, low[1] + sizes.behind, low[2] + sizes.behind};
    const Vec3 out {high[0] - sizes.behind, high[1] - sizes.behind, high[2] - sizes.behind};
    EXPECT_LT(widestHole(body, in, out, 0.5 * sizes.radius), 2.0 * sizes.radius);
    // a wall that is not moved has no sheet of its own
    EXPECT_TRUE(Splish::bodyOf(model, 0).empty());

    Splish::Case made;
    std::string why;
    ASSERT_TRUE(Splish::makeCase(model, {}, made, why)) << why;
    ASSERT_EQ(made.bodies.size(), 1U);
    EXPECT_EQ(made.bodies[0], body.size());
    ASSERT_TRUE(made.files.contains("body.0.points"));
    EXPECT_EQ(made.files.at("body.0.points").size(), body.size() * 12U);
    EXPECT_EQ(made.files.at("boundary.points").size(), wall.size() * 12U);
    // how many particles, the mass, its centre, the inertia about it
    EXPECT_NE(made.files.at("case.txt")
                  .find("body " + std::to_string(body.size())
                        + " 0.3 0.25 0.11 0.03 0.0004 0.0005 0.0006 1e-05 2e-05 3e-05\n"),
              std::string::npos)
        << made.files.at("case.txt");
    // the water keeps out of it, as out of any part
    model.fluids[0].surface = boxSurface({0.0, 0.0, 0.0}, {0.4, 0.2, 0.2});
    for (const Vec3& p : Splish::particlesOf(model)) {
        EXPECT_GT(outside(p, low, high), 0.0);
    }

    // a wall with nothing to weigh cannot be moved
    model.walls.back().mass.mass = 0.0;
    EXPECT_FALSE(Splish::makeCase(model, {}, made, why));
    EXPECT_NE(why.find("Doc#Float"), std::string::npos) << why;
}

TEST(SplishCase, whatCannotBeACase)
{
    Splish::Case made;
    std::string why;
    Model model = spaceModel();
    model.fluids.clear();
    EXPECT_FALSE(Splish::makeCase(model, {}, made, why));
    EXPECT_NE(why.find("no fluid"), std::string::npos);
    // water wholly inside a part leaves no particle
    model = spaceModel();
    model.walls.push_back({"Doc#Block", boxSurface({-0.1, -0.1, -0.1}, {0.2, 0.3, 0.3}), {}, {}});
    EXPECT_FALSE(Splish::makeCase(model, {}, made, why));
    EXPECT_NE(why.find("no fluid"), std::string::npos);
    model = spaceModel();
    model.resolution = 0.0;
    EXPECT_FALSE(Splish::makeCase(model, {}, made, why));
    EXPECT_NE(why.find("resolution"), std::string::npos);
    model = spaceModel();
    model.gravity = {0.0, 0.0, 0.0};
    model.space.open = {true, true, true, true, true, true};
    model.fluids.push_back({"Doc#Oil", boxSurface({0.2, 0.0, 0.0}, {0.3, 0.2, 0.2}), {800.0, 0.05}});
    EXPECT_FALSE(Splish::makeCase(model, {}, made, why));
    EXPECT_NE(why.find("one liquid"), std::string::npos);
}

TEST(SplishCase, theRunnersLines)
{
    Splish::Event event = Splish::parseLine("frame 3 0.15 4096");
    EXPECT_EQ(event.kind, Splish::Event::Frame);
    EXPECT_EQ(event.index, 3);
    EXPECT_DOUBLE_EQ(event.time, 0.15);
    EXPECT_EQ(event.count, 4096U);

    event = Splish::parseLine("sample 0.0125 0.2 nan 1500.5\r");
    EXPECT_EQ(event.kind, Splish::Event::Sample);
    EXPECT_DOUBLE_EQ(event.time, 0.0125);
    ASSERT_EQ(event.values.size(), 3U);
    EXPECT_DOUBLE_EQ(event.values[0], 0.2);
    EXPECT_TRUE(std::isnan(event.values[1]));
    EXPECT_DOUBLE_EQ(event.values[2], 1500.5);

    // where a moved wall is at the frame that follows
    event = Splish::parseLine("body 1 0.01 0 -0.25 0 0 0.6 0.8");
    EXPECT_EQ(event.kind, Splish::Event::Body);
    EXPECT_EQ(event.index, 1);
    ASSERT_EQ(event.values.size(), 7U);
    EXPECT_DOUBLE_EQ(event.values[2], -0.25);
    EXPECT_DOUBLE_EQ(event.values[6], 0.8);
    EXPECT_EQ(Splish::parseLine("body 1 0.01 0 -0.25").kind, Splish::Event::Other);

    event = Splish::parseLine("error the case has no fluid");
    EXPECT_EQ(event.kind, Splish::Event::Error);
    EXPECT_EQ(event.text, "the case has no fluid");

    event = Splish::parseLine("version 2.19.1");
    EXPECT_EQ(event.kind, Splish::Event::Version);
    EXPECT_EQ(event.text, "2.19.1");

    // anything else is the solver talking
    EXPECT_EQ(Splish::parseLine("frame three").kind, Splish::Event::Other);
    EXPECT_EQ(Splish::parseLine("").kind, Splish::Event::Other);
    EXPECT_EQ(Splish::parseLine("[INFO] 12 iterations").kind, Splish::Event::Other);
}

TEST(SplishCase, pointsReadBack)
{
    const float raw[] = {0.5F, 0.25F, 1.0F, -2.0F, 0.0F, 3.0F};
    std::string bytes(sizeof(raw), '\0');
    std::memcpy(bytes.data(), raw, sizeof(raw));
    std::vector<Vec3> points;
    ASSERT_TRUE(Splish::pointsOf(bytes, 2, points));
    ASSERT_EQ(points.size(), 2U);
    EXPECT_DOUBLE_EQ(points[1][0], -2.0);
    EXPECT_DOUBLE_EQ(points[1][2], 3.0);
    // a file still being written is not a frame
    EXPECT_FALSE(Splish::pointsOf(bytes.substr(0, 20), 2, points));
    EXPECT_FALSE(Splish::pointsOf(bytes, 3, points));
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
