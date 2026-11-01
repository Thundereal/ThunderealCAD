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

#include <array>
#include <string>
#include <utility>
#include <vector>

#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

#include <Mod/PartDesign/PartDesignGlobal.h>

#include "Model.h"

namespace PartDesign::SheetMetal
{

/// What a sheet lies flat as
struct PartDesignExport FlatResult
{
    TopoDS_Shape solid;
    struct BendLine
    {
        std::string name;
        TopoDS_Edge edge;
        /// The material comes up toward one looking down on the flat: the
        /// bend's inner skin is on top
        bool up = false;
        double angle = 0;
        /// Where the bend meets its walls, on the same skin as the line
        std::array<TopoDS_Edge, 2> tangents;
    };
    /// Each bend's line, along its middle on the inner skin
    std::vector<BendLine> bendLines;
    /// Each face of the folded part and the face of the solid it lies flat as
    std::vector<std::pair<TopoDS_Shape, TopoDS_Shape>> faces;
    /// The grain arrow, where asked for (grainArrow())
    std::vector<TopoDS_Edge> grain;
};

/** Lay a folded sheet flat (docs/SheetMetal.md sec 4.7)
 *
 * Every face of the folded solid goes through the map of the element that
 * owns it: a wall's rigidly, a bend's unrolled. The root wall's skin --
 * its second if asked, else its first -- is laid on XY, the material above.
 * A pure function of its inputs, no document. Given alongX, an edge of the
 * folded part, the flat is turned about Z so that the edge's image runs
 * along X, from its first parameter to its last.
 *
 * Throws Base::ValueError naming a face no element owns, a face of a bend
 * that does not lie flat as a plane, a flat that does not close, or an
 * alongX that is not on the part or does not lie straight along the flat.
 */
PartDesignExport FlatResult flatten(const TopoDS_Shape& folded, const Model& model, int root,
                                    bool secondSkin, const TopoDS_Edge& alongX = TopoDS_Edge());

/** The grain arrow on a flat solid: a shaft through the middle of its top,
 * at angle (rad) from X, two fifths of its narrower side long, and the two
 * lines of its head at the end it points to
 */
PartDesignExport std::vector<TopoDS_Edge> grainArrow(const TopoDS_Shape& solid, double angle);

}  // namespace PartDesign::SheetMetal
