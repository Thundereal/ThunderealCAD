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
    /// Bend at the rule's radius; off, at BendRadius (docs/SheetMetal.md
    /// sec 12, ruled 2026-11-03)
    App::PropertyBool UseRuleRadius;
    App::PropertyLength BendRadius;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalFold";
    }

protected:
    /** A jog's (SheetMetalJog) straight run between its two bends and the
     * width of the wall it takes, given T, R, the angle and a bend's
     * allowance ba; false for a fold's one bend, ba taken
     */
    virtual bool jogged(double T, double R, double A, double ba, double& run,
                        double& taken) const;
    /// The feature's name in its messages
    virtual const char* featureNoun() const
    {
        return "fold";
    }

private:
    static const char* LinePositionEnums[];
};

/** A wall stepped along a sketched line (docs/SheetMetal.md sec 5.1, ruled
 * 2026-11-02)
 *
 * A fold (SheetMetalFold) and a second bend turning back by the same
 * angle, a straight run between them, so the moving side stays parallel,
 * Offset away. The line is the first bend's, as a fold's. OffsetMeasure
 * says what Offset is: the Step the wall moves (a skin to the same skin),
 * the gap Inside between the facing skins, or the Overall of the outer
 * skins. With FixProjectedLength the moving side's end stays where it was,
 * the jog adding its material and the flat longer by that; without, the
 * jog takes it from the wall, as a fold does.
 */
class PartDesignExport SheetMetalJog : public SheetMetalFold
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::SheetMetalJog);

public:
    SheetMetalJog();

    /// How far the moving side is stepped, as OffsetMeasure says
    App::PropertyLength Offset;
    /// What Offset measures: Step, Inside, Overall
    App::PropertyEnumeration OffsetMeasure;
    /// The moving side's end stays where it was, the jog adding material
    App::PropertyBool FixProjectedLength;

    short mustExecute() const override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalJog";
    }

protected:
    bool jogged(double T, double R, double A, double ba, double& run,
                double& taken) const override;
    const char* featureNoun() const override
    {
        return "jog";
    }

private:
    static const char* OffsetMeasureEnums[];
};

}  // namespace PartDesign
