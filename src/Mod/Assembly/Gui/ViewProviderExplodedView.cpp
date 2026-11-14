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

#include <App/Application.h>
#include <Base/Color.h>
#include <Base/Parameter.h>
#include <Mod/Assembly/App/Views.h>

#include "ViewProviderExplodedView.h"

using namespace AssemblyGui;

PROPERTY_SOURCE(AssemblyGui::ViewProviderExplodedView, Gui::ViewProviderDocumentObjectGroup)

bool ViewProviderExplodedView::getEdgeInfo(const char* mappedName, EdgeInfo& info) const
{
    if (!Assembly::ExplodedView::isTrailName(mappedName)) {
        return false;
    }
    // As a step draws its trail in the 3D view: dashed, in its colour
    // (CommandCreateView.ViewProviderExplodedViewStep)
    auto hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly");
    info.visible = true;
    info.style = 2;  // Qt::DashLine
    // Its colour only: the preference's last byte is not an opacity (the 3D
    // view's SoBaseColor reads the colour alone, and the default's is 0)
    Base::Color color;
    color.setPackedValue(hGrp->GetUnsigned("StepLineColor", 0xCC333300));
    info.color = Base::Color(color.r, color.g, color.b, 1.0F);
    info.width = 0;
    return true;
}

namespace Gui
{
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(AssemblyGui::ViewProviderExplodedViewPython,
                         AssemblyGui::ViewProviderExplodedView)
/// @endcond

template class AssemblyGuiExport ViewProviderFeaturePythonT<AssemblyGui::ViewProviderExplodedView>;
}  // namespace Gui
