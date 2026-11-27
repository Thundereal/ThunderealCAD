// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "Backend.h"

namespace Fluid::Solver
{

// Test data, not a simulation: the fluid's volumes sampled as particles at
// the model's resolution, each falling freely -- p0 + g t^2 / 2 -- until the
// space's box stops it, without the others or the walls. Every frame has a
// closed form, which is what the workbench's frame, its kept results and
// its playback are tested against before a solver is behind them
// (docs/FluidSimulation.md sec 12, S1). Never chosen for a study that names
// no backend.
class FluidExport FreeFallBackend: public Backend
{
public:
    static constexpr const char* Name = "FreeFall";

    std::string name() const override
    {
        return Name;
    }
    std::string label() const override
    {
        return "Free fall (test data)";
    }
    Capabilities capabilities() const override;
    std::unique_ptr<Session> createSession() const override;
};

}  // namespace Fluid::Solver
