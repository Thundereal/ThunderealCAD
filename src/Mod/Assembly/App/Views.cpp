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

#include "Views.h"

using namespace Assembly;


PROPERTY_SOURCE_WITH_EXTENSIONS(Assembly::ExplodedView, App::DocumentObject)

ExplodedView::ExplodedView()
{
    GroupExtension::initExtension(this);
}


PROPERTY_SOURCE(Assembly::ExplodedViewStep, App::DocumentObject)

const char* ExplodedViewStep::MoveTypeEnums[] = {"Normal", "Radial", nullptr};

ExplodedViewStep::ExplodedViewStep()
{
    ADD_PROPERTY_TYPE(References, (nullptr), "Exploded Move", App::Prop_None,
                      "The objects moved by the move");
    ADD_PROPERTY_TYPE(MovementTransform, (Base::Placement()), "Exploded Move", App::Prop_None,
                      "This is the movement of the move. The end placement is the result of "
                      "the start placement * this placement.");
    ADD_PROPERTY_TYPE(MoveType, (0L), "Exploded Move", App::Prop_None, "The type of the move");
    MoveType.setEnums(MoveTypeEnums);
}


PROPERTY_SOURCE(Assembly::Snapshot, App::DocumentObject)

Snapshot::Snapshot()
{
    ADD_PROPERTY_TYPE(Components, (), "Snapshot", App::Prop_None,
                      "List of components captured in this snapshot.");
    ADD_PROPERTY_TYPE(Placements, (), "Snapshot", App::Prop_None,
                      "List of corresponding placements for the components.");
    ADD_PROPERTY_TYPE(Visibilities, (), "Snapshot", App::Prop_None,
                      "List of visibility states for the components.");
    ADD_PROPERTY_TYPE(SolveOnActivation, (true), "Snapshot", App::Prop_None,
                      "If true, applying the snapshot will solve the assembly after restoring "
                      "the placements.");
}


// Python features --------------------------------------------------------

namespace App
{
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(Assembly::ExplodedViewPython, Assembly::ExplodedView)
template<>
const char* Assembly::ExplodedViewPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::ExplodedView>;

PROPERTY_SOURCE_TEMPLATE(Assembly::ExplodedViewStepPython, Assembly::ExplodedViewStep)
template<>
const char* Assembly::ExplodedViewStepPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::ExplodedViewStep>;

PROPERTY_SOURCE_TEMPLATE(Assembly::SnapshotPython, Assembly::Snapshot)
template<>
const char* Assembly::SnapshotPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::Snapshot>;
/// @endcond
}  // namespace App
