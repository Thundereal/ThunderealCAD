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

#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Circle.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_Conic.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ElementarySurface.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

#include <App/AuxGroup.h>
#include <App/Document.h>
#include <App/DocumentObjectPy.h>
#include <App/DocumentObserver.h>
#include <App/FeaturePythonPyImp.h>
#include <App/GeoFeature.h>
#include <App/GeoFeatureGroupExtension.h>
#include <App/GroupExtension.h>
#include <Base/Console.h>
#include <Base/Tools.h>
#include <Mod/Part/App/FeatureElement.h>
#include <Mod/Part/App/PartFeature.h>

#include "AssemblyGroup.h"
#include "AssemblyObject.h"
#include "ElementGeometry.h"
#include "Groups.h"
#include "Joint.h"
#include "JointPy.h"

using namespace Assembly;

namespace
{

/// The direct child of the assembly an object is, or null
App::DocumentObject* directChild(const AssemblyObject* assembly, App::DocumentObject* obj)
{
    if (!assembly || !obj) {
        return nullptr;
    }
    return assembly->Group.find(obj->getNameInDocument()) == obj ? obj : nullptr;
}

/// The element an element link's (object, subname) resolves to, or null
Part::Element* resolveElement(App::DocumentObject* obj, const std::string& sub)
{
    if (!obj) {
        return nullptr;
    }
    return Base::freecad_dynamic_cast<Part::Element>(obj->getSubObject(sub.c_str()));
}

const char* sideName(int side)
{
    return side == 1 ? "first" : "second";
}

}  // namespace

// ------------------------------------------------------------------------

std::string JointSide::elementType() const
{
    if (shape.isNull()) {
        return {};
    }
    switch (Part::singleSubShape(shape.getShape()).ShapeType()) {
        case TopAbs_VERTEX:
            return "Vertex";
        case TopAbs_EDGE:
            return "Edge";
        case TopAbs_FACE:
            return "Face";
        default:
            return {};
    }
}

// ------------------------------------------------------------------------

PROPERTY_SOURCE(Assembly::Joint, App::DocumentObject)

const char* Joint::JointTypeEnums[] = {"Fixed",
                                       "Revolute",
                                       "Cylindrical",
                                       "Slider",
                                       "Ball",
                                       "Distance",
                                       "Parallel",
                                       "Perpendicular",
                                       "Angle",
                                       "RackPinion",
                                       "Screw",
                                       "Gears",
                                       "Belt",
                                       "Cam",
                                       nullptr};

