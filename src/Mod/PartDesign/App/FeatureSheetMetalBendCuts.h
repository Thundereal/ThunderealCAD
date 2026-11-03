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

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include "FeatureAddSub.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** Hinge and perforation cuts across bends (docs/SheetMetal.md sec 5.2,
 * ruled 2026-11-03)
 *
 * Bends are faces of the bends to cut, of the feature this builds on. The
 * cuts are laid out in each bend's strip in the flat and folded back, so
 * the flat shows them as cut (SheetMetal::bendCutTools()): Slots, one row
 * across the strip for bending by hand; Staggered, rows of them, every
 * other one shifted, a laser living hinge; Sketch, a sketch's closed shapes
 * tiled along the bend. The bend stays folded, its radius and K as they
 * were.
 */
class PartDesignExport SheetMetalBendCuts : public FeatureAddSub, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalBendCuts);

public:
    SheetMetalBendCuts();

    /// A face of each bend to cut, of the feature this builds on
    App::PropertyLinkSub Bends;
    App::PropertyEnumeration Pattern;
    App::PropertyLength SlotLength;
    App::PropertyLength BridgeLength;
    App::PropertyLength EndMargin;
    /// Across the strip, about its middle; 0 the whole strip (Slots), half
    /// of each row's share (Staggered)
    App::PropertyLength SlotWidth;
    App::PropertyIntegerConstraint Rows;
    /// Sketch's tile: X along the bend, Y across it from its middle
    App::PropertyLinkSub Tile;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalBendCuts";
    }
    bool shouldApplyPlacement() override
    {
        // built in place, as the flange is (SheetMetalFlange::shouldApplyPlacement())
        return false;
    }

private:
    /// What the pattern takes, the rest read only
    void showPatternValues();

    static const char* PatternEnums[];
};

}  // namespace PartDesign
