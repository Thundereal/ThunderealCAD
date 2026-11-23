// SPDX-License-Identifier: LGPL-2.1-or-later

// A fluid model written as an OpenFOAM case, and what OpenFOAM writes read
// back (docs/FluidSimulation.md sec 12, S2). Text only: no OpenFOAM is run.

#include <gtest/gtest.h>

#include <cmath>

#include <Mod/Fluid/App/Solver/OpenFoamCase.h>

using namespace Fluid::Solver;
using namespace Fluid::Solver::OpenFoam;

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

// A floor slab under a space 0.4 x 0.1 x 0.21, a block of water on it.
Model damModel()
{
    Model model;
    model.walls.push_back({"Floor", boxSurface({0.0, 0.0, -0.01}, {0.4, 0.1, 0.0}), {}, {}});
    model.fluids.push_back({"Water", boxSurface({0.0, 0.0, 0.0}, {0.1, 0.1, 0.1}), {998.2, 1.002e-3}});
    model.space.min = {0.0, 0.0, -0.01};
    model.space.max = {0.4, 0.1, 0.2};
    model.space.open[5] = true;
    model.resolution = 0.01;
    model.timeEnd = 0.3;
    model.outputInterval = 0.02;
    model.monitors.push_back({"Height", MonitorKind::Height, {0.05, 0.05, 0.0}});
    model.monitors.push_back({"P1", MonitorKind::Pressure, {0.399, 0.05, 0.003}});
    model.monitors.push_back({"P2", MonitorKind::Pressure, {0.399, 0.05, 0.015}});
    return model;
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(OpenFoamCase, filesOfADamBreak)
{
    Case made;
    std::string why;
    ASSERT_TRUE(makeCase(damModel(), {}, made, why)) << why;
    EXPECT_EQ(made.cells, (std::array<long, 3> {40, 10, 21}));
    for (const char* name : {"system/blockMeshDict",
                             "system/snappyHexMeshDict",
                             "system/controlDict",
                             "system/fvSchemes",
                             "system/fvSolution",
                             "system/setFieldsDict",
                             "system/decomposeParDict",
                             "constant/transportProperties",
                             "constant/turbulenceProperties",
                             "constant/g",
                             "constant/triSurface/wall0.stl",
                             "constant/triSurface/fluid0.stl",
                             "0/alpha.water",
                             "0/U",
                             "0/p_rgh"}) {
        EXPECT_TRUE(made.files.contains(name)) << name;
    }
    const std::string& block = made.files["system/blockMeshDict"];
    EXPECT_TRUE(has(block, "(40 10 21)"));
    EXPECT_TRUE(has(block, "(0.4 0.1 0.2)"));
    // the top open, the five others closed
    EXPECT_TRUE(has(block, "open { type patch; faces ( (4 5 6 7) ); }"));
    EXPECT_TRUE(has(block, "space { type wall; faces ( (0 4 7 3) (1 2 6 5) (0 1 5 4) (3 7 6 2) (0 3 2 1) ); }"));

    const std::string& control = made.files["system/controlDict"];
    EXPECT_TRUE(has(control, "application interFoam;"));
    EXPECT_TRUE(has(control, "endTime 0.3;"));
    EXPECT_TRUE(has(control, "writeInterval 0.02;"));
    EXPECT_TRUE(has(control, "isoField alpha.water; isoValue 0.5;"));
    // the monitors by kind, in the model's order, at every step
    EXPECT_TRUE(has(control, "probeLocations ( (0.399 0.05 0.003) (0.399 0.05 0.015) );"));
    EXPECT_TRUE(has(control, "locations ( (0.05 0.05 0) );"));
    EXPECT_TRUE(has(control, "writeControl timeStep;"));

    const std::string& transport = made.files["constant/transportProperties"];
    EXPECT_TRUE(has(transport, "rho 998.2;"));
    // the kinematic viscosity, 1.002e-3 / 998.2
    EXPECT_TRUE(has(transport, "nu 1.0038068523342e-06;")) << transport;
    EXPECT_TRUE(has(made.files["constant/g"], "value (0 0 -9.80665);"));
    EXPECT_TRUE(has(made.files["system/setFieldsDict"], "surfaceName fluid0.stl;"));
    EXPECT_TRUE(has(made.files["system/snappyHexMeshDict"], "file \"wall0.stl\";"));
    EXPECT_TRUE(has(made.files["0/p_rgh"], "open { type totalPressure; p0 uniform 0; }"));
    EXPECT_TRUE(has(made.files["0/U"], "\".*\" { type noSlip; }"));

    // where the mesh is: in the water, on no cell's face
    EXPECT_TRUE(inside(damModel().fluids[0].surface, made.inside));
    for (std::size_t i = 0; i < 3; ++i) {
        const double cell = (made.inside[i] - damModel().space.min[i]) / 0.01;
        EXPECT_GT(std::abs(cell - std::round(cell)), 0.05);
    }

    ASSERT_EQ(made.steps.size(), 4U);
    EXPECT_EQ(made.steps[0].command, "blockMesh");
    EXPECT_EQ(made.steps[1].command, "snappyHexMesh -overwrite");
    EXPECT_EQ(made.steps[2].command, "setFields");
    EXPECT_EQ(made.steps[3].command, "interFoam");
    EXPECT_TRUE(made.steps[3].solver);
    EXPECT_FALSE(made.steps[0].solver);
}

TEST(OpenFoamCase, closedSpaceNoWallsAndProcesses)
{
    Model model = damModel();
    model.walls.clear();
    model.space.open = {};
    model.monitors.clear();
    Case made;
    std::string why;
    CaseOptions options;
    options.processes = 4;
    ASSERT_TRUE(makeCase(model, options, made, why)) << why;
    EXPECT_FALSE(made.files.contains("system/snappyHexMeshDict"));
    EXPECT_FALSE(has(made.files["system/blockMeshDict"], "open"));
    EXPECT_FALSE(has(made.files["0/p_rgh"], "totalPressure"));
    EXPECT_FALSE(has(made.files["system/controlDict"], "probes"));
    EXPECT_FALSE(has(made.files["system/controlDict"], "interfaceHeight"));
    EXPECT_TRUE(has(made.files["system/decomposeParDict"], "numberOfSubdomains 4;"));
    ASSERT_EQ(made.steps.size(), 4U);
    EXPECT_EQ(made.steps[2].command, "decomposePar");
    EXPECT_EQ(made.steps[3].command, "mpirun -np 4 interFoam -parallel");
    EXPECT_TRUE(made.steps[3].solver);
}

TEST(OpenFoamCase, whatCannotBeACase)
{
    Case made;
    std::string why;
    Model model = damModel();
    model.fluids.clear();
    EXPECT_FALSE(makeCase(model, {}, made, why));
    EXPECT_TRUE(has(why, "no fluid"));
    model = damModel();
    model.resolution = 0.0;
    EXPECT_FALSE(makeCase(model, {}, made, why));
    model = damModel();
    model.space.max[1] = model.space.min[1];
    EXPECT_FALSE(makeCase(model, {}, made, why));
    EXPECT_TRUE(has(why, "no space"));
    model = damModel();
    model.resolution = 1.0;  // coarser than the water
    EXPECT_FALSE(makeCase(model, {}, made, why));
}

TEST(OpenFoamCase, theDuctAnInstallIsPutTo)
{
    const Case alongX = ductCase(0);
    for (const char* name : {"system/blockMeshDict", "system/controlDict", "system/fvSchemes",
                             "system/fvSolution", "constant/transportProperties",
                             "constant/turbulenceProperties", "0/U", "0/p"}) {
        EXPECT_TRUE(alongX.files.count(name)) << name;
    }
    ASSERT_EQ(alongX.steps.size(), 2U);
    EXPECT_EQ(alongX.steps[0].command, "blockMesh");
    EXPECT_EQ(alongX.steps[1].command, "simpleFoam");
    EXPECT_NE(alongX.files.at("0/U").find("value uniform (0.1 0 0);"), std::string::npos);
    EXPECT_NE(alongX.files.at("system/controlDict").find("probeLocations ((0.04 0 0) (0.09 0 0));"),
              std::string::npos);
    EXPECT_NE(alongX.files.at("system/blockMeshDict").find("(0.1 0.005 0.005)"), std::string::npos);
    // the same duct turned: what was along x is along y, or z
    const Case alongY = ductCase(1);
    EXPECT_NE(alongY.files.at("0/U").find("value uniform (0 0.1 0);"), std::string::npos);
    EXPECT_NE(alongY.files.at("system/controlDict").find("probeLocations ((0 0.04 0) (0 0.09 0));"),
              std::string::npos);
    EXPECT_NE(alongY.files.at("system/blockMeshDict").find("(0.005 0.1 0.005)"), std::string::npos);
    const Case alongZ = ductCase(2);
    EXPECT_NE(alongZ.files.at("0/U").find("value uniform (0 0 0.1);"), std::string::npos);
    // the stress term the check is of is the one a solver is told to use
    EXPECT_NE(alongX.files.at("system/fvSchemes").find("div((nuEff*dev2(T(grad(U)))))"),
              std::string::npos);

    EXPECT_NEAR(ductLoss("# Probe 0 (0.04 0 0)\n# Time 0 1\n1 0.9 0.1\n2 0.24 0.04\n"), 0.2, 1e-15);
    EXPECT_TRUE(std::isnan(ductLoss("# nothing yet\n")));
    EXPECT_TRUE(std::isnan(ductLoss("1 0.5\n")));
    EXPECT_TRUE(sameLoss(0.13707, 0.13712));
    EXPECT_FALSE(sameLoss(0.1695, 0.1371));
    EXPECT_FALSE(sameLoss(0.1371, 0.1695));
    EXPECT_FALSE(sameLoss(0.0, 0.0));
    EXPECT_FALSE(sameLoss(std::nan(""), 0.1371));
}

TEST(OpenFoamCase, stlOfASurface)
{
    Surface s;
    s.points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 2.0, 0.0}};
    s.triangles = {{0, 1, 2}};
    EXPECT_EQ(stlOf(s, "part"),
              "solid part\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n"
              "   vertex 1 0 0\n   vertex 0 2 0\n  endloop\n endfacet\nendsolid part\n");
}

