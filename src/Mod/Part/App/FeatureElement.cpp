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

#include <App/Application.h>
#include <App/AuxGroup.h>
#include <App/Document.h>
#include <App/GroupExtension.h>
#include <Base/Exception.h>
#include <Base/Tools.h>

#include "FeatureElement.h"

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
    // The link copies geometry and places nothing: Global scope keeps it out
    // of the coordinate-system checks and of what moves with what
    Target.setScope(App::LinkScope::Global);
}

App::DocumentObjectExecReturn *Element::execute()
{
    if (Detach.getValue())
        return App::DocumentObject::StdReturn;
    if (!Target.getValue())
        return new App::DocumentObjectExecReturn("No target");
    auto shape = linkedShape(Target);
    if (shape.isNull())
        return new App::DocumentObjectExecReturn("Target not found");
    setCopy(this, shape);
    return App::DocumentObject::StdReturn;
}

void Element::onChanged(const App::Property *prop)
{
    // What the target is may decide where the element is shown: a body
    // shows it under the solid body of its target feature
    auto doc = getDocument();
    if (prop == &Target && doc && !isRestoring() && !isRemoving()
            && !doc->testStatus(App::Document::Restoring)
            && !doc->isPerformingTransaction()) {
        if (auto owner = dynamic_cast<App::AuxGroupOwner*>(getContainer()))
            owner->onElementsChanged();
    }
    inherited::onChanged(prop);
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
    if (shape.isNull())
        return new App::DocumentObjectExecReturn("Element not found");
    setCopy(this, shape);
    return App::DocumentObject::StdReturn;
}
