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

// The neutral joint equations (JointEquations.h), shared by the Reference
// backend and the Chrono backend's NeutralLink. Moved here unchanged from
// ReferenceBackend.cpp.

#include "JointEquations.h"

namespace Assembly::Solver::Equations
{

namespace
{

void push(std::vector<Equation>& rows, const S& s, Category category, double natural)
{
    Equation row;
    row.s = s;
    row.natural = natural;
    row.category = category;
    rows.push_back(row);
}

void push(std::vector<Equation>& rows, const S& s, Category category)
{
    push(rows, s, category, s.v);
}

// OndselSolver's InLine: oJ on I's z axis (its x and y in I's frame).
void inLineRows(const FrameState& fi, const FrameState& fj, std::vector<Equation>& rows)
{
    const V r = origin(fj) - origin(fi);
    push(rows, dot(r, axis(fi, 0)), Category::Displacement);
    push(rows, dot(r, axis(fi, 1)), Category::Displacement);
}

// OndselSolver's InPlane: oJ at 'offset' along I's z.
void inPlaneRow(
    const FrameState& fi,
    const FrameState& fj,
    double offset,
    std::vector<Equation>& rows
)
{
    const V r = origin(fj) - origin(fi);
    push(rows, dot(r, axis(fi, 2)) - constant(offset), Category::Displacement);
}

// A direction cosine: axis a of I against axis b of J, less a constant.
void cosineRow(
    const FrameState& fi,
    int a,
    const FrameState& fj,
    int b,
    double c,
    std::vector<Equation>& rows
)
{
    push(rows, dot(axis(fi, a), axis(fj, b)) - constant(c), Category::Perpendicular);
}

void parallelRows(const FrameState& fi, const FrameState& fj, std::vector<Equation>& rows)
{
    cosineRow(fi, 2, fj, 0, 0.0, rows);
    cosineRow(fi, 2, fj, 1, 0.0, rows);
}

// x^2 + y^2 - d^2 of oJ in I's frame (DistancexyConstraintIJ); the
// natural error is the distance's.
void distanceXYRow(const FrameState& fi, const FrameState& fj, double d, std::vector<Equation>& rows)
{
    const V r = origin(fj) - origin(fi);
    const S x = dot(r, axis(fi, 0));
    const S y = dot(r, axis(fi, 1));
    const S s = x * x + y * y - constant(d * d);
    push(rows, s, Category::Displacement, std::sqrt(x.v * x.v + y.v * y.v) - std::abs(d));
}

// A point on a curve: x = oJ in I's axes (three scalars with their
// gradients), the curve's two equations at x, and their gradients through x.
void curveRows(
    const CurveGeometry& curve,
    double offset,
    const FrameState& fi,
    const FrameState& fj,
    double from,
    std::vector<Equation>& rows,
    double& measured
)
{
    const V r = origin(fj) - origin(fi);
    const std::array<S, 3> x {dot(r, axis(fi, 0)), dot(r, axis(fi, 1)), dot(r, axis(fi, 2))};
    const PointOnCurveRows eq =
        pointOnCurveRows(curve, offset, Base::Vector3d(x[0].v, x[1].v, x[2].v), from);
    measured = eq.u;
    for (int k = 0; k < 2; ++k) {
        S s;
        for (int m = 0; m < 3; ++m) {
            s = s + eq.gradient[k][m] * x[m];
        }
        s.v = eq.value[k];
        push(rows, s, Category::Displacement);
    }
}

}  // namespace

Base::Vector3d localPoint(const FrameState& fi, const FrameState& fj)
{
    const Vec3 x = fi.A.transpose() * (fj.o - fi.o);
    return Base::Vector3d(x.x(), x.y(), x.z());
}

void atPointRows(const FrameState& fi, const FrameState& fj, std::vector<Equation>& rows)
{
    const V r = origin(fj) - origin(fi);
    for (int k = 0; k < 3; ++k) {
        push(rows, dot(r, Vec3::Unit(k)), Category::Displacement);
    }
}

void fixedRows(const FrameState& fi, const FrameState& fj, std::vector<Equation>& rows)
{
    atPointRows(fi, fj, rows);
    cosineRow(fi, 1, fj, 0, 0.0, rows);
    cosineRow(fi, 2, fj, 0, 0.0, rows);
    cosineRow(fi, 2, fj, 1, 0.0, rows);
}

int jointAngleSlots(JointKind kind)
{
    switch (kind) {
        case JointKind::Gear:
        case JointKind::CoaxialGear:
            return 2;
        case JointKind::RackPinion:
        case JointKind::Screw:
        case JointKind::PointOnCurve:
            return 1;
        default:
            return 0;
    }
}

bool framesAreCoaxial(const FrameState& fi, const FrameState& fj)
{
    const Vec3 zi = fi.A.col(2);
    const Vec3 zj = fj.A.col(2);
    if (1.0 - std::abs(zi.dot(zj)) > coaxialTolerance) {
        return false;
    }
    return (fj.o - fi.o).cross(zi).norm() <= coaxialTolerance;
}

std::pair<double, double> rawAngleZ(const FrameState& fi, const FrameState& fj)
{
    const Vec3 xj = fj.A.col(0);
    return {fi.A.col(1).dot(xj), fi.A.col(0).dot(xj)};
}

std::pair<double, double> rawOrbit(const FrameState& fi, const FrameState& fj)
{
    const Vec3 r = fj.o - fi.o;
    return {r.dot(fi.A.col(1)), r.dot(fi.A.col(0))};
}

std::array<std::pair<double, double>, 2> rawCouplerAngles(
    const Joint& joint,
    bool coaxial,
    const FrameState& fi,
    const FrameState& fj,
    const FrameState* fk
)
{
    std::array<std::pair<double, double>, 2> raw {};
    switch (joint.kind) {
        case JointKind::Gear:
        case JointKind::CoaxialGear:
            if (coaxial) {
                if (fk) {
                    raw[0] = rawAngleZ(*fk, fi);
                    raw[1] = rawAngleZ(*fk, fj);
                }
            }
            else {
                raw[0] = rawOrbit(fi, fj);
                raw[1] = rawOrbit(fj, fi);
            }
            break;
        case JointKind::RackPinion:
        case JointKind::Screw:
            raw[0] = rawAngleZ(fi, fj);
            break;
        default:
            break;
    }
    return raw;
}

JointState startState(
    const Joint& joint,
    bool coaxial,
    const FrameState& fi,
    const FrameState& fj,
    const FrameState* fk,
    const CurveGeometry* curve
)
{
    JointState state;
    state.coaxial = coaxial;
    if (joint.kind == JointKind::PointOnCurve) {
        // No constant: the curve is the relation.
        if (curve) {
            state.angles[0] = projectOnCurveGlobally(*curve, localPoint(fi, fj)).u;
        }
        return state;
    }
    if (jointAngleSlots(joint.kind) == 0) {
        return state;
    }
    const auto raw = rawCouplerAngles(joint, coaxial, fi, fj, fk);
    for (int k = 0; k < 2; ++k) {
        state.angles[k] = initialAngle(raw[k].first, raw[k].second);
    }
    std::vector<Equation> rows;
    std::array<double, 2> measured {};
    jointRows(joint, fi, fj, fk, state, true, rows, measured);
    if (!rows.empty()) {
        state.constant = rows.front().s.v;
    }
    return state;
}

void jointRows(
    const Joint& joint,
    const FrameState& fi,
    const FrameState& fj,
    const FrameState* fk,
    const JointState& state,
    bool raw,
    std::vector<Equation>& rows,
    std::array<double, 2>& measured,
    const CurveGeometry* curve
)
{
    measured = state.angles;
    switch (joint.kind) {
        case JointKind::Fixed:
            fixedRows(fi, fj, rows);
            break;
        case JointKind::Revolute:
            atPointRows(fi, fj, rows);
            parallelRows(fi, fj, rows);
            break;
        case JointKind::Cylindrical:
            inLineRows(fi, fj, rows);
            parallelRows(fi, fj, rows);
            break;
        case JointKind::Prismatic:
            inLineRows(fi, fj, rows);
            cosineRow(fi, 1, fj, 0, 0.0, rows);
            parallelRows(fi, fj, rows);
            break;
        case JointKind::Spherical:
            atPointRows(fi, fj, rows);
            break;
        case JointKind::Planar:
            inPlaneRow(fi, fj, joint.value, rows);
            parallelRows(fi, fj, rows);
            break;
        case JointKind::ParallelZ:
            parallelRows(fi, fj, rows);
            break;
        case JointKind::PerpendicularZ:
            cosineRow(fi, 2, fj, 2, 0.0, rows);
            break;
        case JointKind::AngleZ:
            cosineRow(fi, 2, fj, 2, std::cos(joint.value), rows);
            break;
        case JointKind::PointInPlane:
            inPlaneRow(fi, fj, joint.value, rows);
            break;
        case JointKind::LineInPlane:
            inPlaneRow(fi, fj, joint.value, rows);
            cosineRow(fi, 2, fj, 2, 0.0, rows);
            break;
        case JointKind::PointOnLine:
            inLineRows(fi, fj, rows);
            break;
        case JointKind::PointPointDistance: {
            const S s = norm(origin(fj) - origin(fi)) - constant(joint.value);
            push(rows, s, Category::Displacement);
            break;
        }
        case JointKind::PointLineDistance:
            distanceXYRow(fi, fj, joint.value, rows);
            break;
        case JointKind::LineLineDistance:
            distanceXYRow(fi, fj, joint.value, rows);
            parallelRows(fi, fj, rows);
            break;
        case JointKind::Gear:
        case JointKind::CoaxialGear: {
            if (joint.value2 == 0.0) {
                break;
            }
            const double ratio = joint.value / joint.value2;
            S a0, a1;
            if (state.coaxial) {
                if (!fk) {
                    break;
                }
                a0 = angleZ(*fk, fi);  // angleKI
                a1 = angleZ(*fk, fj);  // angleKJ
            }
            else {
                a0 = orbit(fi, fj);  // orbitIeJe
                a1 = orbit(fj, fi);  // orbitJeIe
            }
            a0.v = unwrap(a0.v, state.angles[0]);
            a1.v = unwrap(a1.v, state.angles[1]);
            measured = {a0.v, a1.v};
            const S s = a1 + ratio * a0 - constant(raw ? 0.0 : state.constant);
            push(rows, s, Category::Essential);
            break;
        }
        case JointKind::RackPinion: {
            S thez = angleZ(fi, fj);
            thez.v = unwrap(thez.v, state.angles[0]);
            measured[0] = thez.v;
            const S x = dot(origin(fj) - origin(fi), axis(fi, 0));
            const S s = x + joint.value * thez - constant(raw ? 0.0 : state.constant);
            push(rows, s, Category::Essential);
            break;
        }
        case JointKind::Screw: {
            S thez = angleZ(fi, fj);
            thez.v = unwrap(thez.v, state.angles[0]);
            measured[0] = thez.v;
            const S z = dot(origin(fj) - origin(fi), axis(fi, 2));
            const S s = twoPi * z - joint.value * thez - constant(raw ? 0.0 : state.constant);
            push(rows, s, Category::Essential, s.v / twoPi);
            break;
        }
        case JointKind::PointOnCurve:
            if (curve) {
                curveRows(*curve, joint.value, fi, fj, state.angles[0], rows, measured[0]);
            }
            break;
        case JointKind::Count_:
            break;
    }
}

}  // namespace Assembly::Solver::Equations
