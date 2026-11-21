// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "OpenFoamCase.h"

using namespace Fluid::Solver;
using namespace Fluid::Solver::OpenFoam;

namespace
{
// Shortest text that reads back as the same double.
std::string num(double v)
{
    char text[40];
    std::snprintf(text, sizeof(text), "%.15g", v);
    return text;
}

std::string vec(const Vec3& v)
{
    return "(" + num(v[0]) + " " + num(v[1]) + " " + num(v[2]) + ")";
}

std::string header(const std::string& cls, const std::string& object)
{
    return "FoamFile\n{\n    version 2.0;\n    format ascii;\n    class " + cls + ";\n    object "
        + object + ";\n}\n\n";
}

std::string dictionary(const std::string& object, const std::string& body)
{
    return header("dictionary", object) + body;
}

// The side of the space most against gravity, 0 to 5 as Space::open.
bool anyOpen(const Space& space)
{
    return std::ranges::any_of(space.open, [](bool open) {
        return open;
    });
}

// The background box's faces as blockMesh numbers its corners, in the
// order of Space::open: -x, +x, -y, +y, -z, +z.
constexpr const char* sideFaces[6] =
    {"(0 4 7 3)", "(1 2 6 5)", "(0 1 5 4)", "(3 7 6 2)", "(0 3 2 1)", "(4 5 6 7)"};

std::string blockMeshDict(const Model& model, const std::array<long, 3>& cells)
{
    const Vec3& a = model.space.min;
    const Vec3& b = model.space.max;
    std::ostringstream out;
    out << "scale 1;\nvertices\n(\n";
    for (int i = 0; i < 8; ++i) {
        // 0 1 2 3 the bottom going round, 4 5 6 7 the top above them
        const bool x = i == 1 || i == 2 || i == 5 || i == 6;
        const bool y = i == 2 || i == 3 || i == 6 || i == 7;
        const bool z = i >= 4;
        out << "    " << vec({x ? b[0] : a[0], y ? b[1] : a[1], z ? b[2] : a[2]}) << "\n";
    }
    out << ");\nblocks\n(\n    hex (0 1 2 3 4 5 6 7) (" << cells[0] << " " << cells[1] << " "
        << cells[2] << ") simpleGrading (1 1 1)\n);\nedges ();\nboundary\n(\n";
    std::string open;
    std::string closed;
    for (std::size_t side = 0; side < 6; ++side) {
        (model.space.open[side] ? open : closed) += std::string(" ") + sideFaces[side];
    }
    if (!open.empty()) {
        out << "    open { type patch; faces (" << open << " ); }\n";
    }
    if (!closed.empty()) {
        out << "    space { type wall; faces (" << closed << " ); }\n";
    }
    out << ");\n";
    return dictionary("blockMeshDict", out.str());
}

std::string snappyHexMeshDict(const Model& model, const Vec3& inside)
{
    std::ostringstream out;
    out << "castellatedMesh true;\nsnap true;\naddLayers false;\ngeometry\n{\n";
    for (std::size_t i = 0; i < model.walls.size(); ++i) {
        out << "    wall" << i << " { type triSurfaceMesh; file \"wall" << i << ".stl\"; }\n";
    }
    out << "}\ncastellatedMeshControls\n{\n"
           "    maxLocalCells 4000000;\n    maxGlobalCells 16000000;\n"
           "    minRefinementCells 0;\n    nCellsBetweenLevels 2;\n    features ();\n"
           "    refinementSurfaces\n    {\n";
    for (std::size_t i = 0; i < model.walls.size(); ++i) {
        out << "        wall" << i << " { level (0 0); patchInfo { type wall; } }\n";
    }
    out << "    }\n    resolveFeatureAngle 30;\n    refinementRegions {}\n    locationInMesh "
        << vec(inside)
        << ";\n    allowFreeStandingZoneFaces false;\n}\n"
           "snapControls\n{\n    nSmoothPatch 3;\n    tolerance 2.0;\n    nSolveIter 30;\n"
           "    nRelaxIter 5;\n    nFeatureSnapIter 10;\n    implicitFeatureSnap true;\n"
           "    explicitFeatureSnap false;\n    multiRegionFeatureSnap false;\n}\n"
           "addLayersControls\n{\n    relativeSizes true;\n    layers {}\n"
           "    expansionRatio 1.0;\n    finalLayerThickness 0.3;\n    minThickness 0.1;\n"
           "    nGrow 0;\n    featureAngle 60;\n    nRelaxIter 3;\n"
           "    nSmoothSurfaceNormals 1;\n    nSmoothNormals 3;\n    nSmoothThickness 10;\n"
           "    maxFaceThicknessRatio 0.5;\n    maxThicknessToMedialRatio 0.3;\n"
           "    minMedianAxisAngle 90;\n    nBufferCellsNoExtrude 0;\n    nLayerIter 50;\n}\n"
           "meshQualityControls\n{\n    maxNonOrtho 65;\n    maxBoundarySkewness 20;\n"
           "    maxInternalSkewness 4;\n    maxConcave 80;\n    minVol 1e-13;\n"
           "    minTetQuality 1e-15;\n    minArea -1;\n    minTwist 0.02;\n"
           "    minDeterminant 0.001;\n    minFaceWeight 0.05;\n    minVolRatio 0.01;\n"
           "    minTriangleTwist -1;\n    nSmoothScale 4;\n    errorReduction 0.75;\n}\n"
           "mergeTolerance 1e-6;\n";
    return dictionary("snappyHexMeshDict", out.str());
}

std::string controlDict(const Model& model)
{
    const std::string every =
        "        writeControl adjustable;\n        writeInterval " + num(model.outputInterval)
        + ";\n";
    std::ostringstream out;
    // Only the last time's fields are kept, to start again from; what a
    // frame needs is written by the function objects below, and is small
    out << "application interFoam;\nstartFrom startTime;\nstartTime 0;\nstopAt endTime;\n"
        << "endTime " << num(model.timeEnd) << ";\ndeltaT "
        << num(std::min(1.0e-4, model.outputInterval))
        << ";\nwriteControl adjustable;\nwriteInterval " << num(model.outputInterval)
        << ";\npurgeWrite 1;\nwriteFormat binary;\nwritePrecision 8;\ntimeFormat general;\n"
        << "timePrecision 10;\nrunTimeModifiable no;\nadjustTimeStep yes;\n"
        << "maxCo 0.5;\nmaxAlphaCo 0.5;\nmaxDeltaT " << num(model.outputInterval)
        << ";\nfunctions\n{\n"
        << "    freeSurface\n    {\n        type surfaces;\n        libs (sampling);\n"
        << every
        << "        surfaceFormat vtk;\n"
           "        formatOptions { vtk { format ascii; legacy true; } }\n"
           "        fields ();\n        interpolationScheme cellPoint;\n"
           "        surfaces\n        {\n"
           "            water { type isoSurface; isoField alpha.water; isoValue 0.5; "
           "interpolate true; }\n        }\n    }\n";
    std::string pressures;
    std::string heights;
    for (const auto& monitor : model.monitors) {
        (monitor.kind == MonitorKind::Pressure ? pressures : heights) += " " + vec(monitor.point);
    }
    // The monitors at every step of the solver: an impact's peak is over
    // in milliseconds, between two frames
    const std::string each = "        writeControl timeStep;\n        writeInterval 1;\n";
    if (!pressures.empty()) {
        out << "    pressure\n    {\n        type probes;\n        libs (sampling);\n"
            << each << "        fields (p);\n        probeLocations (" << pressures
            << " );\n    }\n";
    }
    if (!heights.empty()) {
        out << "    height\n    {\n        type interfaceHeight;\n"
               "        libs (fieldFunctionObjects);\n"
            << each << "        alpha alpha.water;\n        locations (" << heights
            << " );\n    }\n";
    }
    out << "}\n";
    return dictionary("controlDict", out.str());
}

constexpr const char* fvSchemes =
    "ddtSchemes { default Euler; }\n"
    "gradSchemes { default Gauss linear; }\n"
    "divSchemes\n{\n"
    "    div(rhoPhi,U) Gauss linearUpwind grad(U);\n"
    "    div(phi,alpha) Gauss vanLeer;\n"
    "    div(phirb,alpha) Gauss linear;\n"
    "    div(((rho*nuEff)*dev2(T(grad(U))))) Gauss linear;\n}\n"
    "laplacianSchemes { default Gauss linear corrected; }\n"
    "interpolationSchemes { default linear; }\n"
    "snGradSchemes { default corrected; }\n";

constexpr const char* fvSolution =
    "solvers\n{\n"
    "    \"alpha.water.*\"\n    {\n        nAlphaCorr 2;\n        nAlphaSubCycles 1;\n"
    "        cAlpha 1;\n        MULESCorr yes;\n        nLimiterIter 5;\n"
    "        solver smoothSolver;\n        smoother symGaussSeidel;\n"
    "        tolerance 1e-8;\n        relTol 0;\n    }\n"
    "    \"pcorr.*\" { solver PCG; preconditioner DIC; tolerance 1e-5; relTol 0; }\n"
    "    p_rgh { solver PCG; preconditioner DIC; tolerance 1e-07; relTol 0.05; }\n"
    "    p_rghFinal { $p_rgh; relTol 0; }\n"
    "    U { solver smoothSolver; smoother symGaussSeidel; tolerance 1e-06; relTol 0; }\n}\n"
    "PIMPLE\n{\n    momentumPredictor no;\n    nOuterCorrectors 1;\n    nCorrectors 3;\n"
    "    nNonOrthogonalCorrectors 1;\n    pRefCell 0;\n    pRefValue 0;\n}\n"
    "relaxationFactors { equations { \".*\" 1; } }\n";

std::string field(const std::string& name,
                  const std::string& cls,
                  const std::string& dimensions,
                  const std::string& internal,
                  const std::string& open,
                  const std::string& wall,
                  bool hasOpen)
{
    std::string out = header(cls, name) + "dimensions " + dimensions + ";\ninternalField uniform "
        + internal + ";\nboundaryField\n{\n";
    if (hasOpen) {
        out += "    open { " + open + " }\n";
    }
    out += "    \".*\" { " + wall + " }\n}\n";
    return out;
}
}  // namespace