Joint::Joint()
{
    ADD_PROPERTY_TYPE(JointType, (0L), "Joint", App::Prop_None, "The type of the joint");
    JointType.setEnums(JointTypeEnums);

    ADD_PROPERTY_TYPE(ElementLink1, (nullptr), "Joint Connector 1", App::Prop_None,
                      "The element link of the first side, a child of the assembly");
    ADD_PROPERTY_TYPE(ElementLink2, (nullptr), "Joint Connector 2", App::Prop_None,
                      "The element link of the second side, a child of the assembly");
    ADD_PROPERTY_TYPE(Placement1, (Base::Placement()), "Joint Connector 1",
                      App::PropertyType(App::Prop_Output | App::Prop_ReadOnly),
                      "The first joint coordinate system, relative to the side's component");
    ADD_PROPERTY_TYPE(Placement2, (Base::Placement()), "Joint Connector 2",
                      App::PropertyType(App::Prop_Output | App::Prop_ReadOnly),
                      "The second joint coordinate system, relative to the side's component");

    ADD_PROPERTY_TYPE(Angle, (0.0), "Joint", App::Prop_None,
                      "The angle of the joint. It is used only by the Angle joint.");
    ADD_PROPERTY_TYPE(Distance, (0.0), "Joint", App::Prop_None,
                      "The distance of the joint. It is used only by the Distance joint and\n"
                      "Rack and Pinion (pitch radius), Screw and Gears and Belt (radius1)");
    ADD_PROPERTY_TYPE(Distance2, (0.0), "Joint", App::Prop_None,
                      "The second distance of the joint. It is used only by the gear joint\n"
                      "to store the second radius.");

    ADD_PROPERTY_TYPE(EnableLengthMin, (false), "Limits", App::Prop_None,
                      "Enable the minimum length limit of the joint");
    ADD_PROPERTY_TYPE(EnableLengthMax, (false), "Limits", App::Prop_None,
                      "Enable the maximum length limit of the joint");
    ADD_PROPERTY_TYPE(EnableAngleMin, (false), "Limits", App::Prop_None,
                      "Enable the minimum angle limit of the joint");
    ADD_PROPERTY_TYPE(EnableAngleMax, (false), "Limits", App::Prop_None,
                      "Enable the maximum angle limit of the joint");
    ADD_PROPERTY_TYPE(LengthMin, (0.0), "Limits", App::Prop_None,
                      "The minimum limit for the length between both coordinate systems\n"
                      "(along their z-axis)");
    ADD_PROPERTY_TYPE(LengthMax, (0.0), "Limits", App::Prop_None,
                      "The maximum limit for the length between both coordinate systems\n"
                      "(along their z-axis)");
    ADD_PROPERTY_TYPE(AngleMin, (0.0), "Limits", App::Prop_None,
                      "The minimum limit for the angle between both coordinate systems\n"
                      "(between their x-axis)");
    ADD_PROPERTY_TYPE(AngleMax, (0.0), "Limits", App::Prop_None,
                      "The maximum limit for the angle between both coordinate systems\n"
                      "(between their x-axis)");

    // As the Python joint sets them
    for (auto prop : {&Distance, &Distance2, &LengthMin, &LengthMax}) {
        prop->enableNegative(true);
    }

    App::SuppressibleExtension::initExtension(this);
}

Part::ElementLink* Joint::getElementLink(int side) const
{
    auto& prop = side == 1 ? ElementLink1 : ElementLink2;
    return Base::freecad_dynamic_cast<Part::ElementLink>(prop.getValue());
}

AssemblyObject* Joint::getAssembly() const
{
    return Base::freecad_dynamic_cast<AssemblyObject>(
        App::GeoFeatureGroupExtension::getGroupOfObject(this)
    );
}

App::DocumentObject* Joint::getComponent(int side) const
{
    auto link = getElementLink(side);
    if (!link) {
        return nullptr;
    }
    auto assembly = getAssembly();
    auto obj = link->Element.getValue();
    if (auto child = directChild(assembly, obj)) {
        return child;
    }
    // A convenience element in the assembly's own Elements group: its
    // target is the component
    const auto& subs = link->Element.getSubValues();
    auto element = resolveElement(obj, subs.empty() ? std::string() : subs.front());
    return element ? directChild(assembly, element->Target.getValue()) : nullptr;
}

