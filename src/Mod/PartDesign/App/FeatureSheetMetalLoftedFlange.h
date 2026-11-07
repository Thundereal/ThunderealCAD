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

#include "FeatureSketchBased.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** A sheet lofted between two outlines (docs/SheetMetal.md sec 5.1, M5)
 *
 * Profile and Section each a circle or an arc, on parallel planes about one
 * axis, make a cone frustum -- a cylinder where they are the same size --
 * one conical bend of the sheet model (SheetMetal::ConeBend), which lies
 * flat exactly. Two arcs turn through the same angle; two circles leave a
 * seam the rule's Gap wide on the narrow one.
 *
 * Each a chain of lines, as many in each, on parallel planes, make a wall
 * between each line of the profile and the section's line in its place
 * (the one running its way, and on round from there): a flat facet where
 * they are parallel, two triangles either side of the shorter diagonal
 * where they are not, and a bend at BendRadius at every fold. Two closed
 * chains leave a seam the rule's Gap wide across the middle of the
 * profile's first line, or along its diagonal.
 *
 * The ends are cut square to the sheet, so they lie flat square to the
 * blank, and the flat is exact.
 */
class PartDesignExport SheetMetalLoftedFlange : public ProfileBased, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalLoftedFlange);

public:
    SheetMetalLoftedFlange();

    /// The other outline the sheet runs to
    App::PropertyLink Section;
    /// Where the material is against the sketches: Outside, the sketches on
    /// the inner skin, the one facing the axis; Inside, on the outer skin;
    /// Middle
    App::PropertyEnumeration MaterialSide;
    /// Lines to lines: bend at the rule's radius; off, at BendRadius
    App::PropertyBool UseRuleRadius;
    App::PropertyLength BendRadius;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalLoftedFlange";
    }

private:
    static const char* MaterialSideEnums[];
};

}  // namespace PartDesign
