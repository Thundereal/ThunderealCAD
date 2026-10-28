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

#include "Physics.h"

using namespace Assembly;

PROPERTY_SOURCE(Assembly::PhysicsItem, App::DocumentObject)

PROPERTY_SOURCE(Assembly::MassOverride, Assembly::PhysicsItem)

const char* MassOverride::ModeEnums[] = {"Density", "Mass", "Full", nullptr};

MassOverride::MassOverride()
{
    ADD_PROPERTY_TYPE(Component,
                      (nullptr),
                      "Mass",
                      App::Prop_None,
                      "The component whose mass properties this gives");
    ADD_PROPERTY_TYPE(Mode,
                      (0L),
                      "Mass",
                      App::Prop_None,
                      "Density: the shape at this density. Mass: the shape, scaled to this "
                      "mass.\nFull: this mass, centre of mass and inertia.");
    Mode.setEnums(ModeEnums);
    // Aluminium's, the default material's
    ADD_PROPERTY_TYPE(Density, (2.7e-6), "Mass", App::Prop_None, "Density: the density");
    ADD_PROPERTY_TYPE(Mass, (1.0), "Mass", App::Prop_None, "Mass, Full: the mass");
    ADD_PROPERTY_TYPE(CenterOfMass,
                      (Base::Vector3d()),
                      "Mass",
                      App::Prop_None,
                      "Full: the centre of mass, in the component's coordinates");
    ADD_PROPERTY_TYPE(Inertia,
                      (0.0),
                      "Mass",
                      App::Prop_None,
                      "Full: the inertia about the centre of mass, along the component's "
                      "axes:\nxx, yy, zz, xy, xz, yz in kg mm^2 (xy is minus the integral of "
                      "x y dm)");
    Inertia.setValues({0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
}

namespace App
{
PROPERTY_SOURCE_TEMPLATE(Assembly::MassOverridePython, Assembly::MassOverride)
template<>
const char* Assembly::MassOverridePython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::MassOverride>;
}  // namespace App
