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
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "SheetMetalFlat.h"

namespace PartDesign
{

class SolidBody;

/** A sheet's flat, one per sheet (docs/SheetMetal.md sec 12, ruled 2026-11-04)
 *
 * In the body, as the rule is, out of the solid chain; made by the body when
 * its solid first holds sheet metal, hidden. It holds the flat's settings,
 * which every feature of the sheet is laid flat by (SolidBody::flatOf()),
 * and saves one shape, its solid's tip's flat, with what a bill of
 * materials takes of it. It lays flat in its own recompute only, and only
 * while it is shown or something uses it; otherwise it keeps what it last
 * laid and says it is out of date.
 */
class PartDesignExport SheetMetalFlatPattern: public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::SheetMetalFlatPattern);

public:
    SheetMetalFlatPattern();

    /// The sheet's solid it is the flat of
    App::PropertyLink Sheet;

    /** @name How the sheet lies flat */
    //@{
    /// The wall laid on XY, by its name; the first wall if empty
    App::PropertyString Root;
    /// The root wall's second skin on XY, not its first
    App::PropertyBool SecondSkin;
    /// An edge of the folded part, by its element name, the flat is turned
    /// to run along X by, where the feature laid flat has it
    App::PropertyString AlignEdge;
    /// Each bend's tangent lines in the flat
    App::PropertyBool TangentLines;
    /// The grain arrow in the flat
    App::PropertyBool Grain;
    /// The grain's direction, from the flat's X
    App::PropertyAngle GrainAngle;
    //@}

    /** @name What a bill of materials takes (sec 5.4), of the shape */
    //@{
    App::PropertyLength FlatLength;
    App::PropertyLength FlatWidth;
    App::PropertyArea FlatArea;
    App::PropertyLength Thickness;
    App::PropertyString MaterialName;
    App::PropertyInteger BendCount;
    App::PropertyAngle MaxBendAngle;
    App::PropertyMap BendDirections;
    /// Empty when the shape is the tip's flat; else why not: out of date,
    /// or why the tip does not lie flat
    App::PropertyString Status;
    //@}

    /// What Status says of a flat left as it was while hidden
    static const char* OutOfDate;

    /// The settings above
    FlatSettings settings() const;
    /// The solid it is the flat of
    SolidBody* getSheet() const;
    /// Whether it lays flat in its recompute: shown, or used by an object
    /// that is not its body
    bool isWanted() const;

    short mustExecute() const override;
    App::DocumentObjectExecReturn* execute() override;
    bool shouldApplyPlacement() override
    {
        return true;
    }

protected:
    void onChanged(const App::Property* prop) override;
    bool skipRecompute() override;
};

}  // namespace PartDesign
