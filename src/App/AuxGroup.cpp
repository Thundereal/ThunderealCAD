/****************************************************************************
 *   Copyright (c) 2020 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 *   This library  is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Library General Public License for more details.                   *
 *                                                                          *
 *   You should have received a copy of the GNU Library General Public      *
 *   License along with this library; see the file COPYING.LIB. If not,     *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,          *
 *   Suite 330, Boston, MA  02111-1307, USA                                 *
 *                                                                          *
 ****************************************************************************/

#include "PreCompiled.h"

#include <algorithm>
#include <functional>

#include "AuxGroup.h"
#include "Document.h"
#include "DocumentObserver.h"
#include "GroupExtension.h"

using namespace App;

AuxGroupOwner::~AuxGroupOwner() = default;

AuxGroup *AuxGroupOwner::getElementsGroup(bool)
{
    return nullptr;
}

PropertyLinkList *AuxGroupOwner::getAuxGroupSource(const AuxGroup *) const
{
    auto owner = dynamic_cast<const DocumentObject*>(this);
    auto group = owner ? owner->getExtensionByType<GroupExtension>(true) : nullptr;
    return group ? &group->Group : nullptr;
}

bool AuxGroupOwner::hasAuxGroup(const AuxGroup *group) const
{
    auto source = AuxGroupOwner::getAuxGroupSource(group);
    return source && group && group->isAttachedToDocument()
        && source->find(group->getNameInDocument()) == group;
}

bool AuxGroupOwner::isAuxGroupChild(const DocumentObject *obj) const
{
    auto owner = dynamic_cast<const DocumentObject*>(this);
    if (!owner)
        return false;
    auto group = owner->getExtensionByType<GroupExtension>(true);
    return group && group->hasObject(obj);
}

PROPERTY_SOURCE_ABSTRACT(App::AuxGroup, App::DocumentObject)

AuxGroup::AuxGroup()
{
    ADD_PROPERTY_TYPE(Group, (nullptr), "Base",
            (App::PropertyType)(App::Prop_Hidden|App::Prop_ReadOnly|App::Prop_Output), 0);
    // A view of its owner's children only refers to them (attachOwner())
    Group.setScope(LinkScope::Global);
    ADD_PROPERTY_TYPE(Role, (""), "Base",
            (App::PropertyType)(App::Prop_Hidden|App::Prop_ReadOnly),
            "The kind of children shown; empty for a group the user fills");
    ADD_PROPERTY_TYPE(_GroupTouched, (false), "Base",
            (App::PropertyType)(App::Prop_Hidden|App::Prop_Output|App::Prop_Transient),
            "Touched when a child of an owning group changes");
}

AuxGroup::~AuxGroup() = default;

const char *AuxGroup::ElementsRole = "Elements";

bool AuxGroup::isOwning() const
{
    if (getRole() != ElementsRole)
        return false;
    auto owner = getAuxGroupOwner();
    return owner && owner->getElementsGroup(false) == this;
}

AuxGroup *AuxGroup::getOwningGroupOf(const DocumentObject *obj)
{
    if (!obj || !obj->isAttachedToDocument())
        return nullptr;
    for (auto parent : obj->getInList()) {
        auto group = Base::freecad_dynamic_cast<AuxGroup>(parent);
        if (group && group->isOwning() && group->Group.find(obj->getNameInDocument()) == obj)
            return group;
    }
    return nullptr;
}

void AuxGroup::unsetupObject()
{
    // An owning group takes its children along, and tells its owner, which
    // may have views of them to take away
    if (isOwning()) {
        auto owner = getAuxGroupOwner();
        std::vector<DocumentObjectT> children;
        for (auto child : Group.getValues())
            children.emplace_back(child);
        for (auto &childT : children) {
            auto child = childT.getObject();
            if (child && !child->isRemoving())
                child->getDocument()->removeObject(child->getNameInDocument());
        }
        if (owner)
            owner->onElementsChanged();
    }
    inherited::unsetupObject();
}

void AuxGroup::setOwnerProperty(PropertyLinkHidden *prop)
{
    ownerProp = prop;
}

void AuxGroup::onChanged(const Property* prop)
{
    if (prop == &Role)
        defaultRoleValid = false;
    auto doc = getDocument();
    if (doc && !doc->isPerformingTransaction()
            && !doc->testStatus(App::Document::Restoring)) {
        if (prop == ownerProp) {
            attachOwner();
            refresh();
        }
        else if (prop == &Role) {
            // Whether it owns its children goes with the role
            attachOwner();
            refresh();
        }
        else if (prop == &Group && !isRemoving() && isOwning()) {
            if (auto owner = getAuxGroupOwner())
                owner->onElementsChanged();
        }
    }
    inherited::onChanged(prop);
}

