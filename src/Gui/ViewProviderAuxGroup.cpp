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

#include <App/AuxGroup.h>
#include <App/Document.h>
#include <App/GroupExtension.h>

#include "ViewProviderAuxGroup.h"
#include "Application.h"
#include "Document.h"
#include "Selection.h"
#include "SoFCUnifiedSelection.h"

using namespace Gui;

PROPERTY_SOURCE(Gui::ViewProviderAuxGroup, Gui::ViewProviderDocumentObject)

ViewProviderAuxGroup::ViewProviderAuxGroup()
{
    sPixmap = "folder";
    pcChildren = new SoFCSelectionRoot;
    pcChildren->ref();
}

ViewProviderAuxGroup::~ViewProviderAuxGroup()
{
    pcChildren->unref();
}

void ViewProviderAuxGroup::attach(App::DocumentObject *obj)
{
    inherited::attach(obj);
    addDisplayMaskMode(pcChildren, "Group");
}

std::vector<std::string> ViewProviderAuxGroup::getDisplayModes() const
{
    auto modes = inherited::getDisplayModes();
    modes.emplace_back("Group");
    return modes;
}

void ViewProviderAuxGroup::setDisplayMode(const char *mode)
{
    if (mode && strcmp(mode, "Group") == 0)
        setDisplayMaskMode("Group");
    inherited::setDisplayMode(mode);
}

const char *ViewProviderAuxGroup::getDefaultDisplayMode() const
{
    return "Group";
}

std::vector<App::DocumentObject*> ViewProviderAuxGroup::claimChildren3D() const
{
    auto group = getAuxGroup();
    if (group && group->isOwning())
        return group->Group.getValues();
    return {};
}

SoGroup *ViewProviderAuxGroup::getChildRoot() const
{
    auto group = getAuxGroup();
    return group && group->isOwning() ? pcChildren : nullptr;
}

App::AuxGroup *ViewProviderAuxGroup::getAuxGroup() const
{
    return Base::freecad_dynamic_cast<App::AuxGroup>(getObject());
}

ViewProvider *ViewProviderAuxGroup::getOwnerViewProvider() const
{
    auto group = getAuxGroup();
    if (!group || !group->getOwner())
        return nullptr;
    return Application::Instance->getViewProvider(group->getOwner());
}

bool ViewProviderAuxGroup::canDropObject(App::DocumentObject *obj) const
{
    auto group = getAuxGroup();
    if (!group)
        return false;
    auto owner = group->getAuxGroupOwner();
    if (!owner || !group->isObjectAllowed(obj))
        return false;
    if (group->isCurated()) {
        if (owner->isAuxGroupChild(obj)
                && !group->Group.find(obj->getNameInDocument()))
            return true;
    }
    auto ownerVp = getOwnerViewProvider();
    return ownerVp && ownerVp->canDropObject(obj);
}

bool ViewProviderAuxGroup::canDragAndDropObject(App::DocumentObject *) const
{
    return true;
}

void ViewProviderAuxGroup::dropObject(App::DocumentObject *obj)
{
    auto group = getAuxGroup();
    if (!group)
        return;
    auto owner = group->getAuxGroupOwner();
    if (!owner || !group->isObjectAllowed(obj))
        return;

    if (!owner->isAuxGroupChild(obj)) {
        if (auto ownerVp = getOwnerViewProvider())
            ownerVp->dropObject(obj);
    }
    if (group->isCurated()) {
        if (owner->isAuxGroupChild(obj)
                && !group->Group.find(obj->getNameInDocument())) {
            auto children = group->Group.getValues();
            children.push_back(obj);
            group->Group.setValues(children);
            if (auto ownerGroup = group->getOwnerGroup())
                ownerGroup->_GroupTouched.touch();
            return;
        }
    }
}

bool ViewProviderAuxGroup::canDragObject(App::DocumentObject * obj) const {
    auto group = getAuxGroup();
    if (!group)
        return true;

    auto owner = group->getOwner();
    auto ownerVp = getOwnerViewProvider();

    if (!group->isCurated()) {
        if (ownerVp)
            return ownerVp->canDragObject(obj);
        return false;
    }

    if (owner) {
        auto target = Gui::Selection().getContext(1).getSubObject();
        if (owner == target)
           return true;

        if (auto targetGroup = Base::freecad_dynamic_cast<App::AuxGroup>(target))
            return targetGroup->getOwner() == owner;

        if (ownerVp)
            return ownerVp->canDragObject(obj);
    }
    return true;
}

void ViewProviderAuxGroup::dragObject(App::DocumentObject *obj) {
    if (auto group = getAuxGroup()) {
        auto children = group->Group.getValues();
        children.erase(std::remove(children.begin(), children.end(), obj), children.end());
        group->Group.setValues(children);
    }
}

std::vector<App::DocumentObject*> ViewProviderAuxGroup::claimChildren() const {
    auto group = getAuxGroup();
    if (!group)
        return {};
    if (!group->isOwning())
        return group->Group.getValues();
    // An owning group leaves out what a view of it shows: a body's Elements
    // group lists only the elements no solid body shows
    std::vector<App::DocumentObject*> res;
    for (auto child : group->Group.getValues()) {
        bool shown = false;
        for (auto parent : child->getInList()) {
            auto view = Base::freecad_dynamic_cast<App::AuxGroup>(parent);
            if (view && view != group && !view->isOwning()
                    && view->getRole() == group->getRole()
                    && view->Group.find(child->getNameInDocument()) == child) {
                shown = true;
                break;
            }
        }
        if (!shown)
            res.push_back(child);
    }
    return res;
}

bool ViewProviderAuxGroup::reorderObjects(const std::vector<App::DocumentObject*> &objs,
                                          App::DocumentObject *before)
{
    auto ownerVp = getOwnerViewProvider();
    return ownerVp && ownerVp->reorderObjects(objs, before);
}

bool ViewProviderAuxGroup::canReorderObject(App::DocumentObject *obj,
                                            App::DocumentObject *before)
{
    auto ownerVp = getOwnerViewProvider();
    return ownerVp && ownerVp->canReorderObject(obj, before);
}

void ViewProviderAuxGroup::updateData(const App::Property *prop)
{
    auto doc = getDocument()->getDocument();
    if (doc && !doc->isPerformingTransaction()
            && !doc->testStatus(App::Document::Restoring)) {
        auto group = getAuxGroup();
        if (group && prop == &group->Group) {
            if (auto ownerGroup = group->getOwnerGroup())
                ownerGroup->_GroupTouched.touch();
        }
    }
    inherited::updateData(prop);
}

bool ViewProviderAuxGroup::canDelete(App::DocumentObject* obj) const
{
    if (auto ownerVp = getOwnerViewProvider())
        return ownerVp->canDelete(obj);
    return true;
}
