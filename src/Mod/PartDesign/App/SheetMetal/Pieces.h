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
#include <utility>
#include <vector>

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <Mod/Part/App/TopoShape.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "Fold.h"
#include "Model.h"

namespace PartDesign::SheetMetal
{

/** The sections the pieces of a sheet are made from (docs/SheetMetal.md sec 4.4)
 *
 * Each a planar face, named, its edges named by role, so the faces swept
 * from them carry those names.
 */

/// A planar face of the given edges in order, the face and each edge named
PartDesignExport Part::TopoShape
namedFace(const std::vector<std::pair<TopoDS_Edge, std::string>>& edges, const std::string& name);

PartDesignExport TopoDS_Edge lineEdge(const gp_Pnt& a, const gp_Pnt& b);

/** Each planar face's edges given their curve on it, stored: OCCT computes
 * a curve on a plane it is not given each time it is asked, and a join walks
 * every face of the part asking, each feature down a chain again
 */
PartDesignExport void storePCurves(const Part::TopoShape& shape);
/// An arc about the frame's Z, from its X through angle, reversed if asked
PartDesignExport TopoDS_Edge arcEdge(const gp_Ax3& frame, double radius, double angle,
                                     bool reversed);

/** A bend's section in the plane through its frame's origin normal to its
 * axis: the edges name + Start (angle 0), Inner, End (the angle), Outer
 */
PartDesignExport Part::TopoShape bendSection(const CylBend& bend, const std::string& name);

/** A straight run's section: from start along a length, across by the
 * thickness; the edges name + Skin1 (at start, along), End, Skin2, Start
 */
PartDesignExport Part::TopoShape wallSection(const gp_Pnt& start, const gp_Dir& along,
                                             double length, const gp_Dir& across,
                                             double thickness, const std::string& name);

/// A bend relief: its shape, its width along the edge and its depth from the wall's end
struct PartDesignExport Relief
{
    enum class Shape
    {
        Rectangle,
        Obround,
        Slit,
    };
    Shape shape = Shape::Rectangle;
    double width = 0;
    double depth = 0;
};

/** The cuts of a bend relief beside a bend that ends inside its wall's end
 * (docs/SheetMetal.md sec 5.2): the wall's inner skin at the edge's first
 * vertex is inner, the edge runs along, the end faces endOut, and the bend
 * turns toward up. The bend spans span along the edge from gap1 in, gap2
 * left after it; at each end with a gap, a relief the width wide right
 * beside the bend, from the wall's end back the depth, through the sheet,
 * named name + "1" or "2". An obround's straight part stops at its half
 * circle's middle, the circle reaching the depth.
 */
PartDesignExport std::vector<Part::TopoShape>
reliefCuts(const Relief& relief, const gp_Pnt& inner, const gp_Dir& along, const gp_Dir& endOut,
           const gp_Dir& up, double thickness, double gap1, double span, double gap2,
           const std::string& name, const App::StringHasherRef& hasher);

/// A straight edge where a wall's skin meets its end, as a flange or a hem takes it
struct PartDesignExport WallEdge
{
    /// its ends and its way from the first to the last
    gp_Pnt p0, p1;
    gp_Dir along;
    /// the skin's and the end's outward normals
    gp_Dir skinOut, endOut;
    /// the wall's index in the model
    int wall = -1;
};

/** The edge ref of base, a wall's in the model; label names it in errors.
 * Throws Base::ValueError for no edge, one not straight, or one not where a
 * wall's skin meets an end across the sheet.
 */
PartDesignExport WallEdge wallEdge(const Part::TopoShape& base, const Model& model,
                                   const std::string& ref, const std::string& label);


/// The faces of shape the wall owns lying on the plane
PartDesignExport std::vector<TopoDS_Face>
skinFaces(const TopoDS_Shape& shape, const Model& model, int wall, const gp_Pln& plane);

}  // namespace PartDesign::SheetMetal
