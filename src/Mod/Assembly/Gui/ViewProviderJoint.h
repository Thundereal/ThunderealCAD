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

#pragma once

#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

#include <Gui/ViewProviderDocumentObject.h>
#include <Gui/ViewProviderFeaturePython.h>


namespace AssemblyGui
{

/** The view provider of a C++ joint (Assembly::Joint)
 *
 * The joint holds its element links, and a multiplied joint its instance
 * joints: it claims them in the tree. Claimed in C++, not by the joint's
 * Python view provider: a group's tree export asks every child for its
 * claims, and an assembly's children are its joints, so a Python call per
 * joint for each change of the assembly's Group made building joints
 * quadratic (docs/AssemblyAudit.md sec 8.2, Phase 2). The markers come with
 * the Python view provider of Assembly::JointPython, the proxy of
 * ViewProviderJointPython, whose claimChildren is not asked here; it claims
 * the same for a joint whose file states the generic Python view provider.
 */
class AssemblyGuiExport ViewProviderJoint: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderJoint);

public:
    ViewProviderJoint();

    std::vector<App::DocumentObject*> claimChildren() const override;
};

using ViewProviderJointPython = Gui::ViewProviderFeaturePythonT<ViewProviderJoint>;

}  // namespace AssemblyGui
