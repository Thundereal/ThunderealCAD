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
#include "AppliedLoadPy.h"
#include "FramedItemPy.h"
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

App::Part* PhysicsItem::getContainer() const
{
    // The Physics group is the assembly's: the assembly is the geometry group
    return Base::freecad_dynamic_cast<App::Part>(
        App::GeoFeatureGroupExtension::getGroupOfObject(this));
}

Part::ElementBinding* PhysicsItem::bindingOf(const App::PropertyLink& prop)
{
    return Base::freecad_dynamic_cast<Part::ElementBinding>(prop.getValue());
}

App::DocumentObject* PhysicsItem::endComponent(const App::PropertyLink& binding,
                                               const App::PropertyLink& component,
                                               Base::Placement* placement) const
{
    if (auto bound = bindingOf(binding)) {
        Base::Placement plc;
        auto on = Joint::componentOfBinding(getContainer(), bound, &plc);
        if (placement) {
            *placement = plc;
        }
        return on;
    }
    auto on = component.getValue();
    if (placement) {
        *placement = on ? placementIn(getContainer(), on) : Base::Placement();
    }
    return on;
}

Part::ElementBinding*
PhysicsItem::makeEndBinding(App::DocumentObject* component, const char* subname, const char* vertex)
{
    auto assembly = Base::freecad_dynamic_cast<AssemblyObject>(getContainer());
    if (!assembly) {
        FC_THROWM(Base::RuntimeError, getFullName() << " is in no assembly");
    }
    return Joint::makeSide(assembly, component, subname, vertex);
}

bool PhysicsItem::resolveEnd(Part::ElementBinding* binding,
                             const Base::Placement& last,
                             Base::Placement& local) const
{
    const JointSide end = Joint::resolveBinding(getContainer(), binding, last);
    if (!end.isValid()) {
        return false;
    }
    local = end.placement.inverse() * end.frame;
    return true;
}

void PhysicsItem::releaseBinding(Part::ElementBinding* binding)
{
    if (binding && !binding->isRemoving() && !Joint::isBindingUsedByAnother(binding, this)) {
        Joint::removeBinding(getContainer(), binding);
    }
}

void PhysicsItem::syncComponent(const App::PropertyLink& binding, App::PropertyLink& component)
{
    auto bound = bindingOf(binding);
    if (bound && !isRestoring() && !bound->isRestoring()) {
        auto on = Joint::componentOfBinding(getContainer(), bound);
        if (on && component.getValue() != on) {
            component.setValue(on);
        }
    }
}

void PhysicsItem::releaseBindings(std::initializer_list<const App::PropertyLink*> props)
{
    std::vector<App::DocumentObjectT> bindings;
    for (auto prop : props) {
        if (auto binding = bindingOf(*prop)) {
            bindings.emplace_back(binding);
        }
    }
    for (auto& bindingT : bindings) {
        releaseBinding(Base::freecad_dynamic_cast<Part::ElementBinding>(bindingT.getObject()));
    }
}

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

Part::ElementBinding* SpringDamper::getBinding(int side) const
{
    return bindingOf(side == 1 ? Binding1 : Binding2);
}