TEST(OpenFoamCase, surfaceReadBack)
{
    const std::string vtk = "# vtk DataFile Version 2.0\ntime='0.1'\nASCII\nDATASET POLYDATA\n"
                            "FIELD FieldData 1\nTimeValue 1 1 float\n0.1\n\n"
                            "POINTS 5 float\n0 0 0 1 0 0 1 1 0\n0 1 0 0.5 0.5 1\n\n"
                            "POLYGONS 2 9\n3 0 1 4\n4 0 1 2 3\n";
    Frame frame;
    ASSERT_TRUE(readSurfaceVtk(vtk, frame));
    ASSERT_EQ(frame.points.size(), 5U);
    EXPECT_EQ(frame.points[4], (Vec3 {0.5, 0.5, 1.0}));
    // the quadrilateral fanned into two
    ASSERT_EQ(frame.triangles.size(), 3U);
    EXPECT_EQ(frame.triangles[0], (std::array<int, 3> {0, 1, 4}));
    EXPECT_EQ(frame.triangles[1], (std::array<int, 3> {0, 1, 2}));
    EXPECT_EQ(frame.triangles[2], (std::array<int, 3> {0, 2, 3}));
    // a file still being written
    EXPECT_FALSE(readSurfaceVtk(vtk.substr(0, vtk.size() - 8), frame));
    EXPECT_FALSE(readSurfaceVtk(vtk.substr(0, 140), frame));
    EXPECT_FALSE(readSurfaceVtk("", frame));
    // a corner that is no point
    EXPECT_FALSE(readSurfaceVtk("DATASET POLYDATA\nPOINTS 1 float\n0 0 0\nPOLYGONS 1 4\n3 0 0 7\n", frame));
}

