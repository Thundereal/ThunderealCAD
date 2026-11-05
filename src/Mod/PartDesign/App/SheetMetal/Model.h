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

#include <string>
#include <vector>

#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

class TopoDS_Face;

#include <Mod/PartDesign/PartDesignGlobal.h>

#include "Fold.h"

namespace PartDesign::SheetMetal
{

/** One wall or bend of a sheet (docs/SheetMetal.md sec 4.1)
 *
 * A wall is a slab: its frame's origin on its first skin, its Z the
 * normal from there into the material, the second skin a thickness along
 * it. A bend is a CylBend, between the wall it starts from (before) and
 * the one it ends in (after).
 */
struct PartDesignExport Element
{
    enum class Kind
    {
        Wall,
        Bend,
    };
    Kind kind = Kind::Wall;
    /// Its name by role, as the feature that made it gave it ("Wall1", "Bend2")
    std::string name;
    /// A wall's frame
    gp_Ax3 frame;
    /// A bend's geometry
    CylBend bend;
    /// A bend's walls, by index
    int before = -1;
    int after = -1;
    /// Planes the element stops at, its material on the side against each
    /// normal: a miter's, where two sides of a flange meet at a corner and
    /// their slabs and tubes would overlap; a bend's sides, where its flange
    /// ends inside the wall's end and its tube runs on through the wall
    std::vector<gp_Pln> limits;
};

/** A sheet's walls and bends, and how they lie flat
 *
 * Which element a point of the solid is in is told by its geometry: in a
 * bend's tube sector, or in a wall's slab on the wall's side of each bend
 * it meets, short of any of its limits. Flat, a chosen wall (the root) stays where it is, and every
 * other element moves with the bends between it and the root unrolled.
 */
class PartDesignExport Model
{
public:
    double thickness = 0;
    std::vector<Element> elements;

    /// Add a wall; returns its index
    int addWall(const std::string& name, const gp_Ax3& frame);
    /// Add a bend between two walls already added; returns its index.
    /// Throws Base::IndexError for a wall that is not one.
    int addBend(const std::string& name, const CylBend& bend, int before, int after);
    /// The element of that name, -1 for none
    int find(const std::string& name) const;

    /// Whether the point is in the element's material, within tol
    bool contains(int index, const gp_Pnt& point, double tol = 1e-7) const;
    /// The element the point is in, bends tried first; -1 for none
    int owner(const gp_Pnt& point, double tol = 1e-7) const;
    /// The element a face is in, by a point inside it; -1 for none
    int owner(const TopoDS_Face& face, double tol = 1e-7) const;
    /// Move every element
    void transform(const gp_Trsf& trsf);
    /** Take an element out, every index after it moved down. A bend's walls
     * stay; a wall still joined by a bend throws Base::ValueError.
     */
    void remove(int index);

    /** The first bend that closes a loop of walls and bends -- a tube's --
     * which does not lie flat; -1 for none
     */
    int closingBend() const;
    /// The first wall not joined to the first wall by bends, -1 for none
    int cutOff() const;

    /** What moves an element flat with root fixed: for a wall all of it,
     * for a bend what follows its own map (CylBend::toFlat()). Throws
     * Base::ValueError for a root that is not a wall, Base::IndexError for an
     * element not joined to it.
     */
    gp_Trsf placement(int index, int root) const;
    /// Where a point of the element lies flat with root fixed
    gp_Pnt toFlat(int index, const gp_Pnt& point, int root) const;

private:
    void checkIndex(int index) const;
};

}  // namespace PartDesign::SheetMetal