std::string Fluid::Solver::OpenFoam::stlOf(const Surface& surface, const std::string& name)
{
    std::ostringstream out;
    out << "solid " << name << "\n";
    for (const auto& t : surface.triangles) {
        const Vec3& a = surface.points[static_cast<std::size_t>(t[0])];
        const Vec3& b = surface.points[static_cast<std::size_t>(t[1])];
        const Vec3& c = surface.points[static_cast<std::size_t>(t[2])];
        const Vec3 u {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 v {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        Vec3 n {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length > 0.0) {
            n = {n[0] / length, n[1] / length, n[2] / length};
        }
        out << " facet normal " << num(n[0]) << " " << num(n[1]) << " " << num(n[2])
            << "\n  outer loop\n";
        for (const Vec3* p : {&a, &b, &c}) {
            out << "   vertex " << num((*p)[0]) << " " << num((*p)[1]) << " " << num((*p)[2])
                << "\n";
        }
        out << "  endloop\n endfacet\n";
    }
    out << "endsolid " << name << "\n";
    return out.str();
}

bool Fluid::Solver::OpenFoam::makeCase(const Model& model,
                                       const CaseOptions& options,
                                       Case& made,
                                       std::string& why)
{
    made = Case {};
    if (model.resolution <= 0.0 || model.outputInterval <= 0.0 || model.timeEnd <= 0.0) {
        why = "the resolution, the output interval and the end time must be positive";
        return false;
    }
    if (model.fluids.empty()) {
        why = "the study has no fluid";
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const double side = model.space.max[i] - model.space.min[i];
        if (!(side > 0.0)) {
            why = "the study has no space: its walls and fluid have no size";
            return false;
        }
        made.cells[i] = std::max(std::lround(side / model.resolution), 1L);
    }
    // Where the mesh is: a point of the fluid, off every cell's face and
    // centre by odd fractions of a cell
    bool found = false;
    for (const auto& fluid : model.fluids) {
        const auto points = sampled(fluid.surface, model.resolution);
        if (!points.empty()) {
            const Vec3& p = points[points.size() / 2];
            made.inside = {p[0] + 0.137 * model.resolution,
                           p[1] + 0.151 * model.resolution,
                           p[2] + 0.173 * model.resolution};
            found = true;
            break;
        }
    }
    if (!found) {
        why = "there is no fluid: no volume holds a point at this resolution";
        return false;
    }
    const bool hasOpen = anyOpen(model.space);
    auto& files = made.files;
    files["system/blockMeshDict"] = blockMeshDict(model, made.cells);
    files["system/controlDict"] = controlDict(model);
    files["system/fvSchemes"] = dictionary("fvSchemes", fvSchemes);
    files["system/fvSolution"] = dictionary("fvSolution", fvSolution);
    files["system/decomposeParDict"] = dictionary(
        "decomposeParDict",
        "numberOfSubdomains " + std::to_string(std::max(options.processes, 1))
            + ";\nmethod scotch;\n");

    for (std::size_t i = 0; i < model.walls.size(); ++i) {
        const std::string name = "wall" + std::to_string(i);
        files["constant/triSurface/" + name + ".stl"] = stlOf(model.walls[i].surface, name);
    }
    if (!model.walls.empty()) {
        files["system/snappyHexMeshDict"] = snappyHexMeshDict(model, made.inside);
    }
    // The fluid at the start: the cells whose centres are inside a volume
    std::string regions;
    for (std::size_t i = 0; i < model.fluids.size(); ++i) {
        const std::string name = "fluid" + std::to_string(i);
        files["constant/triSurface/" + name + ".stl"] = stlOf(model.fluids[i].surface, name);
        regions += "    searchableSurfaceToCell\n    {\n        surfaceType triSurfaceMesh;\n"
                   "        surfaceName "
            + name + ".stl;\n        fieldValues ( volScalarFieldValue alpha.water 1 );\n    }\n";
    }
    files["system/setFieldsDict"] = dictionary(
        "setFieldsDict",
        "defaultFieldValues ( volScalarFieldValue alpha.water 0 );\nregions\n(\n" + regions
            + ");\n");

    // The first volume's material is the liquid's; the gas is air at 20
    // degrees
    const Material& liquid = model.fluids.front().material;
    files["constant/transportProperties"] = dictionary(
        "transportProperties",
        "phases (water air);\nwater { transportModel Newtonian; nu "
            + num(liquid.viscosity / liquid.density) + "; rho " + num(liquid.density)
            + "; }\nair { transportModel Newtonian; nu 1.516e-05; rho 1.204; }\nsigma 0.0728;\n");
    files["constant/turbulenceProperties"] =
        dictionary("turbulenceProperties", "simulationType laminar;\n");
    files["constant/g"] = header("uniformDimensionedVectorField", "g")
        + "dimensions [0 1 -2 0 0 0 0];\nvalue " + vec(model.gravity) + ";\n";

    files["0/alpha.water"] = field("alpha.water",
                                   "volScalarField",
                                   "[0 0 0 0 0 0 0]",
                                   "0",
                                   "type inletOutlet; inletValue uniform 0; value uniform 0;",
                                   "type zeroGradient;",
                                   hasOpen);
    files["0/U"] = field("U",
                         "volVectorField",
                         "[0 1 -1 0 0 0 0]",
                         "(0 0 0)",
                         "type pressureInletOutletVelocity; value uniform (0 0 0);",
                         "type noSlip;",
                         hasOpen);
    files["0/p_rgh"] = field("p_rgh",
                             "volScalarField",
                             "[1 -1 -2 0 0 0 0]",
                             "0",
                             "type totalPressure; p0 uniform 0;",
                             "type fixedFluxPressure; value uniform 0;",
                             hasOpen);

    made.steps.push_back({"blockMesh", "blockMesh"});
    if (!model.walls.empty()) {
        made.steps.push_back({"snappyHexMesh", "snappyHexMesh -overwrite"});
    }
    made.steps.push_back({"setFields", "setFields"});
    if (options.processes > 1) {
        made.steps.push_back({"decomposePar", "decomposePar"});
        made.steps.push_back({"interFoam",
                              "mpirun -np " + std::to_string(options.processes)
                                  + " interFoam -parallel",
                              true});
    }
    else {
        made.steps.push_back({"interFoam", "interFoam", true});
    }
    return true;
}

bool Fluid::Solver::OpenFoam::readSurfaceVtk(const std::string& text, Frame& frame)
{
    frame.points.clear();
    frame.triangles.clear();
    std::istringstream in(text);
    std::string word;
    while (in >> word) {
        if (word == "POINTS") {
            std::size_t count = 0;
            std::string type;
            if (!(in >> count >> type)) {
                return false;
            }
            frame.points.resize(count);
            for (auto& p : frame.points) {
                if (!(in >> p[0] >> p[1] >> p[2])) {
                    return false;
                }
            }
        }
        else if (word == "POLYGONS") {
            std::size_t count = 0;
            std::size_t size = 0;
            if (!(in >> count >> size)) {
                return false;
            }
            for (std::size_t i = 0; i < count; ++i) {
                int corners = 0;
                if (!(in >> corners) || corners < 3) {
                    return false;
                }
                std::vector<int> at(static_cast<std::size_t>(corners));
                for (int& corner : at) {
                    if (!(in >> corner) || corner < 0
                        || static_cast<std::size_t>(corner) >= frame.points.size()) {
                        return false;
                    }
                }
                for (std::size_t k = 1; k + 1 < at.size(); ++k) {
                    frame.triangles.push_back({at[0], at[k], at[k + 1]});
                }
            }
            return true;
        }
    }
    // A surface with no polygons is a frame with no fluid's surface in it,
    // when the file says so itself
    return text.find("POLYDATA") != std::string::npos
        && text.find("POINTS 0 ") != std::string::npos;
}

std::vector<std::vector<double>> Fluid::Solver::OpenFoam::readTable(const std::string& text)
{
    std::vector<std::vector<double>> rows;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        // a probe of a vector writes (x y z): the brackets are not numbers
        std::ranges::replace(line, '(', ' ');
        std::ranges::replace(line, ')', ' ');
        std::istringstream cells(line);
        std::vector<double> row;
        for (double v = 0.0; cells >> v;) {
            row.push_back(v);
        }
        if (!row.empty()) {
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

const std::vector<double>*
Fluid::Solver::OpenFoam::rowAt(const std::vector<std::vector<double>>& table,
                               double time,
                               double tolerance)
{
    const std::vector<double>* best = nullptr;
    double gap = tolerance;
    for (const auto& row : table) {
        const double d = std::abs(row.front() - time);
        if (d <= gap) {
            gap = d;
            best = &row;
        }
    }
    return best;
}

double Fluid::Solver::OpenFoam::lastTimeOf(const std::string& log)
{
    const auto at = log.rfind("\nTime = ");
    if (at == std::string::npos) {
        return -1.0;
    }
    try {
        return std::stod(log.substr(at + 8, 40));
    }
    catch (const std::exception&) {
        return -1.0;
    }
}
