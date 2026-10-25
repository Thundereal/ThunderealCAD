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

// The exploded view, its steps and the snapshot as C++ objects
// (docs/AssemblyObjectModel.md sec 7.2): the data upstream's Python objects
// carry, typed, under the same names, so that their Python proxies, view
// providers and task panel attach to the ...Python features unchanged.

#pragma once

#include <App/DocumentObject.h>
#include <App/FeaturePython.h>
#include <App/GroupExtension.h>
#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace Assembly
{

/// An exploded view: owns its steps in Group, applied in order
class AssemblyExport ExplodedView: public App::DocumentObject, public App::GroupExtension
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::ExplodedView);

public:
    ExplodedView();

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObjectGroup";
    }
};

using ExplodedViewPython = App::FeaturePythonT<ExplodedView>;


/// One step of an exploded view, a child of it
class AssemblyExport ExplodedViewStep: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::ExplodedViewStep);

public:
    ExplodedViewStep();

    /// The parts the step moves, as subnames from one selection root: a
    /// part may be inside a sub-assembly. Hidden, as upstream's: the root
    /// can be an ancestor of the assembly that owns the step
    App::PropertyXLinkSubHidden References;
    /// The move: the end placement is this times the start placement
    App::PropertyPlacement MovementTransform;
    /// The same enumeration, in the same order, as
    /// CommandCreateView.ExplodedViewStepTypes
    App::PropertyEnumeration MoveType;

    static const char* MoveTypeEnums[];

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

using ExplodedViewStepPython = App::FeaturePythonT<ExplodedViewStep>;


/// The placements and visibilities of an assembly's parts, to go back to
class AssemblyExport Snapshot: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::Snapshot);

public:
    Snapshot();

    App::PropertyLinkList Components;
    App::PropertyPlacementList Placements;
    App::PropertyBoolList Visibilities;
    App::PropertyBool SolveOnActivation;

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

using SnapshotPython = App::FeaturePythonT<Snapshot>;

}  // namespace Assembly
