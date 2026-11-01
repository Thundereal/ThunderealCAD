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

/** Write a flat as SVG (docs/SheetMetal.md sec 4.7), the DXF's layers as
 * groups (Inkscape layers) of the same names: OUTLINE, INNER, BEND_UP,
 * BEND_DOWN, LABELS, and with tangents BEND_TANGENT; GRAIN where the flat
 * has its grain arrow. A layer with nothing on it is left out. The drawing
 * is as large as the flat, in mm, looking down on it (Y up). Lines, circles
 * and arcs as they are, any other curve as a polyline. Throws
 * Base::ValueError for a flat with no face on XY.
 */
PartDesignExport void writeSVG(std::ostream& out, const FlatResult& flat, bool tangents = false);

}  // namespace PartDesign::SheetMetal
