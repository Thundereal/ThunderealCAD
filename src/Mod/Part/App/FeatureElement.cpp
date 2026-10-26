// SPDX-License-Identifier: LGPL-2.1-or-later

/****************************************************************************
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
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU      *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ****************************************************************************/

#include "PreCompiled.h"

#include <algorithm>

#include <BRep_Tool.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <App/Application.h>
#include <App/AuxGroup.h>
#include <App/Document.h>
#include <App/ElementNamingUtils.h>
#include <App/GroupExtension.h>
#include <App/Link.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Tools.h>

#include "FeatureElement.h"
#include "ElementGeometry.h"

using namespace Part;

/// The shape of the first link of a link property, at its object's
/// placement in the object's parent
static TopoShape linkedShape(const App::PropertyLinkSub &prop)
{
    auto obj = prop.getValue();
    if (!obj || !obj->isAttachedToDocument())
        return TopoShape();
    const auto &subs = prop.getSubValues();
    const char *sub = subs.empty() ? "" : subs.front().c_str();
    return Feature::getTopoShape(obj, sub, true);
}

/// The object a link's subname resolves to, when not the linked object
static App::DocumentObject *resolvedObject(const App::PropertyLinkSub &prop)
{
    auto obj = prop.getValue();
    if (!obj || !obj->isAttachedToDocument())
        return nullptr;
    const auto &subs = prop.getSubValues();
    if (subs.empty() || subs.front().empty())
        return nullptr;
    auto sobj = obj->getSubObject(subs.front().c_str());
    return sobj && sobj != obj ? sobj : nullptr;
}

/// Keep \a resolved on what \a prop resolves to
static void syncResolved(App::PropertyXLink &resolved, const App::PropertyLinkSub &prop)
{
    auto obj = resolvedObject(prop);
    if (resolved.getValue() != obj)
        resolved.setValue(obj);
}

/** Keep the copy of what is not loaded now, and say so: not an error, the
 * copy serves until the geometry is back. The dependency on the geometry
 * is kept as well, so that it is followed again once loaded.
 *
 * @return true when the link stops at an object not loaded
 */
static bool keepUnresolved(Part::Feature *feature,
                           const App::PropertyLinkSub &prop,
                           App::PropertyBool &unresolved)
{
    if (!Element::stopsUnloaded(prop)) {
        if (unresolved.getValue())
            unresolved.setValue(false);
        return false;
    }
    if (!unresolved.getValue()) {
        unresolved.setValue(true);
        Base::Console().Warning("%s keeps its copy: %s is not loaded\n",
                                feature->getFullName().c_str(),
                                prop.getValue()->getFullName().c_str());
    }
    return true;
}

static bool isEditing(const App::DocumentObject *obj)
{
    auto doc = obj->getDocument();
    return doc && !obj->isRestoring() && !obj->isRemoving()
        && !doc->testStatus(App::Document::Restoring) && !doc->isPerformingTransaction();
}

/** Follow the document of the geometry closing and coming back
 *
 * Closed, the link on the geometry keeps the object's name, and the copy
 * serves. Back, the link is attached again, and the copy is taken again if
 * that document changed since: the link keeps the touch then, and purges it
 * when not (PropertyXLink::restoreLink()).
 */
static void followResolved(Part::Feature *feature,
                           const App::PropertyXLink &resolved,
                           App::PropertyBool &unresolved)
{
    if (resolved.getValue()) {
        if (unresolved.getValue()) {
            unresolved.setValue(false);
            feature->touch();
        }
        return;
    }
    const char *name = resolved.getObjectName();
    if (name && *name && !unresolved.getValue())
        unresolved.setValue(true);
}

/// Keep \a shape as the copy, where it is: a feature's Placement is its
/// shape's, and setting Shape alone would put the copy at the old one
static void setCopy(Part::Feature *feature, const TopoShape &shape)
{
    feature->Placement.setValue(shape.getPlacement());
    feature->Shape.setValue(shape);
}

PROPERTY_SOURCE(Part::Element, Part::Feature)

