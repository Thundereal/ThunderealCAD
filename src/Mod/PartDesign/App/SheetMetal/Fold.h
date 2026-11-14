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

#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <Mod/PartDesign/PartDesignGlobal.h>

namespace PartDesign::SheetMetal
{

/** A cylindrical bend and how it folds (docs/SheetMetal.md sec 4.1)
 *
 * The frame's origin is on the bend's axis, its Z the axis, turned about
 * which by +angle the sheet bends, and its X the way from the axis to where
 * the inner skin starts. The sheet comes into the bend along Z x X (the
 * frame's Y), its inner skin at the radius, its outer one a thickness
 * further out.
 *
 * Flat, the wall before the bend stays where it is and the bend lies on in
 * its plane: a point at angle a into the bend and depth d below the inner
 * skin goes a (R + K T) along the frame's Y, d below the inner skin -- every
 * fibre the neutral fibre's length, as the K-factor has it. The wall after
 * the bend moves with the bend's last fibres, rigidly (unbend()).
 */
struct PartDesignExport CylBend
{
    gp_Ax3 frame;
    double radius = 0;
    double thickness = 0;
    double angle = 0;
    double k = 0.44;

    /// The neutral fibre's radius, R + K T
    double neutral() const
    {
        return radius + k * thickness;
    }
    /// Where a point of the bend's material lies flat
    gp_Pnt toFlat(const gp_Pnt& folded) const;
    /// Where a point of the flat strip lies folded: toFlat()'s inverse
    gp_Pnt toFolded(const gp_Pnt& flat) const;
    /// What lays the wall after the bend flat: back through the angle about
    /// the axis, then along the strip by its length
    gp_Trsf unbend() const;
    /// unbend()'s inverse
    gp_Trsf refold() const;
    /** Which part of the sheet a point is in, by its angle about the axis:
     * below 0 before the bend (the wall it starts from), 0 in it, above 0
     * after it, as the wall before the bend sees it. Angles are taken from
     * -90 deg, where that wall runs off to, so none of its points is after
     * the bend.
     */
    int side(const gp_Pnt& point) const;
    /** side() as the wall after the bend sees it: angles taken from where
     * that wall runs off to, the bend's angle + 90 deg, so none of its points
     * is before the bend. For a bend past 180 deg, a teardrop's, they run
     * through -90 deg.
     */
    int sideAfter(const gp_Pnt& point) const;
    /// Whether the point is in the bend's material: in its angle, and
    /// between its skins within tol
    bool contains(const gp_Pnt& point, double tol = 1e-7) const;

private:
    /// The point's angle about the axis in [from, from + 2 pi), its distance
    /// from the axis and its place along it
    void polar(const gp_Pnt& point, double from, double& a, double& r, double& z) const;
    /// Where the bend's own angles are taken from: halfway round the turn it
    /// leaves out, so a roll's material past 270 deg is the bend's
    double from() const;
};

/** A conical bend and how it lies flat (docs/SheetMetal.md sec 4.1, M5's
 * lofted flange)
 *
 * The frame's origin is on the cone's axis, its Z the axis, the way the
 * cone opens, and its X the way from the axis to where the sheet starts.
 * The inner skin, the side facing the axis, passes through the circle of
 * the radius about the origin in the frame's XY plane and opens at
 * semiAngle (0 <= semiAngle < 90 deg; 0 a cylinder); the outer skin is a
 * thickness further out, square to it. The sheet runs from angle 0 about Z
 * through angle.
 *
 * A point of the sheet is told by its angle a about the axis, t, how far
 * along the cone's line it is from that circle, and d, its depth under the
 * inner skin, square to it. Flat, the sheet lies on the plane touching the
 * inner skin where it starts (startFrame()), and the neutral cone, a K T
 * under the inner skin, unrolls onto it: its apex goes to a point on the
 * start line, a point at a from the start to a sin(semiAngle) about it at
 * its distance from the apex, the same depth under the plane -- every fibre
 * the neutral fibre's length, as for a cylinder.
 */
struct PartDesignExport ConeBend
{
    gp_Ax3 frame;
    double radius = 0;
    double semiAngle = 0;
    double thickness = 0;
    double angle = 0;
    double k = 0.44;

    /// The neutral cone's radius where the inner skin's is the radius
    double neutral() const;
    /// The plane the sheet lies flat on: its origin on the inner skin where
    /// the sheet starts, in the frame's XY plane, its Z square to the skin
    /// into the material, its X along the cone's line, the way it opens
    gp_Ax3 startFrame() const;
    /// Where a point of the bend's material lies flat
    gp_Pnt toFlat(const gp_Pnt& folded) const;
    /// Where a point of the flat lies folded: toFlat()'s inverse
    gp_Pnt toFolded(const gp_Pnt& flat) const;
    /// Whether the point is in the bend's material: in its angle, and
    /// between its skins within tol
    bool contains(const gp_Pnt& point, double tol = 1e-7) const;
    /// The point's angle about the axis, taken from halfway round the turn
    /// the sheet leaves out, its t and its d
    void local(const gp_Pnt& point, double& a, double& t, double& d) const;
};

}  // namespace PartDesign::SheetMetal
