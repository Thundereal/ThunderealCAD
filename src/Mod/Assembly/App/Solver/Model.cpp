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

#include <algorithm>
#include <cmath>

#include "Model.h"

using namespace Assembly::Solver;

namespace
{

// Exact comparisons: Base's operator== on vectors and rotations has a tolerance.
// sameMechanism()'s tolerance while it compares; 0, bit for bit
double slack = 0.0;

bool same(const Base::Vector3d& a, const Base::Vector3d& b)
{
    if (slack > 0.0) {
        return (a - b).Length() <= slack * std::max(1.0, a.Length());
    }
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same(const Base::Rotation& a, const Base::Rotation& b)
{
    double a0 {}, a1 {}, a2 {}, a3 {};
    double b0 {}, b1 {}, b2 {}, b3 {};
    a.getValue(a0, a1, a2, a3);
    b.getValue(b0, b1, b2, b3);
    if (slack > 0.0) {
        // q and -q are one rotation
        const double minus =
            std::max({std::abs(a0 - b0), std::abs(a1 - b1), std::abs(a2 - b2), std::abs(a3 - b3)});
        const double plus =
            std::max({std::abs(a0 + b0), std::abs(a1 + b1), std::abs(a2 + b2), std::abs(a3 + b3)});
        return std::min(minus, plus) <= slack;
    }
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
    return a.id == b.id && a.body == b.body && same(a.local, b.local) && a.label == b.label
        && a.part == b.part;
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
        && a.c == b.c && same(a.direction, b.direction) && a.followsFrame == b.followsFrame
        && a.magnitude.text == b.magnitude.text && a.k6 == b.k6 && a.c6 == b.c6
        && a.preload6 == b.preload6;
}

bool same(const InitialVelocity& a, const InitialVelocity& b)
{
    return a.source == b.source && a.kind == b.kind && a.frameI == b.frameI && a.frameJ == b.frameJ
        && a.rate == b.rate && same(a.v, b.v) && same(a.w, b.w);
}

bool same(const Sensor& a, const Sensor& b)
{
    return a.source == b.source && a.watched == b.watched && a.quantity == b.quantity
        && a.component == b.component && a.below == b.below && a.crosses == b.crosses
        && a.value == b.value && a.stop == b.stop && a.count == b.count && a.repeat == b.repeat
        && a.frames == b.frames;
}

bool same(const Stop& a, const Stop& b)
{
    return a.source == b.source && a.limit == b.limit && a.mode == b.mode
        && a.restitution == b.restitution && a.k == b.k && a.c == b.c;
}

bool same(const Friction& a, const Friction& b)
{
    return a.source == b.source && a.frameI == b.frameI && a.frameJ == b.frameJ
        && a.slides == b.slides && a.turns == b.turns && a.muStatic == b.muStatic
        && a.muDynamic == b.muDynamic && a.vStiction == b.vStiction
        && a.vFriction == b.vFriction && a.radius == b.radius
        && a.preloadForce == b.preloadForce && a.preloadTorque == b.preloadTorque;
}

bool same(const Inertia& a, const Inertia& b)
{
    if (slack > 0.0) {
        // From the shape where the part stands: its last bits move with it
        double largest = 0.0;
        double off = 0.0;
        for (std::size_t i = 0; i < a.inertia.size(); ++i) {
            largest = std::max(largest, std::abs(a.inertia[i]));
            off = std::max(off, std::abs(a.inertia[i] - b.inertia[i]));
        }
        return a.body == b.body && std::abs(a.mass - b.mass) <= slack * std::abs(a.mass)
            && same(a.com, b.com) && off <= slack * largest && a.origin == b.origin;
    }
    return a.body == b.body && a.mass == b.mass && same(a.com, b.com) && a.inertia == b.inertia
        && a.origin == b.origin;
}

bool same(const ContactLaw& a, const ContactLaw& b)
{
    return a.from == b.from && a.radius == b.radius && a.damping == b.damping
        && a.restitution == b.restitution && a.stiffness == b.stiffness
        && a.exponent == b.exponent && a.maxDamping == b.maxDamping
        && a.penetration == b.penetration && a.allowance == b.allowance
        && a.muStatic == b.muStatic && a.muDynamic == b.muDynamic
        && a.vStiction == b.vStiction && a.vFriction == b.vFriction;
}

bool same(const ContactShape& a, const ContactShape& b)
{
    if (a.frame != b.frame || a.kind != b.kind || !same(a.size, b.size)
        || a.points.size() != b.points.size() || a.triangles != b.triangles
        || a.modulus != b.modulus || a.volume != b.volume || a.loops != b.loops
        || a.pieces.size() != b.pieces.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        if (!same(a.points[i], b.points[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.pieces.size(); ++i) {
        const ProfilePiece& x = a.pieces[i];
        const ProfilePiece& y = b.pieces[i];
        if (!same(x.from, y.from) || !same(x.to, y.to) || !same(x.centre, y.centre)
            || x.sweep != y.sweep) {
            return false;
        }
    }
    return true;
}

bool same(const ContactPair& a, const ContactPair& b)
{
    return a.id == b.id && a.source == b.source && a.groupA == b.groupA && a.groupB == b.groupB
        && same(a.law, b.law) && a.cam == b.cam && a.side == b.side;
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

namespace
{
// Everything but the bodies
bool sameBeyondBodies(const Model& a, const Model& b)
{
    if (a.dynamics.has_value() != b.dynamics.has_value()
        || (a.dynamics
            && (!same(a.dynamics->gravity, b.dynamics->gravity)
                || !sameRecords(a.dynamics->inertias, b.dynamics->inertias)
                || !sameRecords(a.dynamics->forces, b.dynamics->forces)
                || !sameRecords(a.dynamics->initial, b.dynamics->initial)
                || !sameRecords(a.dynamics->contactShapes, b.dynamics->contactShapes)
                || !sameRecords(a.dynamics->contacts, b.dynamics->contacts)
                || !sameRecords(a.dynamics->sensors, b.dynamics->sensors)
                || !sameRecords(a.dynamics->stops, b.dynamics->stops)
                || !sameRecords(a.dynamics->frictions, b.dynamics->frictions)))) {
        return false;
    }
    return a.version == b.version && a.sources == b.sources && sameRecords(a.frames, b.frames)
        && sameRecords(a.joints, b.joints) && sameRecords(a.grounds, b.grounds)
        && sameRecords(a.limits, b.limits) && sameRecords(a.drivers, b.drivers)
        && sameRecords(a.curves, b.curves);
}
}  // namespace

bool Assembly::Solver::sameMechanism(const Model& a, const Model& b, double tolerance)
{
    struct Slack
    {
        explicit Slack(double tolerance)
        {
            slack = tolerance;
        }
        ~Slack()
        {
            slack = 0.0;
        }
    } scope(tolerance);
    if (a.bodies.size() != b.bodies.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.bodies.size(); ++i) {
        const Body& p = a.bodies[i];
        const Body& q = b.bodies[i];
        if (p.id != q.id || p.source != q.source || p.grounded != q.grounded) {
            return false;
        }
    }
    return sameBeyondBodies(a, b);
}

bool Assembly::Solver::sameExceptDragWeights(const Model& a, const Model& b)
{
    if (a.dynamics.has_value() != b.dynamics.has_value()
        || (a.dynamics
            && (!same(a.dynamics->gravity, b.dynamics->gravity)
                || !sameRecords(a.dynamics->inertias, b.dynamics->inertias)
                || !sameRecords(a.dynamics->forces, b.dynamics->forces)
                || !sameRecords(a.dynamics->initial, b.dynamics->initial)
                || !sameRecords(a.dynamics->contactShapes, b.dynamics->contactShapes)
                || !sameRecords(a.dynamics->contacts, b.dynamics->contacts)
                || !sameRecords(a.dynamics->sensors, b.dynamics->sensors)
                || !sameRecords(a.dynamics->stops, b.dynamics->stops)
                || !sameRecords(a.dynamics->frictions, b.dynamics->frictions)))) {
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
    for (auto& curve : out.curves) {
        curve.geometry = scaledCurve(curve.geometry, length);
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
        // stiffness or a damping, a force over a length, not at all. A
        // torque, and so a torsion's stiffness and damping, as a length
        // squared; its angle not at all.
        for (auto& force : out.dynamics->forces) {
            if (force.kind == ForceKind::AppliedForce) {
                force.magnitude = scaledFormula(force.magnitude, length);
                continue;
            }
            if (force.kind == ForceKind::AppliedTorque) {
                force.magnitude = scaledFormula(force.magnitude, length * length);
                continue;
            }
            if (force.kind == ForceKind::Bushing) {
                for (int i = 3; i < 6; ++i) {
                    force.k6[i] *= length * length;
                    force.c6[i] *= length * length;
                    force.preload6[i] *= length * length;
                }
                for (int i = 0; i < 3; ++i) {
                    force.preload6[i] *= length;
                }
                continue;
            }
            if (force.kind == ForceKind::TorsionSpring) {
                force.k *= length * length;
                force.preload *= length * length;
                force.c *= length * length;
                continue;
            }
            force.rest *= length;
            force.preload *= length;
        }
        // A penalty stop's on a turn, a torque's, as a length squared; on a
        // slide, a force over a length or a speed, not at all
        for (auto& stop : out.dynamics->stops) {
            if (stop.limit < out.limits.size()
                && out.limits[stop.limit].kind == LimitKind::RotationZ) {
                stop.k *= length * length;
                stop.c *= length * length;
            }
        }
        // A friction's speeds and radius as a length, its preloads as a
        // force and a torque
        for (auto& friction : out.dynamics->frictions) {
            friction.vStiction *= length;
            friction.vFriction *= length;
            friction.radius *= length;
            friction.preloadForce *= length;
            friction.preloadTorque *= length * length;
        }
        // A speed scales as a length does, a turning rate not at all
        for (auto& initial : out.dynamics->initial) {
            if (initial.kind == InitialVelocity::Kind::TranslationZ) {
                initial.rate *= length;
            }
            initial.v = initial.v * length;
        }
        // A modulus, a force over an area: as a length to the -1, the
        // force scaling as a length does here (as the impact law's
        // stiffness has it below); Hertz's k = 4/3 E* R^0.5 as a length to
        // the -1/2 then, as the law's at e = 1.5
        for (auto& shape : out.dynamics->contactShapes) {
            shape.size = shape.size * length;
            for (auto& point : shape.points) {
                point = point * length;
            }
            for (auto& piece : shape.pieces) {
                piece.from = piece.from * length;
                piece.to = piece.to * length;
                piece.centre = piece.centre * length;
            }
            shape.modulus /= length;
            shape.volume *= length * length * length;
        }
        // The impact law's stiffness, a force over a length to the e: as a
        // length to the 1 - e; its damping, a force over a speed, not at all
        for (auto& contact : out.dynamics->contacts) {
            ContactLaw& law = contact.law;
            law.stiffness *= std::pow(length, 1.0 - law.exponent);
            law.penetration *= length;
            law.allowance *= length;
            law.vStiction *= length;
            law.vFriction *= length;
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
