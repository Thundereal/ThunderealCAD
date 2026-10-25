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

// The assembly's Joints, Simulations and Views groups as aux groups
// (docs/AssemblyObjectModel.md sec 3): views of the assembly's own Group by
// role, in place of upstream's JointGroup, SimulationGroup and ViewGroup,
// which own their members.

#pragma once

#include <App/AuxGroup.h>
#include <fastsignals/signal.h>

#include <Mod/Assembly/AssemblyGlobal.h>
#include <Mod/Part/App/PropertyTopoShape.h>

namespace Assembly
{

class AssemblyExport AssemblyGroup: public App::PartAuxGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::AssemblyGroup);

public:
    AssemblyGroup();

    static const char* JointsRole;
    static const char* SimulationsRole;
    static const char* ViewsRole;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderAssemblyGroup";
    }
    PyObject* getPyObject() override;

    /** Make an object in the owner, where this group shows it: what
     * upstream's groups' newObject() is to scripts and the workbench
     */
    App::DocumentObject* newObject(const char* type, const char* name);
    /// Give the owner an object, for this group to show
    void addObject(App::DocumentObject* obj);

protected:
    void onSettingDocument() override;

private:
    /// A child's proxy set after the child was added -- an upstream joint
    /// is made, then given its proxy -- changes the role it has
    fastsignals::scoped_connection connProxy;
};

/** The Parts group of an assembly (docs/AssemblyObjectModel.md sec 3.1)
 *
 * A Parts group with a shape: the compound of the components at their
 * placements, which a frozen assembly keeps and shows in their place, and
 * which a document linking to the assembly loads without them.
 */
class AssemblyExport PartsGroup: public App::PartAuxGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::PartsGroup);

public:
    PartsGroup();

    /// The components at their placements, taken when the assembly
    /// freezes; empty when it is not frozen
    Part::PropertyPartShape Shape;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderPartsGroup";
    }
    /// The stored shape, or an element of it, while there is one
    App::DocumentObject* getSubObject(
        const char* subname,
        PyObject** pyObj,
        Base::Matrix4D* mat,
        bool transform,
        int depth
    ) const override;
};

}  // namespace Assembly
