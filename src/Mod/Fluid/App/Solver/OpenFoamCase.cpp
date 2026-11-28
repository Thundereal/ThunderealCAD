// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
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
           "    minMedialAxisAngle 90;\n    nBufferCellsNoExtrude 0;\n    nLayerIter 50;\n}\n"
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

// The triangles of a surface that are of one patch, as a surface of its
// own; its points are the whole surface's.
Surface patchOf(const Surface& surface, int patch)
{
    Surface part;
    part.points = surface.points;
    for (std::size_t i = 0; i < surface.triangles.size(); ++i) {
        if ((i < surface.patches.size() ? surface.patches[i] : 0) == patch) {
            part.triangles.push_back(surface.triangles[i]);
        }
    }
    return part;
}

// A steady flow's schemes and solvers: what the pipe of sec 14.2 was
// measured with.
constexpr const char* steadySchemes =
    "ddtSchemes { default steadyState; }\ngradSchemes { default Gauss linear; }\n"
    "divSchemes\n{\n    default none;\n"
    "    div(phi,U) bounded Gauss linearUpwind grad(U);\n"
    "    div((nuEff*dev2(T(grad(U))))) Gauss linear;\n}\n"
    "laplacianSchemes { default Gauss linear corrected; }\n"
    "interpolationSchemes { default linear; }\nsnGradSchemes { default corrected; }\n";

constexpr const char* steadySolution =
    "solvers\n{\n"
    "    p { solver GAMG; smoother GaussSeidel; tolerance 1e-8; relTol 0.05; }\n"
    "    U { solver smoothSolver; smoother symGaussSeidel; tolerance 1e-9; relTol 0.1; }\n"
    "}\nSIMPLE\n{\n    nNonOrthogonalCorrectors 1;\n    consistent yes;\n"
    "    residualControl { p 1e-7; U 1e-8; }\n}\n"
    "relaxationFactors { equations { U 0.9; } }\n";

// Through time (sec 14.11): second order in time, the convection not held
// bounded -- a street of eddies is what there is to see; two pressure
// corrections a step.
constexpr const char* timeSchemes =
    "ddtSchemes { default backward; }\ngradSchemes { default Gauss linear; }\n"
    "divSchemes\n{\n    default none;\n"
    "    div(phi,U) Gauss linearUpwind grad(U);\n"
    "    div((nuEff*dev2(T(grad(U))))) Gauss linear;\n}\n"
    "laplacianSchemes { default Gauss linear corrected; }\n"
    "interpolationSchemes { default linear; }\nsnGradSchemes { default corrected; }\n";

constexpr const char* timeSolution =
    "solvers\n{\n"
    "    p { solver GAMG; smoother GaussSeidel; tolerance 1e-7; relTol 0.01; }\n"
    "    pFinal { $p; relTol 0; }\n"
    "    \"(U|UFinal)\" { solver smoothSolver; smoother symGaussSeidel; tolerance 1e-8; "
    "relTol 0; }\n"
    "}\nPIMPLE\n{\n    nOuterCorrectors 1;\n    nCorrectors 2;\n"
    "    nNonOrthogonalCorrectors 1;\n}\n";

constexpr const char* slideName = "slide";

// With the eddies modelled (sec 14.6): k and omega carried upwind, the
// distance to the wall their model asks for, and a stop the turbulent pipes
// had long settled by.
constexpr const char* eddySchemes =
    "ddtSchemes { default steadyState; }\ngradSchemes { default Gauss linear; }\n"
    "divSchemes\n{\n    default none;\n"
    "    div(phi,U) bounded Gauss linearUpwind grad(U);\n"
    "    div(phi,k) bounded Gauss upwind;\n    div(phi,omega) bounded Gauss upwind;\n"
    "    div((nuEff*dev2(T(grad(U))))) Gauss linear;\n}\n"
    "laplacianSchemes { default Gauss linear corrected; }\n"
    "interpolationSchemes { default linear; }\nsnGradSchemes { default corrected; }\n"
    "wallDist { method meshWave; }\n";