App::DocumentObject*
SpringDamper::getEnd(int side, Base::Vector3d& point, Base::Placement* placement) const
{
    point = (side == 1 ? Point1 : Point2).getValue();
    return endComponent(side == 1 ? Binding1 : Binding2,
                        side == 1 ? Component1 : Component2,
                        placement);
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
    auto binding = makeEndBinding(component, subname, vertex);
    clearEnd(side);
    (side == 1 ? Binding1 : Binding2).setValue(binding);
    // The point follows at the next recompute: taken now, so that the end is
    // where it goes before that
    Base::Placement local;
    if (resolveEnd(binding, Base::Placement(), local)) {
        (side == 1 ? Point1 : Point2).setValue(local.getPosition());
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
    releaseBinding(old);
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
    if (prop == &Binding1) {
        updateEditable();
        syncComponent(Binding1, Component1);
    }
    else if (prop == &Binding2) {
        updateEditable();
        syncComponent(Binding2, Component2);
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
        Base::Placement local;
        if (!resolveEnd(binding, Base::Placement(point.getValue(), {}), local)) {
            return new App::DocumentObjectExecReturn(std::string("The ") + endName(side)
                                                     + " end does not resolve");
        }
        if (!point.getValue().IsEqual(local.getPosition(), 1e-12)) {
            point.setValue(local.getPosition());
        }
    }
    return PhysicsItem::execute();
}

void SpringDamper::unsetupObject()
{
    // The spring owns its element bindings: they go with it
    releaseBindings({&Binding1, &Binding2});
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

PROPERTY_SOURCE(Assembly::FramedItem, Assembly::PhysicsItem)

FramedItem::FramedItem()
{
    ADD_PROPERTY_TYPE(Component,
                      (nullptr),
                      "Frame",
                      App::Prop_None,
                      "The component the frame is on");
    ADD_PROPERTY_TYPE(Frame,
                      (Base::Placement()),
                      "Frame",
                      App::Prop_None,
                      "The frame, in Component's coordinates (the assembly's without it)");
    ADD_PROPERTY_TYPE(Binding,
                      (nullptr),
                      "Frame",
                      App::Prop_None,
                      "The element binding of the frame: the element's frame, Component and "
                      "Frame its output.\nNone: the free frame they give");
}

void FramedItem::updateEditable()
{
    const bool bound = Binding.getValue() != nullptr;
    Component.setStatus(App::Property::ReadOnly, bound);
    Frame.setStatus(App::Property::ReadOnly, bound);
}

Part::ElementBinding* FramedItem::getBinding() const
{
    return bindingOf(Binding);
}

App::DocumentObject* FramedItem::getEnd(Base::Placement& frame, Base::Placement* placement) const
{
    frame = Frame.getValue();
    return endComponent(Binding, Component, placement);
}

Base::Placement FramedItem::getEndPlacement() const
{
    Base::Placement frame;
    Base::Placement placement;
    getEnd(frame, &placement);
    return placement * frame;
}

Part::ElementBinding*
FramedItem::setEnd(App::DocumentObject* component, const char* subname, const char* vertex)
{
    auto binding = makeEndBinding(component, subname, vertex);
    clearEnd();
    Binding.setValue(binding);
    // The frame follows at the next recompute: taken now, so that the item
    // is where it goes before that
    Base::Placement local;
    if (resolveEnd(binding, Base::Placement(), local)) {
        Frame.setValue(local);
    }
    return binding;
}

void FramedItem::clearEnd()
{
    auto old = getBinding();
    if (!old) {
        return;
    }
    // The free frame is where the frame is
    Base::Placement frame;
    auto component = getEnd(frame);
    Binding.setValue(nullptr);
    Component.setValue(component);
    Frame.setValue(frame);
    releaseBinding(old);
}

void FramedItem::onChanged(const App::Property* prop)
{
    if (prop == &Binding) {
        updateEditable();
        syncComponent(Binding, Component);
    }
    PhysicsItem::onChanged(prop);
}

void FramedItem::onDocumentRestored()
{
    updateEditable();
    PhysicsItem::onDocumentRestored();
}

short FramedItem::mustExecute() const
{
    if (Binding.isTouched()) {
        return 1;
    }
    return PhysicsItem::mustExecute();
}

App::DocumentObjectExecReturn* FramedItem::execute()
{
    if (auto binding = getBinding()) {
        // The stored copy of a binding that does not resolve now is placed
        // where the frame was last
        Base::Placement local;
        if (!resolveEnd(binding, Frame.getValue(), local)) {
            return new App::DocumentObjectExecReturn("The frame's element does not resolve");
        }
        if (!Frame.getValue().isSame(local, 1e-12)) {
            Frame.setValue(local);
        }
    }
    return PhysicsItem::execute();
}

void FramedItem::unsetupObject()
{
    // The item owns its element binding: it goes with it
    releaseBindings({&Binding});
    PhysicsItem::unsetupObject();
}

PyObject* FramedItem::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new FramedItemPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

PROPERTY_SOURCE(Assembly::AppliedLoad, Assembly::FramedItem)

const char* AppliedLoad::KindEnums[] = {"Force", "Torque", nullptr};
const char* AppliedLoad::DirectionAxesEnums[] = {"Frame", "Assembly", nullptr};

AppliedLoad::AppliedLoad()
{
    ADD_PROPERTY_TYPE(Kind, (0L), "Load", App::Prop_None, "A force, or a torque");
    Kind.setEnums(KindEnums);
    ADD_PROPERTY_TYPE(Direction,
                      (Base::Vector3d(0, 0, 1)),
                      "Load",
                      App::Prop_None,
                      "The direction of the force, or the axis of the torque");
    ADD_PROPERTY_TYPE(DirectionAxes,
                      (0L),
                      "Load",
                      App::Prop_None,
                      "Frame: Direction in the frame's axes, turning with the component.\n"
                      "Assembly: in the assembly's, fixed");
    DirectionAxes.setEnums(DirectionAxesEnums);
    ADD_PROPERTY_TYPE(Magnitude,
                      (0.0),
                      "Load",
                      App::Prop_None,
                      "The force, or the torque (1 N*m is 1 N m)");
    ADD_PROPERTY_TYPE(Profile,
                      (""),
                      "Load",
                      App::Prop_None,
                      "The magnitude's factor over time, a formula of time (as a motion's); "
                      "empty: 1");
    ADD_PROPERTY_TYPE(Reaction,
                      (nullptr),
                      "Load",
                      App::Prop_None,
                      "The component taking the opposite load, at where the frame's origin was "
                      "at the start;\nnone: the ground");
    updateUnit();
}

void AppliedLoad::updateUnit()
{
    // A force: mass length / time^2; a torque: mass length^2 / time^2
    Magnitude.setUnit(isTorque() ? Base::Unit(2, 1, -2) : Base::Unit(1, 1, -2));
}

Base::Vector3d AppliedLoad::getDirectionNow() const
{
    Base::Vector3d dir = Direction.getValue();
    if (dir.Length() < 1e-12) {
        return Base::Vector3d(0, 0, 0);
    }
    dir.Normalize();
    if (DirectionAxes.getValue() == 0) {
        Base::Vector3d out;
        getEndPlacement().getRotation().multVec(dir, out);
        return out;
    }
    return dir;
}

void AppliedLoad::onChanged(const App::Property* prop)
{
    if (prop == &Kind) {
        updateUnit();
    }
    FramedItem::onChanged(prop);
}

void AppliedLoad::onDocumentRestored()
{
    updateUnit();
    FramedItem::onDocumentRestored();
}

PyObject* AppliedLoad::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new AppliedLoadPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

PROPERTY_SOURCE(Assembly::Bushing, Assembly::FramedItem)

Bushing::Bushing()
{
    ADD_PROPERTY_TYPE(Other,
                      (nullptr),
                      "Bushing",
                      App::Prop_None,
                      "The component holding Component at the frame; none: the ground");
    ADD_PROPERTY_TYPE(TranslationalStiffness,
                      (Base::Vector3d()),
                      "Bushing",
                      App::Prop_None,
                      "The stiffness along the frame's x, y and z, in N/m (mN/mm)");
    ADD_PROPERTY_TYPE(RotationalStiffness,
                      (Base::Vector3d()),
                      "Bushing",
                      App::Prop_None,
                      "The stiffness about the frame's x, y and z, in mN mm per radian (1 N m "
                      "per radian is 1e6)");
    ADD_PROPERTY_TYPE(TranslationalDamping,
                      (Base::Vector3d()),
                      "Bushing",
                      App::Prop_None,
                      "The damping along the frame's x, y and z, in N s/m (mN s/mm)");
    ADD_PROPERTY_TYPE(RotationalDamping,
                      (Base::Vector3d()),
                      "Bushing",
                      App::Prop_None,
                      "The damping about the frame's x, y and z, in mN mm s per radian (1 N m s "
                      "per radian is 1e6)");
    ADD_PROPERTY_TYPE(Preload,
                      (Base::Vector3d()),
                      "Bushing",
                      App::Prop_None,
                      "The force on Component where the run starts, in the frame's axes, in mN");
    ADD_PROPERTY_TYPE(PreloadTorque,
                      (Base::Vector3d()),
                      "Bushing",
                      App::Prop_None,
                      "The torque on Component where the run starts, in the frame's axes, in "
                      "mN mm");
}

PROPERTY_SOURCE(Assembly::ContactSet, Assembly::PhysicsItem)

const char* ContactSet::StiffnessFromEnums[] = {"Materials", "Value", "Penetration", nullptr};
const char* ContactSet::RadiusFromEnums[] = {"Patch", "Part", nullptr};
const char* ContactSet::DampingFromEnums[] = {"Restitution", "Value", nullptr};

ContactSet::ContactSet()
{
    ADD_PROPERTY_TYPE(GroupA,
                      (nullptr),
                      "Contact",
                      App::Prop_None,
                      "The components touching those of GroupB, or one another when GroupB is "
                      "empty");
    ADD_PROPERTY_TYPE(GroupB,
                      (nullptr),
                      "Contact",
                      App::Prop_None,
                      "The components those of GroupA touch; empty: those of GroupA touch one "
                      "another");
    ADD_PROPERTY_TYPE(StiffnessFrom,
                      (0L),
                      "Contact",
                      App::Prop_None,
                      "Materials: Hertz's law from the components' materials, a stiffness of "
                      "4/3 E* R^0.5 and an exponent of 1.5.\nValue: Stiffness and Exponent as "
                      "given.\nPenetration: the stiffness at which the lighter part of each "
                      "pair sinks AllowedPenetration under its weight, at Exponent.");
    StiffnessFrom.setEnums(StiffnessFromEnums);
    ADD_PROPERTY_TYPE(AllowedPenetration,
                      (0.01),
                      "Contact",
                      App::Prop_None,
                      "For StiffnessFrom Penetration: how far the lighter part of each pair "
                      "sinks under its weight, the most to accept -- the larger, the faster "
                      "the run");
    ADD_PROPERTY_TYPE(RadiusFrom,
                      (0L),
                      "Contact",
                      App::Prop_None,
                      "Hertz's contact radius R, for StiffnessFrom Materials.\nPatch: from the "
                      "curvatures where the shapes touch, at most the extent of the contact.\n"
                      "Part: from the sizes of the two solids, each the radius of the ball of "
                      "its volume.");
    RadiusFrom.setEnums(RadiusFromEnums);
    ADD_PROPERTY_TYPE(DampingFrom,
                      (0L),
                      "Contact",
                      App::Prop_None,
                      "Restitution: by the coefficient of restitution, the damping scaling "
                      "with the stiffness (Hunt-Crossley's, exactly).\nValue: MaxDamping, "
                      "reached at Penetration.");
    DampingFrom.setEnums(DampingFromEnums);
    ADD_PROPERTY_TYPE(Restitution,
                      (0.6),
                      "Contact",
                      App::Prop_None,
                      "The coefficient of restitution, for DampingFrom Restitution: the speed "
                      "a contact parts at over the speed it came in at; 1 elastic");
    // SOLIDWORKS's and Adams View's defaults (docs/AssemblyDynamics.md sec 2.3)
    ADD_PROPERTY_TYPE(Stiffness,
                      (1e5),
                      "Contact",
                      App::Prop_None,
                      "The impact law's stiffness, in N / mm^Exponent, for StiffnessFrom "
                      "Value");
    ADD_PROPERTY_TYPE(Exponent,
                      (1.5),
                      "Contact",
                      App::Prop_None,
                      "The impact law's exponent: the force grows as the penetration to it");
    ADD_PROPERTY_TYPE(MaxDamping,
                      (10.0),
                      "Contact",
                      App::Prop_None,
                      "The impact law's damping, reached at Penetration, in N s/mm");
    ADD_PROPERTY_TYPE(Penetration,
                      (0.01),
                      "Contact",
                      App::Prop_None,
                      "The penetration at which the damping is whole");
    ADD_PROPERTY_TYPE(StaticFriction,
                      (0.3),
                      "Contact",
                      App::Prop_None,
                      "The friction coefficient at the stiction velocity; 0: no friction");
    ADD_PROPERTY_TYPE(DynamicFriction,
                      (0.25),
                      "Contact",
                      App::Prop_None,
                      "The friction coefficient from the friction velocity on");
    ADD_PROPERTY_TYPE(StictionVelocity,
                      (0.1),
                      "Contact",
                      App::Prop_None,
                      "The sliding speed at which the static friction is whole");
    ADD_PROPERTY_TYPE(FrictionVelocity,
                      (10.16),
                      "Contact",
                      App::Prop_None,
                      "The sliding speed from which the friction is dynamic");
    static const App::PropertyFloatConstraint::Constraints positive = {0.0, 1e30, 1.0};
    static const App::PropertyFloatConstraint::Constraints exponent = {1.0, 10.0, 0.1};
    static const App::PropertyFloatConstraint::Constraints coefficient = {0.0, 10.0, 0.05};
    static const App::PropertyFloatConstraint::Constraints ratio = {0.01, 1.0, 0.05};
    Stiffness.setConstraints(&positive);
    Exponent.setConstraints(&exponent);
    Restitution.setConstraints(&ratio);
    MaxDamping.setConstraints(&positive);
    StaticFriction.setConstraints(&coefficient);
    DynamicFriction.setConstraints(&coefficient);
}

void ContactSet::Restore(Base::XMLReader& reader)
{
    // Saved before StiffnessFrom and DampingFrom: the set's file has
    // neither, and its stiffness and damping were the ones it ran with
    StiffnessFrom.setValue("Value");
    DampingFrom.setValue("Value");
    PhysicsItem::Restore(reader);
}

PROPERTY_SOURCE(Assembly::CamContact, Assembly::ContactSet)

CamContact::CamContact()
{
    ADD_PROPERTY_TYPE(Joint,
                      (nullptr),
                      "Cam",
                      App::Prop_None,
                      "The Cam joint whose follower lifts off in a dynamic or static run");
    ADD_PROPERTY_TYPE(Reverse,
                      (false),
                      "Cam",
                      App::Prop_None,
                      "The follower on the other side of the cam's curve than the one it is "
                      "taken to start on");
    // A curve has no width for Hertz's law: by an allowance; a roller rolls
    StiffnessFrom.setValue("Penetration");
    StaticFriction.setValue(0.0);
    DynamicFriction.setValue(0.0);
    for (App::Property* unused : {static_cast<App::Property*>(&GroupA),
                                  static_cast<App::Property*>(&GroupB),
                                  static_cast<App::Property*>(&RadiusFrom)}) {
        unused->setStatus(App::Property::Hidden, true);
    }
}

PROPERTY_SOURCE(Assembly::ProfileContact, Assembly::ContactSet)

ProfileContact::ProfileContact()
{
    ADD_PROPERTY_TYPE(ProfileA,
                      (nullptr),
                      "Profile",
                      App::Prop_None,
                      "Planar faces of one component, whose boundaries touch ProfileB's in their "
                      "plane");
    ADD_PROPERTY_TYPE(ProfileB,
                      (nullptr),
                      "Profile",
                      App::Prop_None,
                      "Planar faces of another component, parallel to ProfileA's");
    // A profile has no width for Hertz's law: by an allowance; a mechanism's
    // pieces slide or roll as they are lubricated
    StiffnessFrom.setValue("Penetration");
    StaticFriction.setValue(0.0);
    DynamicFriction.setValue(0.0);
    for (App::Property* unused : {static_cast<App::Property*>(&GroupA),
                                  static_cast<App::Property*>(&GroupB),
                                  static_cast<App::Property*>(&RadiusFrom)}) {
        unused->setStatus(App::Property::Hidden, true);
    }
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

PROPERTY_SOURCE(Assembly::JointStop, Assembly::PhysicsItem)

namespace
{
const char* StopModeEnums[] = {"Rigid", "Penalty", "Off", nullptr};
}

JointStop::JointStop()
{
    ADD_PROPERTY_TYPE(Joint,
                      (nullptr),
                      "Stop",
                      App::Prop_None,
                      "The Slider, Cylindrical or Revolute joint whose limits it stops");
    ADD_PROPERTY_TYPE(Mode,
                      (0L),
                      "Stop",
                      App::Prop_None,
                      "Rigid: held at the bound, bouncing back by the restitution. Penalty: "
                      "pushed back by a spring and a damper on how far past the bound the "
                      "joint is. Off: not stopped");
    Mode.setEnums(StopModeEnums);
    ADD_PROPERTY_TYPE(Restitution,
                      (0.0),
                      "Stop",
                      App::Prop_None,
                      "Rigid: the speed the joint leaves its bound at over the speed it came "
                      "in at, 0 (it stays) to 1 (it bounces back as fast)");
    static const App::PropertyFloatConstraint::Constraints unit = {0.0, 1.0, 0.05};
    Restitution.setConstraints(&unit);
    // Defaults: a stiff rubber bumper's, about a millimetre under a 1 kN load
    ADD_PROPERTY_TYPE(Stiffness,
                      (1e6),
                      "Stop",
                      App::Prop_None,
                      "Penalty, at a length limit: the force per length past the bound");
    ADD_PROPERTY_TYPE(Damping,
                      (1e3),
                      "Stop",
                      App::Prop_None,
                      "Penalty, at a length limit: the force per speed going further past "
                      "the bound (1 N*s/m)");
    // A force over a speed: mass / time
    Damping.setUnit(Base::Unit(0, 1, -1));
    ADD_PROPERTY_TYPE(TorsionalStiffness,
                      (1e9),
                      "Stop",
                      App::Prop_None,
                      "Penalty, at an angle limit: the torque per radian past the bound "
                      "(1 N*m is 1 N m per radian)");
    TorsionalStiffness.setUnit(Base::Unit(2, 1, -2));
    ADD_PROPERTY_TYPE(TorsionalDamping,
                      (1e6),
                      "Stop",
                      App::Prop_None,
                      "Penalty, at an angle limit: the torque per radian per second going "
                      "further past the bound (1 N*m*s is 1 N m s per radian)");
    TorsionalDamping.setUnit(Base::Unit(2, 1, -1));
    updateEditable();
}

void JointStop::onChanged(const App::Property* prop)
{
    if (prop == &Joint || prop == &Mode) {
        updateEditable();
    }
    PhysicsItem::onChanged(prop);
}

void JointStop::onDocumentRestored()
{
    PhysicsItem::onDocumentRestored();
    updateEditable();
}

void JointStop::updateEditable()
{
    // A joint of no known type shows both
    bool slides = true;
    bool turns = true;
    if (auto* joint = Joint.getValue()) {
        if (auto* type =
                dynamic_cast<App::PropertyEnumeration*>(joint->getPropertyByName("JointType"))) {
            const std::string name = type->getValueAsString();
            slides = name != "Revolute";
            turns = name != "Slider";
        }
    }
    const int mode = Mode.getValue();
    Restitution.setStatus(App::Property::Hidden, mode != 0);
    Stiffness.setStatus(App::Property::Hidden, mode != 1 || !slides);
    Damping.setStatus(App::Property::Hidden, mode != 1 || !slides);
    TorsionalStiffness.setStatus(App::Property::Hidden, mode != 1 || !turns);
    TorsionalDamping.setStatus(App::Property::Hidden, mode != 1 || !turns);
}

PROPERTY_SOURCE(Assembly::JointFriction, Assembly::PhysicsItem)

JointFriction::JointFriction()
{
    ADD_PROPERTY_TYPE(Joint,
                      (nullptr),
                      "Friction",
                      App::Prop_None,
                      "The Slider, Cylindrical or Revolute joint whose slip it opposes");
    static const App::PropertyFloatConstraint::Constraints coefficient = {0.0, 10.0, 0.05};
    // Defaults: dry steel's, as a contact set's
    ADD_PROPERTY_TYPE(StaticFriction,
                      (0.3),
                      "Friction",
                      App::Prop_None,
                      "The coefficient reached at the stiction velocity");
    StaticFriction.setConstraints(&coefficient);
    ADD_PROPERTY_TYPE(DynamicFriction,
                      (0.25),
                      "Friction",
                      App::Prop_None,
                      "The coefficient from the friction velocity on");
    DynamicFriction.setConstraints(&coefficient);
    ADD_PROPERTY_TYPE(StictionVelocity,
                      (0.1),
                      "Friction",
                      App::Prop_None,
                      "The slip at which the static coefficient is reached; slower, the "
                      "friction falls to none at rest");
    ADD_PROPERTY_TYPE(FrictionVelocity,
                      (10.16),
                      "Friction",
                      App::Prop_None,
                      "The slip from which the dynamic coefficient holds");
    ADD_PROPERTY_TYPE(PinRadius,
                      (5.0),
                      "Friction",
                      App::Prop_None,
                      "A turn's: the radius the joint's load presses at, and its slip is "
                      "measured at");
    ADD_PROPERTY_TYPE(PreloadForce,
                      (0.0),
                      "Friction",
                      App::Prop_None,
                      "A slide's: a load pressing whatever the joint's reaction, as a seal's");
    ADD_PROPERTY_TYPE(PreloadTorque,
                      (0.0),
                      "Friction",
                      App::Prop_None,
                      "A turn's: a torque the friction takes whatever the joint's reaction");
    PreloadTorque.setUnit(Base::Unit::Moment);
    updateEditable();
}

void JointFriction::onChanged(const App::Property* prop)
{
    if (prop == &Joint) {
        updateEditable();
    }
    PhysicsItem::onChanged(prop);
}

void JointFriction::onDocumentRestored()
{
    PhysicsItem::onDocumentRestored();
    updateEditable();
}

void JointFriction::updateEditable()
{
    // A joint of no known type shows both
    bool slides = true;
    bool turns = true;
    if (auto* joint = Joint.getValue()) {
        if (auto* type =
                dynamic_cast<App::PropertyEnumeration*>(joint->getPropertyByName("JointType"))) {
            const std::string name = type->getValueAsString();
            slides = name != "Revolute";
            turns = name != "Slider";
        }
    }
    PreloadForce.setStatus(App::Property::Hidden, !slides);
    PinRadius.setStatus(App::Property::Hidden, !turns);
    PreloadTorque.setStatus(App::Property::Hidden, !turns);
}

namespace App
{
PROPERTY_SOURCE_TEMPLATE(Assembly::BushingPython, Assembly::Bushing)
template<>
const char* Assembly::BushingPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template<>
PyObject* Assembly::BushingPython::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new App::FeaturePythonPyT<Assembly::FramedItemPy>(this), true);
    }
    return Py::new_reference_to(PythonObject);
}
template class AssemblyExport FeaturePythonT<Assembly::Bushing>;

PROPERTY_SOURCE_TEMPLATE(Assembly::ContactSetPython, Assembly::ContactSet)
template<>
const char* Assembly::ContactSetPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::ContactSet>;

PROPERTY_SOURCE_TEMPLATE(Assembly::AppliedLoadPython, Assembly::AppliedLoad)
template<>
const char* Assembly::AppliedLoadPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template<>
PyObject* Assembly::AppliedLoadPython::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new App::FeaturePythonPyT<Assembly::AppliedLoadPy>(this), true);
    }
    return Py::new_reference_to(PythonObject);
}
template class AssemblyExport FeaturePythonT<Assembly::AppliedLoad>;

PROPERTY_SOURCE_TEMPLATE(Assembly::TorsionSpringPython, Assembly::TorsionSpring)
template<>
const char* Assembly::TorsionSpringPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::TorsionSpring>;

PROPERTY_SOURCE_TEMPLATE(Assembly::JointStopPython, Assembly::JointStop)
template<>
const char* Assembly::JointStopPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::JointStop>;

PROPERTY_SOURCE_TEMPLATE(Assembly::JointFrictionPython, Assembly::JointFriction)
template<>
const char* Assembly::JointFrictionPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::JointFriction>;

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
