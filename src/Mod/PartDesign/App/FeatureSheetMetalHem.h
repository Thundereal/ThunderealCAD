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

/** A wall's end folded back over its skin (docs/SheetMetal.md sec 5.1)
 *
 * Each edge is where a wall's skin meets its end, as a flange's is; the
 * hem folds toward the side of that skin, Reversed toward the other. A
 * closed hem is a 180 deg bend whose inner skins are the rule's Gap apart,
 * so that they never touch (ruled 2026-11-02); an open one the same with a
 * gap of its own. Inside puts the fold's outer face where the wall ended,
 * the wall cut back under it; Outside adds the fold past the end. Length
 * runs from the fold's outer face to the leg's end. Edges meeting at a
 * corner are refused, as their hems would fold into each other.
 */
class PartDesignExport SheetMetalHem : public FeatureAddSub, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalHem);

public:
    SheetMetalHem();

    /// The edges, of the feature the hem builds on
    App::PropertyLinkSub Edges;
    /// Closed: the rule's Gap inside; Open: Gap inside
    App::PropertyEnumeration HemType;
    /// From the fold's outer face to the leg's end
    App::PropertyLength Length;
    /// An open hem's gap between the skins; read only for a closed one
    App::PropertyLength Gap;
    /// Inside: the fold's outer face where the wall ended; Outside: past it
    App::PropertyEnumeration Position;
    App::PropertyBool Reversed;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalHem";
    }
    bool shouldApplyPlacement() override
    {
        // built in place, as the flange is (SheetMetalFlange::shouldApplyPlacement())
        return false;
    }

private:
    /// Gap read only while the hem is closed
    void showGapValue();

    static const char* HemTypeEnums[];
    static const char* PositionEnums[];
};

}  // namespace PartDesign
