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
#include <App/PropertyUnits.h>

#include "FeatureAddSub.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** A wall's end pushed out (docs/SheetMetal.md sec 5.1)
 *
 * Each face is a wall's end, across the sheet; it is carried along its
 * normal by Length, or up to UpToFace's plane when that is given. A
 * negative length cuts the wall back. Gap1 and Gap2 narrow it at the end's
 * two ends. An extension that would meet another wall of the part stops
 * the rule's Gap short of it. The extension is the wall's own: the sheet's
 * model is as it was.
 */
class PartDesignExport SheetMetalExtendWall : public FeatureAddSub, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalExtendWall);

public:
    SheetMetalExtendWall();

    /// The ends, of the feature this builds on
    App::PropertyLinkSub Faces;
    App::PropertyDistance Length;
    /// A plane face to carry the ends up to, in place of Length
    App::PropertyLinkSub UpToFace;
    /// How far in from each end of a wall's end the extension stops
    App::PropertyLength Gap1;
    App::PropertyLength Gap2;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalExtendWall";
    }
    bool shouldApplyPlacement() override
    {
        // built in place, as the flange is (SheetMetalFlange::shouldApplyPlacement())
        return false;
    }
};

}  // namespace PartDesign
