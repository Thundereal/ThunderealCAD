// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>
#include <vector>

#include "Backend.h"

namespace Fluid::Solver
{

// An OpenFOAM found on this machine, and how to call it.
struct OpenFoamInstall
{
    bool found = false;
    std::string where;     // what was found: the folder, or "the environment"
    std::string preamble;  // shell that makes its programs callable
    bool cleanEnvironment = true;  // run without this process's library paths
    std::string version;   // its API number, "2412"
    bool parallel = false;  // its programs can run under MPI
    std::string searched;   // when not found: where was looked, and why not
    // An install that was found and is not used: where, and what is wrong
    // with it. Empty when there is none.
    std::string refused;
};

// OpenFOAM, run as separate programs over a case's files
// (docs/FluidSimulation.md secs 5.5, 11.3, 12 S2, 14.4): a free surface by
// interFoam in volume-of-fluid cells, the walls meshed around by
// snappyHexMesh; a steady flow by simpleFoam in the space inside them. GPL
// code, never in this process and never shipped: it is looked for -- the
// preference Mod/Fluid/OpenFoamPath, the environment's FLUID_OPENFOAM_PATH
// and PATH, our own download's folder, the usual places -- and when it is not there the backend says
// where it looked.
class FluidExport OpenFoamBackend: public Backend
{
public:
    static constexpr const char* Name = "OpenFOAM";

    std::string name() const override
    {
        return Name;
    }
    std::string label() const override
    {
        return "OpenFOAM";
    }
    Capabilities capabilities() const override;
    Availability availability() const override;
    std::unique_ptr<Session> createSession() const override;

    // The install the backend runs on, looked for once and kept; again
    // after refresh().
    static const OpenFoamInstall& install();
    static void refresh();
    // What is wrong with the way an OpenFOAM made callable by that preamble
    // computes a viscous flow (docs/FluidSimulation.md sec 14.2): the same
    // duct run along x and along y, which must lose the same pressure.
    // Empty when it is right, and when the ducts could not be run -- an
    // install is not refused for what was not seen.
    static std::string viscousFault(const std::string& preamble, bool cleanEnvironment);
    // The same of the OpenFOAM in a folder, or made callable by a file;
    // empty as well when there is none there.
    static std::string faultAt(const std::string& place);
    // The folders an install is looked for in, after the preference and the
    // environment: our download's first.
    static std::vector<std::string> usualPlaces();
    // The folder our own download goes to.
    static std::string downloadFolder();
};

}  // namespace Fluid::Solver
