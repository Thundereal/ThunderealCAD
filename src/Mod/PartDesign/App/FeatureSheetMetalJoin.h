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

/** Two walls joined by a bend (docs/SheetMetal.md sec 5.1, ruled 2026-11-03)
 *
 * Edges are two, one at each wall's end where a skin meets it, as a
 * flange's; they lie parallel, and the walls are not joined yet. The bend is
 * the rule's radius, its inner skin touching both walls' inner skins, the
 * inside of the turn the side the walls lie toward; each wall is extended or
 * trimmed to where the bend starts, over the length the two edges share.
 * Where a wall is trimmed and runs on past that length, the bend relief goes
 * beside the bend. The first edge's wall is the bend's before.
 *
 * Parallel walls (2026-11-11) take two bends of 90 deg and a run between
 * them, a wall of the sheet, across from the one to the other: the first
 * edge's wall stays as it is, its bend starting at its end, and the second
 * is extended or trimmed to where its bend starts -- past the run for ends
 * facing each other (an S), back over the first for ends the same way (a
 * U). Their facing skins are the bends' inner ones, twice the radius apart
 * at least; at just that, ends the same way, the one bend of a half turn.
 */
class PartDesignExport SheetMetalJoin : public FeatureAddSub, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalJoin);

public:
    SheetMetalJoin();

    /// The two edges, of the feature the join builds on
    App::PropertyLinkSub Edges;
    /// The bend relief where a wall is trimmed under the bend and runs on
    /// beside it: Rule follows the rule's, another shape is the join's own
    App::PropertyEnumeration ReliefShape;
    App::PropertyLength ReliefWidth;
    App::PropertyLength ReliefDepth;
    /// Bend at the rule's radius; off, at BendRadius (docs/SheetMetal.md
    /// sec 12, ruled 2026-11-03)
    App::PropertyBool UseRuleRadius;
    App::PropertyLength BendRadius;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    /// its own faces are those tracing to it, as a pad's are
    bool isElementGenerated(const Part::TopoShape& shape,
                            const Data::MappedName& name) const override
    {
        return isElementTraced(shape, name);
    }
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalJoin";
    }
    bool shouldApplyPlacement() override
    {
        // built in place, as the flange is (SheetMetalFlange::shouldApplyPlacement())
        return false;
    }

private:
    /// The relief's width and depth, read only while it follows the rule
    void showReliefValues();

    static const char* ReliefShapeEnums[];
};

}  // namespace PartDesign
