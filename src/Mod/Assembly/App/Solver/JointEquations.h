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

#pragma once

// The neutral layer's own joint equations (docs/AssemblySolverInterface.md
// sec 6.13 and 6.17): every JointKind written from the frames' global origins
// and axes, in the form OndselSolver uses for it, each equation with its
// analytic gradient with respect to the poses of the (at most three) bodies
// it depends on. The Reference backend solves them; the Chrono backend's
// NeutralLink holds them for the kinds Chrono has no link for. Internal to
// App/Solver (Eigen in the interface, nothing exported).
//
// Variables, as the Reference has them: per body a position and a rotation
// increment, the spatial rotation vector applied on the left of the body's
// rotation (q' = exp(dtheta) q). A gradient is a row of six numbers per
// body: d/dx (3), then d/dtheta (3).

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "Model.h"

namespace Assembly::Solver::Equations
{

using Vec3 = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;
using Row6 = Eigen::Matrix<double, 1, 6>;
using Mat36 = Eigen::Matrix<double, 3, 6>;

inline constexpr double pi = std::numbers::pi;
inline constexpr double twoPi = 2.0 * std::numbers::pi;
// Within a gear joint's coaxial test (OndselSolver's GearJoint).
inline constexpr double coaxialTolerance = 1.0e-9;

inline Mat3 skew(const Vec3& v)
{
    Mat3 m;
    m << 0.0, -v.z(), v.y(), v.z(), 0.0, -v.x(), -v.y(), v.x(), 0.0;
    return m;
}

inline double wrapAngle(double a)
{
    // To (-pi, pi].
    a = std::remainder(a, twoPi);
    return a <= -pi ? a + twoPi : a;
}

// 'raw' taken the whole turns nearest to 'reference'.
inline double unwrap(double raw, double reference)
{
    return reference + wrapAngle(raw - reference);
}

// OndselSolver's first reading of an angle: atan2 when the cosine is
// positive, else the angle in [0, 2 pi) (AngleZIecJec / OrbitAngleZIecJec
// postInput). It decides where an angle starts counting its turns.
inline double initialAngle(double s, double c)
{
    if (c > 0.0) {
        return std::atan2(s, c);
    }
    return s >= 0.0 ? std::atan2(s, c) : twoPi + std::atan2(s, c);
}

// ---------------------------------------------------------------- derivatives

// d(scalar) / d(dx, dtheta) for the bodies it depends on (at most three: a
// coaxial gear's I, J and carrier K). A body index below 0 is no variable.
struct Grad
{
    std::array<int, 3> body {-1, -1, -1};
    std::array<Row6, 3> g;
    int n = 0;

    void add(int b, const Row6& v)
    {
        if (b < 0) {
            return;
        }
        for (int i = 0; i < n; ++i) {
            if (body[i] == b) {
                g[i] += v;
                return;
            }
        }
        if (n == 3) {
            throw std::logic_error("an equation on more than three bodies");
        }
        body[n] = b;
        g[n] = v;
        ++n;
    }
    void addScaled(const Grad& o, double k)
    {
        for (int i = 0; i < o.n; ++i) {
            add(o.body[i], o.g[i] * k);
        }
    }
};

// d(vector) / d(dx, dtheta), for at most two bodies (a difference of origins).
struct VGrad
{
    std::array<int, 2> body {-1, -1};
    std::array<Mat36, 2> g;
    int n = 0;

