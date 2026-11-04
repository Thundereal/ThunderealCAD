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

#include <functional>
#include <vector>

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <Mod/PartDesign/PartDesignGlobal.h>

#include "Model.h"

namespace PartDesign::SheetMetal
{

/// A thin part's sheet, found from its geometry (recognise())
struct PartDesignExport Recognised
{
    /// Its walls and bends: the fixed face's wall first, Wall1, its first
    /// skin the fixed face; each bend its own radius
    Model model;
    /** Faces to split the part with, where each bend meets its walls (two
     * half planes a bend, about its tangent lines): a face across the sheet
     * running from a wall on through a bend -- one cap over a whole profile,
     * as a part made elsewhere has it -- lies in one element after
     */
    std::vector<TopoDS_Face> cutters;
};

/** Recognise a thin part (docs/SheetMetal.md sec 4.2 way 3, ruled 2026-11-04)
 *
 * Its thickness is from the fixed face, a plane, to the skin opposite it in
 * the material. A wall is a pair of planar skins that far apart; a bend a
 * pair of coaxial cylinders that far apart, of its own radius, between the
 * two walls whose skins meet it. Any other face is the side of a wall (a
 * hole, a cap) where it lies within the wall's thickness. \a kOf gives each
 * bend's K from its angle and radius.
 *
 * Throws Base::ValueError, naming the face: a fixed face that is no plane, or
 * has no skin opposite it; a skin whose opposite one is not that far (the
 * thickness varies); a curved face that is no bend nor in a wall (only
 * planar walls are taken, rolled walls later).
 */
PartDesignExport Recognised recognise(const TopoDS_Shape& solid, const TopoDS_Face& fixed,
                                      const std::function<double(double, double)>& kOf);

}  // namespace PartDesign::SheetMetal
