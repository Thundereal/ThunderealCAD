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

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS.hxx>

#include "Drawing.h"
#include "Font.h"
#include "GeometricTolerance.h"

using namespace PMI;

namespace
{
/// Segments of a full circle
constexpr int CircleSteps = 32;

gp_Pnt p3(double x, double y)
{
    return gp_Pnt(x, y, 0.0);
}
}  // namespace

Drawing::Drawing()
{
    BRep_Builder().MakeCompound(_comp);
    BRep_Builder().MakeCompound(_rest);
}

void Drawing::beginUpright()
{
    _uprights.emplace_back();
    BRep_Builder().MakeCompound(_uprights.back().shape);
    _open = true;
    _openEmpty = true;
}

void Drawing::endUpright()
{
    if (!_open) {
        return;
    }
    _open = false;
    if (_openEmpty) {
        _uprights.pop_back();
        return;
    }
    _uprights.back().x = (_ox0 + _ox1) / 2;
    _uprights.back().y = (_oy0 + _oy1) / 2;
}

void Drawing::uprightAbout(double y)
{
    for (auto& u : _uprights) {
        u.y = y;
    }
}

void Drawing::uprightAll()
{
    if (!_uprights.empty() || _empty) {
        return;
    }
    _uprights.push_back({_rest, (xmin + xmax) / 2, (ymin + ymax) / 2});
    BRep_Builder().MakeCompound(_rest);
}

void Drawing::grow(double x, double y)
{
    if (_open) {
        if (_openEmpty) {
            _ox0 = _ox1 = x;
            _oy0 = _oy1 = y;
            _openEmpty = false;
        }
        else {
            _ox0 = std::min(_ox0, x);
            _ox1 = std::max(_ox1, x);
            _oy0 = std::min(_oy0, y);
            _oy1 = std::max(_oy1, y);
        }
    }
    if (_empty) {
        xmin = xmax = x;
        ymin = ymax = y;
        _empty = false;
        return;
    }
    xmin = std::min(xmin, x);
    xmax = std::max(xmax, x);
    ymin = std::min(ymin, y);
    ymax = std::max(ymax, y);
}

void Drawing::add(const TopoDS_Shape& s)
{
    if (!s.IsNull()) {
        BRep_Builder().Add(_comp, s);
        BRep_Builder().Add(_open ? _uprights.back().shape : _rest, s);
    }
}

void Drawing::line(double x1, double y1, double x2, double y2)
{
    if (std::hypot(x2 - x1, y2 - y1) < 1e-9) {
        return;
    }
    add(BRepBuilderAPI_MakeEdge(p3(x1, y1), p3(x2, y2)).Shape());
    grow(x1, y1);
    grow(x2, y2);
}

void Drawing::polyline(const std::vector<gp_Pnt2d>& points, bool closed)
{
    for (std::size_t i = 1; i < points.size(); ++i) {
        line(points[i - 1].X(), points[i - 1].Y(), points[i].X(), points[i].Y());
    }
    if (closed && points.size() > 2) {
        line(points.back().X(), points.back().Y(), points.front().X(), points.front().Y());
    }
}