void AuxGroup::onDocumentRestored()
{
    attachOwner();
    inherited::onDocumentRestored();
}

DocumentObject *AuxGroup::getOwner() const
{
    return ownerProp ? ownerProp->getValue() : nullptr;
}

void AuxGroup::setOwner(DocumentObject *owner)
{
    if (ownerProp)
        ownerProp->setValue(owner);
}

AuxGroupOwner *AuxGroup::getAuxGroupOwner() const
{
    return dynamic_cast<AuxGroupOwner*>(getOwner());
}

GroupExtension *AuxGroup::getOwnerGroup() const
{
    auto owner = getOwner();
    if (!owner || !getAuxGroupOwner())
        return nullptr;
    return owner->getExtensionByType<GroupExtension>(true);
}

void AuxGroup::attachOwner()
{
    // An owning group's children are its own, not a view of the owner's.
    // A view only refers to them: Global, so that it is no claim of them,
    // and the owner still exports them as its own (an App::Part with a
    // Parts group), nor a link that moving one of them to another
    // coordinate system must drag along
    bool owning = isOwning();
    Group.setScope(owning ? LinkScope::Local : LinkScope::Global);
    auto owner = getAuxGroupOwner();
    auto source = owner && !owning ? owner->getAuxGroupSource(this) : nullptr;
    if (!source)
        connOwner.disconnect();
    else
        connOwner = source->signalChanged.connect(
                std::bind(&AuxGroup::refresh, this));
}

std::string AuxGroup::getDefaultRole() const
{
    return {};
}

const std::string &AuxGroup::getRole() const
{
    if (!Role.getStrValue().empty())
        return Role.getStrValue();
    if (!defaultRoleValid) {
        // The name is what a file before Role says the role with
        if (!getNameInDocument()) {
            static const std::string empty;
            return empty;
        }
        defaultRole = getDefaultRole();
        defaultRoleValid = true;
    }
    return defaultRole;
}

bool AuxGroup::allowDuplicateLabel() const
{
    // Every owner may have its own "Sketches"
    return !getNameInDocument() || !isCurated();
}

int AuxGroup::canLoadPartial() const
{
    auto owner = getOwner();
    return owner && owner != this && owner->canLoadPartial() == 1 ? 1 : 0;
}

bool AuxGroup::isObjectAllowed(const DocumentObject *obj) const
{
    if (!obj)
        return false;
    auto owner = getAuxGroupOwner();
    return owner && owner->isAuxGroupAllowed(this, obj);
}

bool AuxGroup::isChildVisible() const
{
    for (auto child : Group.getValues()) {
        if (child->Visibility.getValue())
            return true;
    }
    return false;
}

void AuxGroup::setChildrenVisible(bool visible)
{
    for (auto child : Group.getValues())
        child->Visibility.setValue(visible);
}

void AuxGroup::refresh()
{
    if (isRemoving() || isOwning())
        return;
    auto owner = getAuxGroupOwner();
    auto source = owner ? owner->getAuxGroupSource(this) : nullptr;
    if (!source) {
        if (Group.getSize())
            Group.setValues();
        return;
    }
    if (!owner->hasAuxGroup(this)) {
        setOwner(nullptr);
        Group.setValues();
        return;
    }
    std::vector<DocumentObject *> children;
    if (isCurated()) {
        auto curated = Group.getValues();
        std::sort(curated.begin(), curated.end());
        for (auto obj : source->getValues()) {
            auto it = std::lower_bound(curated.begin(), curated.end(), obj);
            if (it != curated.end() && *it == obj) {
                children.push_back(obj);
                curated.erase(it);
            }
        }
    }
    else {
        for (auto obj : source->getValues()) {
            if (isObjectAllowed(obj))
                children.push_back(obj);
        }
    }
    if (Group.getValues() != children)
        Group.setValues(children);
}

// ------------------------------------------------------------------------

PROPERTY_SOURCE(App::PartAuxGroup, App::AuxGroup)

PartAuxGroup::PartAuxGroup()
{
    ADD_PROPERTY_TYPE(_Owner, (nullptr), "Base",
            (App::PropertyType)(App::Prop_Hidden|App::Prop_ReadOnly|App::Prop_Output), 0);
    setOwnerProperty(&_Owner);
}
