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
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <Precision.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
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
#include <App/Link.h>
#include <App/Part.h>
#include <Base/Console.h>
#include <Base/Tools.h>
#include <Mod/Part/App/FeatureElement.h>
#include <Mod/Part/App/PartFeature.h>

#include "AssemblyGroup.h"
#include "AssemblyLink.h"
#include "AssemblyObject.h"
#include "AssemblyUtils.h"
#include "ElementGeometry.h"
#include "Groups.h"
#include "Joint.h"
#include "JointPy.h"

using namespace Assembly;

namespace
{

/// The direct child of the assembly an object is, or null
App::DocumentObject* directChild(const App::Part* assembly, App::DocumentObject* obj)
{
    if (!assembly || !obj) {
        return nullptr;
    }
    return assembly->Group.find(obj->getNameInDocument()) == obj ? obj : nullptr;
}

/// A sub-assembly instance whose parts move each on its own
AssemblyLink* flexibleInstance(App::DocumentObject* obj)
{
    auto link = Base::freecad_dynamic_cast<AssemblyLink>(obj);
    return link && !link->isRigid() ? link : nullptr;
}

/** The component (object, subname) is on: the direct child of the assembly
 * it starts with, or, through a link group whose elements are each a
 * component (an expanded link array, isElementLinkGroup()), the element the
 * path steps into, or, through the instance of a flexible sub-assembly, the
 * part of it the path steps into, at any depth. Null when obj is not a
 * direct child.
 *
 * @param placement: optionally, the component's placement in the
 *                   assembly's coordinate system (JointSide::placement)
 */
App::DocumentObject* componentOf(const App::Part* assembly,
                                 App::DocumentObject* obj,
                                 const std::string& sub,
                                 Base::Placement* placement = nullptr)
{
    auto child = directChild(assembly, obj);
    if (!child) {
        return nullptr;
    }
    if (placement) {
        *placement = App::GeoFeature::getPlacementFromProp(child, "Placement");
    }
    auto dot = sub.find('.');
    if (isElementLinkGroup(child)) {
        if (dot == std::string::npos) {
            return child;
        }
        auto element = child->getSubObject(sub.substr(0, dot + 1).c_str(), nullptr, nullptr, false);
        auto group = static_cast<App::Link*>(child);
        if (element && element != child
            && group->ElementList.find(element->getNameInDocument()) == element) {
            if (placement) {
                *placement = App::GeoFeature::getPlacementFromProp(element, "Placement");
            }
            return element;
        }
        return child;
    }
    // Down the flexible instances, each a container of its parts
    std::string rest = sub;
    for (auto instance = flexibleInstance(child); instance; instance = flexibleInstance(child)) {
        dot = rest.find('.');
        if (dot == std::string::npos) {
            break;
        }
        auto part = instance->Group.find(rest.substr(0, dot).c_str());
        if (!part) {
            break;
        }
        if (placement) {
            *placement = *placement * App::GeoFeature::getPlacementFromProp(part, "Placement");
        }
        child = part;
        rest = rest.substr(dot + 1);
    }
    return child;
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

    ADD_PROPERTY_TYPE(Multiply, (false), "Multiply", App::Prop_None,
                      "Multiply the joint over the elements of the link array the first\n"
                      "side is on, each paired with an element of the second side's\n"
                      "component like the second side's: a circle of its radius in its\n"
                      "plane, or a cylinder of its radius and length level with it");
    ADD_PROPERTY_TYPE(AutoCount, (true), "Multiply", App::Prop_None,
                      "Grow the array to as many elements as there are targets");
    ADD_PROPERTY_TYPE(Targets, (), "Multiply", App::Prop_None,
                      "The elements of the second side's component to pair with, picked\n"
                      "by hand (Edge5, Face12, or a path below the component); empty,\n"
                      "every one like the second side's");
    ADD_PROPERTY_TYPE(_Instances, (), "Multiply",
                      App::PropertyType(App::Prop_Hidden | App::Prop_Output | App::Prop_ReadOnly),
                      "The instance joints of a multiplied joint, one per pair");
    ADD_PROPERTY_TYPE(_Multiplier, (nullptr), "Multiply",
                      App::PropertyType(App::Prop_Hidden | App::Prop_ReadOnly),
                      "The multiplied joint this is an instance of");
    ADD_PROPERTY_TYPE(_Instance, (-1), "Multiply",
                      App::PropertyType(App::Prop_Hidden | App::Prop_ReadOnly),
                      "The index of the array element of this instance");
    ADD_PROPERTY_TYPE(_Target, (""), "Multiply",
                      App::PropertyType(App::Prop_Hidden | App::Prop_ReadOnly),
                      "The edge of the second side's component this instance is paired with");

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

App::Part* Joint::getContainer() const
{
    auto group = App::GeoFeatureGroupExtension::getGroupOfObject(this);
    if (group && (group->isDerivedFrom<AssemblyObject>() || group->isDerivedFrom<AssemblyLink>())) {
        return static_cast<App::Part*>(group);
    }
    return nullptr;
}

namespace
{
/// The placement of a component in the assembly: a link group's element is
/// placed in its group
Base::Placement componentPlacement(App::DocumentObject* obj)
{
    Base::Placement plc = App::GeoFeature::getPlacementFromProp(obj, "Placement");
    App::Link* group = nullptr;
    if (linkElementIndex(obj, &group) >= 0 && group) {
        plc = App::GeoFeature::getPlacementFromProp(group, "Placement") * plc;
    }
    return plc;
}

/// The element of a multiplied joint's array an instance is on, or null
App::DocumentObject* arrayElement(App::DocumentObject* picked, int index)
{
    App::Link* array = nullptr;
    if (!picked || linkElementIndex(picked, &array) < 0 || !array) {
        return nullptr;
    }
    const auto& elements = array->ElementList.getValues();
    return index >= 0 && index < static_cast<int>(elements.size()) ? elements[index] : nullptr;
}
}  // namespace

Joint* Joint::getMultiplier() const
{
    return Base::freecad_dynamic_cast<Joint>(_Multiplier.getValue());
}

std::vector<Joint*> Joint::getInstances() const
{
    std::vector<Joint*> res;
    for (auto obj : _Instances.getValues()) {
        auto joint = Base::freecad_dynamic_cast<Joint>(obj);
        if (joint && !joint->isRemoving() && joint->getMultiplier() == this) {
            res.push_back(joint);
        }
    }
    return res;
}

App::DocumentObject* Joint::getComponent(int side) const
{
    if (auto multiplier = getMultiplier()) {
        // The multiplied joint's, on side 1 the instance's array element
        auto component = multiplier->getComponent(side);
        return side == 1 ? arrayElement(component, _Instance.getValue()) : component;
    }
    auto link = getElementLink(side);
    if (!link) {
        return nullptr;
    }
    return componentOfLink(getContainer(), link, nullptr);
}

App::DocumentObject* Joint::componentOfLink(App::Part* assembly,
                                            Part::ElementLink* link,
                                            Base::Placement* placement)
{
    auto obj = link->Element.getValue();
    const auto& subs = link->Element.getSubValues();
    const std::string sub = subs.empty() ? std::string() : subs.front();
    if (auto child = ::componentOf(assembly, obj, sub, placement)) {
        return child;
    }
    // A convenience element in the assembly's own Elements group: its
    // target is the component
    auto element = resolveElement(obj, sub);
    if (!element) {
        return nullptr;
    }
    const auto& tsubs = element->Target.getSubValues();
    return ::componentOf(assembly,
                         element->Target.getValue(),
                         tsubs.empty() ? std::string() : tsubs.front(),
                         placement);
}

JointSide Joint::getSide(int side) const
{
    if (auto multiplier = getMultiplier()) {
        // The multiplied joint's side, carried: on side 1 from the element
        // picked to this instance's, on side 2 from the edge picked to this
        // instance's edge, the offsets kept
        JointSide res = multiplier->getSide(side);
        if (!res.isValid() || res.wholeObject) {
            return JointSide();
        }
        if (side == 1) {
            auto element = arrayElement(res.component, _Instance.getValue());
            if (!element) {
                return JointSide();
            }
            const Base::Placement move
                = componentPlacement(element) * componentPlacement(res.component).inverse();
            res.component = element;
            res.placement = App::GeoFeature::getPlacementFromProp(element, "Placement");
            res.frame = move * res.frame;
            res.shape.transformShape(move.toMatrix(), false);
            return res;
        }
        Part::TopoShape target;
        try {
            target = Part::Feature::getTopoShape(res.component, _Target.getValue(), true);
        }
        catch (const Base::Exception&) {
        }
        if (target.isNull()) {
            return JointSide();
        }
        res.frame = findElementFrame(target) * findElementFrame(res.shape).inverse() * res.frame;
        res.shape = target;
        return res;
    }
    JointSide res;
    res.link = getElementLink(side);
    if (!res.link) {
        return res;
    }
    res.component = componentOfLink(getContainer(), res.link, &res.placement);

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
    if (!directChild(getContainer(), obj)) {
        auto element = resolveElement(obj, sub);
        if (element && !element->Detach.getValue() && element->Target.getValue()) {
            geomObj = element->Target.getValue();
            const auto& tsubs = element->Target.getSubValues();
            geomSub = tsubs.empty() ? std::string() : tsubs.front();
        }
    }
    // An element of an array (componentOf()) through the element itself: the
    // array's shape caches its elements where they were, which a solve, that
    // moves an element without a recompute, makes stale
    if (res.component && res.component != geomObj && directChild(getContainer(), geomObj)
        && isElementLinkGroup(geomObj)) {
        auto dot = geomSub.find('.');
        if (dot != std::string::npos) {
            geomObj = res.component;
            geomSub = geomSub.substr(dot + 1);
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
        res.stored = !res.shape.isNull();
    }
    // A path naming no element is a whole object, framed by its placement
    // whether it has a shape or not, as upstream's joint takes it: a body,
    // an empty link, a datum, an axis (whose shape is an edge, but whose
    // frame is along it)
    Base::Placement wholePlacement;
    if (Part::Element::getWholeObjectPlacement(geomObj, geomSub.c_str(), wholePlacement)) {
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
    if (res.stored && res.component) {
        // The copy is where the component was when it was taken, which a
        // solve since makes stale: the frame is the last one the geometry
        // gave, relative to the component as execute() keeps it, and the
        // copy goes with it
        const auto& local = side == 1 ? Placement1 : Placement2;
        const Base::Placement now = res.placement * local.getValue();
        res.shape.transformShape((now * res.frame.inverse()).toMatrix(), false);
        res.frame = now;
    }
    return res;
}

bool Joint::getStoredSide(int side,
                          App::DocumentObject*& component,
                          Base::Placement& placement,
                          Base::Placement& local) const
{
    if (isTouched() || isError() || getMultiplier() || mustExecute()) {
        return false;
    }
    auto link = getElementLink(side);
    if (!link || link->isTouched()) {
        return false;
    }
    component = componentOfLink(getContainer(), link, &placement);
    if (!component) {
        return false;
    }
    local = (side == 1 ? Placement1 : Placement2).getValue();
    return true;
}

Base::Placement Joint::getFrame(int side) const
{
    App::DocumentObject* component = nullptr;
    Base::Placement placement;
    Base::Placement local;
    if (getStoredSide(side, component, placement, local)) {
        return placement * local;
    }
    return getSide(side).frame;
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
        const Base::Placement local = jointSide.placement.inverse() * jointSide.frame;
        if (!plc.getValue().isSame(local, 1e-12)) {
            plc.setValue(local);
        }
    }
    if (!isInstance() && (Multiply.getValue() || _Instances.getSize())) {
        try {
            updateInstances();
        }
        catch (const Base::Exception& e) {
            return new App::DocumentObjectExecReturn(e.what());
        }
    }
    return App::DocumentObject::StdReturn;
}

namespace
{
/// What makes a multiplied joint's target like the one picked: a circle's
/// radius and plane, or a cylinder's radius, axis direction and length,
/// level with it
struct Likeness
{
    TopAbs_ShapeEnum type = TopAbs_SHAPE;
    gp_Dir axis;
    double radius = 0.0;
    double length = 0.0;
    /// The frame's origin: a circle's centre, the point of a cylinder's
    /// axis level with its centre of gravity
    Base::Vector3d origin;

    bool isLike(const Likeness& other) const
    {
        const double tol = Precision::Confusion();
        if (type != other.type || std::abs(radius - other.radius) > tol
            || std::abs(length - other.length) > tol
            || !axis.IsParallel(other.axis, Precision::Angular())) {
            return false;
        }
        const Base::Vector3d n(axis.X(), axis.Y(), axis.Z());
        return std::abs((other.origin - origin) * n) <= tol;
    }
};

/// The likeness of a circular edge or a cylindrical face, if it is one
bool likenessOf(const Part::TopoShape& shape, Likeness& res)
{
    if (shape.isNull()) {
        return false;
    }
    const TopoDS_Shape s = Part::singleSubShape(shape.getShape());
    if (s.ShapeType() == TopAbs_EDGE) {
        BRepAdaptor_Curve curve(TopoDS::Edge(s));
        if (curve.GetType() != GeomAbs_Circle) {
            return false;
        }
        res.radius = curve.Circle().Radius();
        res.axis = curve.Circle().Axis().Direction();
    }
    else if (s.ShapeType() == TopAbs_FACE) {
        BRepAdaptor_Surface surface(TopoDS::Face(s));
        if (surface.GetType() != GeomAbs_Cylinder) {
            return false;
        }
        res.radius = surface.Cylinder().Radius();
        res.axis = surface.Cylinder().Axis().Direction();
        res.length = std::abs(surface.LastVParameter() - surface.FirstVParameter());
    }
    else {
        return false;
    }
    res.type = s.ShapeType();
    res.origin = findElementFrame(shape).getPosition();
    return true;
}

/// The properties an instance joint takes from its multiplied joint
const char* const instanceProperties[] = {"JointType",
                                          "Angle",
                                          "Distance",
                                          "Distance2",
                                          "EnableLengthMin",
                                          "EnableLengthMax",
                                          "EnableAngleMin",
                                          "EnableAngleMax",
                                          "LengthMin",
                                          "LengthMax",
                                          "AngleMin",
                                          "AngleMax",
                                          "Suppressed"};

void syncInstance(const Joint* multiplier, Joint* instance)
{
    for (const char* name : instanceProperties) {
        auto from = multiplier->getPropertyByName(name);
        auto to = instance->getPropertyByName(name);
        if (from && to && !from->isSame(*to)) {
            to->Paste(*from);
        }
    }
}
}  // namespace

void Joint::onChanged(const App::Property* prop)
{
    App::DocumentObject::onChanged(prop);
    if (isRestoring() || isInstance() || !_Instances.getSize()) {
        return;
    }
    const char* name = prop->getName();
    if (!name) {
        return;
    }
    for (const char* synced : instanceProperties) {
        if (strcmp(name, synced) == 0) {
            for (auto instance : getInstances()) {
                syncInstance(this, instance);
            }
            return;
        }
    }
}

void Joint::updateInstances()
{
    auto doc = getDocument();
    auto assembly = getAssembly();
    auto current = getInstances();
    auto removeAll = [&]() {
        _Instances.setValues({});
        for (auto instance : current) {
            if (!instance->isRemoving()) {
                doc->removeObject(instance->getNameInDocument());
            }
        }
    };
    if (!Multiply.getValue() || !assembly) {
        removeAll();
        return;
    }

    // The array and the element picked
    const auto side1 = getSide(1);
    const auto side2 = getSide(2);
    App::Link* array = nullptr;
    if (!side1.isValid() || linkElementIndex(side1.component, &array) < 0 || !array) {
        removeAll();
        FC_THROWM(Base::ValueError, "To multiply, the first side is on an element of a link array");
    }
    if (!side2.isValid() || side2.wholeObject) {
        removeAll();
        FC_THROWM(Base::ValueError, "To multiply, the second side is an element");
    }
    const double tol = Precision::Confusion();
    auto targetShape = [&](const std::string& name) {
        try {
            return Part::Feature::getTopoShape(side2.component, name.c_str(), true);
        }
        catch (const Base::Exception&) {
            return Part::TopoShape();
        }
    };

    std::vector<std::string> targets;
    if (Targets.getSize()) {
        // Picked by hand: those that resolve
        for (const auto& name : Targets.getValues()) {
            if (!name.empty() && !targetShape(name).isNull()
                && std::find(targets.begin(), targets.end(), name) == targets.end()) {
                targets.push_back(name);
            }
        }
    }
    else {
        // The elements like the one picked: circles of its radius in its
        // plane, or cylinders of its radius and length level with it. One
        // per place, where a circle or a cylinder is split in two.
        Likeness picked;
        if (!likenessOf(side2.shape, picked)) {
            removeAll();
            FC_THROWM(Base::ValueError,
                      "To multiply, the second side is a circular edge or a cylindrical face, "
                      "or the targets are picked");
        }
        const auto whole = Part::Feature::getTopoShape(side2.component, "", false);
        const TopAbs_ShapeEnum type = picked.type;
        const char* prefix = type == TopAbs_EDGE ? "Edge" : "Face";
        std::vector<Base::Vector3d> origins;
        const int count = static_cast<int>(whole.countSubShapes(type));
        for (int i = 1; i <= count; ++i) {
            const std::string name = prefix + std::to_string(i);
            Likeness like;
            if (!likenessOf(Part::TopoShape(whole.getSubShape(type, i)), like)
                || !picked.isLike(like)) {
                continue;
            }
            if (std::any_of(origins.begin(), origins.end(), [&](const Base::Vector3d& o) {
                    return Base::Distance(o, like.origin) < tol;
                })) {
                continue;
            }
            origins.push_back(like.origin);
            targets.push_back(name);
        }
    }

    if (AutoCount.getValue() && array->ElementList.getSize() < static_cast<int>(targets.size())) {
        array->ElementCount.setValue(static_cast<long>(targets.size()));
    }
    const auto& elements = array->ElementList.getValues();
    const std::size_t pairs = std::min(elements.size(), targets.size());

    // Where each element's and each edge's frame is, for pairing
    const Base::Placement pickedPlc = componentPlacement(side1.component);
    auto elementOrigin = [&](std::size_t i) {
        return (componentPlacement(elements[i]) * pickedPlc.inverse() * side1.frame).getPosition();
    };
    const Base::Placement pickedFrame = findElementFrame(side2.shape).inverse() * side2.frame;
    std::map<std::string, Base::Vector3d> targetOrigins;
    for (const auto& name : targets) {
        targetOrigins[name] = (findElementFrame(targetShape(name)) * pickedFrame).getPosition();
    }
    auto targetOrigin = [&](const std::string& name) {
        return targetOrigins[name];
    };

    // The pairs kept, while their element and edge are there
    std::vector<std::pair<int, std::string>> made;
    std::vector<bool> elementUsed(elements.size(), false);
    std::set<std::string> targetUsed;
    std::vector<Joint*> kept;
    for (auto instance : current) {
        const int i = instance->_Instance.getValue();
        const std::string t = instance->_Target.getValue();
        if (made.size() >= pairs || i < 0 || i >= static_cast<int>(elements.size())
            || elementUsed[i] || targetUsed.count(t)
            || std::find(targets.begin(), targets.end(), t) == targets.end()) {
            continue;
        }
        elementUsed[i] = true;
        targetUsed.insert(t);
        made.emplace_back(i, t);
        kept.push_back(instance);
    }
    std::vector<std::pair<int, std::string>> added;
    // The element and the edge picked, together
    const int pickedIndex = linkElementIndex(side1.component);
    if (pickedIndex >= 0 && !elementUsed[pickedIndex] && made.size() < pairs) {
        for (const auto& t : targets) {
            if (!targetUsed.count(t)
                && Base::Distance(targetOrigin(t), side2.frame.getPosition()) < tol) {
                elementUsed[pickedIndex] = true;
                targetUsed.insert(t);
                added.emplace_back(pickedIndex, t);
                break;
            }
        }
    }
    // The rest, nearest first
    std::vector<std::tuple<double, int, std::string>> distances;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (elementUsed[i]) {
            continue;
        }
        const auto origin = elementOrigin(i);
        for (const auto& t : targets) {
            if (!targetUsed.count(t)) {
                distances.emplace_back(Base::Distance(origin, targetOrigin(t)), static_cast<int>(i), t);
            }
        }
    }
    std::sort(distances.begin(), distances.end());
    for (const auto& [distance, i, t] : distances) {
        if (made.size() + added.size() >= pairs) {
            break;
        }
        if (elementUsed[i] || targetUsed.count(t)) {
            continue;
        }
        elementUsed[i] = true;
        targetUsed.insert(t);
        added.emplace_back(i, t);
    }

    // The instance joints: the kept, and one for each new pair
    std::vector<App::DocumentObject*> instances(kept.begin(), kept.end());
    for (auto instance : current) {
        if (std::find(kept.begin(), kept.end(), instance) == kept.end() && !instance->isRemoving()) {
            doc->removeObject(instance->getNameInDocument());
        }
    }
    for (const auto& [i, t] : added) {
        auto instance = static_cast<Joint*>(
            doc->addObject("Assembly::Joint", (std::string(getNameInDocument()) + "_Instance").c_str())
        );
        // An instance before the assembly takes it: its Joints group shows
        // the joints, not their instances
        instance->_Multiplier.setValue(this);
        instance->_Instance.setValue(i);
        instance->_Target.setValue(t);
        instance->Label.setValue(std::string(Label.getValue()) + "_" + std::to_string(i));
        // The assembly's child, as the joint is, whatever its GroupMode
        auto children = assembly->Group.getValues();
        children.push_back(instance);
        assembly->Group.setValues(children);
        instances.push_back(instance);
    }
    _Instances.setValues(instances);
    for (auto instance : getInstances()) {
        syncInstance(this, instance);
    }
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
void removeElementLink(App::Part* assembly, Part::ElementLink* link)
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
        removeElementLink(getContainer(), old);
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
    // A multiplied joint's instance joints go with it
    for (auto instance : getInstances()) {
        if (!instance->isRemoving() && instance->getDocument()) {
            instance->getDocument()->removeObject(instance->getNameInDocument());
        }
    }
    // The joint holds its element links: they go with it, unless another
    // joint uses one too
    auto assembly = getContainer();
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

void Joint::relocateCopies(App::Part* assembly,
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
        const auto& subs = link->Element.getSubValues();
        const std::string sub = subs.empty() ? std::string() : subs.front();
        if (directChild(assembly, obj)) {
            // An element the component declares, or one of an array's
            // elements through the array
            if (componentOf(assembly, obj, sub) == component) {
                move(link);
            }
            continue;
        }
        auto element = resolveElement(obj, sub);
        if (!element || element->Detach.getValue()) {
            continue;
        }
        const auto& tsubs = element->Target.getSubValues();
        if (componentOf(assembly,
                        element->Target.getValue(),
                        tsubs.empty() ? std::string() : tsubs.front())
            != component) {
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

/// Whether a link property holds this object and subname
bool holds(const App::PropertyLinkSub& prop, App::DocumentObject* obj, const std::string& sub)
{
    if (prop.getValue() != obj) {
        return false;
    }
    const auto& subs = prop.getSubValues();
    return (subs.empty() && sub.empty()) || (!subs.empty() && subs.front() == sub);
}

/// An element of the group with this target and vertex, or null
Part::Element* findElement(App::AuxGroup* group,
                           App::DocumentObject* target,
                           const std::string& sub,
                           App::DocumentObject* vertex,
                           const std::string& vertexSub)
{
    if (!group) {
        return nullptr;
    }
    for (auto child : group->Group.getValues()) {
        auto element = Base::freecad_dynamic_cast<Part::Element>(child);
        if (element && holds(element->Target, target, sub)
            && holds(element->Vertex, vertex, vertex ? vertexSub : std::string())) {
            return element;
        }
    }
    return nullptr;
}

/** An element of the container on this target, made if there is none
 *
 * A vertex picked with the geometry places the frame's origin: the element
 * keeps it, and its Offset follows it, so that an element link refers to an
 * element only (docs/AssemblyObjectModel.md sec 4.3). An element with
 * another vertex, or none, is another element.
 */
Part::Element* declare(App::DocumentObject* container,
                       App::DocumentObject* target,
                       const std::string& sub,
                       App::DocumentObject* vertex,
                       const std::string& vertexSub,
                       const char* label)
{
    auto owner = dynamic_cast<App::AuxGroupOwner*>(container);
    if (auto element = findElement(owner ? owner->getElementsGroup(false) : nullptr,
                                   target,
                                   sub,
                                   vertex,
                                   vertexSub)) {
        return element;
    }
    auto element = Part::Element::create(container, target, sub.c_str(), label);
    if (vertex) {
        element->Vertex.setValue(vertex, std::vector<std::string> {vertexSub});
        // Followed now on the pick's geometry: the new element has no copy yet
        element->updateVertexOffset(Part::Feature::getTopoShape(target, sub.c_str(), true));
    }
    return element;
}

/** The direct child of the assembly that a part of a flexible sub-assembly
 * instance is in, at any depth, and the path from it to the part ("B001.",
 * "Inner.B001."); null when the part is in none
 */
App::DocumentObject* instanceOf(const App::Part* assembly,
                                App::DocumentObject* part,
                                std::string& path)
{
    path.clear();
    auto obj = part;
    for (int depth = 0; obj && depth < 16; ++depth) {
        App::DocumentObject* holder = nullptr;
        for (auto parent : obj->getInList()) {
            auto instance = flexibleInstance(parent);
            if (instance && instance->Group.find(obj->getNameInDocument()) == obj) {
                holder = instance;
                break;
            }
        }
        if (!holder) {
            return nullptr;
        }
        path = std::string(obj->getNameInDocument()) + "." + path;
        if (directChild(assembly, holder)) {
            return holder;
        }
        obj = holder;
    }
    return nullptr;
}

/** The direct child of a container a path from it starts with, and the rest
 * of the path: a body's own shape is its tip's
 */
App::DocumentObject* childOf(App::DocumentObject* container, const std::string& sub, std::string& rest)
{
    auto dot = sub.find('.');
    if (dot != std::string::npos) {
        const std::string name = sub.substr(0, dot);
        auto child = container->getDocument()->getObject(name.c_str());
        auto group = container->getExtensionByType<App::GroupExtension>(true);
        if (child && group && group->Group.find(name.c_str()) == child) {
            rest = sub.substr(dot + 1);
            return child;
        }
    }
    else if (auto tip = container->getPropertyByName<App::PropertyLink>("Tip")) {
        rest = sub;
        return tip->getValue();
    }
    return nullptr;
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
    std::string sub = subname ? subname : "";
    std::string vertexSub = vertex ? vertex : "";
    // An element of a link array whose elements are each a component is
    // taken through the array, which is the assembly's child
    App::Link* array = nullptr;
    if (linkElementIndex(component, &array) >= 0 && array && directChild(assembly, array)
        && isElementLinkGroup(array)) {
        const std::string step = std::string(component->getNameInDocument()) + ".";
        sub = step + sub;
        if (!vertexSub.empty()) {
            vertexSub = step + vertexSub;
        }
        component = array;
    }
    // A part of a flexible sub-assembly instance is reached through the
    // instance, the assembly's child, with the path to the part in front
    App::DocumentObject* via = component;
    std::string prefix;
    if (!directChild(assembly, component)) {
        via = instanceOf(assembly, component, prefix);
        if (!via) {
            FC_THROWM(Base::ValueError,
                      component->getFullName() << " is not a component of "
                                               << assembly->getFullName());
        }
    }
    // The path names the element of the array: what declares elements is
    // what every element links to, so the element is the assembly's own
    if (isElementLinkGroup(component)) {
        local = true;
    }

    Part::ElementLink* link = nullptr;
    // The component declares elements: the element goes in its Elements
    // group, as a direct child of it plus the rest of the path, and so does
    // its vertex
    if (local ? nullptr : elementsOwner(component)) {
        auto container = component->getLinkedObject(true);
        std::string rest;
        std::string vertexRest;
        auto target = childOf(container, sub, rest);
        auto vertex = vertexSub.empty() ? nullptr : childOf(container, vertexSub, vertexRest);
        if (target && (vertex || vertexSub.empty())) {
            auto element = declare(container, target, rest, vertex, vertexRest, nullptr);
            link = Part::ElementLink::create(
                assembly,
                via,
                (prefix + "$$Elements.$" + element->Label.getValue() + ".").c_str()
            );
        }
    }
    if (!link) {
        // A convenience element in the assembly's own Elements group
        auto element = declare(assembly,
                               via,
                               prefix + sub,
                               vertexSub.empty() ? nullptr : via,
                               vertexSub.empty() ? vertexSub : prefix + vertexSub,
                               "_Element");
        link = Part::ElementLink::create(
            assembly,
            assembly->getElementsGroup(false),
            (std::string("$") + element->Label.getValue() + ".").c_str()
        );
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
