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

#include <vector>

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include "Feature.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

class SheetMetalRule;

/** A part no sheet metal feature made, taken as sheet metal
 * (docs/SheetMetal.md sec 5.3, ruled 2026-11-04)
 *
 * Built on the feature before it -- an imported or a Part solid, as the
 * body's base. A thin part: its walls and bends found from its geometry
 * (SheetMetal::recognise()), each bend its own radius; its shape kept but for
 * faces split where a bend meets a wall. A solid or a shell: thickened by
 * the rule's thickness from its faces, inside or outside, a solid's open
 * faces taken away, each sharp edge between two of its faces rounded into a
 * bend of the rule's radius but where ripped, a corner relief where bends meet
 * at a rip, then taken as a thin part. The fixed face is the wall the flat
 * lies on, Wall1.
 */
class PartDesignExport SheetMetalConvert : public PartDesign::Feature, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalConvert);

public:
    SheetMetalConvert();

    /// A face of the part to convert, a wall's skin: the wall the flat lies on
    App::PropertyLinkSub FixedFace;
    /// A solid's faces to leave open
    App::PropertyLinkSub OpenFaces;
    /// Edges of a solid or a shell to rip, a gap across them, not bent
    App::PropertyLinkSub RipEdges;
    /// Which way a solid or a shell is thickened from its faces: Inside,
    /// Outside
    App::PropertyEnumeration Side;
    /// What the part was taken for: "Thin part", "Solid", "Shell"
    App::PropertyString Kind;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    bool shouldApplyPlacement() override
    {
        // the base's shape as it is, in place
        return false;
    }

private:
    /// A solid or a shell made a thin part: thickened, its rips and corner
    /// reliefs cut, its bends rounded
    Part::TopoShape thickened(const Part::TopoShape& base, const TopoDS_Face& fixed,
                              const std::vector<TopoDS_Shape>& open,
                              const std::vector<TopoDS_Shape>& rips,
                              const SheetMetalRule& rule);
};

}  // namespace PartDesign