JointSide Joint::getSide(int side) const
{
    JointSide res;
    res.link = getElementLink(side);
    if (!res.link) {
        return res;
    }
    res.component = getComponent(side);

    auto obj = res.link->Element.getValue();
    const auto& subs = res.link->Element.getSubValues();
    const std::string sub = subs.empty() ? std::string() : subs.front();

    // A convenience element in the assembly's own Elements group keeps a copy
    // taken where its component was at its last recompute, which a solve
    // makes stale: the geometry is taken through the element's target, so
    // that it is where the component is now. An element a component declares
    // is reached through the component, which places it where it is now.
    App::DocumentObject* geomObj = obj;
    std::string geomSub = sub;
    if (!directChild(getAssembly(), obj)) {
        auto element = resolveElement(obj, sub);
        if (element && !element->Detach.getValue() && element->Target.getValue()) {
            geomObj = element->Target.getValue();
            const auto& tsubs = element->Target.getSubValues();
            geomSub = tsubs.empty() ? std::string() : tsubs.front();
        }
    }
    try {
        res.shape = Part::Feature::getTopoShape(geomObj, geomSub.c_str(), true);
    }
    catch (const Base::Exception&) {
        res.shape = Part::TopoShape();
    }
    if (res.shape.isNull()) {
        // Not resolved now (a component not loaded, a broken reference): the
        // copy the element link keeps
        res.shape = res.link->Shape.getShape();
    }
    Base::Placement wholePlacement;
    if (res.shape.isNull()
        && Part::Element::getWholeObjectPlacement(geomObj, geomSub.c_str(), wholePlacement)) {
        res.wholeObject = true;
    }
    if (res.shape.isNull() && !res.wholeObject) {
        return res;
    }

    // The offsets down the chain of elements, the deepest first, then the
    // element link's own: each is a placement in the frame before it
    std::vector<Base::Placement> offsets;
    std::vector<const Part::Element*> visited;
    auto element = resolveElement(obj, sub);
    while (element
           && std::find(visited.begin(), visited.end(), element) == visited.end()) {
        visited.push_back(element);
        offsets.push_back(element->Offset.getValue());
        const auto& tsubs = element->Target.getSubValues();
        element = resolveElement(element->Target.getValue(),
                                 tsubs.empty() ? std::string() : tsubs.front());
    }
    res.frame = res.wholeObject ? wholePlacement : findElementFrame(res.shape);
    for (auto it = offsets.rbegin(); it != offsets.rend(); ++it) {
        res.frame = res.frame * *it;
    }
    res.frame = res.frame * res.link->Offset.getValue();
    return res;
}

short Joint::mustExecute() const
{
    if (ElementLink1.isTouched() || ElementLink2.isTouched()) {
        return 1;
    }
    return App::DocumentObject::mustExecute();
}

App::DocumentObjectExecReturn* Joint::execute()
{
    for (int side : {1, 2}) {
        if (!getElementLink(side)) {
            continue;  // incomplete, as while the joint task is open
        }
        const auto jointSide = getSide(side);
        if (!jointSide.isValid()) {
            return new App::DocumentObjectExecReturn(
                std::string("The ") + sideName(side) + " side does not resolve"
            );
        }
        // Relative to the component, as the Python joint's are to their
        // object: it does not change when a solve moves the component
        auto& plc = side == 1 ? Placement1 : Placement2;
        const Base::Placement local
            = App::GeoFeature::getPlacementFromProp(jointSide.component, "Placement").inverse()
            * jointSide.frame;
        if (!plc.getValue().isSame(local, 1e-12)) {
            plc.setValue(local);
        }
    }
    return App::DocumentObject::StdReturn;
}

namespace
{
bool isUsedByAnotherJoint(const Part::ElementLink* link, const Joint* joint)
{
    for (auto user : link->getInList()) {
        if (user != joint && user->isDerivedFrom<Joint>()) {
            return true;
        }
    }
    return false;
}

/// Delete an element link, and the convenience element it used when no
/// other element link of the assembly uses that one
void removeElementLink(AssemblyObject* assembly, Part::ElementLink* link)
{
    auto doc = link->getDocument();
    Part::Element* convenience = nullptr;
    if (assembly && !directChild(assembly, link->Element.getValue())) {
        const auto& subs = link->Element.getSubValues();
        convenience = resolveElement(link->Element.getValue(),
                                     subs.empty() ? std::string() : subs.front());
    }
    App::DocumentObjectT elementT(convenience);
    doc->removeObject(link->getNameInDocument());

    convenience = Base::freecad_dynamic_cast<Part::Element>(elementT.getObject());
    if (!convenience || convenience->isRemoving()) {
        return;
    }
    for (auto child : assembly->Group.getValues()) {
        auto other = Base::freecad_dynamic_cast<Part::ElementLink>(child);
        if (!other || other->isRemoving()) {
            continue;
        }
        const auto& subs = other->Element.getSubValues();
        if (resolveElement(other->Element.getValue(), subs.empty() ? std::string() : subs.front())
            == convenience) {
            return;
        }
    }
    doc->removeObject(convenience->getNameInDocument());
}
}  // namespace

