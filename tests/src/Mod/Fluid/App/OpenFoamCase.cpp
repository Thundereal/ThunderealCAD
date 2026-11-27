// SPDX-License-Identifier: LGPL-2.1-or-later

// A fluid model written as an OpenFOAM case, and what OpenFOAM writes read
// back (docs/FluidSimulation.md sec 12, S2). Text only: no OpenFOAM is run.

#include <numbers>
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

// A duct along x, 0.1 x 0.01 x 0.01: in at its -x end at 0.1 m/s, out at
// its +x end at 500 Pa, a liquid of 1000 kg/m^3 and 0.1 Pa s.
Model ductModel()
{
    Model model;
    model.analysis = Analysis::Steady;
    model.iterations = 300;
    model.domain = boxSurface({0.0, 0.0, 0.0}, {0.1, 0.01, 0.01});
    model.domain.patches.assign(model.domain.triangles.size(), 0);
    model.domain.patches[8] = model.domain.patches[9] = 1;
    model.domain.patches[10] = model.domain.patches[11] = 2;
    Inlet inlet;
    inlet.source = "Inlet";
    inlet.direction = {1.0, 0.0, 0.0};
    inlet.speed = 0.1;
    inlet.material = {1000.0, 0.1};
    model.inlets.push_back(inlet);
    model.outlets.push_back({"Outlet", {}, 500.0});
    model.space.min = {0.0, 0.0, 0.0};
    model.space.max = {0.1, 0.01, 0.01};
    model.resolution = 0.001;
    model.monitors.push_back({"P1", MonitorKind::Pressure, {0.04, 0.005, 0.005}});
    model.monitors.push_back({"P2", MonitorKind::Pressure, {0.09, 0.005, 0.005}});
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

TEST(OpenFoamCase, filesOfASteadyFlow)
{
    Case made;
    std::string why;
    ASSERT_TRUE(makeCase(ductModel(), {}, made, why)) << why;
    // the fluid's box and a cell and a half about it
    EXPECT_EQ(made.cells, (std::array<long, 3> {103, 13, 13}));
    for (const char* name : {"system/blockMeshDict",
                             "system/snappyHexMeshDict",
                             "system/controlDict",
                             "system/fvSchemes",
                             "system/fvSolution",
                             "system/decomposeParDict",
                             "constant/transportProperties",
                             "constant/turbulenceProperties",
                             "constant/triSurface/wall.stl",
                             "constant/triSurface/inlet0.stl",
                             "constant/triSurface/outlet0.stl",
                             "0/U",
                             "0/p"}) {
        EXPECT_TRUE(made.files.contains(name)) << name;
    }
    // nothing of a free surface's
    for (const char* name : {"0/alpha.water", "0/p_rgh", "system/setFieldsDict", "constant/g"}) {
        EXPECT_FALSE(made.files.contains(name)) << name;
    }
    const std::string& block = made.files["system/blockMeshDict"];
    EXPECT_TRUE(has(block, "(103 13 13)"));
    EXPECT_TRUE(has(block, "(-0.0015 -0.0015 -0.0015)"));
    // each patch its own surface: eight triangles of wall, two an opening
    const auto facets = [&](const char* name) {
        const std::string& stl = made.files[std::string("constant/triSurface/") + name];
        std::size_t count = 0;
        for (auto at = stl.find("facet normal"); at != std::string::npos;
             at = stl.find("facet normal", at + 1)) {
            ++count;
        }
        return count;
    };
    EXPECT_EQ(facets("wall.stl"), 8U);
    EXPECT_EQ(facets("inlet0.stl"), 2U);
    EXPECT_EQ(facets("outlet0.stl"), 2U);
    const std::string& snappy = made.files["system/snappyHexMeshDict"];
    EXPECT_TRUE(has(snappy, "wall { level (0 0); patchInfo { type wall; } }"));
    EXPECT_TRUE(has(snappy, "inlet0 { level (0 0); patchInfo { type patch; } }"));
    EXPECT_TRUE(has(snappy, "outlet0 { level (0 0); patchInfo { type patch; } }"));
    // the mesh is told a point of the fluid
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_GT(made.inside[i], ductModel().space.min[i]);
        EXPECT_LT(made.inside[i], ductModel().space.max[i]);
    }
    const std::string& control = made.files["system/controlDict"];
    EXPECT_TRUE(has(control, "application simpleFoam;"));
    EXPECT_TRUE(has(control, "endTime 300;"));
    EXPECT_TRUE(has(control, "probeLocations ( (0.04 0.005 0.005) (0.09 0.005 0.005) );"));
    // each opening's mean pressure and what goes through it, and the
    // pressure on every face
    for (const char* name : {"inlet0P", "inlet0Q", "outlet0P", "outlet0Q", "skin"}) {
        EXPECT_TRUE(has(control, std::string("    ") + name + "\n")) << name;
    }
    EXPECT_TRUE(has(made.files["0/U"], "inlet0 { type fixedValue; value uniform (0.1 0 0); }"));
    EXPECT_TRUE(has(made.files["0/U"], "outlet0 { type zeroGradient; }"));
    // the pressure is the solver's own, over the density
    EXPECT_TRUE(has(made.files["0/p"], "outlet0 { type fixedValue; value uniform 0.5; }"));
    EXPECT_TRUE(has(made.files["0/p"], "inlet0 { type zeroGradient; }"));
    EXPECT_TRUE(has(made.files["constant/transportProperties"], "nu 0.0001;"));
    EXPECT_TRUE(has(made.files["system/fvSchemes"], "steadyState"));
    // the fluid inside is written once, at the end: each cell's own values
    EXPECT_TRUE(has(made.files["system/controlDict"], "inside\n"));
    EXPECT_TRUE(has(made.files["system/controlDict"], "writeControl onEnd;"));
    EXPECT_TRUE(has(made.files["system/controlDict"], "cells { type cellCentre; }"));
    ASSERT_EQ(made.steps.size(), 3U);
    EXPECT_EQ(made.steps[0].name, "blockMesh");
    EXPECT_EQ(made.steps[1].name, "snappyHexMesh");
    EXPECT_EQ(made.steps[2].name, "simpleFoam");
    EXPECT_TRUE(made.steps[2].solver);

    CaseOptions four;
    four.processes = 4;
    ASSERT_TRUE(makeCase(ductModel(), four, made, why)) << why;
    ASSERT_EQ(made.steps.size(), 4U);
    EXPECT_EQ(made.steps[2].name, "decomposePar");
    EXPECT_EQ(made.steps[3].command, "mpirun -np 4 simpleFoam -parallel");
    EXPECT_TRUE(has(made.files["system/decomposeParDict"], "numberOfSubdomains 4;"));
}

