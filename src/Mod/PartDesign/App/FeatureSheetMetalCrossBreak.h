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

#include "Feature.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** Cross breaks on walls (docs/SheetMetal.md sec 12, ruled 2026-11-05)
 *
 * Faces are of the feature this builds on, a face of each wall to break.
 * The solid is not changed: the sheet's model gives each wall its break
 * (SheetMetal::Element::crossBreak), Angle rising out of the face picked, or
 * into it if Reversed, and the flat draws the wall's diagonals, Up or Down
 * as it lies. The diagonals in 3D are crossBreakLines()'s.
 */
class PartDesignExport SheetMetalCrossBreak : public PartDesign::Feature, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalCrossBreak);

public:
    SheetMetalCrossBreak();

    /// A face of each wall to break, of the feature this builds on
    App::PropertyLinkSub Faces;
    /// The angle each diagonal bends by, above 0 and up to 45 deg
    App::PropertyAngle Angle;
    /// Rising into the face picked, not out of it
    App::PropertyBool Reversed;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalCrossBreak";
    }
    bool shouldApplyPlacement() override
    {
        // the base's shape as it is, in place
        return false;
    }
};

}  // namespace PartDesign
