// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <algorithm>

#include <App/Application.h>
#include <App/DocumentObject.h>
#include <App/PropertyStandard.h>
#include <Base/Console.h>

#include "Registry.h"

using namespace Assembly::Solver;

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
    auto it = std::ranges::find_if(backends, [&](const auto& b) { return b->name() == name; });
    if (it != backends.end()) {
        *it = std::move(backend);
    }
    else {
        backends.push_back(std::move(backend));
    }
}

bool Registry::remove(std::string_view name)
{
    auto it = std::ranges::find_if(backends, [&](const auto& b) { return b->name() == name; });
    if (it == backends.end()) {
        return false;
    }
    backends.erase(it);
    return true;
}

Backend* Registry::find(std::string_view name) const
{
    auto it = std::ranges::find_if(backends, [&](const auto& b) { return b->name() == name; });
    return it == backends.end() ? nullptr : it->get();
}

std::vector<std::string> Registry::names() const
{
    std::vector<std::string> result;
    result.reserve(backends.size());
    for (const auto& backend : backends) {
        result.push_back(backend->name());
    }
    return result;
}

std::string Registry::requestedName(const App::DocumentObject* assembly)
{
    if (assembly) {
        const App::Property* prop = assembly->getPropertyByName("SolverBackend");
        std::string fromProperty;
        if (auto* propEnum = dynamic_cast<const App::PropertyEnumeration*>(prop)) {
            if (propEnum->isValid()) {
                fromProperty = propEnum->getValueAsString();
            }
        }
        else if (auto* propString = dynamic_cast<const App::PropertyString*>(prop)) {
            fromProperty = propString->getValue();
        }
        if (!fromProperty.empty()) {
            return fromProperty;
        }
    }

    ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );
    std::string fromPreference = hGrp->GetASCII("SolverBackend", "");
    if (!fromPreference.empty()) {
        return fromPreference;
    }
    return std::string(DefaultBackend);
}

Backend* Registry::select(const App::DocumentObject* assembly) const
{
    return select(requestedName(assembly));
}

Backend* Registry::select(std::string_view name) const
{
    if (Backend* backend = find(name)) {
        return backend;
    }
    if (backends.empty()) {
        return nullptr;
    }
    Backend* fallback = find(DefaultBackend);
    if (!fallback) {
        fallback = backends.front().get();
    }
    if (warnedNames.insert(std::string(name)).second) {
        Base::Console().warning(
            "Assembly: no solver backend named '%s'; using '%s'.\n",
            std::string(name).c_str(),
            fallback->name().c_str()
        );
    }
    return fallback;
}
