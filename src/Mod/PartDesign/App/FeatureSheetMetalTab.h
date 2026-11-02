// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/
#pragma once

#include "FeatureSketchBased.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** A plate added on a wall (docs/SheetMetal.md sec 5.1)
 *
 * Profile is a closed sketch on a wall's skin, or on a plane parallel to
 * it, the nearest such wall it meets. Its faces, holes and all, are taken
 * onto the wall and made solid through its slab, so the tab is the wall's:
 * the sheet model is the one before. A tab running past a bend of the wall
 * or off it is refused.
 */
class PartDesignExport SheetMetalTab : public ProfileBased, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalTab);

public:
    SheetMetalTab();

    App::DocumentObjectExecReturn* execute() override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalTab";
    }
};

}  // namespace PartDesign
