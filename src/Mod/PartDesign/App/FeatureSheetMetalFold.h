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

#include "FeatureSketchBased.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** A wall bent along a sketched line (docs/SheetMetal.md sec 5.1, ruled
 * 2026-11-02)
 *
 * Profile is a sketch of a line lying on a wall, parallel to it. The wall
 * is bent there through Angle with the rule's radius, toward the side of
 * the sketch's normal (Reversed the other way). The strip the bend takes
 * in the flat, its allowance wide, lies about the line (Middle), from it
 * on the moving side (Start) or up to it (End). One side of the line
 * moves, with all that is on it (holes, flanges), carried rigidly: the
 * side of the face picked in Moving, else the smaller. So the flat is the
 * part as it was.
 */
class PartDesignExport SheetMetalFold : public ProfileBased, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalFold);

public:
    SheetMetalFold();

    /// How far the moving side turns
    App::PropertyAngle Angle;
    /// Where the line lies in the bend: Middle, Start, End
    App::PropertyEnumeration LinePosition;
    /// A face on the side that moves, of the feature this builds on; none
    /// for the smaller side
    App::PropertyLinkSub Moving;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalFold";
    }

private:
    static const char* LinePositionEnums[];
};

}  // namespace PartDesign
