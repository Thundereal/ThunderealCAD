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

std::vector<gp_Pnt>
profilePoints(const TopoDS_Wire& wire, const std::string& what, std::vector<double>& arcs)
{
    // each edge's ends and ways out of them, in the wire's order
    struct Run
    {
        bool arc;
        gp_Pnt from, to;
        gp_Dir in, out;
        double radius = 0, sweep = 0;
    };
    std::vector<Run> runs;
    for (BRepTools_WireExplorer it(wire); it.More(); it.Next()) {
        const TopoDS_Edge& edge = it.Current();
        BRepAdaptor_Curve c(edge);
        const bool arc = c.GetType() == GeomAbs_Circle;
        if (!arc && c.GetType() != GeomAbs_Line) {
            FC_THROWM(Base::ValueError, what << " is made of lines and arcs only");
        }
        double u0 = c.FirstParameter(), u1 = c.LastParameter();
        if (edge.Orientation() == TopAbs_REVERSED) {
            std::swap(u0, u1);
        }
        gp_Pnt p0, p1;
        gp_Vec v0, v1;
        c.D1(u0, p0, v0);
        c.D1(u1, p1, v1);
        if (edge.Orientation() == TopAbs_REVERSED) {
            v0.Reverse();
            v1.Reverse();
        }
        Run run {arc, p0, p1, gp_Dir(v0), gp_Dir(v1)};
        if (arc) {
            run.radius = c.Circle().Radius();
            run.sweep = std::abs(u1 - u0);
        }
        runs.push_back(run);
    }
    if (runs.empty() || runs.front().arc || runs.back().arc) {
        FC_THROWM(Base::ValueError,
                  what << " starts or ends in an arc: an arc is rolled between two lines");
    }
    std::vector<gp_Pnt> points {runs.front().from};
    arcs.assign(1, 0);
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const Run& run = runs[i];
        if (!run.arc) {
            // a line's end, unless an arc follows: its corner is the arc's
            const bool arcNext = i + 1 < runs.size() && runs[i + 1].arc;
            if (!arcNext) {
                points.push_back(run.to);
                arcs.push_back(0);
            }
            continue;
        }
        const Run& before = runs[i - 1];
        const Run& after = runs[i + 1];
        if (after.arc) {
            FC_THROWM(Base::ValueError,
                      what << ": arcs " << i + 1 << " and " << i + 2
                           << " meet with no line between; a line between two arcs");
        }
        if (run.sweep >= std::numbers::pi - Precision::Angular()) {
            FC_THROWM(Base::ValueError,
                      what << ": arc " << i + 1 << " turns 180 deg or more; split it in two");
        }
        if (!before.out.IsParallel(run.in, Precision::Angular()) || before.out.Dot(run.in) < 0
            || !run.out.IsParallel(after.in, Precision::Angular()) || run.out.Dot(after.in) < 0) {
            FC_THROWM(Base::ValueError,
                      what << ": arc " << i + 1 << " is not tangent to the lines beside it");
        }
        // the lines' virtual sharp, as far along its way in from its start
        // as R tan(sweep / 2)
        const double back = run.radius * std::tan(run.sweep / 2);
        points.push_back(run.from.Translated(gp_Vec(run.in) * back));
        arcs.push_back(run.radius);
    }
    return points;
}

void dropStraight(std::vector<gp_Pnt>& points, std::vector<double>* arcs)
{
    for (std::size_t i = 1; i + 1 < points.size();) {
        const gp_Vec a(points[i - 1], points[i]), b(points[i], points[i + 1]);
        if (a.Magnitude() > Precision::Confusion() && b.Magnitude() > Precision::Confusion()
            && a.IsParallel(b, Precision::Angular()) && a.Dot(b) > 0) {
            points.erase(points.begin() + static_cast<long>(i));
            if (arcs && i < arcs->size()) {
                arcs->erase(arcs->begin() + static_cast<long>(i));
            }
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
    // each corner's inner radius: the radius, or an arc's as drawn on the
    // sketch's line, from a to b along the left: the axis b + r to the left
    // of the line on a left turn, a - r on a right one
    std::vector<double> r(count, radius);
    for (std::size_t j = 1; j < count; ++j) {
        if (j < arcs.size() && arcs[j] > 0) {
            r[j] = turn[j] > 0 ? arcs[j] - b : arcs[j] + a;
            if (r[j] <= Precision::Confusion()) {
                FC_THROWM(Base::ValueError,
                          "The profile's arc " << arcs[j]
                                               << " in radius is too tight for the thickness "
                                                  "on its side of the sketch");
            }
        }
    }
    // where each line's straight part starts and ends, and the bends' axes
    std::vector<gp_Pnt> start(p.begin(), p.end() - 1), end(p.begin() + 1, p.end());
    std::vector<gp_Pnt> center(count);
    for (std::size_t j = 1; j < count; ++j) {
        const double offset = turn[j] > 0 ? b + r[j] : a - r[j];
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
                                    ownName(name, wallsBefore, bendsBefore)));
        if (i > 0) {
            bend(center[i], l[i - 1], n, turn[i], previous, wall, r[i]);
        }
        previous = wall;
    }
}

