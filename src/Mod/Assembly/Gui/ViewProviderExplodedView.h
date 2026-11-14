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

#include <Gui/ViewProviderDocumentObjectGroup.h>
#include <Gui/ViewProviderFeaturePython.h>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace AssemblyGui
{

/** An exploded view's view provider: its group of steps, as before, and
 * how its shape's trail edges are drawn where the shape is drawn again --
 * a TechDraw view of the exploded view draws them dashed, in the steps'
 * line colour (docs/AssemblyExplode.md sec 5)
 */
class AssemblyGuiExport ViewProviderExplodedView: public Gui::ViewProviderDocumentObjectGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderExplodedView);

public:
    ViewProviderExplodedView() = default;

    bool getEdgeInfo(const char* mappedName, EdgeInfo& info) const override;
};

using ViewProviderExplodedViewPython = Gui::ViewProviderFeaturePythonT<ViewProviderExplodedView>;

}  // namespace AssemblyGui
