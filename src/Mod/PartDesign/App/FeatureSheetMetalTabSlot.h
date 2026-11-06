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
#include <Mod/Part/App/PropertyTopoShape.h>

#include "Feature.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** Tabs on a wall's end, for slots in a part it stands on (docs/SheetMetal.md
 * sec 4.8, ruled 2026-11-06)
 *
 * Face is an end face of a wall of the feature this builds on, across its
 * thickness. Count tabs, each Width wide along it and Length out from it,
 * the wall's thickness through, are spread between Margin in from either
 * end, their middles evenly apart (one in the middle). They are the wall's,
 * lying flat with it. TabShape holds them alone, in this feature's frame,
 * for SheetMetalSlots to cut.
 */
class PartDesignExport SheetMetalLocatingTabs : public PartDesign::Feature,
                                                public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalLocatingTabs);

public:
    SheetMetalLocatingTabs();

    /// A wall's end face, across its thickness, of the feature this builds on
    App::PropertyLinkSub Face;
    App::PropertyIntegerConstraint Count;
    /// Each tab along the end face
    App::PropertyLength Width;
    /// How far out of the end face each tab goes: the slotted part's thickness, or more
    App::PropertyLength Length;
    /// Kept clear of the end face's ends
    App::PropertyLength Margin;
    /// The tabs alone
    Part::PropertyPartShape TabShape;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalLocatingTabs";
    }
    bool shouldApplyPlacement() override
    {
        return false;
    }

private:
    bool sized = false;
};

/** Slots for another part's tabs (docs/SheetMetal.md sec 4.8)
 *
 * Tabs is a SheetMetalLocatingTabs, in this body or another: its tabs, where
 * they are in this feature's frame, grown by Clearance all round, are cut
 * through the feature this builds on. They follow the tabs: their count,
 * their sizes, their part moved.
 */
class PartDesignExport SheetMetalSlots : public PartDesign::Feature, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalSlots);

public:
    SheetMetalSlots();

    App::PropertyLink Tabs;
    App::PropertyLength Clearance;
    /// The tabs' body, set with Tabs: its placement moves them, so this
    /// follows it too
    App::PropertyLink TabsBody;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalSlots";
    }
    bool shouldApplyPlacement() override
    {
        return false;
    }
};

}  // namespace PartDesign