TEST(OpenFoamCase, filesOfATurbulentFlow)
{
    Model model = ductModel();
    Case made;
    std::string why;
    // a laminar flow has none of it
    ASSERT_TRUE(makeCase(model, {}, made, why)) << why;
    EXPECT_FALSE(made.files.contains("0/k"));
    EXPECT_FALSE(has(made.files["system/controlDict"], "yPlus"));
    EXPECT_TRUE(has(made.files["constant/turbulenceProperties"], "simulationType laminar;"));

    model.turbulence = Turbulence::KOmegaSST;
    // the eddies that come in are sized by the inlet's part of the fluid's
    // space, 0.01 x 0.01: as wide as a round face of that area
    Model none = model;
    none.domain.patches.assign(none.domain.triangles.size(), 0);
    none.domain.patches[10] = none.domain.patches[11] = 2;
    EXPECT_FALSE(makeCase(none, {}, made, why));
    EXPECT_TRUE(has(why, "Inlet"));
    model.inlets[0].speed = 2.0;
    ASSERT_TRUE(makeCase(model, {}, made, why)) << why;
    for (const char* name : {"0/U", "0/p", "0/k", "0/omega", "0/nut"}) {
        EXPECT_TRUE(made.files.contains(name)) << name;
    }
    EXPECT_TRUE(has(made.files["constant/turbulenceProperties"], "RASModel kOmegaSST;"));
    // stirred a twentieth of its speed: k = 1.5 (0.05 x 2)^2
    EXPECT_TRUE(has(made.files["0/k"], "inlet0 { type fixedValue; value uniform 0.015; }"));
    EXPECT_TRUE(has(made.files["0/k"], "internalField uniform 0.015;"));
    EXPECT_TRUE(has(made.files["0/k"], "outlet0 { type inletOutlet; inletValue uniform 0.015;"));
    EXPECT_TRUE(has(made.files["0/k"], "kqRWallFunction"));
    // in eddies 7% of the width across: omega = sqrt(k) / (0.09^0.25 0.07 w)
    const double width = std::sqrt(4.0 * 1.0e-4 / std::numbers::pi);
    const double rate = std::sqrt(0.015) / (std::pow(0.09, 0.25) * 0.07 * width);
    const auto at = made.files["0/omega"].find("inlet0 { type fixedValue; value uniform ");
    ASSERT_NE(at, std::string::npos);
    EXPECT_NEAR(std::stod(made.files["0/omega"].substr(at + 40)), rate, 1e-4 * rate);
    EXPECT_TRUE(has(made.files["0/omega"], "omegaWallFunction"));
    EXPECT_TRUE(has(made.files["0/nut"], "nutkWallFunction"));
    EXPECT_TRUE(has(made.files["system/fvSchemes"], "div(phi,omega)"));
    EXPECT_TRUE(has(made.files["system/fvSchemes"], "wallDist"));
    EXPECT_TRUE(has(made.files["system/fvSolution"], "\"(U|k|omega)\""));
    EXPECT_TRUE(has(made.files["system/controlDict"], "type yPlus;"));
}

