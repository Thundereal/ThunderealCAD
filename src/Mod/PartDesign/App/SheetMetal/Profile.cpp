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

#include "PreCompiled.h"

#include <cmath>
#include <numbers>

#include <BRepAdaptor_Curve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax3.hxx>
#include <gp_Vec.hxx>

#include <Base/Exception.h>

#include "Pieces.h"
#include "Profile.h"

namespace PartDesign::SheetMetal
{

std::vector<gp_Pnt> linePoints(const TopoDS_Wire& wire, const std::string& what)
{
    std::vector<gp_Pnt> points;
    for (BRepTools_WireExplorer it(wire); it.More(); it.Next()) {
        const TopoDS_Edge& edge = it.Current();
        if (BRepAdaptor_Curve(edge).GetType() != GeomAbs_Line) {
            FC_THROWM(Base::ValueError, what << " is made of lines only");
        }
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(edge, v1, v2, true);
        if (points.empty()) {
            points.push_back(BRep_Tool::Pnt(v1));
        }
        points.push_back(BRep_Tool::Pnt(v2));
    }
    return points;
}

void dropStraight(std::vector<gp_Pnt>& points)
{
    for (std::size_t i = 1; i + 1 < points.size();) {
        const gp_Vec a(points[i - 1], points[i]), b(points[i], points[i + 1]);
        if (a.Magnitude() > Precision::Confusion() && b.Magnitude() > Precision::Confusion()
            && a.IsParallel(b, Precision::Angular()) && a.Dot(b) > 0) {
            points.erase(points.begin() + static_cast<long>(i));
        }
        else {
            ++i;
        }
    }
}

std::vector<double>
ProfileBuilder::turns(const std::vector<gp_Pnt>& p, const gp_Dir& n, std::vector<gp_Dir>& d) const
{
    const std::size_t count = p.size() - 1;
    d.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const gp_Vec v(p[i], p[i + 1]);
        if (v.Magnitude() < Precision::Confusion()) {
            THROWM(Base::ValueError, "A profile wall has a line of no length");
        }
        d.emplace_back(v);
    }
    std::vector<double> turn(count, 0);
    for (std::size_t j = 1; j < count; ++j) {
        turn[j] = std::atan2(d[j - 1].Crossed(d[j]).Dot(n), d[j - 1].Dot(d[j]));
        if (std::abs(turn[j]) > std::numbers::pi - 1e-6) {
            THROWM(Base::ValueError, "A profile wall cannot turn back on itself");
        }
    }
    return turn;
}

void ProfileBuilder::profile(const std::vector<gp_Pnt>& p, const gp_Dir& n, int side)
{
    std::vector<gp_Dir> d;
    const std::vector<double> turn = turns(p, n, d);
    // the material from a along each line's left
    const bool left = turn.size() < 2 || turn[1] > 0;
    double a = -thickness / 2;
    if (side == 0) {
        a = left ? -thickness : 0;
    }
    else if (side == 1) {
        a = left ? 0 : -thickness;
    }
    profileAt(p, n, a);
}

void ProfileBuilder::profileAt(const std::vector<gp_Pnt>& p, const gp_Dir& n, double a)
{
    std::vector<gp_Dir> d;
    const std::vector<double> turn = turns(p, n, d);
    const std::size_t count = d.size();
    std::vector<gp_Dir> l;
    for (const gp_Dir& way : d) {
        l.push_back(n.Crossed(way));
    }
    const double b = a + thickness;
    // where each line's straight part starts and ends, and the bends' axes
    std::vector<gp_Pnt> start(p.begin(), p.end() - 1), end(p.begin() + 1, p.end());
    std::vector<gp_Pnt> center(count);
    for (std::size_t j = 1; j < count; ++j) {
        const double offset = turn[j] > 0 ? b + radius : a - radius;
        const gp_Vec miter =
            (gp_Vec(l[j - 1]) + gp_Vec(l[j])) * (offset / (1 + d[j - 1].Dot(d[j])));
        center[j] = p[j].Translated(miter);
        end[j - 1] = p[j].Translated(gp_Vec(d[j - 1]) * miter.Dot(gp_Vec(d[j - 1])));
        start[j] = p[j].Translated(gp_Vec(d[j]) * miter.Dot(gp_Vec(d[j])));
    }
    int previous = -1;
    for (std::size_t i = 0; i < count; ++i) {
        if (i == 0 && first >= 0) {
            // the wall there already
            firstEnd = end[0];
            previous = first;
            continue;
        }
        if (gp_Vec(start[i], end[i]).Dot(gp_Vec(d[i])) < Precision::Confusion()) {
            FC_THROWM(Base::ValueError,
                      "Line " << (first >= 0 ? i : i + 1)
                              << " of the profile is too short for its bends");
        }
        const gp_Pnt origin = start[i].Translated(gp_Vec(l[i]) * a);
        const std::string name = nextWall();
        const int wall = model.addWall(name, gp_Ax3(origin, l[i], d[i]));
        added.push_back(wall);
        faces.push_back(wallSection(origin, d[i], start[i].Distance(end[i]), l[i], thickness,
                                    name));
        if (i > 0) {
            bend(center[i], l[i - 1], n, turn[i], previous, wall);
        }
        previous = wall;
    }
}

void ProfileBuilder::bend(const gp_Pnt& c, const gp_Dir& l, const gp_Dir& n, double turn,
                          int before, int after)
{
    const bool left = turn > 0;
    CylBend bend;
    bend.frame = gp_Ax3(c, left ? n : n.Reversed(), left ? l.Reversed() : l);
    bend.radius = radius;
    bend.thickness = thickness;
    bend.angle = std::abs(turn);
    bend.k = k(bend.angle, radius);
    const std::string name = nextBend();
    const int index = model.addBend(name, bend, before, after);
    added.push_back(index);
    if (firstBend < 0) {
        firstBend = index;
    }
    faces.push_back(bendSection(bend, name));
}

}  // namespace PartDesign::SheetMetal
