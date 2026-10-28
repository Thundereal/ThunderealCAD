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
#include <cstring>

#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>

#include <App/Application.h>
#include <App/Datums.h>
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

/// The object a link's subname resolves to, through a link to what it
/// links, when not the linked object: what the geometry comes from
static App::DocumentObject *resolvedObject(const App::PropertyLinkSub &prop)
{
    auto obj = prop.getValue();
    if (!obj || !obj->isAttachedToDocument())
        return nullptr;
    const auto &subs = prop.getSubValues();
    if (subs.empty() || subs.front().empty())
        return nullptr;
    auto sobj = obj->getSubObject(subs.front().c_str());
    if (sobj)
        sobj = sobj->getLinkedObject(true);
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
    ADD_PROPERTY_TYPE(Vertex, (nullptr), "Element", App::Prop_None,
            "A vertex placing the frame's origin on the geometry; Offset follows it");
    ADD_PROPERTY_TYPE(VertexPosition, (Base::Vector3d()), "Element", App::Prop_Hidden,
            "Where Vertex was in the geometry's frame when Offset last followed it");
    // The links copy geometry and place nothing: Global scope keeps them out
    // of the coordinate-system checks and of what moves with what
    Target.setScope(App::LinkScope::Global);
    Vertex.setScope(App::LinkScope::Global);
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
    auto sobj = obj->getSubObject(subname, nullptr, &mat, true);
    if (!sobj)
        return false;
    placement.fromMatrix(mat);
    // An axis of a coordinate system is the frame along it, as upstream's
    // joint takes it (UtilsAssembly.findPlacement): Z on the line
    if (auto line = Base::freecad_dynamic_cast<App::Line>(sobj->getLinkedObject(true))) {
        const char *role = line->Role.getValue();
        if (strcmp(role, App::LocalCoordinateSystem::AxisRoles[0]) == 0
            || strcmp(role, App::LocalCoordinateSystem::AxisRoles[1]) == 0)
            placement = placement * Base::Placement(Base::Vector3d(),
                                                    Base::Rotation(0.5, 0.5, 0.5, 0.5));
        else if (strcmp(role, App::LocalCoordinateSystem::AxisRoles[2]) == 0)
            placement = placement * Base::Placement(Base::Vector3d(),
                                                    Base::Rotation(-0.5, 0.5, -0.5, 0.5));
    }
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
    // What the geometry comes from changed under the target: a component
    // replaced. An edit of Target itself keeps _Resolved in step (onChanged).
    auto resolved = resolvedObject(Target);
    bool replaced = resolved && _Resolved.getValue() && resolved != _Resolved.getValue();
    // The vertex where it was, before the copy is taken again
    const TopoShape vertexCopy = vertexCopyShape();
    if ((shape.isNull() || replaced) && repair(Target, Shape.getShape(), replaced))
        shape = linkedShape(Target);
    syncResolved(_Resolved, Target);
    if (shape.isNull()) {
        if (takeWholeObject(this, Target))
            return App::DocumentObject::StdReturn;
        return new App::DocumentObjectExecReturn("Target not found");
    }
    setCopy(this, shape);
    // The vertex is repaired as the target is: where its name names nothing
    // now, or where the component it is on is replaced
    const bool vertexReplaced = replaced && Vertex.getValue() == Target.getValue();
    Base::Vector3d point;
    if (Vertex.getValue() && !vertexCopy.isNull()
        && (vertexReplaced || !getVertexPoint(point))) {
        const auto &vsubs = Vertex.getSubValues();
        if (vertexReplaced
            || (!vsubs.empty() && Data::hasMissingElement(vsubs.front().c_str())))
            repair(Vertex, vertexCopy, vertexReplaced);
    }
    if (!updateVertexOffset(shape))
        return new App::DocumentObjectExecReturn("Vertex not found");
    return App::DocumentObject::StdReturn;
}

TopoShape Element::vertexCopyShape() const
{
    if (!Vertex.getValue())
        return TopoShape();
    const TopoShape copy = Shape.getShape();
    if (copy.isNull())
        return TopoShape();
    Base::Vector3d point;
    findElementFrame(copy).multVec(VertexPosition.getValue(), point);
    const TopoDS_Shape vertex =
        BRepBuilderAPI_MakeVertex(gp_Pnt(point.x, point.y, point.z)).Vertex();
    return TopoShape(vertex);
}

bool Element::getVertexPoint(Base::Vector3d &point) const
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

bool Element::updateVertexOffset(const TopoShape &geometry)
{
    // The vertex in the geometry's frame; none is the frame's own origin
    Base::Vector3d pos;
    if (Vertex.getValue()) {
        Base::Vector3d point;
        if (geometry.isNull() || !getVertexPoint(point))
            return false;
        findElementFrame(geometry).inverse().multVec(point, pos);
    }
    const Base::Vector3d old = VertexPosition.getValue();
    if (pos.IsEqual(old, 1e-12))
        return true;
    // Offset = T(pos) * T(old)^-1 * Offset: only the vertex's share moves
    Offset.setValue(Base::Placement(pos - old, Base::Rotation()) * Offset.getValue());
    VertexPosition.setValue(pos);
    return true;
}