Part::ElementLink* Joint::setSide(int side,
                                  App::DocumentObject* component,
                                  const char* subname,
                                  const char* vertex,
                                  bool local)
{
    auto assembly = getAssembly();
    if (!assembly) {
        FC_THROWM(Base::RuntimeError, getFullName() << " is in no assembly");
    }
    auto link = makeSide(assembly, component, subname, vertex, local);
    clearSide(side);
    (side == 1 ? ElementLink1 : ElementLink2).setValue(link);
    return link;
}

void Joint::clearSide(int side)
{
    auto old = getElementLink(side);
    (side == 1 ? ElementLink1 : ElementLink2).setValue(nullptr);
    if (old && !old->isRemoving() && !isUsedByAnotherJoint(old, this)) {
        removeElementLink(getAssembly(), old);
    }
}

PyObject* Joint::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new JointPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

void Joint::unsetupObject()
{
    // The joint holds its element links: they go with it, unless another
    // joint uses one too
    auto assembly = getAssembly();
    std::vector<App::DocumentObjectT> links;
    for (int side : {1, 2}) {
        auto link = getElementLink(side);
        if (!link || link->isRemoving()) {
            continue;
        }
        if (!isUsedByAnotherJoint(link, this)) {
            links.emplace_back(link);
        }
    }
    for (auto& linkT : links) {
        auto link = Base::freecad_dynamic_cast<Part::ElementLink>(linkT.getObject());
        if (link && !link->isRemoving()) {
            removeElementLink(assembly, link);
        }
    }
    App::DocumentObject::unsetupObject();
}

void Joint::relocateCopies(AssemblyObject* assembly,
                           App::DocumentObject* component,
                           const Base::Placement& delta)
{
    if (!assembly || !component) {
        return;
    }
    // A solve writes placements without a recompute, so the copies that
    // follow a component in the assembly's coordinate system are moved with
    // it: its element links, and the convenience elements on it
    auto move = [&delta](Part::Feature* feature) {
        feature->Placement.setValue(delta * feature->Placement.getValue());
        feature->purgeTouched();
    };
    std::vector<Part::Element*> moved;
    for (auto child : assembly->Group.getValues()) {
        auto link = Base::freecad_dynamic_cast<Part::ElementLink>(child);
        if (!link) {
            continue;
        }
        auto obj = link->Element.getValue();
        if (obj == component) {
            move(link);
            continue;
        }
        if (directChild(assembly, obj)) {
            continue;
        }
        const auto& subs = link->Element.getSubValues();
        auto element = resolveElement(obj, subs.empty() ? std::string() : subs.front());
        if (!element || element->Target.getValue() != component || element->Detach.getValue()) {
            continue;
        }
        move(link);
        if (std::find(moved.begin(), moved.end(), element) == moved.end()) {
            moved.push_back(element);
            move(element);
        }
    }
}

// ------------------------------------------------------------------------

namespace
{

/// The container of a component that declares elements, or null
App::AuxGroupOwner* elementsOwner(App::DocumentObject* component)
{
    auto linked = component->getLinkedObject(true);
    auto owner = dynamic_cast<App::AuxGroupOwner*>(linked);
    if (!owner || !linked->getPropertyByName("Elements")) {
        return nullptr;
    }
    return owner;
}

/// An element of the group with this target, or null
Part::Element* findElement(App::AuxGroup* group, App::DocumentObject* target, const std::string& sub)
{
    if (!group) {
        return nullptr;
    }
    for (auto child : group->Group.getValues()) {
        auto element = Base::freecad_dynamic_cast<Part::Element>(child);
        if (!element || element->Target.getValue() != target) {
            continue;
        }
        const auto& subs = element->Target.getSubValues();
        if ((subs.empty() && sub.empty()) || (!subs.empty() && subs.front() == sub)) {
            return element;
        }
    }
    return nullptr;
}

Part::Element* declare(App::DocumentObject* container,
                       App::DocumentObject* target,
                       const std::string& sub,
                       const char* label)
{
    auto owner = dynamic_cast<App::AuxGroupOwner*>(container);
    if (auto element = findElement(owner ? owner->getElementsGroup(false) : nullptr, target, sub)) {
        return element;
    }
    return Part::Element::create(container, target, sub.c_str(), label);
}

}  // namespace

