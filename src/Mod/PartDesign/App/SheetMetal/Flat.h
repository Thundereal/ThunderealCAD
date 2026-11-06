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
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

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
        /// The line, and each tangent line, as far as it lies on the blank,
        /// in order along it: cut where it runs over a hole, a slot or a
        /// relief, or off the outline (docs/SheetMetal.md sec 12)
        std::vector<TopoDS_Edge> segments;
        std::array<std::vector<TopoDS_Edge>, 2> tangentSegments;
    };
    /// Each bend's line, along its middle on the inner skin
    std::vector<BendLine> bendLines;
    /// Each face of the folded part and the face of the solid it lies flat as
    std::vector<std::pair<TopoDS_Shape, TopoDS_Shape>> faces;
    /// The grain arrow, where asked for (grainArrow())
    std::vector<TopoDS_Edge> grain;
    struct CrossBreak
    {
        /// The wall's
        std::string name;
        /// Corner to corner of the wall's region, on the blank's top
        std::array<TopoDS_Edge, 2> diagonals;
        /// The break rises toward one looking down on the flat
        bool up = false;
        /// rad
        double angle = 0;
    };
    /// Each wall's cross break (Element::crossBreak)
    std::vector<CrossBreak> crossBreaks;
    /// The flat-only markings on the blank's top, the flat object's
    /// (SheetMetalFlatPattern::Markings)
    std::vector<TopoDS_Edge> markings;
    /// What lays the root wall, where it stands, on XY: its skin there, the
    /// flat turned as asked
    gp_Trsf toXY;
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

/** The faces of a flat solid on XY, the faces of one element each merged
 * where they lie in one plane: what a DXF or an SVG draws. Throws
 * Base::ValueError for a flat with no face on XY.
 */
/** What of \a line, a straight line on a skin of the flat \a solid (a bend's
 * line or tangent line), lies on the blank, in order along it: cut where it
 * runs over a hole, a slot, a relief or off the outline
 */
PartDesignExport std::vector<TopoDS_Edge> onBlank(const TopoDS_Shape& solid,
                                                  const TopoDS_Edge& line);

PartDesignExport std::vector<TopoDS_Face> facesOnXY(const TopoDS_Shape& solid);

/** A cross break's note, "CROSS BREAK UP 3" and \a degree after the angle
 * (deg, to 6 places at most)
 */
PartDesignExport std::string crossBreakNote(const FlatResult::CrossBreak& cross,
                                            const char* degree);

/** The face of wall \a index of \a shape its cross break rises from: of
 * the wall's plane faces whose outer normal goes the way the break rises
 * (Element::crossBreak), the largest; null for none
 */
PartDesignExport TopoDS_Face crossBreakFace(const TopoDS_Shape& shape, const Model& model,
                                            int index);

/** The diagonals of a plane \a face: corner to corner of its bounds in the
 * frame of its longest straight edge, each as far as it runs over the face
 * from its first meeting to its last; none for a face with no straight edge
 */
PartDesignExport std::vector<TopoDS_Edge> diagonals(const TopoDS_Face& face);

/// A face a wall's cross break bows (crossBreakPatches())
struct PartDesignExport CrossBreakPatch
{
    /// The face, 0 based, as TopExp::MapShapes() numbers them
    int face = -1;
    /// The wall's, by its index in the model
    int wall = -1;
    /// The middle of the rectangle the break is over, on the face's plane
    gp_Pnt center;
    /// Its long side's way (the longest straight edge's), and the way the
    /// break rises, both in the plane's frame
    gp_Dir direction;
    gp_Dir normal;
    /// Its half length along direction, half width across it
    double halfLength = 0;
    double halfWidth = 0;
    /// The break's angle, rad, above 0
    double angle = 0;
    /// Whether it rises out of this face, not into it
    bool rises = false;
};

/** The faces of \a shape each wall's cross break bows: of the wall's plane
 * faces, those on either skin, over the rectangle its diagonals run corner
 * to corner of (diagonals() of crossBreakFace()); none for a wall with none
 */
PartDesignExport std::vector<CrossBreakPatch> crossBreakPatches(const TopoDS_Shape& shape,
                                                                const Model& model);

/// A notch's shape at a bend's line (SheetMetalFlatPattern::Notch)
enum class NotchKind
{
    None,
    V,
    Slot,
};

/** The notches of a flat at its bends (docs/SheetMetal.md sec 12): where a
 * bend's line meets the blank's outer outline, on a straight piece of it
 * within 10 deg of square to the line -- not where the line ends at a hole it
 * runs over, nor in a corner's miter or round relief -- a prism through the
 * blank each, to cut from it: a V \a depth along the line into the blank
 * and \a width across it at the outline, or a slot of that size. A tool
 * reaches past the outline only so far as nothing else of the blank is
 * there.
 *
 * Throws Base::ValueError for a notch of no depth or width.
 */
PartDesignExport std::vector<TopoDS_Shape>
notchTools(const FlatResult& flat, NotchKind kind, double depth, double width);

}  // namespace PartDesign::SheetMetal
