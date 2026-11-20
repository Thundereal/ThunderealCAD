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

#include "Advisor.h"
#include "GeometricTolerance.h"
#include "TolerancedFeature.h"

using namespace PMI;

namespace
{
using GT = GeometricTolerance;

enum Shape
{
    Other,
    PlaneFace,
    Width,
    Axial,
    Spherical,
    Conical,
};

Shape shapeOf(const TolerancedFeature* f)
{
    if (!f) {
        return Other;
    }
    const auto& faces = f->facePrimitives();
    if (f->getKind() == TolerancedFeature::Axis) {
        return Axial;
    }
    if (f->getKind() == TolerancedFeature::CentrePlane) {
        return Width;
    }
    if (faces.empty()) {
        return Other;
    }
    // A width, a pattern of them, a slot; else what stands for it: a
    // counterbored hole's hole
    double width = 0;
    if (f->IsSize.getValue() && Geom::width(faces, width)) {
        return Width;
    }
    switch (f->primitive().type) {
        case Primitive::Plane:
            return PlaneFace;
        case Primitive::Cylinder:
            return Axial;
        case Primitive::Sphere:
            return Spherical;
        case Primitive::Cone:
            return Conical;
        default:
            return Other;
    }
}
}  // namespace

std::vector<int> Advisor::characteristics(const TolerancedFeature* f)
{
    switch (shapeOf(f)) {
        case PlaneFace:
            return {GT::Flatness, GT::Parallelism, GT::Perpendicularity, GT::Angularity,
                    GT::ProfileOfSurface, GT::Position, GT::TotalRunout, GT::CircularRunout,
                    GT::Straightness, GT::ProfileOfLine};
        case Width:
            return {GT::Position, GT::Symmetry, GT::Parallelism, GT::Perpendicularity,
                    GT::Angularity, GT::Flatness, GT::ProfileOfSurface};
        case Axial:
            return {GT::Position, GT::Perpendicularity, GT::Cylindricity, GT::Circularity,
                    GT::CircularRunout, GT::TotalRunout, GT::Coaxiality, GT::Concentricity,
                    GT::Parallelism, GT::Angularity, GT::Straightness, GT::ProfileOfSurface};
        case Spherical:
            return {GT::Position, GT::ProfileOfSurface, GT::Circularity};
        case Conical:
            return {GT::ProfileOfSurface, GT::CircularRunout, GT::Circularity, GT::Angularity,
                    GT::ProfileOfLine};
        default:
            return {GT::ProfileOfSurface, GT::ProfileOfLine};
    }
}

std::vector<int> Advisor::zoneShapes(const TolerancedFeature* f, int c)
{
    std::vector<int> res {GT::NoZoneShape};
    Shape s = shapeOf(f);
    bool axial = s == Axial;
    if ((c == GT::Position && (axial || s == Spherical))
        || (axial && (c == GT::Coaxiality || c == GT::Concentricity || c == GT::Straightness
                      || GT::category(static_cast<GT::CharacteristicValue>(c))
                          == GT::Orientation))) {
        res.insert(res.begin(), GT::DiameterZone);
    }
    if (c == GT::Position && s == Spherical) {
        res.insert(res.begin(), GT::SphericalDiameterZone);
    }
    return res;
}

bool Advisor::materialCondition(const TolerancedFeature* f, int c)
{
    if (!f || !f->IsSize.getValue()) {
        return false;
    }
    auto cat = GT::category(static_cast<GT::CharacteristicValue>(c));
    return cat != GT::Runout && cat != GT::Profile && c != GT::Circularity
        && c != GT::Cylindricity;
}

int Advisor::datums(int c)
{
    auto cat = GT::category(static_cast<GT::CharacteristicValue>(c));
    if (cat == GT::Form) {
        return -1;
    }
    if (cat == GT::Orientation || cat == GT::Runout || c == GT::Coaxiality
        || c == GT::Concentricity || c == GT::Symmetry) {
        return 1;
    }
    return 0;
}
