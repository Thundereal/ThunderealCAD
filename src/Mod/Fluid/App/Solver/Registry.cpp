// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>

#include "Registry.h"

using namespace Fluid::Solver;

Registry& Registry::instance()
{
    static Registry registry;
    return registry;
}

void Registry::add(std::unique_ptr<Backend> backend)
{
    if (!backend) {
        return;
    }
    const std::string name = backend->name();
    for (auto& held : backends) {
        if (held->name() == name) {
            held = std::move(backend);
            return;
        }
    }
    backends.push_back(std::move(backend));
}

bool Registry::remove(std::string_view name)
{
    const auto it = std::ranges::find_if(backends, [&](const auto& held) {
        return held->name() == name;
    });
    if (it == backends.end()) {
        return false;
    }
    backends.erase(it);
    return true;
}

Backend* Registry::find(std::string_view name) const
{
    for (const auto& held : backends) {
        if (held->name() == name) {
            return held.get();
        }
    }
    return nullptr;
}

std::vector<std::string> Registry::names() const
{
    std::vector<std::string> names;
    for (const auto& held : backends) {
        names.push_back(held->name());
    }
    return names;
}

Backend* Registry::select(std::string_view name, Analysis analysis) const
{
    if (!name.empty()) {
        return find(name);
    }
    for (const auto& held : backends) {
        const Capabilities can = held->capabilities();
        if (analysis == Analysis::Steady && !can.steady) {
            continue;
        }
        if (analysis == Analysis::Unsteady && !can.unsteady) {
            continue;
        }
        if (!can.testData && held->availability().available) {
            return held.get();
        }
    }
    return nullptr;
}