    void add(int b, const Mat36& v)
    {
        if (b < 0) {
            return;
        }
        for (int i = 0; i < n; ++i) {
            if (body[i] == b) {
                g[i] += v;
                return;
            }
        }
        if (n == 2) {
            throw std::logic_error("a vector on more than two bodies");
        }
        body[n] = b;
        g[n] = v;
        ++n;
    }
};

// A scalar and its gradient.
struct S
{
    double v = 0.0;
    Grad g;
};

// A vector and its gradient.
struct V
{
    Vec3 v = Vec3::Zero();
    VGrad g;
};

inline S constant(double value)
{
    S s;
    s.v = value;
    return s;
}

inline S operator+(S a, const S& b)
{
    a.v += b.v;
    a.g.addScaled(b.g, 1.0);
    return a;
}

inline S operator-(S a, const S& b)
{
    a.v -= b.v;
    a.g.addScaled(b.g, -1.0);
    return a;
}

inline S operator*(double k, const S& a)
{
    S r;
    r.v = k * a.v;
    r.g.addScaled(a.g, k);
    return r;
}

inline S operator*(const S& a, const S& b)
{
    S r;
    r.v = a.v * b.v;
    r.g.addScaled(a.g, b.v);
    r.g.addScaled(b.g, a.v);
    return r;
}

inline V operator-(const V& a, const V& b)
{
    V r;
    r.v = a.v - b.v;
    for (int i = 0; i < a.g.n; ++i) {
        r.g.add(a.g.body[i], a.g.g[i]);
    }
    for (int i = 0; i < b.g.n; ++i) {
        r.g.add(b.g.body[i], -b.g.g[i]);
    }
    return r;
}

inline S dot(const V& a, const V& b)
{
    S r;
    r.v = a.v.dot(b.v);
    for (int i = 0; i < a.g.n; ++i) {
        r.g.add(a.g.body[i], b.v.transpose() * a.g.g[i]);
    }
    for (int i = 0; i < b.g.n; ++i) {
        r.g.add(b.g.body[i], a.v.transpose() * b.g.g[i]);
    }
    return r;
}

// The component of a along a constant direction.
inline S dot(const V& a, const Vec3& u)
{
    S r;
    r.v = a.v.dot(u);
    for (int i = 0; i < a.g.n; ++i) {
        r.g.add(a.g.body[i], u.transpose() * a.g.g[i]);
    }
    return r;
}

inline S norm(const V& a)
{
    S r;
    r.v = a.v.norm();
    if (r.v > 1e-300) {
        const Vec3 u = a.v / r.v;
        for (int i = 0; i < a.g.n; ++i) {
            r.g.add(a.g.body[i], u.transpose() * a.g.g[i]);
        }
    }
    // At 0 the distance has no gradient (a cone's tip); OndselSolver's has none
    // either.
    return r;
}

// atan2(y, x) and its derivative; the value is atan2's, the caller unwraps.
inline S angle(const S& y, const S& x)
{
    S r;
    r.v = std::atan2(y.v, x.v);
    const double d = x.v * x.v + y.v * y.v;
    if (d > 1e-300) {
        r.g.addScaled(y.g, x.v / d);
        r.g.addScaled(x.g, -y.v / d);
    }
    return r;
}

// A frame's global state at some body poses.
struct FrameState
{
    int body = -1;  // the variable's index; -1: not a variable (the ground, a pinned body)
    Vec3 o = Vec3::Zero();  // origin
    Vec3 base = Vec3::Zero();  // the body's position (o - base is the lever arm)
    Mat3 A = Mat3::Identity();  // axes as columns
};

inline V origin(const FrameState& f)
{
    V r;
    r.v = f.o;
    if (f.body >= 0) {
        Mat36 g;
        g.leftCols<3>() = Mat3::Identity();
        g.rightCols<3>() = -skew(f.o - f.base);
        r.g.add(f.body, g);
    }
    return r;
}

inline V axis(const FrameState& f, int k)
{
    V r;
    r.v = f.A.col(k);
    if (f.body >= 0) {
        Mat36 g;
        g.leftCols<3>().setZero();
        g.rightCols<3>() = -skew(r.v);
        r.g.add(f.body, g);
    }
    return r;
}

// thez(I, J): the angle of xJ about zI, measured from xI (AngleZIeJe).
inline S angleZ(const FrameState& fi, const FrameState& fj)
{
    const V xj = axis(fj, 0);
    return angle(dot(axis(fi, 1), xj), dot(axis(fi, 0), xj));
}

// The orbit of J's origin about I's z (OrbitAngleZIeJe).
inline S orbit(const FrameState& fi, const FrameState& fj)
{
    const V r = origin(fj) - origin(fi);
    return angle(dot(r, axis(fi, 1)), dot(r, axis(fi, 0)));
}

// ---------------------------------------------------------------- equations

// OndselSolver's three categories, in the order its rank analysis takes them.
enum class Category : std::uint8_t
{
    Essential,
    Displacement,
    Perpendicular
};

struct Equation
{
    S s;  // the raw residual, as a solver drives it to zero
    double natural = 0.0;  // the error in natural units (the checker's)
    Category category = Category::Essential;
};

// Origins coincide, as OndselSolver's AtPoint: three global components.
void atPointRows(const FrameState& fi, const FrameState& fj, std::vector<Equation>& rows);
// The Fixed joint's (and a Ground's): AtPoint and three direction cosines.
void fixedRows(const FrameState& fi, const FrameState& fj, std::vector<Equation>& rows);

// How many angles a coupler measures (and counts the turns of); for a
// PointOnCurve, the one slot is the curve parameter of the closest point,
// which a closed curve counts the turns of too.
int jointAngleSlots(JointKind kind);

// Whether a gear's frames share their axis (OndselSolver's GearJoint test).
bool framesAreCoaxial(const FrameState& fi, const FrameState& fj);

// (sin, cos) of thetaZ(I, J), and of the orbit of J's origin about zI.
std::pair<double, double> rawAngleZ(const FrameState& fi, const FrameState& fj);
std::pair<double, double> rawOrbit(const FrameState& fi, const FrameState& fj);

// What a joint keeps between evaluations: whether a gear's frames are
// coaxial (decided at the start), a coupler's constant (the relation it has
// at the start), and the angles it measures, unwrapped -- their turns counted
// -- against the last committed reading. A PointOnCurve keeps the parameter
// of the curve's point closest to frame J's origin in angles[0]: the next
// evaluation projects from it (Curve.h, projectOnCurve), so the follower
// stays on the stretch of the curve it is on.
struct JointState
{
    bool coaxial = false;
    double constant = 0.0;
    std::array<double, 2> angles {0.0, 0.0};
};

// The (sin, cos) of each angle a coupler measures, in its slot order. 'fk' is
// the carrier frame, null when the joint has none (a coaxial gear without
// one measures nothing).
std::array<std::pair<double, double>, 2> rawCouplerAngles(
    const Joint& joint,
    bool coaxial,
    const FrameState& fi,
    const FrameState& fj,
    const FrameState* fk
);

// The state at the start poses: OndselSolver's first reading of each angle
// the joint measures, and its constant -- what the equation without it reads
// there -- so that a coupler holds the relation it starts with. A
// PointOnCurve's parameter is the closest point over the whole curve
// (projectOnCurveGlobally): the one place it is searched for, not followed.
// 'curve' is the joint's curve, for a PointOnCurve.
JointState startState(
    const Joint& joint,
    bool coaxial,
    const FrameState& fi,
    const FrameState& fj,
    const FrameState* fk,
    const CurveGeometry* curve = nullptr
);

// The joint's equations at the frames' states, appended to 'rows' in
// OndselSolver's form and order. A coupler's angles are unwrapped against
// state.angles and written to 'measured' (a commit takes them once the poses
// are accepted); 'raw' leaves out the coupler's constant (to measure it).
// A coaxial gear without a carrier (fk null), or with a zero second radius,
// has no equation; nor has a PointOnCurve without its curve.
//
// PointOnCurve: frame J's origin in frame I's coordinates, x, held on the
// curve (offset by joint.value in its plane): the two rows of
// pointOnCurveRows() (Curve.h), displacements, their gradient through x by
// the chain rule; the closest point is followed from state.angles[0] and its
// parameter written to measured[0].
void jointRows(
    const Joint& joint,
    const FrameState& fi,
    const FrameState& fj,
    const FrameState* fk,
    const JointState& state,
    bool raw,
    std::vector<Equation>& rows,
    std::array<double, 2>& measured,
    const CurveGeometry* curve = nullptr
);

// Frame J's origin in frame I's coordinates (what a PointOnCurve holds on
// its curve).
Base::Vector3d localPoint(const FrameState& fi, const FrameState& fj);

}  // namespace Assembly::Solver::Equations