Element::Element()
{
    ADD_PROPERTY_TYPE(Target, (nullptr), "Element", App::Prop_None,
            "A direct child of the container, and a subname into it");
    ADD_PROPERTY_TYPE(Offset, (Base::Placement()), "Element", App::Prop_None,
            "A placement of the element's frame relative to its geometry,\n"
            "for every user of the element");
    ADD_PROPERTY_TYPE(Detach, (false), "Element", App::Prop_None,
            "Keep the copy of the geometry and stop following the target");
    ADD_PROPERTY_TYPE(_Resolved, (nullptr), "Element",
            (App::PropertyType)(App::Prop_Hidden|App::Prop_Output),
            "What Target resolves to, the dependency on the geometry itself");
    _Resolved.setScope(App::LinkScope::Global);
    // Its document may be closed, or missing: not an error of the element
    _Resolved.setAllowPartial(true);
    ADD_PROPERTY_TYPE(Unresolved, (false), "Element",
            (App::PropertyType)(App::Prop_ReadOnly|App::Prop_Output),
            "The target is not loaded now (a link to a closed or missing\n"
            "document): the copy is kept until it is back");
    // The link copies geometry and places nothing: Global scope keeps it out
    // of the coordinate-system checks and of what moves with what
    Target.setScope(App::LinkScope::Global);
}

bool Element::isUnloaded(const App::DocumentObject *obj)
{
    if (!obj || !obj->isAttachedToDocument())
        return false;
    if (obj->testStatus(App::PartialObject))
        return true;
    auto ext = obj->getExtensionByType<App::LinkBaseExtension>(true);
    auto xlink = ext ? Base::freecad_dynamic_cast<App::PropertyXLink>(
            ext->getLinkedObjectProperty()) : nullptr;
    if (!xlink || xlink->getValue())
        return false;
    const char *name = xlink->getObjectName();
    return name && *name;
}

bool Element::stopsUnloaded(const App::PropertyLinkSub &prop)
{
    auto obj = prop.getValue();
    if (!obj || !obj->isAttachedToDocument())
        return false;
    if (isUnloaded(obj))
        return true;
    const auto &subs = prop.getSubValues();
    if (subs.empty())
        return false;
    // Each step of the path, as the selection makes them, up to the first
    // that does not resolve
    const std::string &sub = subs.front();
    for (auto pos = sub.find('.'); pos != std::string::npos; pos = sub.find('.', pos + 1)) {
        auto sobj = obj->getSubObject(sub.substr(0, pos + 1).c_str());
        if (!sobj)
            return false;
        if (isUnloaded(sobj))
            return true;
    }
    return false;
}

bool Element::getWholeObjectPlacement(App::DocumentObject *obj,
                                      const char *subname,
                                      Base::Placement &placement)
{
    if (!obj || !obj->isAttachedToDocument())
        return false;
    if (!subname)
        subname = "";
    if (*Data::findElementName(subname))
        return false;
    Base::Matrix4D mat;
    if (!obj->getSubObject(subname, nullptr, &mat, true))
        return false;
    placement.fromMatrix(mat);
    return true;
}

/// A link to a whole object with no shape: its placement, and no copy
static bool takeWholeObject(Part::Feature *feature, const App::PropertyLinkSub &prop)
{
    const auto &subs = prop.getSubValues();
    Base::Placement plc;
    if (!Element::getWholeObjectPlacement(prop.getValue(),
                                          subs.empty() ? "" : subs.front().c_str(), plc))
        return false;
    feature->Shape.setValue(TopoShape());
    feature->Placement.setValue(plc);
    return true;
}

App::DocumentObjectExecReturn *Element::execute()
{
    if (Detach.getValue())
        return App::DocumentObject::StdReturn;
    if (!Target.getValue())
        return new App::DocumentObjectExecReturn("No target");
    auto shape = linkedShape(Target);
    if (shape.isNull() && keepUnresolved(this, Target, Unresolved))
        return App::DocumentObject::StdReturn;
    if (!shape.isNull() && Unresolved.getValue())
        Unresolved.setValue(false);
    syncResolved(_Resolved, Target);
    if (shape.isNull()) {
        if (takeWholeObject(this, Target))
            return App::DocumentObject::StdReturn;
        return new App::DocumentObjectExecReturn("Target not found");
    }
    setCopy(this, shape);
    return App::DocumentObject::StdReturn;
}

void Element::onChanged(const App::Property *prop)
{
    // What the target is may decide where the element is shown: a body
    // shows it under the solid body of its target feature
    if (prop == &Target && isEditing(this)) {
        syncResolved(_Resolved, Target);
        if (auto owner = dynamic_cast<App::AuxGroupOwner*>(getContainer()))
            owner->onElementsChanged();
    }
    if (prop == &_Resolved && isEditing(this))
        followResolved(this, _Resolved, Unresolved);
    if (prop == &Shape && !isRestoring())
        touchGroups();
    inherited::onChanged(prop);
}

