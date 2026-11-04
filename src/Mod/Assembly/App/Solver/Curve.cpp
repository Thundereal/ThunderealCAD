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

// The neutral curve projector and the PointOnCurve equations (Curve.h).

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <Eigen/Dense>

#include "Curve.h"

using namespace Assembly::Solver;

namespace
{

using Vec3 = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;

Vec3 toEigen(const Base::Vector3d& v)
{
    return Vec3(v.x, v.y, v.z);
}

Base::Vector3d toBase(const Vec3& v)
{
    return Base::Vector3d(v.x(), v.y(), v.z());
}

struct Point
{
    Vec3 p;
    Vec3 d1;
    Vec3 d2;
};

Point pointAt(const CurveGeometry& curve, double u)
{
    const CurvePoint c = evaluateCurve(curve, u);
    return {toEigen(c.p), toEigen(c.d1), toEigen(c.d2)};
}

constexpr int maxIterations = 100;
// Sampled for the reference direction and the global search.
constexpr int referenceSamples = 64;
constexpr int searchSamples = 128;

}  // namespace

CurveGeometry::~CurveGeometry() = default;

namespace
{

class ScaledCurve: public CurveGeometry
{
public:
    ScaledCurve(std::shared_ptr<const CurveGeometry> base, double factor)
        : base(std::move(base))
        , factor(factor)
    {}

    double first() const override
    {
        return base->first();
    }
    double last() const override
    {
        return base->last();
    }
    bool closed() const override
    {
        return base->closed();
    }
    CurvePoint evaluate(double u) const override
    {
        const CurvePoint c = base->evaluate(u);
        return {c.p * factor, c.d1 * factor, c.d2 * factor};
    }
    Base::Vector3d planeNormal() const override
    {
        return base->planeNormal();
    }
    std::string describe() const override
    {
        return "scaled " + std::to_string(factor) + " " + base->describe();
    }
    bool same(const CurveGeometry& other) const override
    {
        const auto* scaled = dynamic_cast<const ScaledCurve*>(&other);
        return scaled && scaled->factor == factor && base->same(*scaled->base);
    }

private:
    std::shared_ptr<const CurveGeometry> base;
    double factor;
};

}  // namespace

std::shared_ptr<const CurveGeometry>
Assembly::Solver::scaledCurve(std::shared_ptr<const CurveGeometry> curve, double factor)
{
    if (!curve || factor == 1.0) {
        return curve;
    }
    return std::make_shared<ScaledCurve>(std::move(curve), factor);
}

const Base::Vector3d& CurveGeometry::reference() const
{
    if (!haveReference) {
        referenceDirection = curveReference(*this);
        haveReference = true;
    }
    return referenceDirection;
}

CurvePoint Assembly::Solver::evaluateCurve(const CurveGeometry& curve, double u)
{
    const double a = curve.first();
    const double b = curve.last();
    if (curve.closed()) {
        const double period = b - a;
        if (!(period > 0.0)) {
            return curve.evaluate(a);
        }
        double w = std::fmod(u - a, period);
        if (w < 0.0) {
            w += period;
        }
        if (!(w < period)) {
            w = 0.0;  // rounding of a value just below a whole period
        }
        return curve.evaluate(a + w);
    }
    // Open: the end tangents carry the curve on.
    if (u < a || u > b) {
        const double end = u < a ? a : b;
        CurvePoint c = curve.evaluate(end);
        c.p = c.p + c.d1 * (u - end);
        c.d2 = Base::Vector3d();
        return c;
    }
    return curve.evaluate(u);
}

Base::Vector3d Assembly::Solver::curveReference(const CurveGeometry& curve, double* worstCosine)
{
    std::vector<Vec3> tangents;
    tangents.reserve(referenceSamples);
    const double span = curve.last() - curve.first();
    for (int i = 0; i < referenceSamples; ++i) {
        const double u = curve.first() + span * (i + 0.5) / referenceSamples;
        const Vec3 d1 = toEigen(curve.evaluate(u).d1);
        const double n = d1.norm();
        if (n > 0.0) {
            tangents.push_back(d1 / n);
        }
    }
    Vec3 a = toEigen(curve.planeNormal());
    if (a.norm() > 0.5) {
        a.normalize();
    }
    else {
        Mat3 M = Mat3::Zero();
        for (const Vec3& t : tangents) {
            M += t * t.transpose();
        }
        Eigen::SelfAdjointEigenSolver<Mat3> solver(M);
        a = solver.eigenvectors().col(0);  // the smallest eigenvalue first
        // A sign that does not depend on the eigen solver: the largest
        // component positive.
        int k = 0;
        a.cwiseAbs().maxCoeff(&k);
        if (a[k] < 0.0) {
            a = -a;
        }
    }
    if (worstCosine) {
        double worst = 0.0;
        for (const Vec3& t : tangents) {
            worst = std::max(worst, std::abs(a.dot(t)));
        }
        *worstCosine = worst;
    }
    return toBase(a);
}

