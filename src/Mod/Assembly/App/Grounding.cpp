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

#include "Grounding.h"

using namespace Assembly;


PROPERTY_SOURCE(Assembly::GroundedJoint, App::DocumentObject)

GroundedJoint::GroundedJoint()
{
    ADD_PROPERTY_TYPE(ObjectToGround, (nullptr), "Ground", App::Prop_None, "The object to ground");
}


PROPERTY_SOURCE(Assembly::RigidGroup, App::DocumentObject)

RigidGroup::RigidGroup()
{
    ADD_PROPERTY_TYPE(ObjectsToRigidGroup, (), "RigidGroup", App::Prop_None,
                      "List of references to components to group together");
    // It refers to the components, as a joint's grounding does: no claim of
    // them in the tree or the export, nor a link moving one of them must
    // drag the others along
    ObjectsToRigidGroup.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(RigidPlacements, (), "RigidGroup",
                      (App::PropertyType)(App::Prop_Hidden | App::Prop_ReadOnly),
                      "Relative placements for restore");
    App::SuppressibleExtension::initExtension(this);
}


// Python features --------------------------------------------------------

namespace App
{
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(Assembly::GroundedJointPython, Assembly::GroundedJoint)
template<>
const char* Assembly::GroundedJointPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::GroundedJoint>;

PROPERTY_SOURCE_TEMPLATE(Assembly::RigidGroupPython, Assembly::RigidGroup)
template<>
const char* Assembly::RigidGroupPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::RigidGroup>;
/// @endcond
}  // namespace App
