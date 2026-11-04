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

#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include "FeatureSketchBased.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** A sheet's first wall (docs/SheetMetal.md sec 5.1)
 *
 * A closed sketch makes a plate of the rule's thickness. An open sketch of
 * lines makes a profile wall: a wall along each line and a bend of the
 * rule's radius at each corner, extruded by Length. Each wall and bend is
 * built as a piece of its own, its edges named by role, and the pieces
 * joined, so every face of the result lies in one element of the model.
 */
class PartDesignExport SheetMetalBaseWall : public ProfileBased, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalBaseWall);

public:
    SheetMetalBaseWall();

    /// Where the material is against the sketch: Outside, the sketch on
    /// the first bend's inner skin (a plate: on the sketch's normal side);
    /// Inside, on its outer skin (a plate: against the normal); Middle
    App::PropertyEnumeration MaterialSide;
    /// How far a profile wall goes along the sketch's normal
    App::PropertyLength Length;
    /// Bend at the rule's radius; off, at BendRadius (docs/SheetMetal.md
    /// sec 12, ruled 2026-11-03)
    App::PropertyBool UseRuleRadius;
    App::PropertyLength BendRadius;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalBaseWall";
    }

private:
    static const char* MaterialSideEnums[];
};

}  // namespace PartDesign
