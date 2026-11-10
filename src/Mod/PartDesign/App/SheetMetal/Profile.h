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
#include <string>
#include <vector>

#include <TopoDS_Wire.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <Mod/Part/App/TopoShape.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "Model.h"

namespace PartDesign::SheetMetal
{

/** The lines of an open wire as points in order; what names the sketch in
 * the error for an edge not a line
 */
PartDesignExport std::vector<gp_Pnt> linePoints(const TopoDS_Wire& wire, const std::string& what);

/** The lines and arcs of an open wire as its corners in order: an arc
 * (the rolled wall's, docs/SheetMetal.md sec 5.1, M5), tangent to a line
 * either side and turning less than 180 deg, is a corner at its lines'
 * virtual sharp, its radius as drawn in arcs at that corner's index (0 at
 * a corner of two lines). what names the sketch in the errors: an edge
 * neither, an arc at either end of the chain, two with no line between,
 * one not tangent or of 180 deg or more
 */
PartDesignExport std::vector<gp_Pnt>
profilePoints(const TopoDS_Wire& wire, const std::string& what, std::vector<double>& arcs);

/// Drops a point between two lines going on in one direction, and its arc
/// if arcs are given
PartDesignExport void dropStraight(std::vector<gp_Pnt>& points,
                                   std::vector<double>* arcs = nullptr);

/** A sheet along a chain of lines on a plane (docs/SheetMetal.md sec 5.1's
 * profile wall): a wall on each line, a bend of the radius at each corner,
 * their sections in the plane, to be swept across it. The walls and bends
 * go into the model, named on from the counts.
 */
struct PartDesignExport ProfileBuilder
{
    ProfileBuilder(Model& model, int& walls, int& bends)
        : model(model)
        , walls(walls)
        , bends(bends)
        , wallsBefore(walls)
        , bendsBefore(bends)
    {}

    double thickness = 0;
    double radius = 0;
    /// the bend's K factor from its angle and radius
    std::function<double(double, double)> k;
    Model& model;
    int& walls;
    int& bends;
    /// the counts the sheet had before this profile
    const int wallsBefore;
    const int bendsBefore;
    /// the sections made, each named as its element is among the profile's
    /// own (ownName())
    std::vector<Part::TopoShape> faces;
    /// with first a wall of the model, the first line is that wall already:
    /// no wall nor section is made for it, and firstEnd is where its
    /// straight part ends
    int first = -1;
    gp_Pnt firstEnd;
    /// the first bend made, -1 for none; every element made
    int firstBend = -1;
    std::vector<int> added;
    /// at each corner drawn as an arc, its radius as drawn (profilePoints()):
    /// its bend that radius on the sketch's line, not the radius; 0 or none
    /// for a corner of two lines
    std::vector<double> arcs;

    std::string nextWall()
    {
        return "Wall" + std::to_string(++walls);
    }
    std::string nextBend()
    {
        return "Bend" + std::to_string(++bends);
    }

    /// side 0 outside (the sketch on the first bend's inner skin), 1 inside,
    /// 2 middle, on the plane of normal n
    void profile(const std::vector<gp_Pnt>& p, const gp_Dir& n, int side);
    /// the material from a to a + the thickness along each line's left, n x
    /// its way
    void profileAt(const std::vector<gp_Pnt>& p, const gp_Dir& n, double a);
    /** A tube (docs/SheetMetal.md sec 12, ruled 2026-11-05): a closed chain
     * of lines, a wall on each and a bend at every corner, the last joining
     * the last wall to the first -- a loop, which does not lie flat until
     * ripped. side 0 outside the loop (the sketch on the bends' inner
     * skins), 1 inside, 2 middle.
     */
    void closedProfile(std::vector<gp_Pnt> p, const gp_Dir& n, int side);
    /// A bend of inner radius r about the axis through c along n, from the
    /// wall before (its left l) through the turn
    void bend(const gp_Pnt& c, const gp_Dir& l, const gp_Dir& n, double turn, int before,
              int after, double r);

private:
    /// the turn at each corner, signed about n, and each line's way
    std::vector<double> turns(const std::vector<gp_Pnt>& p, const gp_Dir& n,
                              std::vector<gp_Dir>& d) const;
};

}  // namespace PartDesign::SheetMetal