constexpr const char* eddySolution =
    "solvers\n{\n"
    "    p { solver GAMG; smoother GaussSeidel; tolerance 1e-8; relTol 0.05; }\n"
    "    \"(U|k|omega)\" { solver smoothSolver; smoother symGaussSeidel; tolerance 1e-9; "
    "relTol 0.1; }\n"
    "}\nSIMPLE\n{\n    nNonOrthogonalCorrectors 1;\n    consistent yes;\n"
    "    residualControl { p 1e-5; U 1e-6; \"(k|omega)\" 1e-6; }\n}\n"
    "relaxationFactors { equations { \".*\" 0.9; } }\n";

// What comes in at an inlet is taken as stirred a twentieth of its speed,
// in eddies 7% of the inlet's width across: a pipe's flow, come from
// upstream.
double eddyEnergy(const Inlet& inlet)
{
    const double stirred = 0.05 * inlet.speed;
    return 1.5 * stirred * stirred;
}

double eddyRate(const Model& model, std::size_t inlet)
{
    const double width = Fluid::Solver::widthOf(model, inlet);
    return width > 0.0 ? std::sqrt(eddyEnergy(model.inlets[inlet]))
            / (std::sqrt(std::sqrt(0.09)) * 0.07 * width)
                       : 0.0;
}

