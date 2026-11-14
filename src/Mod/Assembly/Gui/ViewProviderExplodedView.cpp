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
#include <Gui/BitmapFactory.h>
#include <Base/Parameter.h>
#include <Mod/Assembly/App/Views.h>

#include "ViewProviderExplodedView.h"

using namespace AssemblyGui;

PROPERTY_SOURCE(AssemblyGui::ViewProviderExplodedView, Gui::ViewProviderDocumentObjectGroup)

namespace
{

bool trailEdgeInfo(const char* mappedName, Gui::ViewProvider::EdgeInfo& info)
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

}  // namespace

/// A part's edge: its number in the view's BOM, as a balloon, the edge drawn
/// as the drawing draws it (docs/AssemblyExplode.md sec 9)
static bool partEdgeInfo(const App::DocumentObject* view,
                  const char* mappedName,
                  Gui::ViewProvider::EdgeInfo& info)
{
    auto exploded = freecad_cast<const Assembly::ExplodedView*>(view);
    if (!exploded) {
        return false;
    }
    auto number = Assembly::ExplodedView::partNumberOf(exploded->Bom.getValue(),
                                                       exploded->getDocument(),
                                                       mappedName);
    if (number.empty()) {
        return false;
    }
    info.styled = false;
    info.label = number;
    info.oncePerLabel = true;
    info.bubble = "Circular";
    return true;
}

bool ViewProviderExplodedView::getEdgeInfo(const char* mappedName, EdgeInfo& info) const
{
    return trailEdgeInfo(mappedName, info) || partEdgeInfo(getObject(), mappedName, info);
}

PROPERTY_SOURCE(AssemblyGui::ViewProviderExplodedViewAt, Gui::ViewProviderDocumentObject)

bool ViewProviderExplodedViewAt::getEdgeInfo(const char* mappedName, EdgeInfo& info) const
{
    auto at = freecad_cast<Assembly::ExplodedViewAt*>(getObject());
    return trailEdgeInfo(mappedName, info)
        || (at && partEdgeInfo(at->View.getValue(), mappedName, info));
}

QIcon ViewProviderExplodedViewAt::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_ExplodedView");
}

namespace Gui
{
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(AssemblyGui::ViewProviderExplodedViewPython,
                         AssemblyGui::ViewProviderExplodedView)
/// @endcond

template class AssemblyGuiExport ViewProviderFeaturePythonT<AssemblyGui::ViewProviderExplodedView>;
}  // namespace Gui
