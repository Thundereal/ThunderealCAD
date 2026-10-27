// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include "Model.h"

using namespace Assembly::Solver;

namespace
{

// Exact comparisons: Base's operator== on vectors and rotations has a tolerance.
bool same(const Base::Vector3d& a, const Base::Vector3d& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same(const Base::Rotation& a, const Base::Rotation& b)
{
    double a0 {}, a1 {}, a2 {}, a3 {};
    double b0 {}, b1 {}, b2 {}, b3 {};
    a.getValue(a0, a1, a2, a3);
    b.getValue(b0, b1, b2, b3);
    return a0 == b0 && a1 == b1 && a2 == b2 && a3 == b3;
}

bool same(const Base::Placement& a, const Base::Placement& b)
{
    return same(a.getPosition(), b.getPosition()) && same(a.getRotation(), b.getRotation());
}

bool same(const Body& a, const Body& b)
{
    return a.id == b.id && a.source == b.source && same(a.placement, b.placement)
        && a.grounded == b.grounded && a.mass.mass == b.mass.mass && same(a.mass.com, b.mass.com)
        && a.mass.principal == b.mass.principal
        && same(a.mass.principalAxes, b.mass.principalAxes) && a.mass.sized == b.mass.sized;
}

bool same(const Frame& a, const Frame& b)
{
    return a.id == b.id && a.body == b.body && same(a.local, b.local) && a.label == b.label;
}

bool same(const Joint& a, const Joint& b)
{
    return a.id == b.id && a.source == b.source && a.kind == b.kind && a.frameI == b.frameI
        && a.frameJ == b.frameJ && a.frameK == b.frameK && a.value == b.value
        && a.value2 == b.value2 && a.curve == b.curve;
}

bool same(const Curve& a, const Curve& b)
{
    if (a.id != b.id || a.source != b.source) {
        return false;
    }
    if (!a.geometry || !b.geometry) {
        return !a.geometry && !b.geometry;
    }
    return a.geometry == b.geometry || a.geometry->same(*b.geometry);
}

bool same(const Ground& a, const Ground& b)
{
    return a.id == b.id && a.source == b.source && a.frameI == b.frameI && a.frameJ == b.frameJ;
}

bool same(const Limit& a, const Limit& b)
{
    return a.id == b.id && a.source == b.source && a.frameI == b.frameI && a.frameJ == b.frameJ
        && a.kind == b.kind && a.min == b.min && a.max == b.max && a.tolerance == b.tolerance;
}

bool same(const Driver& a, const Driver& b)
{
    return a.id == b.id && a.source == b.source && a.frameI == b.frameI && a.frameJ == b.frameJ
        && a.kind == b.kind && a.rotation.text == b.rotation.text
        && a.translation.text == b.translation.text;
}

template<typename T>
bool sameRecords(const std::vector<T>& a, const std::vector<T>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool Assembly::Solver::sameExceptDragWeights(const Model& a, const Model& b)
{
    return a.version == b.version && a.sources == b.sources && sameRecords(a.bodies, b.bodies)
        && sameRecords(a.frames, b.frames) && sameRecords(a.joints, b.joints)
        && sameRecords(a.grounds, b.grounds) && sameRecords(a.limits, b.limits)
        && sameRecords(a.drivers, b.drivers) && sameRecords(a.curves, b.curves);
}
