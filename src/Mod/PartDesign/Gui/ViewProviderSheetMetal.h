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

#include <Mod/Part/Gui/ViewProvider.h>

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

/// An extend wall, its panel its faces and length
class PartDesignGuiExport ViewProviderSheetMetalExtendWall: public ViewProviderAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalExtendWall);

public:
    ViewProviderSheetMetalExtendWall();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A flat pattern, its panel what it lays flat, its root face and the values
/// a bill of materials takes; a double-click opens it
class PartDesignGuiExport ViewProviderSheetMetalFlatPattern: public PartGui::ViewProviderPart
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalFlatPattern);

public:
    ViewProviderSheetMetalFlatPattern();
    bool doubleClicked() override;

protected:
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
};

/// A sheet metal rule, its panel its card and values; a double-click opens it
class PartDesignGuiExport ViewProviderSheetMetalRule: public PartGui::ViewProviderPart
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalRule);

public:
    ViewProviderSheetMetalRule();
    bool doubleClicked() override;

protected:
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
};

}  // namespace PartDesignGui
