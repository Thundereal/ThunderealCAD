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

// Grounding and rigid groups as C++ objects (docs/AssemblyObjectModel.md
// sec 7.3): what fixes whole components, where a joint relates two sides.
// The data upstream's Python GroundedJoint and RigidGroupJoint carry, typed,
// under the same names, so that their Python proxies and view providers
// attach to the ...Python features unchanged.

#pragma once

#include <App/DocumentObject.h>
#include <App/FeaturePython.h>
#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/SuppressibleExtension.h>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace Assembly
{

/// Fixes one component of the assembly where it is
class AssemblyExport GroundedJoint: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::GroundedJoint);

public:
    GroundedJoint();

    /// The component held. Global, as upstream's: it may be in a group
    App::PropertyLinkGlobal ObjectToGround;

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

using GroundedJointPython = App::FeaturePythonT<GroundedJoint>;


/// Moves components together, at the placements they had relative to the
/// first when the group was made or its positions stored
class AssemblyExport RigidGroup: public App::DocumentObject, public App::SuppressibleExtension
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::RigidGroup);

public:
    RigidGroup();

    App::PropertyLinkList ObjectsToRigidGroup;
    /// Each member's placement relative to the first, for restore
    App::PropertyPlacementList RigidPlacements;

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

using RigidGroupPython = App::FeaturePythonT<RigidGroup>;

}  // namespace Assembly
