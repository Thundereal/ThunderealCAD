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

#include <App/Document.h>
#include <App/DocumentObserver.h>
#include <App/FeaturePythonPyImp.h>
#include <App/GeoFeatureGroupExtension.h>
#include <App/Part.h>
#include <Base/Console.h>
#include <Mod/Part/App/FeatureElement.h>

#include "AssemblyObject.h"
#include "Joint.h"
#include "Physics.h"
#include "SpringDamperPy.h"

using namespace Assembly;

namespace
{
const char* endName(int side)
{
    return side == 1 ? "first" : "second";
}

void checkSide(int side)
{
    if (side != 1 && side != 2) {
        THROWM(Base::ValueError, "The end must be 1 or 2");
    }
}

// The object's placement in its assembly: its own and its containers', up to
// the assembly
Base::Placement placementIn(const App::DocumentObject* assembly, App::DocumentObject* obj)
{
    Base::Placement placement;
    for (auto* step = obj; step && step != assembly;
         step = App::GeoFeatureGroupExtension::getGroupOfObject(step)) {
        if (auto* prop = step->getPlacementProperty()) {
            placement = prop->getValue() * placement;
        }
    }
    return placement;
}
}  // namespace

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
    ADD_PROPERTY_TYPE(Binding1,
                      (nullptr),
                      "Spring",
                      App::Prop_None,
                      "The element binding of the first end: the end is the origin of the "
                      "element's frame,\nComponent1 and Point1 its output. None: the free "
                      "point they give");
    ADD_PROPERTY_TYPE(Binding2,
                      (nullptr),
                      "Spring",
                      App::Prop_None,
                      "The element binding of the second end: the end is the origin of the "
                      "element's frame,\nComponent2 and Point2 its output. None: the free "
                      "point they give");
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

App::Part* SpringDamper::getContainer() const
{
    // The Physics group is the assembly's: the assembly is the geometry group
    return Base::freecad_dynamic_cast<App::Part>(
        App::GeoFeatureGroupExtension::getGroupOfObject(this));
}

Part::ElementBinding* SpringDamper::getBinding(int side) const
{
    return Base::freecad_dynamic_cast<Part::ElementBinding>(
        (side == 1 ? Binding1 : Binding2).getValue());
}

App::DocumentObject*
SpringDamper::getEnd(int side, Base::Vector3d& point, Base::Placement* placement) const
{
    point = (side == 1 ? Point1 : Point2).getValue();
    if (auto binding = getBinding(side)) {
        Base::Placement plc;
        auto component = Joint::componentOfBinding(getContainer(), binding, &plc);
        if (placement) {
            *placement = plc;
        }
        return component;
    }
    auto component = (side == 1 ? Component1 : Component2).getValue();
    if (placement) {
        *placement = component ? placementIn(getContainer(), component) : Base::Placement();
    }
    return component;
}

Base::Vector3d SpringDamper::getEndPoint(int side) const
{
    Base::Vector3d point;
    Base::Placement placement;
    getEnd(side, point, &placement);
    Base::Vector3d res;
    placement.multVec(point, res);
    return res;
}

Part::ElementBinding* SpringDamper::setEnd(int side,
                                           App::DocumentObject* component,
                                           const char* subname,
                                           const char* vertex)
{
    checkSide(side);
    auto assembly = Base::freecad_dynamic_cast<AssemblyObject>(getContainer());
    if (!assembly) {
        FC_THROWM(Base::RuntimeError, getFullName() << " is in no assembly");
    }
    auto binding = Joint::makeSide(assembly, component, subname, vertex);
    clearEnd(side);
    (side == 1 ? Binding1 : Binding2).setValue(binding);
    // The point follows at the next recompute: taken now, so that the end is
    // where it goes before that
    const JointSide end = Joint::resolveBinding(assembly, binding, Base::Placement());
    if (end.isValid()) {
        (side == 1 ? Point1 : Point2).setValue((end.placement.inverse() * end.frame).getPosition());
    }
    return binding;
}

