// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
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

#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoSwitch.h>

#include <App/DocumentObject.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Mod/Part/Gui/ViewProvider.h>

#include <Mod/Assembly/App/AssemblyGroup.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/Groups.h>

#include "ViewProviderGroups.h"

using namespace AssemblyGui;

PROPERTY_SOURCE(AssemblyGui::ViewProviderGroupBase, Gui::ViewProviderDocumentObjectGroup)
PROPERTY_SOURCE(AssemblyGui::ViewProviderAssemblyGroup, Gui::ViewProviderAuxGroup)
PROPERTY_SOURCE(AssemblyGui::ViewProviderPartsGroup, Gui::ViewProviderAuxGroup)
PROPERTY_SOURCE(AssemblyGui::ViewProviderBomGroup, AssemblyGui::ViewProviderGroupBase)
PROPERTY_SOURCE(AssemblyGui::ViewProviderJointGroup, AssemblyGui::ViewProviderGroupBase)
PROPERTY_SOURCE(AssemblyGui::ViewProviderSimulationGroup, AssemblyGui::ViewProviderGroupBase)
PROPERTY_SOURCE(AssemblyGui::ViewProviderSnapshotGroup, AssemblyGui::ViewProviderGroupBase)
PROPERTY_SOURCE(AssemblyGui::ViewProviderViewGroup, AssemblyGui::ViewProviderGroupBase)

QIcon ViewProviderBomGroup::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_BillOfMaterialsGroup.svg");
}

QIcon ViewProviderJointGroup::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_JointGroup.svg");
}

QIcon ViewProviderSimulationGroup::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_SimulationGroup.svg");
}

QIcon ViewProviderSnapshotGroup::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_SnapshotGroup.svg");
}

QIcon ViewProviderViewGroup::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_ExplodedViewGroup.svg");
}

QIcon ViewProviderAssemblyGroup::getIcon() const
{
    auto group = getObject<Assembly::AssemblyGroup>();
    const auto role = group ? group->getRole() : std::string();
    if (role == Assembly::AssemblyGroup::JointsRole) {
        return Gui::BitmapFactory().pixmap("Assembly_JointGroup.svg");
    }
    if (role == Assembly::AssemblyGroup::SimulationsRole) {
        return Gui::BitmapFactory().pixmap("Assembly_SimulationGroup.svg");
    }
    if (role == Assembly::AssemblyGroup::ViewsRole) {
        return Gui::BitmapFactory().pixmap("Assembly_ExplodedViewGroup.svg");
    }
    if (role == Assembly::AssemblyGroup::PhysicsRole) {
        return Gui::BitmapFactory().pixmap("Assembly_PhysicsGroup.svg");
    }
    return Gui::ViewProviderAuxGroup::getIcon();
}

bool ViewProviderAssemblyGroup::onDelete(const std::vector<std::string>&)
{
    auto group = getObject<Assembly::AssemblyGroup>();
    // As upstream's: if empty or orphaned ok to delete
    return group && (group->Group.getValues().empty() || !group->getOwner());
}

bool ViewProviderJointGroup::onDelete(const std::vector<std::string>&)
{
    auto* group = getObject<Assembly::JointGroup>();

    // if empty or orphaned ok to delete.
    return group && (group->Group.getValues().empty() || group->getParents().empty());
};

ViewProviderPartsGroup::ViewProviderPartsGroup()
    : pcFrozen(new SoSwitch)
{
    pcFrozen->ref();
    pcFrozen->whichChild = SO_SWITCH_NONE;
}

ViewProviderPartsGroup::~ViewProviderPartsGroup()
{
    pcFrozen->unref();
}

bool ViewProviderPartsGroup::isFrozen() const
{
    auto group = getObject<Assembly::PartsGroup>();
    auto assembly = group ? freecad_cast<Assembly::AssemblyObject*>(group->getOwner()) : nullptr;
    return assembly && assembly->isFrozen();
}

std::vector<App::DocumentObject*> ViewProviderPartsGroup::claimChildren3D() const
{
    auto group = getObject<Assembly::PartsGroup>();
    if (!group || !isFrozen()) {
        return {};
    }
    return group->Group.getValues();
}

SoGroup* ViewProviderPartsGroup::getChildRoot() const
{
    // Always one, so that unfreezing gives the components back
    return pcFrozen;
}

void ViewProviderPartsGroup::attach(App::DocumentObject* obj)
{
    inherited::attach(obj);
    shapeView = std::make_unique<PartGui::ViewProviderPart>();
    shapeView->setShapePropertyName("Shape");
    shapeView->forceUpdate();
    shapeView->setStatus(Gui::SecondaryView, true);
    shapeView->attach(obj);
    // Its mode switch is not set by a document of its own: "Flat Lines"
    shapeView->setDefaultMode(0);
    getGroupNode()->addChild(shapeView->getRoot());
    getGroupNode()->addChild(pcFrozen);
}

void ViewProviderPartsGroup::reattach(App::DocumentObject* obj)
{
    if (shapeView) {
        shapeView->reattach(obj);
    }
    inherited::reattach(obj);
}

void ViewProviderPartsGroup::beforeDelete()
{
    if (shapeView) {
        shapeView->beforeDelete();
    }
    inherited::beforeDelete();
}

void ViewProviderPartsGroup::updateData(const App::Property* prop)
{
    auto group = getObject<Assembly::PartsGroup>();
    if (shapeView && group && prop == &group->Shape) {
        shapeView->updateData(prop);
    }
    inherited::updateData(prop);
}