bool steadyCase(const Model& model, const CaseOptions& options, Case& made, std::string& why)
{
    const bool unsteady = model.analysis == Analysis::Unsteady;
    if (model.resolution <= 0.0 || model.iterations <= 0) {
        why = "the resolution and the count of steps must be positive";
        return false;
    }
    if (unsteady && (model.timeEnd <= 0.0 || model.outputInterval <= 0.0)) {
        why = "the end time and the time between frames must be positive";
        return false;
    }
    if (unsteady && model.turbulence != Turbulence::Laminar) {
        why = "a flow through time with its eddies modelled is not run yet";
        return false;
    }
    if (model.inlets.empty()) {
        why = "a steady flow needs an inlet";
        return false;
    }
    if (model.outlets.empty()) {
        why = "a steady flow needs an outlet";
        return false;
    }
    Material liquid;
    liquidOf(model, liquid);
    if (!(liquid.density > 0.0) || !(liquid.viscosity > 0.0)) {
        why = "the fluid's density and viscosity must be positive";
        return false;
    }
    if (model.turbulence == Turbulence::KOmegaSST) {
        for (std::size_t i = 0; i < model.inlets.size(); ++i) {
            if (!(widthOf(model, i) > 0.0)) {
                why = "the fluid's space has nothing of the inlet, to size the eddies that "
                      "come in by: "
                    + model.inlets[i].source;
                return false;
            }
        }
    }
    Vec3 low {};
    Vec3 high {};
    if (model.domain.triangles.empty() || !boxOf(model.domain, low, high)) {
        why = "the study has no space for the fluid: no closed space inside its walls";
        return false;
    }
    for (const auto& monitor : model.monitors) {
        if (monitor.kind == MonitorKind::Height) {
            why = "a steady flow has no surface to take the height of: " + monitor.source;
            return false;
        }
        if (monitor.kind == MonitorKind::Force) {
            why = "the force on a part is not reported of a steady flow yet: " + monitor.source;
            return false;
        }
    }
    // The background mesh: the fluid's box and a cell and a half about it,
    // so that no face of the fluid lies on its side
    const double cell = model.resolution;
    Model around = model;
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(high[i] > low[i])) {
            why = "the study has no space for the fluid: it has no size";
            return false;
        }
        around.space.min[i] = low[i] - 1.5 * cell;
        around.space.max[i] = high[i] + 1.5 * cell;
        made.cells[i] =
            std::max(std::lround((around.space.max[i] - around.space.min[i]) / cell), 1L);
    }
    around.space.open = {};
    // Where the mesh is: a point of the fluid off every cell's face and
    // centre, and still inside
    bool found = false;
    const auto points = sampled(model.domain, cell);
    for (std::size_t k = 0; k < points.size() && !found; ++k) {
        const Vec3& p = points[(points.size() / 2 + k) % points.size()];
        // in a cell of the background mesh that is the fluid's all round
        // its centre: the mesher keeps what it reaches from here, and from
        // a cell a wall cuts through the middle it reaches nothing. The
        // background's cells have their centres on the box's own faces.
        Vec3 centre {};
        for (std::size_t i = 0; i < 3; ++i) {
            centre[i] = around.space.min[i]
                + (std::floor((p[i] - around.space.min[i]) / cell) + 0.5) * cell;
        }
        bool clear = inside(model.domain, centre);
        for (std::size_t i = 0; i < 3 && clear; ++i) {
            for (double way : {-0.45, 0.45}) {
                Vec3 beside = centre;
                beside[i] += way * cell;
                clear = clear && inside(model.domain, beside);
            }
        }
        const Vec3 moved {centre[0] + 0.137 * cell,
                          centre[1] + 0.151 * cell,
                          centre[2] + 0.173 * cell};
        if (clear && inside(model.domain, moved)) {
            made.inside = moved;
            found = true;
        }
    }
    if (!found) {
        why = "the fluid's space holds no cell at this resolution";
        return false;
    }
    const std::size_t openings = model.inlets.size() + model.outlets.size();
    std::vector<std::string> patches {"wall"};
    for (std::size_t k = 0; k < openings; ++k) {
        patches.push_back(openingName(model, k));
    }
    // what the fluid slides along, after the openings as its patch is
    const bool slides = anySlide(model);
    if (slides) {
        patches.emplace_back(slideName);
    }
    auto& files = made.files;
    files["system/blockMeshDict"] = blockMeshDict(around, made.cells);
    std::ostringstream snappy;
    snappy << "castellatedMesh true;\nsnap true;\naddLayers false;\ngeometry\n{\n";
    for (std::size_t k = 0; k < patches.size(); ++k) {
        const std::string& name = patches[k];
        files["constant/triSurface/" + name + ".stl"] =
            stlOf(patchOf(model.domain, static_cast<int>(k)), name);
        snappy << "    " << name << " { type triSurfaceMesh; file \"" << name << ".stl\"; }\n";
    }
    snappy << "}\ncastellatedMeshControls\n{\n"
              "    maxLocalCells 4000000;\n    maxGlobalCells 16000000;\n"
              "    minRefinementCells 0;\n    nCellsBetweenLevels 2;\n    features ();\n"
              "    refinementSurfaces\n    {\n";
    for (std::size_t k = 0; k < patches.size(); ++k) {
        snappy << "        " << patches[k] << " { level (0 0); patchInfo { type "
               << (k == 0 ? "wall" : "patch") << "; } }\n";
    }
    snappy << "    }\n    resolveFeatureAngle 30;\n    refinementRegions {}\n    locationInMesh "
           << vec(made.inside)
           << ";\n    allowFreeStandingZoneFaces false;\n}\n"
              "snapControls\n{\n    nSmoothPatch 3;\n    tolerance 2.0;\n    nSolveIter 30;\n"
              "    nRelaxIter 5;\n    nFeatureSnapIter 10;\n    implicitFeatureSnap true;\n"
              "    explicitFeatureSnap false;\n    multiRegionFeatureSnap true;\n}\n"
              "addLayersControls\n{\n    relativeSizes true;\n    layers {}\n"
              "    expansionRatio 1.0;\n    finalLayerThickness 0.3;\n    minThickness 0.1;\n"
              "    nGrow 0;\n    featureAngle 60;\n    nRelaxIter 3;\n"
              "    nSmoothSurfaceNormals 1;\n    nSmoothNormals 3;\n    nSmoothThickness 10;\n"
              "    maxFaceThicknessRatio 0.5;\n    maxThicknessToMedialRatio 0.3;\n"
              "    minMedialAxisAngle 90;\n    nBufferCellsNoExtrude 0;\n    nLayerIter 50;\n}\n"
              "meshQualityControls\n{\n    maxNonOrtho 65;\n    maxBoundarySkewness 20;\n"
              "    maxInternalSkewness 4;\n    maxConcave 80;\n    minVol 1e-13;\n"
              "    minTetQuality 1e-15;\n    minArea -1;\n    minTwist 0.02;\n"
              "    minDeterminant 0.001;\n    minFaceWeight 0.05;\n    minVolRatio 0.01;\n"
              "    minTriangleTwist -1;\n    nSmoothScale 4;\n    errorReduction 0.75;\n}\n"
              "mergeTolerance 1e-6;\n";
    files["system/snappyHexMeshDict"] = dictionary("snappyHexMeshDict", snappy.str());

    // The fields are written each some steps, to go on from, and the faces'
    // pressure goes with them: a frame of the flow as it settles, and one
    // at its end -- the solver writes when it has settled
    const int every = std::max(model.iterations / 40, 1);
    const std::string each = "        writeControl timeStep;\n        writeInterval 1;\n";
    std::ostringstream control;
    if (unsteady) {
        // Through time: the step is the solver's own, as long as the fluid
        // crosses no more than a cell in it, and starts as if it came in
        // at the fastest inlet's speed
        double fastest = 0.0;
        for (const auto& inlet : model.inlets) {
            fastest = std::max(fastest, inlet.speed);
        }
        const double first =
            std::min(fastest > 0.0 ? 0.2 * cell / fastest : model.outputInterval,
                     model.outputInterval);
        control << "application pimpleFoam;\nstartFrom startTime;\nstartTime 0;\n"
                << "stopAt endTime;\nendTime " << num(model.timeEnd) << ";\ndeltaT "
                << num(first) << ";\nadjustTimeStep yes;\nmaxCo 1;\nmaxDeltaT "
                << num(model.outputInterval)
                << ";\nwriteControl adjustable;\nwriteInterval " << num(model.outputInterval)
                << ";\n";
    }
    else {
        control << "application simpleFoam;\nstartFrom startTime;\nstartTime 0;\n"
                << "stopAt endTime;\nendTime " << model.iterations
                << ";\ndeltaT 1;\nwriteControl timeStep;\nwriteInterval " << every << ";\n";
    }
    control << "purgeWrite 1;\nwriteFormat binary;\nwritePrecision 8;\ntimeFormat general;\n"
            << "timePrecision 10;\nrunTimeModifiable no;\nfunctions\n{\n"
            << "    skin\n    {\n        type surfaces;\n        libs (sampling);\n"
               "        writeControl writeTime;\n        surfaceFormat vtk;\n"
               "        formatOptions { vtk { format ascii; legacy true; } }\n"
               "        fields (p);\n        interpolationScheme cellPoint;\n"
               "        surfaces\n        {\n";
    for (const auto& name : patches) {
        control << "            " << name << " { type patch; patches (" << name
                << "); interpolate true; }\n";
    }
    control << "        }\n    }\n";
    for (std::size_t k = 1; k < patches.size(); ++k) {
        for (const char* what : {"P", "Q"}) {
            control << "    " << patches[k] << what
                    << "\n    {\n        type surfaceFieldValue;\n"
                       "        libs (fieldFunctionObjects);\n"
                    << each << "        fields (" << (*what == 'P' ? "p" : "phi")
                    << ");\n        operation " << (*what == 'P' ? "areaAverage" : "sum")
                    << ";\n        regionType patch;\n        name " << patches[k]
                    << ";\n        writeFields no;\n    }\n";
        }
    }
    std::string probes;
    for (const auto& monitor : model.monitors) {
        probes += " " + vec(monitor.point);
    }
    if (!probes.empty()) {
        control << "    pressure\n    {\n        type probes;\n        libs (sampling);\n"
                << each << "        fields (p);\n        probeLocations (" << probes
                << " );\n    }\n";
    }
    // The fluid inside, each cell's own value at its centre: once, of a
    // steady flow as it is left; with every frame of one through time
    control << "    inside\n    {\n        type sets;\n        libs (sampling);\n"
            << "        writeControl " << (unsteady ? "writeTime" : "onEnd")
            << ";\n        setFormat raw;\n"
               "        interpolationScheme cell;\n        fields (p U);\n"
               "        sets { cells { type cellCentre; } }\n    }\n";
    const bool eddies = model.turbulence == Turbulence::KOmegaSST;
    if (eddies) {
        // how far the first cell at the wall is from it, in the wall's own
        // units: what says whether the wall functions were in their range
        control << "    yPlus\n    {\n        type yPlus;\n        libs (fieldFunctionObjects);\n"
                   "        writeControl onEnd;\n    }\n";
    }
    control << "}\n";
    files["system/controlDict"] = dictionary("controlDict", control.str());
    files["system/fvSchemes"] = dictionary(
        "fvSchemes",
        unsteady ? timeSchemes : (eddies ? eddySchemes : steadySchemes));
    files["system/fvSolution"] = dictionary(
        "fvSolution",
        unsteady ? timeSolution : (eddies ? eddySolution : steadySolution));
    files["system/decomposeParDict"] = dictionary(
        "decomposeParDict",
        "numberOfSubdomains " + std::to_string(std::max(options.processes, 1))
            + ";\nmethod scotch;\n");
    files["constant/transportProperties"] = dictionary(
        "transportProperties",
        "transportModel Newtonian;\nnu " + num(liquid.viscosity / liquid.density) + ";\n");
    files["constant/turbulenceProperties"] = dictionary(
        "turbulenceProperties",
        eddies ? "simulationType RAS;\nRAS { RASModel kOmegaSST; turbulence on; printCoeffs off; }\n"
               : "simulationType laminar;\n");

    std::string velocity = header("volVectorField", "U")
        + "dimensions [0 1 -1 0 0 0 0];\ninternalField uniform (0 0 0);\nboundaryField\n{\n";
    std::string pressure = header("volScalarField", "p")
        + "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n";
    for (std::size_t i = 0; i < model.inlets.size(); ++i) {
        const Inlet& inlet = model.inlets[i];
        const std::string name = openingName(model, i);
        velocity += "    " + name + " { type fixedValue; value uniform "
            + vec({inlet.direction[0] * inlet.speed,
                   inlet.direction[1] * inlet.speed,
                   inlet.direction[2] * inlet.speed})
            + "; }\n";
        pressure += "    " + name + " { type zeroGradient; }\n";
    }
    for (std::size_t j = 0; j < model.outlets.size(); ++j) {
        const std::string name = openingName(model, model.inlets.size() + j);
        // through time an eddy may reach the outlet and draw fluid back
        // in: that comes at rest
        velocity += "    " + name
            + (unsteady ? " { type inletOutlet; inletValue uniform (0 0 0); "
                          "value uniform (0 0 0); }\n"
                        : " { type zeroGradient; }\n");
        pressure += "    " + name + " { type fixedValue; value uniform "
            + num(model.outlets[j].pressure / liquid.density) + "; }\n";
    }
    // What the fluid slides along is a plain patch with the sliding said in
    // the fields: a patch of the mesher's 'symmetry' type is not drawn
    // onto its surface -- measured, a channel 3 cells deep came out 2
    const auto slid = [&](const char* how) {
        return slides ? "    " + std::string(slideName) + " { type " + how + "; }\n"
                      : std::string();
    };
    files["0/U"] = velocity + slid("slip") + "    \".*\" { type noSlip; }\n}\n";
    files["0/p"] = pressure + slid("zeroGradient") + "    \".*\" { type zeroGradient; }\n}\n";
    if (eddies) {
        // the fluid starts as the first inlet's comes; what flows back in
        // at an outlet is taken as that too
        const std::string energy = num(eddyEnergy(model.inlets.front()));
        const std::string rate = num(eddyRate(model, 0));
        const auto start = [](const char* name, const char* dimensions, const std::string& all) {
            return header("volScalarField", name) + "dimensions " + dimensions
                + ";\ninternalField uniform " + all + ";\nboundaryField\n{\n";
        };
        std::string k = start("k", "[0 2 -2 0 0 0 0]", energy);
        std::string omega = start("omega", "[0 0 -1 0 0 0 0]", rate);
        std::string nut = start("nut", "[0 2 -1 0 0 0 0]", "0");
        for (std::size_t i = 0; i < model.inlets.size(); ++i) {
            const std::string name = openingName(model, i);
            k += "    " + name + " { type fixedValue; value uniform "
                + num(eddyEnergy(model.inlets[i])) + "; }\n";
            omega += "    " + name + " { type fixedValue; value uniform "
                + num(eddyRate(model, i)) + "; }\n";
            nut += "    " + name + " { type calculated; value uniform 0; }\n";
        }
        for (std::size_t j = 0; j < model.outlets.size(); ++j) {
            const std::string name = openingName(model, model.inlets.size() + j);
            k += "    " + name + " { type inletOutlet; inletValue uniform " + energy
                + "; value uniform " + energy + "; }\n";
            omega += "    " + name + " { type inletOutlet; inletValue uniform " + rate
                + "; value uniform " + rate + "; }\n";
            nut += "    " + name + " { type calculated; value uniform 0; }\n";
        }
        files["0/k"] = k + slid("zeroGradient") + "    \".*\" { type kqRWallFunction; value uniform " + energy + "; }\n}\n";
        files["0/omega"] =
            omega + slid("zeroGradient") + "    \".*\" { type omegaWallFunction; value uniform " + rate + "; }\n}\n";
        files["0/nut"] = nut
            + (slides ? "    " + std::string(slideName) + " { type calculated; value uniform 0; }\n"
                      : std::string())
            + "    \".*\" { type nutkWallFunction; value uniform 0; }\n}\n";
    }

    made.steps.push_back({"blockMesh", "blockMesh"});
    made.steps.push_back({"snappyHexMesh", "snappyHexMesh -overwrite"});
    const std::string solver = unsteady ? "pimpleFoam" : "simpleFoam";
    if (options.processes > 1) {
        made.steps.push_back({"decomposePar", "decomposePar"});
        made.steps.push_back({solver,
                              "mpirun -np " + std::to_string(options.processes) + " " + solver
                                  + " -parallel",
                              true});
    }
    else {
        made.steps.push_back({solver, solver, true});
    }
    return true;
}
}  // namespace