Part::ElementLink* Joint::makeSide(AssemblyObject* assembly,
                                   App::DocumentObject* component,
                                   const char* subname,
                                   const char* vertex,
                                   bool local)
{
    if (!assembly || !component) {
        THROWM(Base::ValueError, "No assembly or component");
    }
    if (!directChild(assembly, component)) {
        FC_THROWM(Base::ValueError,
                  component->getFullName() << " is not a component of " << assembly->getFullName());
    }
    const std::string sub = subname ? subname : "";

    Part::ElementLink* link = nullptr;
    // The component declares elements: the element goes in its Elements
    // group, as a direct child of it plus the rest of the path
    if (auto owner = local ? nullptr : elementsOwner(component)) {
        auto container = component->getLinkedObject(true);
        App::DocumentObject* target = nullptr;
        std::string rest;
        auto dot = sub.find('.');
        if (dot != std::string::npos) {
            const std::string name = sub.substr(0, dot);
            auto child = container->getDocument()->getObject(name.c_str());
            auto group = container->getExtensionByType<App::GroupExtension>(true);
            if (child && group && group->Group.find(name.c_str()) == child) {
                target = child;
                rest = sub.substr(dot + 1);
            }
        }
        else if (auto tip = container->getPropertyByName<App::PropertyLink>("Tip")) {
            // A body's own shape is its tip's
            target = tip->getValue();
            rest = sub;
        }
        if (target) {
            auto element = declare(container, target, rest, nullptr);
            (void)owner;
            link = Part::ElementLink::create(
                assembly,
                component,
                (std::string("$$Elements.$") + element->Label.getValue() + ".").c_str()
            );
        }
    }
    if (!link) {
        // A convenience element in the assembly's own Elements group
        auto element = declare(assembly, component, sub, "_Element");
        link = Part::ElementLink::create(
            assembly,
            assembly->getElementsGroup(false),
            (std::string("$") + element->Label.getValue() + ".").c_str()
        );
    }

    if (vertex && *vertex) {
        // The vertex places the origin on the element: the element link
        // keeps it, and its Offset follows it. Followed now on the pick's
        // geometry: the new element has no copy yet.
        link->Vertex.setValue(component, std::vector<std::string> {vertex});
        link->updateVertexOffset(Part::Feature::getTopoShape(component, sub.c_str(), true));
    }
    return link;
}

Joint* Joint::make(AssemblyObject* assembly,
                   const char* type,
                   App::DocumentObject* component1,
                   const char* subname1,
                   const char* vertex1,
                   App::DocumentObject* component2,
                   const char* subname2,
                   const char* vertex2)
{
    if (!assembly || !assembly->isAttachedToDocument()) {
        THROWM(Base::ValueError, "No assembly");
    }
    auto group = assembly->getAssemblyGroup(AssemblyGroup::JointsRole, true);
    // The Python-extensible one, as the joint task makes, so that a Python
    // view provider can draw its markers
    auto joint = static_cast<Joint*>(assembly->getDocument()->addObject("Assembly::JointPython", type));
    if (auto aux = freecad_cast<AssemblyGroup*>(group)) {
        aux->addObject(joint);
    }
    else if (auto ext = group->getExtensionByType<App::GroupExtension>(true)) {
        // Upstream's JointGroup, in a file not converted
        ext->addObject(joint);
    }
    joint->JointType.setValue(type);
    joint->ElementLink1.setValue(makeSide(assembly, component1, subname1, vertex1));
    joint->ElementLink2.setValue(makeSide(assembly, component2, subname2, vertex2));
    return joint;
}

// Python feature ---------------------------------------------------------

namespace App
{
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(Assembly::JointPython, Assembly::Joint)
template<>
const char* Assembly::JointPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}

template<>
PyObject* Assembly::JointPython::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new App::FeaturePythonPyT<Assembly::JointPy>(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

// explicit template instantiation
template class AssemblyExport FeaturePythonT<Assembly::Joint>;

/// @endcond

}  // namespace App