void SpringDamper::clearEnd(int side)
{
    checkSide(side);
    auto old = getBinding(side);
    if (!old) {
        return;
    }
    // The free point is where the end is
    Base::Vector3d point;
    auto component = getEnd(side, point);
    (side == 1 ? Binding1 : Binding2).setValue(nullptr);
    (side == 1 ? Component1 : Component2).setValue(component);
    (side == 1 ? Point1 : Point2).setValue(point);
    if (!old->isRemoving() && !Joint::isBindingUsedByAnother(old, this)) {
        Joint::removeBinding(getContainer(), old);
    }
}

void SpringDamper::updateEditable()
{
    for (int side : {1, 2}) {
        const bool bound = (side == 1 ? Binding1 : Binding2).getValue() != nullptr;
        (side == 1 ? Component1 : Component2).setStatus(App::Property::ReadOnly, bound);
        (side == 1 ? Point1 : Point2).setStatus(App::Property::ReadOnly, bound);
    }
}

void SpringDamper::onChanged(const App::Property* prop)
{
    if (prop == &Binding1 || prop == &Binding2) {
        updateEditable();
        // The component the binding is on: set here, not by execute(), which
        // would change the links of the graph it is recomputed in
        const int side = prop == &Binding1 ? 1 : 2;
        auto binding = getBinding(side);
        auto& component = side == 1 ? Component1 : Component2;
        if (binding && !isRestoring() && !binding->isRestoring()) {
            auto on = Joint::componentOfBinding(getContainer(), binding);
            if (on && component.getValue() != on) {
                component.setValue(on);
            }
        }
    }
    PhysicsItem::onChanged(prop);
}

void SpringDamper::onDocumentRestored()
{
    updateEditable();
    PhysicsItem::onDocumentRestored();
}

short SpringDamper::mustExecute() const
{
    if (Binding1.isTouched() || Binding2.isTouched()) {
        return 1;
    }
    return PhysicsItem::mustExecute();
}

App::DocumentObjectExecReturn* SpringDamper::execute()
{
    for (int side : {1, 2}) {
        auto binding = getBinding(side);
        if (!binding) {
            continue;
        }
        // The stored copy of a binding that does not resolve now is placed
        // where the end was last
        auto& point = side == 1 ? Point1 : Point2;
        const JointSide end =
            Joint::resolveBinding(getContainer(), binding, Base::Placement(point.getValue(), {}));
        if (!end.isValid()) {
            return new App::DocumentObjectExecReturn(std::string("The ") + endName(side)
                                                     + " end does not resolve");
        }
        const Base::Vector3d local = (end.placement.inverse() * end.frame).getPosition();
        if (!point.getValue().IsEqual(local, 1e-12)) {
            point.setValue(local);
        }
    }
    return PhysicsItem::execute();
}

void SpringDamper::unsetupObject()
{
    // The spring owns its element bindings: they go with it
    auto assembly = getContainer();
    std::vector<App::DocumentObjectT> bindings;
    for (int side : {1, 2}) {
        auto binding = getBinding(side);
        if (binding && !binding->isRemoving() && !Joint::isBindingUsedByAnother(binding, this)) {
            bindings.emplace_back(binding);
        }
    }
    for (auto& bindingT : bindings) {
        auto binding = Base::freecad_dynamic_cast<Part::ElementBinding>(bindingT.getObject());
        if (binding && !binding->isRemoving()) {
            Joint::removeBinding(assembly, binding);
        }
    }
    PhysicsItem::unsetupObject();
}

PyObject* SpringDamper::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new SpringDamperPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
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
template<>
PyObject* Assembly::SpringDamperPython::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new App::FeaturePythonPyT<Assembly::SpringDamperPy>(this), true);
    }
    return Py::new_reference_to(PythonObject);
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