TEST(OpenFoamCase, tablesReadBack)
{
    const std::string probes = "# Probe 0 (1.6 0.07 0.003)\n# Probe 1 (1.6 0.07 0.015)\n"
                               "#   Time     0     1\n"
                               "0.001   12.5   -3\n0.002   13   4e2\n\n0.0035 (1 2 3) 7\n";
    const auto table = readTable(probes);
    ASSERT_EQ(table.size(), 3U);
    EXPECT_EQ(table[1], (std::vector<double> {0.002, 13.0, 400.0}));
    EXPECT_EQ(table[2].size(), 5U);
    ASSERT_NE(rowAt(table, 0.002, 1e-4), nullptr);
    EXPECT_EQ(rowAt(table, 0.00204, 1e-4)->at(1), 13.0);
    // the nearest row within the tolerance, none beyond it
    EXPECT_EQ(rowAt(table, 0.0016, 1e-3)->at(1), 13.0);
    EXPECT_EQ(rowAt(table, 0.01, 1e-3), nullptr);
    EXPECT_TRUE(readTable("# nothing yet\n").empty());

    EXPECT_NEAR(lastTimeOf("Create time\n\nTime = 0.001\n\nCourant\nTime = 0.0125\nPIMPLE"), 0.0125, 1e-12);
    EXPECT_LT(lastTimeOf("Create mesh for time = 0\n"), 0.0);
}