void Element::onDocumentRestored()
{
    inherited::onDocumentRestored();
    // Restored with the document of the geometry missing
    if (!_Resolved.getValue() && *_Resolved.getObjectName() && !Unresolved.getValue())
        Unresolved.setValue(true);
}

void Element::touchGroups()
{
    // What reads the copy through the group, or through the container's
    // "$$Elements." -- a link's shape -- may hold a cached shape: the
    // group, then the container, say that a child changed, as a group's
    // _GroupTouched does
    auto group = getElementsGroup();
    if (!group)
        return;
    group->_GroupTouched.touch();
    auto container = group->getOwner();
    auto touched = container ? Base::freecad_dynamic_cast<App::PropertyBool>(
            container->getPropertyByName("_GroupTouched")) : nullptr;
    if (touched && touched->getContainer() == container) {
        // As an output, so that the container is not marked for recompute
        Base::ObjectStatusLocker<App::Property::Status, App::Property> guard(
                App::Property::Output, touched);
        touched->touch();
    }
}

App::AuxGroup *Element::getElementsGroup() const
{
    return App::AuxGroup::getOwningGroupOf(this);
}

App::DocumentObject *Element::getContainer() const
{
    auto group = getElementsGroup();
    return group ? group->getOwner() : nullptr;
}

int Element::canLoadPartial() const
{
    auto group = getElementsGroup();
    return group ? group->canLoadPartial() : 0;
}

void Element::onBeforeChangeLabel(std::string &newLabel)
{
    // Unique among the siblings: the Elements group is the namespace
    // "$Label" resolves in
    auto group = getElementsGroup();
    if (!group)
        return;
    std::vector<std::string> labels;
    for (auto sibling : group->Group.getValues()) {
        if (sibling != this)
            labels.push_back(sibling->Label.getStrValue());
    }
    if (std::find(labels.begin(), labels.end(), newLabel) != labels.end())
        newLabel = Base::Tools::getUniqueName(newLabel, labels, 3);
}

static ParameterGrp::handle partParams()
{
    return App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Mod/Part");
}

bool Element::autoVisibility()
{
    return partParams()->GetBool("AutoElementVisibility", true);
}

void Element::setAutoVisibility(bool on)
{
    partParams()->SetBool("AutoElementVisibility", on);
}

Element *Element::create(App::DocumentObject *container,
                         App::DocumentObject *target,
                         const char *subname,
                         const char *label)
{
    if (!container || !container->isAttachedToDocument())
        THROWM(Base::ValueError, "No container");
    auto owner = dynamic_cast<App::AuxGroupOwner*>(container);
    auto group = owner ? owner->getElementsGroup(true) : nullptr;
    if (!group)
        FC_THROWM(Base::TypeError, container->getFullName() << " cannot declare elements");

    auto doc = container->getDocument();
    auto element = static_cast<Element*>(doc->addObject("Part::Element", "Element"));
    // In the group first, so that the label is made unique among the siblings
    auto children = group->Group.getValues();
    children.push_back(element);
    group->Group.setValues(children);
    element->Label.setValue(label && *label ? label : "Element");
    std::vector<std::string> subs;
    if (subname && *subname)
        subs.emplace_back(subname);
    element->Target.setValue(target, std::move(subs));
    // The geometry is the target's; with auto visibility the copy is seen
    // while the element is selected or preselected
    element->Visibility.setValue(!autoVisibility());
    return element;
}

// ------------------------------------------------------------------------

PROPERTY_SOURCE(Part::ElementLink, Part::Feature)