std::string Fluid::Solver::OpenFoam::openingName(const Model& model, std::size_t opening)
{
    return opening < model.inlets.size()
        ? "inlet" + std::to_string(opening)
        : "outlet" + std::to_string(opening - model.inlets.size());
}

Case Fluid::Solver::OpenFoam::ductCase(int axis)
{
    // a point of the duct along x, turned to lie along the axis
    const auto turned = [axis](const Vec3& p) -> Vec3 {
        switch (axis) {
            case 1:
                return {p[2], p[0], p[1]};
            case 2:
                return {p[1], p[2], p[0]};
            default:
                return p;
        }
    };
    Case made;
    made.cells = {50, 10, 10};
    std::ostringstream mesh;
    mesh << "scale 1;\nvertices\n(\n";
    for (int i = 0; i < 8; ++i) {
        const bool x = i == 1 || i == 2 || i == 5 || i == 6;
        const bool y = i == 2 || i == 3 || i == 6 || i == 7;
        const bool z = i >= 4;
        mesh << "    " << vec(turned({x ? 0.1 : 0.0, y ? 0.005 : -0.005, z ? 0.005 : -0.005}))
             << "\n";
    }
    mesh << ");\nblocks\n(\n    hex (0 1 2 3 4 5 6 7) (50 10 10) simpleGrading (1 1 1)\n);\n"
            "edges ();\nboundary\n(\n"
            "    inlet { type patch; faces ((0 4 7 3)); }\n"
            "    outlet { type patch; faces ((1 2 6 5)); }\n"
            "    wall { type wall; faces ((0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7)); }\n);\n";
    auto& files = made.files;
    files["system/blockMeshDict"] = dictionary("blockMeshDict", mesh.str());
    files["system/controlDict"] = dictionary(
        "controlDict",
        "application simpleFoam;\nstartFrom startTime;\nstartTime 0;\nstopAt endTime;\n"
        "endTime 500;\ndeltaT 1;\nwriteControl timeStep;\nwriteInterval 500;\npurgeWrite 1;\n"
        "writeFormat ascii;\nwritePrecision 8;\ntimeFormat general;\nrunTimeModifiable no;\n"
        "functions\n{\n    pressure\n    {\n        type probes;\n        libs (sampling);\n"
        "        writeControl timeStep;\n        writeInterval 1;\n        fields (p);\n"
        "        probeLocations ("
            + vec(turned({0.04, 0.0, 0.0})) + " " + vec(turned({0.09, 0.0, 0.0}))
            + ");\n    }\n}\n");
    files["system/fvSchemes"] = dictionary(
        "fvSchemes",
        "ddtSchemes { default steadyState; }\ngradSchemes { default Gauss linear; }\n"
        "divSchemes\n{\n    default none;\n"
        "    div(phi,U) bounded Gauss linearUpwind grad(U);\n"
        "    div((nuEff*dev2(T(grad(U))))) Gauss linear;\n}\n"
        "laplacianSchemes { default Gauss linear corrected; }\n"
        "interpolationSchemes { default linear; }\nsnGradSchemes { default corrected; }\n");
    files["system/fvSolution"] = dictionary(
        "fvSolution",
        "solvers\n{\n"
        "    p { solver PCG; preconditioner DIC; tolerance 1e-9; relTol 0.05; }\n"
        "    U { solver smoothSolver; smoother symGaussSeidel; tolerance 1e-10; relTol 0.1; }\n"
        "}\nSIMPLE\n{\n    nNonOrthogonalCorrectors 0;\n    consistent yes;\n"
        "    residualControl { p 1e-7; U 1e-8; }\n}\n"
        "relaxationFactors { equations { U 0.9; } }\n");
    files["constant/transportProperties"] =
        dictionary("transportProperties", "transportModel Newtonian;\nnu 0.0001;\n");
    files["constant/turbulenceProperties"] =
        dictionary("turbulenceProperties", "simulationType laminar;\n");
    files["0/U"] = header("volVectorField", "U")
        + "dimensions [0 1 -1 0 0 0 0];\ninternalField uniform (0 0 0);\nboundaryField\n{\n"
          "    inlet { type fixedValue; value uniform "
        + vec(turned({0.1, 0.0, 0.0}))
        + "; }\n    outlet { type zeroGradient; }\n    wall { type noSlip; }\n}\n";
    files["0/p"] = header("volScalarField", "p")
        + "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n"
          "    inlet { type zeroGradient; }\n"
          "    outlet { type fixedValue; value uniform 0; }\n"
          "    wall { type zeroGradient; }\n}\n";
    made.steps.push_back({"blockMesh", "blockMesh"});
    made.steps.push_back({"simpleFoam", "simpleFoam", true});
    return made;
}

