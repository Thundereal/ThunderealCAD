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

PROPERTY_SOURCE(Assembly::SpringDamper, Assembly::PhysicsItem)

SpringDamper::SpringDamper()
{
    ADD_PROPERTY_TYPE(Component1,
                      (nullptr),
                      "Spring",
                      App::Prop_None,
                      "The component the first end is on; none: the ground");
    ADD_PROPERTY_TYPE(Point1,
                      (Base::Vector3d()),
                      "Spring",
                      App::Prop_None,
                      "The first end, in Component1's coordinates (the assembly's without it)");
    ADD_PROPERTY_TYPE(Component2,
                      (nullptr),
                      "Spring",
                      App::Prop_None,
                      "The component the second end is on; none: the ground");
    ADD_PROPERTY_TYPE(Point2,
                      (Base::Vector3d()),
                      "Spring",
                      App::Prop_None,
                      "The second end, in Component2's coordinates (the assembly's without it)");
    ADD_PROPERTY_TYPE(Stiffness, (0.0), "Spring", App::Prop_None, "The spring's stiffness");
    ADD_PROPERTY_TYPE(FreeLength,
                      (0.0),
                      "Spring",
                      App::Prop_None,
                      "The length at which the spring neither pushes nor pulls; 0: the length "
                      "when the run starts");
    ADD_PROPERTY_TYPE(Preload,
                      (0.0),
                      "Spring",
                      App::Prop_None,
                      "A constant force pushing the ends apart (negative: pulling them together)");
    ADD_PROPERTY_TYPE(Damping,
                      (0.0),
                      "Spring",
                      App::Prop_None,
                      "The damper's force over the ends' speed apart, kg/s (N s / m)");
    // kg/s: mass over time
    Damping.setUnit(Base::Unit(0, 1, -1));
}

PROPERTY_SOURCE(Assembly::TorsionSpring, Assembly::PhysicsItem)

TorsionSpring::TorsionSpring()
{
    ADD_PROPERTY_TYPE(Joint,
                      (nullptr),
                      "Spring",
                      App::Prop_None,
                      "The Revolute or Cylindrical joint whose turn the spring resists");
    ADD_PROPERTY_TYPE(Stiffness,
                      (0.0),
                      "Spring",
                      App::Prop_None,
                      "The torque per radian of turn (1 N*m is 1 N m per radian)");
    // A torque: mass length^2 / time^2, per radian
    Stiffness.setUnit(Base::Unit(2, 1, -2));
    ADD_PROPERTY_TYPE(FreeAngle,
                      (0.0),
                      "Spring",
                      App::Prop_None,
                      "The joint's turn, from where the run starts, at which the spring "
                      "neither turns it one way nor the other");
    ADD_PROPERTY_TYPE(Preload,
                      (0.0),
                      "Spring",
                      App::Prop_None,
                      "A constant torque turning the joint the positive way (negative: the "
                      "other)");
    Preload.setUnit(Base::Unit(2, 1, -2));
    ADD_PROPERTY_TYPE(Damping,
                      (0.0),
                      "Spring",
                      App::Prop_None,
                      "The damper's torque per radian per second of turning (1 N*m*s is 1 N m "
                      "s per radian)");
    Damping.setUnit(Base::Unit(2, 1, -1));
}

namespace App
{
PROPERTY_SOURCE_TEMPLATE(Assembly::TorsionSpringPython, Assembly::TorsionSpring)
template<>
const char* Assembly::TorsionSpringPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::TorsionSpring>;

PROPERTY_SOURCE_TEMPLATE(Assembly::SpringDamperPython, Assembly::SpringDamper)
template<>
const char* Assembly::SpringDamperPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::SpringDamper>;

PROPERTY_SOURCE_TEMPLATE(Assembly::MassOverridePython, Assembly::MassOverride)
template<>
const char* Assembly::MassOverridePython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::MassOverride>;
}  // namespace App
