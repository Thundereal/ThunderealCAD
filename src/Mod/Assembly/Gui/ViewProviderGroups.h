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

#pragma once

#include <Mod/Assembly/AssemblyGlobal.h>

#include <memory>

#include <Gui/ViewProviderAuxGroup.h>
#include <Gui/ViewProviderDocumentObjectGroup.h>

class SoSwitch;

namespace PartGui
{
class ViewProviderPart;
}


namespace AssemblyGui
{

class AssemblyGuiExport ViewProviderGroupBase: public Gui::ViewProviderDocumentObjectGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderGroupBase);

public:
    ViewProviderGroupBase() = default;
    ~ViewProviderGroupBase() override = default;

    // Prevent dragging of the joints and dropping things inside the joint group.
    bool canDragObjects() const override
    {
        return false;
    };
    bool canDropObjects() const override
    {
        return false;
    };
    bool canDragAndDropObject(App::DocumentObject*) const override
    {
        return false;
    };
};

/// The assembly's Joints, Simulations or Views group (Assembly::AssemblyGroup):
/// upstream's group's icon and its rules -- no drag and drop, and deleted
/// only when empty
class AssemblyGuiExport ViewProviderAssemblyGroup: public Gui::ViewProviderAuxGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderAssemblyGroup);

public:
    ViewProviderAssemblyGroup() = default;
    ~ViewProviderAssemblyGroup() override = default;

    QIcon getIcon() const override;

    bool canDragObjects() const override
    {
        return false;
    }
    bool canDropObjects() const override
    {
        return false;
    }
    bool canDragAndDropObject(App::DocumentObject*) const override
    {
        return false;
    }
    bool onDelete(const std::vector<std::string>&) override;
};

/// The assembly's Parts group (Assembly::PartsGroup): it draws the shape a
/// frozen assembly keeps, which the assembly shows in place of its components
class AssemblyGuiExport ViewProviderPartsGroup: public Gui::ViewProviderAuxGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderPartsGroup);
    using inherited = Gui::ViewProviderAuxGroup;

public:
    ViewProviderPartsGroup();
    ~ViewProviderPartsGroup() override;

    void attach(App::DocumentObject* obj) override;
    void reattach(App::DocumentObject* obj) override;
    void beforeDelete() override;
    void updateData(const App::Property* prop) override;

    /** Frozen, the components: claimed here, under a node that draws
     * nothing, as a child the assembly does not claim would be drawn at
     * the top of the scene
     */
    std::vector<App::DocumentObject*> claimChildren3D() const override;
    SoGroup* getChildRoot() const override;

private:
    bool isFrozen() const;

    /// Draws the group's Shape, in the "Group" display mode's node
    std::unique_ptr<PartGui::ViewProviderPart> shapeView;
    /// Holds the components' nodes while frozen, drawing none
    SoSwitch* pcFrozen;
};

class AssemblyGuiExport ViewProviderBomGroup: public ViewProviderGroupBase
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderBomGroup);

public:
    ViewProviderBomGroup() = default;
    ~ViewProviderBomGroup() override = default;

    QIcon getIcon() const override;
};

class AssemblyGuiExport ViewProviderJointGroup: public ViewProviderGroupBase
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderJointGroup);

public:
    ViewProviderJointGroup() = default;
    ~ViewProviderJointGroup() override = default;

    QIcon getIcon() const override;

    bool onDelete(const std::vector<std::string>&) override;
};

class AssemblyGuiExport ViewProviderSimulationGroup: public ViewProviderGroupBase
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderSimulationGroup);

public:
    ViewProviderSimulationGroup() = default;
    ~ViewProviderSimulationGroup() override = default;

    QIcon getIcon() const override;
};


class AssemblyGuiExport ViewProviderSnapshotGroup: public ViewProviderGroupBase
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderSnapshotGroup);

public:
    ViewProviderSnapshotGroup() = default;
    ~ViewProviderSnapshotGroup() override = default;

    QIcon getIcon() const override;
};

class AssemblyGuiExport ViewProviderViewGroup: public ViewProviderGroupBase
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderViewGroup);

public:
    ViewProviderViewGroup() = default;
    ~ViewProviderViewGroup() override = default;

    QIcon getIcon() const override;
};

}  // namespace AssemblyGui