double Fluid::Solver::OpenFoam::ductLoss(const std::string& table)
{
    const auto rows = readTable(table);
    if (rows.empty() || rows.back().size() < 3) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return rows.back()[1] - rows.back()[2];
}

bool Fluid::Solver::OpenFoam::sameLoss(double first, double second)
{
    if (!(first > 0.0) || !(second > 0.0) || !std::isfinite(first) || !std::isfinite(second)) {
        return false;
    }
    return std::abs(first / second - 1.0) <= 0.01;
}

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
    if (fillsItsSpace(model)) {
        return steadyCase(model, options, made, why);
    }
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

bool Fluid::Solver::OpenFoam::readFieldVtk(const std::string& text,
                                           const std::string& field,
                                           Frame& frame)
{
    frame.values.clear();
    if (!readSurfaceVtk(text, frame)) {
        return false;
    }
    const auto data = text.find("POINT_DATA");
    if (data == std::string::npos) {
        return false;
    }
    const auto at = text.find("\n" + field + " 1 ", data);
    if (at == std::string::npos) {
        return false;
    }
    std::istringstream in(text.substr(at + 1));
    std::string name;
    std::string type;
    int width = 0;
    std::size_t count = 0;
    if (!(in >> name >> width >> count >> type) || count != frame.points.size()) {
        return false;
    }
    frame.values.resize(count);
    for (double& value : frame.values) {
        if (!(in >> value)) {
            frame.values.clear();
            return false;
        }
    }
    // a number cut short reads as a shorter number: the file is whole only
    // when its last line is
    return !text.empty() && text.back() == '\n';
}