TEST(OpenFoamCase, theWallUnitsReadBack)
{
    const std::string text = "# y+ ()\n# Time \tpatch \tmin \tmax \taverage\n"
                             "184 \tinlet0\t1 \t2 \t1.5\n"
                             "184 \twall\t1.45822689e+01\t1.23885348e+02\t8.43915792e+01\n";
    std::array<double, 3> units {};
    ASSERT_TRUE(readWallUnits(text, units));
    EXPECT_NEAR(units[0], 14.5822689, 1e-9);
    EXPECT_NEAR(units[1], 123.885348, 1e-9);
    EXPECT_NEAR(units[2], 84.3915792, 1e-9);
    // nothing of the wall, a row cut short, and no file
    std::array<double, 3> kept {1.0, 2.0, 3.0};
    EXPECT_FALSE(readWallUnits("184 \tinlet0\t1 \t2 \t1.5\n", kept));
    EXPECT_FALSE(readWallUnits("184 \twall\t14.5 \t123.8\n", kept));
    EXPECT_FALSE(readWallUnits("", kept));
    EXPECT_EQ(kept, (std::array<double, 3> {1.0, 2.0, 3.0}));
}

TEST(OpenFoamCase, whatCannotBeASteadyFlow)
{
    Case made;
    std::string why;
    Model model = ductModel();
    model.domain = {};
    EXPECT_FALSE(makeCase(model, {}, made, why));
    EXPECT_TRUE(has(why, "space"));
    model = ductModel();
    model.inlets.clear();
    EXPECT_FALSE(makeCase(model, {}, made, why));
    EXPECT_TRUE(has(why, "inlet"));
    model = ductModel();
    model.outlets.clear();
    EXPECT_FALSE(makeCase(model, {}, made, why));
    EXPECT_TRUE(has(why, "outlet"));
    model = ductModel();
    model.monitors.push_back({"Height", MonitorKind::Height, {0.05, 0.005, 0.0}});
    EXPECT_FALSE(makeCase(model, {}, made, why));
    EXPECT_TRUE(has(why, "height"));
    model = ductModel();
    model.iterations = 0;
    EXPECT_FALSE(makeCase(model, {}, made, why));
}

TEST(OpenFoamCase, aFacesPressureReadBack)
{
    const std::string text = "# vtk DataFile Version 2.0\ntime='110'\nASCII\nDATASET POLYDATA\n"
                             "FIELD FieldData 1\nTimeValue 1 1 float\n110\n\n"
                             "POINTS 4 float\n0 0 0\n1 0 0\n1 1 0\n0 1 0\n\n"
                             "POLYGONS 1 5\n4 0 1 2 3\n\n"
                             "POINT_DATA 4\nFIELD FieldData 2\n"
                             "U 3 4 float\n1 0 0 1 0 0\n1 0 0 1 0 0\n"
                             "p 1 4 float\n0.5 0.25\n0.125 0\n";
    Frame frame;
    ASSERT_TRUE(readFieldVtk(text, "p", frame));
    EXPECT_EQ(frame.points.size(), 4U);
    EXPECT_EQ(frame.triangles.size(), 2U);
    EXPECT_EQ(frame.values, (std::vector<double> {0.5, 0.25, 0.125, 0.0}));
    // a field that is not there, and a file cut short
    EXPECT_FALSE(readFieldVtk(text, "T", frame));
    EXPECT_FALSE(readFieldVtk(text.substr(0, text.size() - 8), "p", frame));
}

TEST(OpenFoamCase, theFluidInsideReadBack)
{
    const std::string text = "0.001 \t-0.002 \t0.003 \t0.5 \t0.1 \t0 \t-0.02\n"
                             "0.002 \t-0.002 \t0.003 \t0.25 \t0.2 \t0.01 \t0\n";
    Frame frame;
    ASSERT_TRUE(readInside(text, frame));
    ASSERT_EQ(frame.centres.size(), 2U);
    EXPECT_EQ(frame.centres[1], (Vec3 {0.002, -0.002, 0.003}));
    EXPECT_EQ(frame.pressures, (std::vector<double> {0.5, 0.25}));
    EXPECT_EQ(frame.velocities[0], (Vec3 {0.1, 0.0, -0.02}));
    EXPECT_EQ(frame.velocities[1], (Vec3 {0.2, 0.01, 0.0}));
    // a file cut short, a row that is not a cell's, and none
    EXPECT_FALSE(readInside(text.substr(0, text.size() - 3), frame));
    EXPECT_TRUE(frame.centres.empty());
    EXPECT_FALSE(readInside("0.001 0.002 0.003 0.5\n", frame));
    EXPECT_FALSE(readInside("", frame));
}
