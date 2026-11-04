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

#pragma once

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

#include "Backend.h"

namespace App
{
class DocumentObject;
}

namespace Assembly::Solver
{

// The name every assembly falls back to.
inline constexpr std::string_view DefaultBackend = "Ondsel";

// The registered solver backends (docs/AssemblySolverInterface.md sec 4.3, 4.8).
class AssemblyExport Registry
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

    // The name an assembly asks for: its 'SolverBackend' property when set,
    // else the preference Mod/Assembly/SolverBackend, else DefaultBackend.
    static std::string requestedName(const App::DocumentObject* assembly);
    // The backend for that name. A name nobody registered warns once per name
    // and falls back to DefaultBackend, then to the first registered backend;
    // nullptr when the registry is empty.
    Backend* select(const App::DocumentObject* assembly) const;
    Backend* select(std::string_view name) const;

private:
    Registry() = default;

    std::vector<std::unique_ptr<Backend>> backends;
    mutable std::set<std::string, std::less<>> warnedNames;
};

}  // namespace Assembly::Solver
