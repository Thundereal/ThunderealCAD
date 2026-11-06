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

#include <algorithm>
#include <cmath>
#include <numbers>

#include <gp_Ax1.hxx>
#include <gp_Vec.hxx>

#include "Fold.h"

using namespace PartDesign::SheetMetal;

double CylBend::from() const
{
    return angle / 2 - std::numbers::pi;
}

void CylBend::polar(const gp_Pnt& point, double from, double& a, double& r, double& z) const
{
    const gp_Vec v(frame.Location(), point);
    const double x = v.Dot(gp_Vec(frame.XDirection()));
    const double y = v.Dot(gp_Vec(frame.YDirection()));
    z = v.Dot(gp_Vec(frame.Direction()));
    r = std::hypot(x, y);
    a = std::atan2(y, x);
    while (a < from) {
        a += 2 * std::numbers::pi;
    }
    while (a >= from + 2 * std::numbers::pi) {
        a -= 2 * std::numbers::pi;
    }
}

gp_Pnt CylBend::toFlat(const gp_Pnt& folded) const
{
    double a, r, z;
    polar(folded, from(), a, r, z);
    return frame.Location().Translated(gp_Vec(frame.XDirection()) * r
                                       + gp_Vec(frame.YDirection()) * (a * neutral())
                                       + gp_Vec(frame.Direction()) * z);
}

gp_Pnt CylBend::toFolded(const gp_Pnt& flat) const
{
    const gp_Vec v(frame.Location(), flat);
    const double r = v.Dot(gp_Vec(frame.XDirection()));
    const double a = v.Dot(gp_Vec(frame.YDirection())) / neutral();
    const double z = v.Dot(gp_Vec(frame.Direction()));
    return frame.Location().Translated(gp_Vec(frame.XDirection()) * (r * std::cos(a))
                                       + gp_Vec(frame.YDirection()) * (r * std::sin(a))
                                       + gp_Vec(frame.Direction()) * z);
}

gp_Trsf CylBend::unbend() const
{
    gp_Trsf back;
    back.SetRotation(gp_Ax1(frame.Location(), frame.Direction()), -angle);
    gp_Trsf along;
    along.SetTranslation(gp_Vec(frame.YDirection()) * (angle * neutral()));
    return along * back;
}

gp_Trsf CylBend::refold() const
{
    return unbend().Inverted();
}

int CylBend::side(const gp_Pnt& point) const
{
    double a, r, z;
    polar(point, -std::numbers::pi / 2, a, r, z);
    if (a < 0) {
        return -1;
    }
    return a > angle ? 1 : 0;
}

int CylBend::sideAfter(const gp_Pnt& point) const
{
    double a, r, z;
    polar(point, angle + std::numbers::pi / 2 - 2 * std::numbers::pi, a, r, z);
    if (a < 0) {
        return -1;
    }
    return a > angle ? 1 : 0;
}

bool CylBend::contains(const gp_Pnt& point, double tol) const
{
    double a, r, z;
    polar(point, from(), a, r, z);
    return a >= 0 && a <= angle && r >= radius - tol && r <= radius + thickness + tol;
}

double ConeBend::neutral() const
{
    return radius + k * thickness * std::cos(semiAngle);
}

gp_Ax3 ConeBend::startFrame() const
{
    const double c = std::cos(semiAngle), s = std::sin(semiAngle);
    const gp_Vec x(frame.XDirection()), z(frame.Direction());
    return {frame.Location().Translated(x * radius), gp_Dir(x * c - z * s), gp_Dir(x * s + z * c)};
}

void ConeBend::local(const gp_Pnt& point, double& a, double& t, double& d) const
{
    const gp_Vec v(frame.Location(), point);
    const double x = v.Dot(gp_Vec(frame.XDirection()));
    const double y = v.Dot(gp_Vec(frame.YDirection()));
    const double z = v.Dot(gp_Vec(frame.Direction()));
    const double rho = std::hypot(x, y);
    const double from = angle / 2 - std::numbers::pi;
    a = std::atan2(y, x);
    while (a < from) {
        a += 2 * std::numbers::pi;
    }
    while (a >= from + 2 * std::numbers::pi) {
        a -= 2 * std::numbers::pi;
    }
    const double c = std::cos(semiAngle), s = std::sin(semiAngle);
    d = (rho - radius) * c - z * s;
    t = (rho - radius) * s + z * c;
}

namespace
{

/// Below this a cone's half angle is a cylinder's: its apex too far off to
/// unroll about
constexpr double flatCone = 1e-9;

}  // namespace

gp_Pnt ConeBend::toFlat(const gp_Pnt& folded) const
{
    double a, t, d;
    local(folded, a, t, d);
    const double s = std::sin(semiAngle);
    const double rn = neutral();
    double u, w;
    if (s < flatCone) {
        u = t;
        w = a * rn;
    }
    else {
        // about the neutral cone's apex, unrolled to -rn / s along the start line
        const double l = (rn + t * s) / s, psi = a * s;
        u = -rn / s + l * std::cos(psi);
        w = l * std::sin(psi);
    }
    const gp_Ax3 flat = startFrame();
    return flat.Location().Translated(gp_Vec(flat.XDirection()) * u
                                      + gp_Vec(frame.YDirection()) * w
                                      + gp_Vec(flat.Direction()) * d);
}

gp_Pnt ConeBend::toFolded(const gp_Pnt& flat) const
{
    const gp_Ax3 start = startFrame();
    const gp_Vec v(start.Location(), flat);
    const double u = v.Dot(gp_Vec(start.XDirection()));
    const double w = v.Dot(gp_Vec(frame.YDirection()));
    const double d = v.Dot(gp_Vec(start.Direction()));
    const double c = std::cos(semiAngle), s = std::sin(semiAngle);
    const double rn = neutral();
    double a, t;
    if (s < flatCone) {
        t = u;
        a = w / rn;
    }
    else {
        const double p = u + rn / s;
        a = std::atan2(w, p) / s;
        t = std::hypot(p, w) - rn / s;
    }
    const double rho = radius + t * s + d * c, z = t * c - d * s;
    return frame.Location().Translated(gp_Vec(frame.XDirection()) * (rho * std::cos(a))
                                       + gp_Vec(frame.YDirection()) * (rho * std::sin(a))
                                       + gp_Vec(frame.Direction()) * z);
}

bool ConeBend::contains(const gp_Pnt& point, double tol) const
{
    double a, t, d;
    local(point, a, t, d);
    // a face where the sheet starts or ends, a seam's, is the cone's: its
    // point's angle off by the rounding
    const gp_Vec v(frame.Location(), point);
    const double rho = v.Crossed(gp_Vec(frame.Direction())).Magnitude();
    const double slack = tol / std::max(rho, tol);
    return a >= -slack && a <= angle + slack && d >= -tol && d <= thickness + tol;
}
