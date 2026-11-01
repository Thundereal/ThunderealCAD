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

#include <ostream>

#include <Mod/PartDesign/PartDesignGlobal.h>

#include "Flat.h"

namespace PartDesign::SheetMetal
{

/** Write a flat as DXF R12 (docs/SheetMetal.md sec 4.7)
 *
 * From its face on XY: the outline on the layer OUTLINE, the inner
 * contours on INNER; the bend lines on BEND_UP and BEND_DOWN, each labelled
 * with its bend's name on LABELS; with tangents, each bend's tangent lines
 * on BEND_TANGENT. Lines, arcs and circles as they are, any other curve as
 * a polyline. Throws Base::ValueError for a flat with no face on XY.
 */
PartDesignExport void writeDXF(std::ostream& out, const FlatResult& flat, bool tangents = false);

}  // namespace PartDesign::SheetMetal
