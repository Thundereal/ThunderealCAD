// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>
#include <vector>

#include "Backend.h"

namespace Fluid::Solver
{

// The runner found on this machine.
struct SplishRunner
{
    bool found = false;
    std::string program;   // its path
    std::string version;   // the solver's, as the runner says it
    std::string searched;  // when not found: where was looked
};

// SPlisHSPlasH (MIT), particles: the fluid as particles under a pressure
// solver that keeps their density (DFSPH), the walls as particles behind
// their faces (docs/FluidSimulation.md secs 10, 12 S3). Run by FluidSplish,
// a program of ours built with the solver: the solver's compiler settings
// and its Eigen are its own, and a run that dies takes no document with it.
// The program is looked for at the preference Mod/Fluid/SplishPath, the
// environment's FLUID_SPLISH_PATH, and beside this program.
class FluidExport SplishBackend: public Backend
{
public:
    static constexpr const char* Name = "SPlisHSPlasH";

    std::string name() const override
    {
        return Name;
    }
    std::string label() const override
    {
        return "SPlisHSPlasH (particles, DFSPH)";
    }
    Capabilities capabilities() const override;
    Availability availability() const override;
    std::unique_ptr<Session> createSession() const override;

    // The runner, looked for once and kept; again after refresh().
    static const SplishRunner& runner();
    static void refresh();
    // Where the runner is looked for, in order.
    static std::vector<std::string> places();
};

}  // namespace Fluid::Solver