void ProfileBuilder::closedProfile(std::vector<gp_Pnt> p, const gp_Dir& n, int side)
{
    if (p.size() > 1 && p.front().Distance(p.back()) < Precision::Confusion()) {
        p.pop_back();
    }
    // no corner that goes on straight, the first one included
    for (bool dropped = true; dropped && p.size() > 3;) {
        dropped = false;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const gp_Vec a(p[(i + p.size() - 1) % p.size()], p[i]);
            const gp_Vec b(p[i], p[(i + 1) % p.size()]);
            if (a.Magnitude() > Precision::Confusion() && b.Magnitude() > Precision::Confusion()
                && a.IsParallel(b, Precision::Angular()) && a.Dot(b) > 0) {
                p.erase(p.begin() + static_cast<long>(i));
                dropped = true;
                break;
            }
        }
    }
    const std::size_t count = p.size();
    if (count < 3) {
        THROWM(Base::ValueError, "A tube's profile has three lines at least");
    }
    std::vector<gp_Dir> d;
    for (std::size_t i = 0; i < count; ++i) {
        const gp_Vec v(p[i], p[(i + 1) % count]);
        if (v.Magnitude() < Precision::Confusion()) {
            THROWM(Base::ValueError, "A profile wall has a line of no length");
        }
        d.emplace_back(v);
    }
    auto before = [count](std::size_t j) {
        return (j + count - 1) % count;
    };
    // the turn at each corner, from the line before it
    std::vector<double> turn(count);
    double total = 0;
    for (std::size_t j = 0; j < count; ++j) {
        const gp_Dir& from = d[before(j)];
        turn[j] = std::atan2(from.Crossed(d[j]).Dot(n), from.Dot(d[j]));
        if (std::abs(turn[j]) > std::numbers::pi - 1e-6) {
            THROWM(Base::ValueError, "A profile wall cannot turn back on itself");
        }
        total += turn[j];
    }
    if (std::abs(std::abs(total) - 2 * std::numbers::pi) > 1e-6) {
        THROWM(Base::ValueError, "A tube's profile goes round more than once");
    }
    // the material from a along each line's left: inside the loop where it
    // turns left
    const bool inLeft = total > 0;
    double a = -thickness / 2;
    if (side == 0) {
        a = inLeft ? -thickness : 0;
    }
    else if (side == 1) {
        a = inLeft ? 0 : -thickness;
    }
    const double b = a + thickness;
    std::vector<gp_Dir> l;
    for (const gp_Dir& way : d) {
        l.push_back(n.Crossed(way));
    }
    // where each line's straight part starts and ends, and the bends' axes,
    // as profileAt() finds them, every corner bent
    std::vector<gp_Pnt> start(count), end(count), center(count);
    for (std::size_t j = 0; j < count; ++j) {
        const std::size_t i = before(j);
        const double offset = turn[j] > 0 ? b + radius : a - radius;
        const gp_Vec miter = (gp_Vec(l[i]) + gp_Vec(l[j])) * (offset / (1 + d[i].Dot(d[j])));
        center[j] = p[j].Translated(miter);
        end[i] = p[j].Translated(gp_Vec(d[i]) * miter.Dot(gp_Vec(d[i])));
        start[j] = p[j].Translated(gp_Vec(d[j]) * miter.Dot(gp_Vec(d[j])));
    }
    std::vector<int> walls(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (gp_Vec(start[i], end[i]).Dot(gp_Vec(d[i])) < Precision::Confusion()) {
            FC_THROWM(Base::ValueError,
                      "Line " << i + 1 << " of the profile is too short for its bends");
        }
        const gp_Pnt origin = start[i].Translated(gp_Vec(l[i]) * a);
        const std::string name = nextWall();
        walls[i] = model.addWall(name, gp_Ax3(origin, l[i], d[i]));
        added.push_back(walls[i]);
        faces.push_back(wallSection(origin, d[i], start[i].Distance(end[i]), l[i], thickness,
                                    ownName(name, wallsBefore, bendsBefore)));
    }
    for (std::size_t j = 0; j < count; ++j) {
        bend(center[j], l[before(j)], n, turn[j], walls[before(j)], walls[j], radius);
    }
}

void ProfileBuilder::bend(const gp_Pnt& c, const gp_Dir& l, const gp_Dir& n, double turn,
                          int before, int after, double r)
{
    const bool left = turn > 0;
    CylBend bend;
    bend.frame = gp_Ax3(c, left ? n : n.Reversed(), left ? l.Reversed() : l);
    bend.radius = r;
    bend.thickness = thickness;
    bend.angle = std::abs(turn);
    bend.k = k(bend.angle, r);
    const std::string name = nextBend();
    const int index = model.addBend(name, bend, before, after);
    added.push_back(index);
    if (firstBend < 0) {
        firstBend = index;
    }
    faces.push_back(bendSection(bend, ownName(name, wallsBefore, bendsBefore)));
}

}  // namespace PartDesign::SheetMetal
