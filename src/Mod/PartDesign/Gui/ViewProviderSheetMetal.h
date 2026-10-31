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

#include "ViewProviderAddSub.h"
#include "ViewProviderSketchBased.h"

namespace PartDesignGui
{

/// A base wall, its panel its values (docs/SheetMetal.md sec 5.1)
class PartDesignGuiExport ViewProviderSheetMetalBaseWall: public ViewProviderSketchBased
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalBaseWall);

public:
    ViewProviderSheetMetalBaseWall();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A flange, its panel its edges and values
class PartDesignGuiExport ViewProviderSheetMetalFlange: public ViewProviderAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalFlange);

public:
    ViewProviderSheetMetalFlange();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

}  // namespace PartDesignGui