bool Element::repair(App::PropertyLinkSub &prop, const TopoShape &copyShape, bool replaced)
{
    auto obj = prop.getValue();
    const auto &subs = prop.getSubValues();
    if (!obj || subs.empty())
        return false;
    const std::string sub = subs.front();
    const char *element = Data::findElementName(sub.c_str());
    // No geometry named: a whole object, or a path to a child's element,
    // which that element repairs
    if (!element || !*element)
        return false;
    const std::string prefix(sub.c_str(), element - sub.c_str());
    const bool missing = Data::hasMissingElement(element);

    std::string found;
    const char *how = nullptr;
    // The naming history: what the element the name had became
    const auto &shadows = prop.getShadowSubs();
    const char *mapped = shadows.empty() ? nullptr
        : Data::findElementName(shadows.front().first.c_str());
    if (missing && mapped && Data::isMappedElement(mapped)) {
        auto sobj = obj->getSubObject(prefix.c_str());
        auto geo = sobj ? sobj->getLinkedObject(true) : nullptr;
        auto related = geo ? Feature::getRelatedElements(geo, mapped)
                           : QVector<Data::MappedElement>();
        if (!related.isEmpty()) {
            related.front().index.appendToStringBuffer(found);
            how = "its naming history";
        }
    }
    // The geometry: where the copy is, in what the target is now
    if (found.empty()) {
        const TopoDS_Shape copy = singleSubShape(copyShape.getShape());
        std::vector<std::string> names;
        if (!copy.IsNull())
            Feature::getTopoShape(obj, prefix.c_str(), false).searchSubShape(TopoShape(copy),
                                                                             &names);
        if (!names.empty()) {
            found = names.front();
            how = "its geometry";
        }
    }
    if (found.empty() || (!missing && found == element)) {
        if (replaced && found.empty())
            Base::Console().Warning("%s.%s: %s is replaced, and the element's geometry is "
                                    "not in it: kept as %s\n", getFullName().c_str(),
                                    prop.getName(), obj->getFullName().c_str(), sub.c_str());
        return false;
    }
    Base::Console().Warning("%s.%s: %s.%s found by %s as %s\n", getFullName().c_str(),
                            prop.getName(), obj->getFullName().c_str(), sub.c_str(), how,
                            found.c_str());
    prop.setValue(obj, std::vector<std::string>{prefix + found});
    return true;
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

PROPERTY_SOURCE(Part::ElementBinding, Part::Feature)

ElementBinding::ElementBinding()
{
    ADD_PROPERTY_TYPE(Element, (nullptr), "Element", App::Prop_None,
            "A direct child of the container and the subname to one of its\n"
            "elements, or the container's own Elements group and an element");
    ADD_PROPERTY_TYPE(Offset, (Base::Placement()), "Element", App::Prop_None,
            "A placement of the frame relative to the element's, for this use only");
    ADD_PROPERTY_TYPE(_Resolved, (nullptr), "Element",
            (App::PropertyType)(App::Prop_Hidden|App::Prop_Output),
            "What Element resolves to, the dependency on the geometry itself");
    _Resolved.setScope(App::LinkScope::Global);
    _Resolved.setAllowPartial(true);
    ADD_PROPERTY_TYPE(Unresolved, (false), "Element",
            (App::PropertyType)(App::Prop_ReadOnly|App::Prop_Output),
            "The element is not loaded now (a link to a closed or missing\n"
            "document): the copy is kept, and the joint is solved on it");
    // As Element::Target: a copy of geometry, out of the coordinate-system
    // checks
    Element.setScope(App::LinkScope::Global);
}

ElementBinding *ElementBinding::create(App::DocumentObject *container,
                                 App::DocumentObject *obj,
                                 const char *subname)
{
    auto group = container ? container->getExtensionByType<App::GroupExtension>(true) : nullptr;
    if (!group)
        THROWM(Base::TypeError, "The container is not a group");
    auto doc = container->getDocument();
    auto link = static_cast<ElementBinding*>(
        doc->addObject("Part::ElementBinding", "ElementBinding"));
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

App::DocumentObjectExecReturn *ElementBinding::execute()
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
    return App::DocumentObject::StdReturn;
}

void ElementBinding::onChanged(const App::Property *prop)
{
    if (prop == &_Resolved && isEditing(this))
        followResolved(this, _Resolved, Unresolved);
    if (prop == &Element && isEditing(this))
        syncResolved(_Resolved, Element);
    inherited::onChanged(prop);
}

void ElementBinding::onDocumentRestored()
{
    inherited::onDocumentRestored();
    // Restored with the document of the geometry missing
    if (!_Resolved.getValue() && *_Resolved.getObjectName() && !Unresolved.getValue())
        Unresolved.setValue(true);
}

const Element *ElementBinding::getVertexElement() const
{
    // Down the chain of elements: this one's, then what it re-exports
    std::vector<const App::DocumentObject *> visited;
    auto obj = Element.getValue();
    const auto *subs = &Element.getSubValues();
    while (obj && obj->isAttachedToDocument()) {
        auto sobj = obj->getSubObject(subs->empty() ? "" : subs->front().c_str());
        auto element = Base::freecad_dynamic_cast<Part::Element>(sobj);
        if (!element || std::find(visited.begin(), visited.end(), element) != visited.end())
            return nullptr;
        if (element->Vertex.getValue())
            return element;
        visited.push_back(element);
        obj = element->Target.getValue();
        subs = &element->Target.getSubValues();
    }
    return nullptr;
}

void ElementBinding::handleChangedPropertyName(Base::XMLReader &reader,
                                            const char *typeName,
                                            const char *propName)
{
    // An element binding of a build before 2026-10-26 kept a vertex of its own;
    // the element keeps it now (docs/AssemblyObjectModel.md sec 4.3)
    if (strcmp(propName, "Vertex") == 0) {
        Base::Console().Warning("%s: its vertex is not kept; an element keeps it now. "
                                "Pick the joint's side again for it.\n",
                                getFullName().c_str());
        return;
    }
    if (strcmp(propName, "VertexPosition") == 0)
        return;
    inherited::handleChangedPropertyName(reader, typeName, propName);
}
