// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "Backend.h"

namespace Fluid::Solver
{

// The registered fluid backends (docs/FluidSimulation.md sec 11.2).
class FluidExport Registry
{
public:
    static Registry& instance();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Replaces a backend of the same name.
    void add(std::unique_ptr<Backend> backend);
    // Removes the backend of that name; false when there was none.
    bool remove(std::string_view name);
    Backend* find(std::string_view name) const;
    // In registration order.
    std::vector<std::string> names() const;
    // The backend a study with that name set runs on: the one of that name;
    // for no name, the first registered that is no test data and can run
    // here. nullptr when there is none -- never another one in its place.
    Backend* select(std::string_view name) const;

private:
    Registry() = default;

    std::vector<std::unique_ptr<Backend>> backends;
};

}  // namespace Fluid::Solver
