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
    /// What the flange did of its own accord, said in its panel: an end kept
    /// off a bend of its wall (ruled 2026-11-04); empty for nothing
    App::PropertyString Notice;
    /// A plane face the leg runs parallel to, in place of Angle
    App::PropertyLinkSub ParallelTo;
    /// A plane face the leg runs up to, in place of Length
    App::PropertyLinkSub UpToFace;
    App::PropertyBool Reversed;
    /// A closed sketch parallel to the leg giving its shape, on one edge
    App::PropertyLinkSub Profile;
    /// Bend at the rule's radius; off, at BendRadius (docs/SheetMetal.md
    /// sec 12, ruled 2026-11-03)
    App::PropertyBool UseRuleRadius;
    App::PropertyLength BendRadius;
    /// The bend relief: Rule follows the rule's; another shape is the
    /// flange's own, with its own width and depth, read only under Rule
    App::PropertyEnumeration ReliefShape;
    App::PropertyLength ReliefWidth;
    App::PropertyLength ReliefDepth;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    /// its own faces are those tracing to it, as a pad's are
    bool isElementGenerated(const Part::TopoShape& shape,
                            const Data::MappedName& name) const override
    {
        return isElementTraced(shape, name);
    }
    bool shouldApplyPlacement() override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalFlange";
    }

protected:
    /// A contour flange's: the leg an open chain of lines sketched from the edge
    virtual bool contour() const
    {
        return false;
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

/** A contour flange (docs/SheetMetal.md sec 5.1, ruled 2026-11-02)
 *
 * Profile is an open chain of lines on a plane square to one of the edges,
 * starting on it. The wall's skin through the edge runs on as the chain's
 * lines, the material on the side the wall's lies on: a bend where the
 * first line leaves the wall and at each corner after, a wall on each line.
 * The same profile goes on each edge, with the flange's gaps, relief and
 * miters; the flange's angle, length and bend position say nothing.
 */
class PartDesignExport SheetMetalContourFlange : public SheetMetalFlange
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::SheetMetalContourFlange);

public:
    SheetMetalContourFlange();

    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalContourFlange";
    }

protected:
    bool contour() const override
    {
        return true;
    }
};

}  // namespace PartDesign