void Drawing::rect(double x, double y, double w, double h)
{
    polyline({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, true);
}

void Drawing::arc(double cx, double cy, double r, double a0, double a1)
{
    int steps = std::max(4, int(std::ceil(std::fabs(a1 - a0) / (2 * M_PI) * CircleSteps)));
    std::vector<gp_Pnt2d> pts;
    for (int i = 0; i <= steps; ++i) {
        double a = a0 + (a1 - a0) * i / steps;
        pts.emplace_back(cx + r * std::cos(a), cy + r * std::sin(a));
    }
    polyline(pts);
}

void Drawing::circle(double cx, double cy, double r)
{
    arc(cx, cy, r, 0.0, 2 * M_PI);
}

void Drawing::fill(const std::vector<gp_Pnt2d>& polygon)
{
    if (polygon.size() < 3) {
        return;
    }
    BRepBuilderAPI_MakePolygon poly;
    for (auto& p : polygon) {
        poly.Add(p3(p.X(), p.Y()));
        grow(p.X(), p.Y());
    }
    poly.Close();
    if (!poly.IsDone()) {
        return;
    }
    BRepBuilderAPI_MakeFace face(poly.Wire(), true);
    if (face.IsDone()) {
        add(face.Shape());
    }
}

void Drawing::arrow(double x, double y, double dx, double dy, double length)
{
    double n = std::hypot(dx, dy);
    if (n < 1e-12) {
        return;
    }
    dx /= n;
    dy /= n;
    // ISO 129-1: a filled 15 deg arrowhead
    double w = length * std::tan(15.0 * M_PI / 180.0);
    double bx = x - dx * length, by = y - dy * length;
    fill({{x, y}, {bx - dy * w, by + dx * w}, {bx + dy * w, by - dx * w}});
}

double Drawing::textWidth(const std::u32string& text, double h) const
{
    return Font::instance().width(text, h);
}

double Drawing::text(const std::u32string& text, double h, double x, double y)
{
    double w = textWidth(text, h);
    add(Font::instance().text(text, h, x, y));
    grow(x, y);
    grow(x + w, y + h);
    return w;
}

double Drawing::circled(char32_t letter, double h, double x, double cy)
{
    // A circle of 1.4 h round a letter of 0.8 h (ISO 1101 modifier circles)
    double r = 0.7 * h;
    double lh = 0.8 * h;
    double cx = x + r;
    circle(cx, cy, r);
    std::u32string s(1, letter);
    double w = textWidth(s, lh);
    text(s, lh, cx - w / 2, cy - lh / 2);
    return 2 * r;
}

void Drawing::characteristic(int which, double x, double y, double s)
{
    using GT = GeometricTolerance;
    // ISO 7083 proportions, roughly: the symbol takes 0.6 of the cell
    const double u = 0.6 * s;
    const double cx = x + s / 2, cy = y + s / 2;
    auto L = [&](double x1, double y1, double x2, double y2) {
        line(cx + x1 * u, cy + y1 * u, cx + x2 * u, cy + y2 * u);
    };
    switch (which) {
        case GT::Straightness:
            L(-0.5, 0, 0.5, 0);
            break;
        case GT::Flatness:
            polyline({{cx - 0.5 * u, cy - 0.25 * u},
                      {cx + 0.3 * u, cy - 0.25 * u},
                      {cx + 0.5 * u, cy + 0.25 * u},
                      {cx - 0.3 * u, cy + 0.25 * u}},
                     true);
            break;
        case GT::Circularity:
            circle(cx, cy, 0.45 * u);
            break;
        case GT::Cylindricity:
            circle(cx, cy, 0.32 * u);
            L(-0.55, -0.45, -0.15, 0.45);
            L(0.15, -0.45, 0.55, 0.45);
            break;
        case GT::ProfileOfLine:
            arc(cx, cy - 0.25 * u, 0.5 * u, 0, M_PI);
            break;
        case GT::ProfileOfSurface:
            arc(cx, cy - 0.25 * u, 0.5 * u, 0, M_PI);
            L(-0.5, -0.25, 0.5, -0.25);
            break;
        case GT::Angularity:
            L(-0.5, -0.4, 0.5, -0.4);
            L(-0.5, -0.4, 0.45, 0.4);
            break;
        case GT::Perpendicularity:
            L(-0.5, -0.4, 0.5, -0.4);
            L(0, -0.4, 0, 0.5);
            break;
        case GT::Parallelism:
            L(-0.45, -0.45, -0.1, 0.45);
            L(0.1, -0.45, 0.45, 0.45);
            break;
        case GT::Position:
            circle(cx, cy, 0.32 * u);
            L(-0.5, 0, 0.5, 0);
            L(0, -0.5, 0, 0.5);
            break;
        case GT::Concentricity:
        case GT::Coaxiality:
            circle(cx, cy, 0.45 * u);
            circle(cx, cy, 0.22 * u);
            break;
        case GT::Symmetry:
            L(-0.5, 0, 0.5, 0);
            L(-0.3, 0.3, 0.3, 0.3);
            L(-0.3, -0.3, 0.3, -0.3);
            break;
        case GT::CircularRunout:
            L(-0.4, -0.4, 0.35, 0.35);
            arrow(cx + 0.45 * u, cy + 0.45 * u, 1, 1, 0.35 * u);
            break;
        case GT::TotalRunout:
            L(-0.5, -0.45, 0.2, -0.45);
            L(-0.5, -0.45, -0.05, 0.3);
            L(-0.05, -0.45, 0.4, 0.3);
            arrow(cx + 0.0 * u, cy + 0.42 * u, 1, 1.6, 0.3 * u);
            arrow(cx + 0.45 * u, cy + 0.42 * u, 1, 1.6, 0.3 * u);
            break;
        default:
            break;
    }
    grow(x, y);
    grow(x + s, y + s);
}