ElementLink::ElementLink()
{
    ADD_PROPERTY_TYPE(Element, (nullptr), "Element", App::Prop_None,
            "A direct child of the container and the subname to one of its\n"
            "elements, or the container's own Elements group and an element");
    ADD_PROPERTY_TYPE(Offset, (Base::Placement()), "Element", App::Prop_None,
            "A placement of the frame relative to the element's, for this use only");
    ADD_PROPERTY_TYPE(Vertex, (nullptr), "Element", App::Prop_None,
            "A vertex placing the frame's origin on the element; Offset follows it");
    ADD_PROPERTY_TYPE(VertexPosition, (Base::Vector3d()), "Element", App::Prop_Hidden,
            "Where Vertex was in the element's frame when Offset last followed it");
    ADD_PROPERTY_TYPE(_Resolved, (nullptr), "Element",
            (App::PropertyType)(App::Prop_Hidden|App::Prop_Output),
            "What Element resolves to, the dependency on the geometry itself");
    _Resolved.setScope(App::LinkScope::Global);
    _Resolved.setAllowPartial(true);
    ADD_PROPERTY_TYPE(Unresolved, (false), "Element",
            (App::PropertyType)(App::Prop_ReadOnly|App::Prop_Output),
            "The element is not loaded now (a link to a closed or missing\n"
            "document): the copy is kept, and the joint is solved on it");
    Vertex.setScope(App::LinkScope::Global);
    // As Element::Target: a copy of geometry, out of the coordinate-system
    // checks
    Element.setScope(App::LinkScope::Global);
}

ElementLink *ElementLink::create(App::DocumentObject *container,
                                 App::DocumentObject *obj,
                                 const char *subname)
{
    auto group = container ? container->getExtensionByType<App::GroupExtension>(true) : nullptr;
    if (!group)
        THROWM(Base::TypeError, "The container is not a group");
    auto doc = container->getDocument();
    auto link = static_cast<ElementLink*>(doc->addObject("Part::ElementLink", "ElementLink"));
    // The container's own, whatever its GroupMode
    auto children = group->Group.getValues();
    children.push_back(link);
    group->Group.setValues(children);
    std::vector<std::string> subs;
    if (subname && *subname)
        subs.emplace_back(subname);
    link->Element.setValue(obj, std::move(subs));
    link->Visibility.setValue(!Element::autoVisibility());
    return link;
}

App::DocumentObjectExecReturn *ElementLink::execute()
{
    if (!Element.getValue())
        return new App::DocumentObjectExecReturn("No element");
    auto shape = linkedShape(Element);
    if (shape.isNull() && keepUnresolved(this, Element, Unresolved))
        return App::DocumentObject::StdReturn;
    if (!shape.isNull() && Unresolved.getValue())
        Unresolved.setValue(false);
    syncResolved(_Resolved, Element);
    if (shape.isNull()) {
        if (takeWholeObject(this, Element))
            return App::DocumentObject::StdReturn;
        return new App::DocumentObjectExecReturn("Element not found");
    }
    setCopy(this, shape);
    if (!updateVertexOffset(shape))
        return new App::DocumentObjectExecReturn("Vertex not found");
    return App::DocumentObject::StdReturn;
}

void ElementLink::onChanged(const App::Property *prop)
{
    if (prop == &_Resolved && isEditing(this))
        followResolved(this, _Resolved, Unresolved);
    if (prop == &Element && isEditing(this))
        syncResolved(_Resolved, Element);
    inherited::onChanged(prop);
}

void ElementLink::onDocumentRestored()
{
    inherited::onDocumentRestored();
    // Restored with the document of the geometry missing
    if (!_Resolved.getValue() && *_Resolved.getObjectName() && !Unresolved.getValue())
        Unresolved.setValue(true);
}

bool ElementLink::getVertexPoint(Base::Vector3d &point) const
{
    if (!Vertex.getValue())
        return false;
    auto shape = linkedShape(Vertex);
    if (shape.isNull())
        return false;
    const TopoDS_Shape s = singleSubShape(shape.getShape());
    if (s.IsNull() || s.ShapeType() != TopAbs_VERTEX)
        return false;
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(s));
    point = Base::Vector3d(p.X(), p.Y(), p.Z());
    return true;
}

bool ElementLink::updateVertexOffset(const TopoShape &element)
{
    // The vertex in the element's frame; none is the frame's own origin
    Base::Vector3d pos;
    if (Vertex.getValue()) {
        Base::Vector3d point;
        if (element.isNull() || !getVertexPoint(point))
            return false;
        findElementFrame(element).inverse().multVec(point, pos);
    }
    const Base::Vector3d old = VertexPosition.getValue();
    if (pos.IsEqual(old, 1e-12))
        return true;
    // Offset = T(pos) * T(old)^-1 * Offset: only the vertex's share moves
    Offset.setValue(Base::Placement(pos - old, Base::Rotation()) * Offset.getValue());
    VertexPosition.setValue(pos);
    return true;
}
