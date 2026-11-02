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

#include "FeatureAddSub.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** Two legs meeting at a corner carried on to close it (docs/SheetMetal.md
 * sec 5.2, ruled 2026-11-02)
 *
 * Faces are the end of a flange's leg on each side of the corner, first A,
 * then B, both flanges bent from the same wall. Butt carries both on to the
 * plane halving the corner, the rule's Gap between, as the automatic miter
 * of two flanges does. Overlap carries A's bend and leg on to B's outer
 * skin, B's leg stopping the gap short of A's inner skin and B's bend the
 * gap short of where A's bend starts, so the two bends never meet;
 * Underlap is the same with A and B the other way round. The wall at the
 * corner, in front of where each bend starts, goes.
 */
class PartDesignExport SheetMetalCornerClosure : public FeatureAddSub, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalCornerClosure);

public:
    SheetMetalCornerClosure();

    /// The two legs' ends, of the feature this builds on
    App::PropertyLinkSub Faces;
    /// Butt, Overlap (the first leg over the second), Underlap
    App::PropertyEnumeration CornerType;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalCornerClosure";
    }
    bool shouldApplyPlacement() override
    {
        // built in place, as the flange is (SheetMetalFlange::shouldApplyPlacement())
        return false;
    }

private:
    static const char* CornerTypeEnums[];
};

}  // namespace PartDesign
