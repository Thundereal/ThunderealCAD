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

/** A bend and a leg on edges of walls (docs/SheetMetal.md sec 5.1)
 *
 * Each edge is where a wall's skin meets a face across the sheet (an end).
 * The flange bends toward the side of that skin, Reversed toward the
 * other, through Angle, with the rule's radius, and goes on for its
 * length. BendPosition says where the bend sits against the wall's end, and
 * cuts the wall back under the flange where it starts inside it. All the
 * edges are joined in one boolean.
 *
 * On one edge, a closed sketch in Profile shapes the leg: the sketch lies
 * parallel to the leg and its outline stands on the outer virtual sharp.
 * Through the bend the outline is a rectangle, which sets the bend's width;
 * above the bend it is the leg, holes and all. Length, LengthType and the
 * gaps then say nothing and are read only.
 */
class PartDesignExport SheetMetalFlange : public FeatureAddSub, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalFlange);

public:
    SheetMetalFlange();

    /// The edges, of the feature the flange builds on
    App::PropertyLinkSub Edges;
    App::PropertyAngle Angle;
    App::PropertyLength Length;
    /// Leg: the straight part; InnerSharp, OuterSharp: from where the inner
    /// or outer skins of the wall and the leg meet; Tangent: from the plane
    /// across the leg that touches the outside of the bend
    App::PropertyEnumeration LengthType;
    /// MaterialInside: the leg's outer skin where the wall ended;
    /// MaterialOutside: its inner skin there; BendOutside: the bend starting
    /// there. Seen at the outer or inner virtual sharp for an angle not 90.
    App::PropertyEnumeration BendPosition;
    /// Moves the bend out from where BendPosition puts it, the wall carried on
    App::PropertyDistance Offset;
    /// Taken off the flange's width at the edge's first and last vertex; a
    /// negative one carries the flange past the edge's end
    App::PropertyDistance Gap1;
    App::PropertyDistance Gap2;
    /// A plane face the leg runs parallel to, in place of Angle
    App::PropertyLinkSub ParallelTo;
    /// A plane face the leg runs up to, in place of Length
    App::PropertyLinkSub UpToFace;
    App::PropertyBool Reversed;
    /// A closed sketch parallel to the leg giving its shape, on one edge
    App::PropertyLinkSub Profile;
    /// The bend relief: Rule follows the rule's; another shape is the
    /// flange's own, with its own width and depth, read only under Rule
    App::PropertyEnumeration ReliefShape;
    App::PropertyLength ReliefWidth;
    App::PropertyLength ReliefDepth;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    bool shouldApplyPlacement() override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalFlange";
    }

private:
    /// Length, LengthType and the gaps read only while a profile shapes the leg
    void showProfileValues();
    /// The relief's width and depth read only while it follows the rule
    void showReliefValues();

    static const char* LengthTypeEnums[];
    static const char* BendPositionEnums[];
    static const char* ReliefShapeEnums[];
};

}  // namespace PartDesign
