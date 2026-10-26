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

#include <Mod/Assembly/App/Joint.h>
#include <Mod/Part/App/FeatureElement.h>

#include "ViewProviderJoint.h"

using namespace AssemblyGui;

PROPERTY_SOURCE(AssemblyGui::ViewProviderJoint, Gui::ViewProviderDocumentObject)

ViewProviderJoint::ViewProviderJoint() = default;

std::vector<App::DocumentObject*> ViewProviderJoint::claimChildren() const
{
    std::vector<App::DocumentObject*> children;
    auto joint = Base::freecad_dynamic_cast<Assembly::Joint>(getObject());
    if (!joint) {
        return children;
    }
    for (int side : {1, 2}) {
        if (auto link = joint->getElementLink(side)) {
            children.push_back(link);
        }
    }
    const auto& instances = joint->_Instances.getValues();
    children.insert(children.end(), instances.begin(), instances.end());
    return children;
}

namespace Gui
{
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(AssemblyGui::ViewProviderJointPython, AssemblyGui::ViewProviderJoint)
/// @endcond

// Not asked of the proxy: the joint's Python view provider claims the same
// for a view provider of the generic type, as a file saved before this one
// has, and the call is what this view provider is for not making
template<>
std::vector<App::DocumentObject*>
ViewProviderFeaturePythonT<AssemblyGui::ViewProviderJoint>::claimChildren() const
{
    return AssemblyGui::ViewProviderJoint::claimChildren();
}

// explicit template instantiation
template class AssemblyGuiExport ViewProviderFeaturePythonT<AssemblyGui::ViewProviderJoint>;
}  // namespace Gui