bool Fluid::Solver::OpenFoam::readWallUnits(const std::string& text, std::array<double, 3>& units)
{
    bool found = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        double step = 0.0;
        std::string patch;
        std::array<double, 3> read {};
        if ((row >> step >> patch >> read[0] >> read[1] >> read[2]) && patch == "wall"
            && std::isfinite(read[0]) && std::isfinite(read[1]) && std::isfinite(read[2])) {
            units = read;
            found = true;
        }
    }
    return found;
}

bool Fluid::Solver::OpenFoam::readInside(const std::string& text, Frame& frame)
{
    frame.centres.clear();
    frame.velocities.clear();
    frame.pressures.clear();
    if (text.empty() || text.back() != '\n') {
        return false;
    }
    for (const auto& row : readTable(text)) {
        if (row.size() != 7) {
            frame.centres.clear();
            frame.velocities.clear();
            frame.pressures.clear();
            return false;
        }
        frame.centres.push_back({row[0], row[1], row[2]});
        frame.pressures.push_back(row[3]);
        frame.velocities.push_back({row[4], row[5], row[6]});
    }
    return !frame.centres.empty();
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

std::size_t Fluid::Solver::OpenFoam::readSteps(const std::string& text,
                                               double& step,
                                               std::vector<std::pair<double, double>>& steps)
{
    std::size_t from = 0;
    while (true) {
        const std::size_t end = text.find('\n', from);
        if (end == std::string::npos) {
            return from;
        }
        const std::string line = text.substr(from, end - from);
        from = end + 1;
        try {
            if (line.starts_with("deltaT = ")) {
                step = std::stod(line.substr(9));
            }
            else if (line.starts_with("Time = ") && step > 0.0) {
                steps.emplace_back(std::stod(line.substr(7)), step);
            }
        }
        catch (const std::exception&) {
        }
    }
}

double Fluid::Solver::OpenFoam::stepAt(const std::vector<std::pair<double, double>>& steps,
                                       double time)
{
    // in the order they were taken
    const auto after = std::ranges::lower_bound(steps, time, {}, [](const auto& taken) {
        return taken.first;
    });
    if (after == steps.end()) {
        return steps.empty() ? 0.0 : steps.back().second;
    }
    if (after != steps.begin() && time - (after - 1)->first < after->first - time) {
        return (after - 1)->second;
    }
    return after->second;
}