CurveProjection Assembly::Solver::projectOnCurve(
    const CurveGeometry& curve,
    const Base::Vector3d& point,
    double from
)
{
    const Vec3 x = toEigen(point);
    const double span = std::max(curve.last() - curve.first(), 1e-300);
    const double maxStep = span / 8.0;

    CurveProjection out;
    double u = from;
    Point c = pointAt(curve, u);
    Vec3 e = x - c.p;
    double f = 0.5 * e.squaredNorm();
    for (int it = 0; it < maxIterations; ++it) {
        out.iterations = it + 1;
        // f(u) = |x - C(u)|^2 / 2: f' = -e.C', f'' = C'.C' - e.C''.
        const double g = -e.dot(c.d1);
        const double h = c.d1.squaredNorm() - e.dot(c.d2);
        double step = 0.0;
        if (h > 0.0) {
            step = -g / h;
        }
        else if (g != 0.0) {
            // Not convex here (past a centre of curvature): downhill by the
            // largest step, backtracked below.
            step = g > 0.0 ? -maxStep : maxStep;
        }
        if (std::abs(step) > maxStep) {
            step = std::copysign(maxStep, step);
        }
        const double negligible = 1e-15 * std::max({1.0, std::abs(u), span});
        if (!(std::abs(step) > negligible)) {
            out.converged = std::isfinite(step);
            break;
        }
        // Backtracked until f decreases; a small Newton step in a convex
        // place is taken as it is (there the decrease of f is below its
        // rounding, and Newton converges).
        double alpha = 1.0;
        bool taken = false;
        Point cn;
        Vec3 en;
        double fn = 0.0;
        for (int k = 0; k < 40; ++k) {
            cn = pointAt(curve, u + alpha * step);
            en = x - cn.p;
            fn = 0.5 * en.squaredNorm();
            if (fn <= f - 1e-4 * alpha * std::abs(step * g)
                || (h > 0.0 && alpha == 1.0 && std::abs(step) < 1e-6 * span)) {
                taken = true;
                break;
            }
            alpha *= 0.5;
        }
        if (!taken) {
            out.converged = true;  // nothing lower nearby: a minimum to rounding
            break;
        }
        u += alpha * step;
        c = cn;
        e = en;
        f = fn;
        if (!(std::abs(alpha * step) > negligible)) {
            out.converged = true;
            break;
        }
    }
    out.u = u;
    out.at = {toBase(c.p), toBase(c.d1), toBase(c.d2)};
    out.distance = e.norm();
    return out;
}

CurveProjection Assembly::Solver::projectOnCurveGlobally(const CurveGeometry& curve, const Base::Vector3d& point)
{
    const Vec3 x = toEigen(point);
    const double a = curve.first();
    const double span = curve.last() - a;
    const int last = curve.closed() ? searchSamples - 1 : searchSamples;
    double best = std::numeric_limits<double>::infinity();
    double bestU = a;
    for (int i = 0; i <= last; ++i) {
        const double u = a + span * i / searchSamples;
        const double d = (x - toEigen(curve.evaluate(u).p)).norm();
        if (d < best) {
            best = d;
            bestU = u;
        }
    }
    return projectOnCurve(curve, point, bestU);
}

PointOnCurveRows Assembly::Solver::pointOnCurveRows(
    const CurveGeometry& curve,
    double offset,
    const Base::Vector3d& point,
    double from
)
{
    PointOnCurveRows out;
    const CurveProjection projection = projectOnCurve(curve, point, from);
    out.u = projection.u;
    out.converged = projection.converged;

    const Vec3 x = toEigen(point);
    const Vec3 C = toEigen(projection.at.p);
    const Vec3 d1 = toEigen(projection.at.d1);
    const Vec3 d2 = toEigen(projection.at.d2);
    const double s = d1.norm();
    if (!(s > 0.0)) {
        // A singular point of the parametrisation: no tangent, no frame.
        out.converged = false;
        return out;
    }
    // The unit tangent and its derivative with respect to u.
    const Vec3 T = d1 / s;
    const Vec3 Tp = (d2 - d2.dot(T) * T) / s;
    // N1 from the reference direction, N2 = T x N1, and their derivatives.
    const Vec3 a = toEigen(curve.reference());
    const Vec3 v1 = a - a.dot(T) * T;
    const double n1 = std::max(v1.norm(), 1e-300);
    const Vec3 v1p = -a.dot(Tp) * T - a.dot(T) * Tp;
    const Vec3 N1 = v1 / n1;
    const Vec3 N1p = (v1p - v1p.dot(N1) * N1) / n1;
    const Vec3 N2 = T.cross(N1);
    const Vec3 N2p = Tp.cross(N1) + T.cross(N1p);

    const Vec3 e = x - C;
    const Vec3 eO = e - offset * N2;  // from the offset curve's point
    const Vec3 Op = d1 + offset * N2p;  // the offset curve's derivative
    // du/dx = C' / D; past the centre of curvature (D <= 0) the closest point
    // is no longer a minimum and has no derivative: kept positive.
    const double D = std::max(d1.squaredNorm() - e.dot(d2), 1e-12 * d1.squaredNorm());

    const std::array<Vec3, 2> N {N1, N2};
    const std::array<Vec3, 2> Np {N1p, N2p};
    for (int k = 0; k < 2; ++k) {
        out.value[k] = e.dot(N[k]) - (k == 1 ? offset : 0.0);
        const Vec3 g = N[k] + (eO.dot(Np[k]) / D) * d1;
        const Mat3 H = (Np[k] * d1.transpose() + d1 * Np[k].transpose()
                        - (Op.dot(Np[k]) / D) * (d1 * d1.transpose()))
            / D;
        for (int i = 0; i < 3; ++i) {
            out.gradient[k][i] = g[i];
            for (int j = 0; j < 3; ++j) {
                out.hessian[k][i][j] = H(i, j);
            }
        }
    }
    return out;
}
