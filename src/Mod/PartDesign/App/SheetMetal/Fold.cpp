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

#include <gp_Ax1.hxx>
#include <gp_Vec.hxx>

#include "Fold.h"

using namespace PartDesign::SheetMetal;

void CylBend::polar(const gp_Pnt& point, double& a, double& r, double& z) const
{
    const gp_Vec v(frame.Location(), point);
    const double x = v.Dot(gp_Vec(frame.XDirection()));
    const double y = v.Dot(gp_Vec(frame.YDirection()));
    z = v.Dot(gp_Vec(frame.Direction()));
    r = std::hypot(x, y);
    a = std::atan2(y, x);
    if (a < -std::numbers::pi / 2) {
        a += 2 * std::numbers::pi;
    }
}

gp_Pnt CylBend::toFlat(const gp_Pnt& folded) const
{
    double a, r, z;
    polar(folded, a, r, z);
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
    polar(point, a, r, z);
    if (a < 0) {
        return -1;
    }
    return a > angle ? 1 : 0;
}
