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

// The assembly's Physics items (docs/AssemblyDynamics.md sec 5.2): the
// elements of the mechanism that studies share, shown in the assembly's
// Physics group (docs/AssemblyObjectModel.md sec 3).

#pragma once

#include <App/DocumentObject.h>
#include <App/FeaturePython.h>
#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace Assembly
{

/// What the assembly's Physics group shows
class AssemblyExport PhysicsItem: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::PhysicsItem);

public:
    PhysicsItem() = default;
};

/** A component's mass properties given, in place of those from its shape and
 * material (MassProperties): its density; its mass, the centre and the
 * inertia still from its shape; or all of them.
 */
class AssemblyExport MassOverride: public PhysicsItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::MassOverride);

public:
    MassOverride();

    /// The component it gives the mass properties of
    App::PropertyLink Component;
    /// Density: the shape at this density; Mass: the shape, scaled to this
    /// mass; Full: this mass, centre of mass and inertia
    App::PropertyEnumeration Mode;
    App::PropertyDensity Density;
    App::PropertyMass Mass;
    /// Full: the centre of mass, in the component's coordinates
    App::PropertyVectorDistance CenterOfMass;
    /// Full: the inertia about the centre of mass, along the component's
    /// axes: xx, yy, zz, xy, xz, yz, kg mm^2 (xy is minus the integral of x
    /// y dm)
    App::PropertyFloatList Inertia;

    static const char* ModeEnums[];

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

using MassOverridePython = App::FeaturePythonT<MassOverride>;

}  // namespace Assembly
