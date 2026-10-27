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

#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>
#include <Mod/Part/App/TopoShape.h>

namespace App
{
class DocumentObject;
}

namespace Assembly
{

class AssemblyObject;

// The static interference check (docs/AssemblyInterference.md): which
// components of an assembly overlap, and, when asked, which touch.
struct InterferenceOptions
{
    enum class Mode
    {
        // On the B-rep: the volume the two solids have in common, the exact
        // distance between them. Authoritative, and as slow as a boolean.
        Exact,
        // On meshes of the solids: their surfaces crossing, by more than the
        // tolerance, confirmed by a point near the crossing inside both solids
        // (two exact point classifications). Within the meshes' error, which
        // the tolerance covers: it is taken at least twice the meshes'
        // deflection.
        Fast
    };
    Mode mode = Mode::Exact;
    // mm. Two components whose surfaces are within this of each other touch
    // (a contact), and do not interfere unless they overlap: in Exact by a
    // volume, in Fast by a crossing deeper than this.
    double tolerance = 0.0;
    // Report the pairs that touch too, not only those that interfere.
    bool contacts = false;
    // mm, Fast only: the meshes' chord error; 0 is 1e-4 of each shape's
    // bounding box diagonal.
    double deflection = 0.0;
};

struct InterferenceResult
{
    enum class Kind
    {
        Interference,
        Contact
    };
    App::DocumentObject* first = nullptr;
    App::DocumentObject* second = nullptr;
    Kind kind = Kind::Interference;
    // Exact: the volume in common (mm^3); 0 in Fast, which only finds that
    // they overlap
    double volume = 0.0;
    // Contact: the distance between them (mm); Exact measures it, Fast only
    // says it is within the tolerance and gives that
    double distance = 0.0;
    // The tolerance the pair was judged with (Fast: at least twice the
    // deflection)
    double tolerance = 0.0;
    // Exact interference: the solid in common, in the assembly's coordinates
    Part::TopoShape overlap;
};

// The components of the assembly (getAssemblyComponents()), each with its
// shape in the assembly's coordinates; those without a solid are left out.
// Pairs in the components' order.
AssemblyExport std::vector<InterferenceResult> findInterferences(
    const AssemblyObject* assembly,
    const InterferenceOptions& options,
    const std::vector<App::DocumentObject*>& only = {}
);

}  // namespace Assembly
