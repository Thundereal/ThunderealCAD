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
        && a.translation.text == b.translation.text && a.rotationControl == b.rotationControl
        && a.translationControl == b.translationControl;
}

bool same(const ForceElement& a, const ForceElement& b)
{
    return a.id == b.id && a.source == b.source && a.kind == b.kind && a.frameI == b.frameI
        && a.frameJ == b.frameJ && a.k == b.k && a.rest == b.rest && a.preload == b.preload
        && a.c == b.c;
}

bool same(const Inertia& a, const Inertia& b)
{
    return a.body == b.body && a.mass == b.mass && same(a.com, b.com) && a.inertia == b.inertia
        && a.origin == b.origin;
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
    if (a.dynamics.has_value() != b.dynamics.has_value()
        || (a.dynamics
            && (!same(a.dynamics->gravity, b.dynamics->gravity)
                || !sameRecords(a.dynamics->inertias, b.dynamics->inertias)
                || !sameRecords(a.dynamics->forces, b.dynamics->forces)))) {
        return false;
    }
    return a.version == b.version && a.sources == b.sources && sameRecords(a.bodies, b.bodies)
        && sameRecords(a.frames, b.frames) && sameRecords(a.joints, b.joints)
        && sameRecords(a.grounds, b.grounds) && sameRecords(a.limits, b.limits)
        && sameRecords(a.drivers, b.drivers) && sameRecords(a.curves, b.curves);
}

Model Assembly::Solver::scaled(const Model& model, double length)
{
    Model out = model;
    auto scale = [length](Base::Placement& placement) {
        placement.setPosition(placement.getPosition() * length);
    };
    for (auto& body : out.bodies) {
        scale(body.placement);
        body.mass.com = body.mass.com * length;
    }
    for (auto& frame : out.frames) {
        scale(frame.local);
    }
    for (auto& joint : out.joints) {
        switch (joint.kind) {
            case JointKind::Planar:
            case JointKind::PointInPlane:
            case JointKind::LineInPlane:
            case JointKind::PointPointDistance:
            case JointKind::PointLineDistance:
            case JointKind::LineLineDistance:
            case JointKind::RackPinion:
            case JointKind::Screw:
            case JointKind::PointOnCurve:
                joint.value *= length;
                break;
            default:
                // Angles, and the gears' radii, whose ratio is all they say
                break;
        }
    }
    for (auto& driver : out.drivers) {
        // A distance, a speed or a force: as a length; a torque: as two
        driver.translation = scaledFormula(driver.translation, length);
        if (driver.rotationControl == MotorControl::Effort) {
            driver.rotation = scaledFormula(driver.rotation, length * length);
        }
    }
    for (auto& limit : out.limits) {
        if (limit.kind == LimitKind::TranslationZ) {
            if (limit.min) {
                *limit.min *= length;
            }
            if (limit.max) {
                *limit.max *= length;
            }
            limit.tolerance *= length;
        }
    }
    if (out.dynamics) {
        out.dynamics->gravity = out.dynamics->gravity * length;
        for (auto& inertia : out.dynamics->inertias) {
            inertia.com = inertia.com * length;
            for (auto& value : inertia.inertia) {
                value *= length * length;
            }
        }
        // A force scales as a length does (mass and time stay): a
        // stiffness or a damping, a force over a length, not at all
        for (auto& force : out.dynamics->forces) {
            force.rest *= length;
            force.preload *= length;
        }
    }
    return out;
}

Formula Assembly::Solver::scaledFormula(const Formula& formula, double factor)
{
    FormulaFunction function = formula.evaluator();
    if (factor == 1.0 || !function) {
        return formula;
    }
    Formula out = formula;
    out.function = [function = std::move(function), factor](double time) {
        FormulaValue value = function(time);
        value.value *= factor;
        value.rate *= factor;
        value.acceleration *= factor;
        return value;
    };
    return out;
}

Inertia Assembly::Solver::moved(const Inertia& inertia, const Base::Placement& toOther)
{
    Inertia out = inertia;
    toOther.multVec(inertia.com, out.com);
    // R I R^T, R's columns the record's axes in the other frame
    const auto& i = inertia.inertia;
    const double m[3][3] = {{i[0], i[3], i[4]}, {i[3], i[1], i[5]}, {i[4], i[5], i[2]}};
    double r[3][3];
    for (int c = 0; c < 3; ++c) {
        const Base::Vector3d axis = toOther.getRotation().multVec(
            Base::Vector3d(c == 0 ? 1.0 : 0.0, c == 1 ? 1.0 : 0.0, c == 2 ? 1.0 : 0.0));
        r[0][c] = axis.x;
        r[1][c] = axis.y;
        r[2][c] = axis.z;
    }
    double t[3][3] {};
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            for (int k = 0; k < 3; ++k) {
                for (int l = 0; l < 3; ++l) {
                    t[a][b] += r[a][k] * m[k][l] * r[b][l];
                }
            }
        }
    }
    out.inertia = {t[0][0], t[1][1], t[2][2], t[0][1], t[0][2], t[1][2]};
    return out;
}

void Assembly::Solver::add(Inertia& a, const Inertia& b)
{
    if (b.origin == Inertia::Origin::AssumedDensity
        || (b.origin == Inertia::Origin::Override && a.origin == Inertia::Origin::Shape)) {
        a.origin = b.origin;
    }
    if (b.mass <= 0.0) {
        return;
    }
    if (a.mass <= 0.0) {
        a.mass = b.mass;
        a.com = b.com;
        a.inertia = b.inertia;
        a.origin = b.origin;
        return;
    }
    const double total = a.mass + b.mass;
    const Base::Vector3d centre = (a.com * a.mass + b.com * b.mass) / total;
    auto shifted = [&](const Inertia& own) {
        const Base::Vector3d d = own.com - centre;
        const double m = own.mass;
        return std::array<double, 6> {own.inertia[0] + m * (d.y * d.y + d.z * d.z),
                                      own.inertia[1] + m * (d.x * d.x + d.z * d.z),
                                      own.inertia[2] + m * (d.x * d.x + d.y * d.y),
                                      own.inertia[3] - m * d.x * d.y,
                                      own.inertia[4] - m * d.x * d.z,
                                      own.inertia[5] - m * d.y * d.z};
    };
    const auto ia = shifted(a);
    const auto ib = shifted(b);
    for (std::size_t k = 0; k < a.inertia.size(); ++k) {
        a.inertia[k] = ia[k] + ib[k];
    }
    a.mass = total;
    a.com = centre;
}
