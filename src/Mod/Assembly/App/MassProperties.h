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

#include <array>
#include <string>
#include <vector>

#include <Base/Vector3D.h>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace App
{
class DocumentObject;
}

namespace Assembly
{

class AssemblyObject;

/** The mass properties of a component, or of several together
 * (docs/AssemblyDynamics.md sec 5.2, phase D0), in the internal units: kg
 * and mm. The centre of mass and the inertia are in the assembly's
 * coordinates, where the component is now; the inertia is about the centre
 * of mass, the tensor's entries xx yy zz xy xz yz in kg mm^2 (xy is minus
 * the integral of x y dm, as OCCT's matrix of inertia has it).
 */
struct AssemblyExport MassProperties
{
    enum class Origin
    {
        // From its shape and its material's density
        Shape,
        // From its shape, with the default material's density: it has none,
        // or one of its pieces has none
        Assumed,
        // Given by a mass override
        Override,
        // No solid: no mass
        None
    };
    App::DocumentObject* obj = nullptr;
    Origin origin = Origin::None;
    // The material, its name; empty when its pieces have different ones
    std::string material;
    // kg/mm^3; 0 when its pieces have different ones
    double density = 0.0;
    // mm^3
    double volume = 0.0;
    // kg
    double mass = 0.0;
    Base::Vector3d com;
    std::array<double, 6> inertia {};

    /// Add another's: the masses and volumes summed, the inertias moved to
    /// the common centre of mass by the parallel-axis theorem
    void add(const MassProperties& other);
};

/** The density of the default material, for a part whose material has none:
 * the material the preference Mod/Assembly/DefaultMaterial names by its
 * UUID, Aluminum Generic (2700 kg/m^3) unless set. kg/mm^3; its name into
 * name.
 */
AssemblyExport double defaultDensity(std::string* name = nullptr);

/// The mass properties of each of the assembly's components
/// (getAssemblyComponents()), in that order
AssemblyExport std::vector<MassProperties> massPropertiesOf(const AssemblyObject* assembly);

/// The mass properties of these together: Assumed if any is
AssemblyExport MassProperties totalOf(const std::vector<MassProperties>& parts);

}  // namespace Assembly
