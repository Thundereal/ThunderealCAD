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

// The curves a PointOnCurve joint holds a point on, and the projector that
// finds the point of a curve closest to a point (docs/AssemblySolverInterface.md
// sec 6.18). Neutral: the Reference, the Chrono backend's NeutralLink and the
// Ondsel adapter evaluate the same equations through these functions, and the
// later lift-off cam (docs/AssemblyDynamics.md sec 7.4) measures its gap with
// the same projector.
//
// A curve is exact geometry behind a small interface: the builder hands an
// OCCT curve (App/CamJoint.cpp), evaluated with its own D0/D1/D2, in the
// coordinates of frame I of the joint. Nothing here knows OCCT.
//
// Parametrisation. The curve's own, over [first, last]. A closed curve
// repeats with the period last - first: its parameter is kept unwrapped (it
// counts the turns a follower makes around a cam) and wrapped only to
// evaluate. An open curve is extended beyond its ends along its end
// tangents (a straight line, second derivative zero), so that the projection
// is defined everywhere and a follower driven past an end goes on along the
// tangent instead of jumping.

#include <array>
#include <memory>
#include <string>

#include <Base/Vector3D.h>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace Assembly::Solver
{

// A point of a curve and its first two derivatives with respect to the
// parameter.
struct CurvePoint
{
    Base::Vector3d p;
    Base::Vector3d d1;
    Base::Vector3d d2;
};

class AssemblyExport CurveGeometry
{
public:
    virtual ~CurveGeometry();

    virtual double first() const = 0;
    virtual double last() const = 0;
    // The curve closes on itself: its parameter repeats with the period
    // last - first.
    virtual bool closed() const = 0;
    // At a parameter in [first, last].
    virtual CurvePoint evaluate(double u) const = 0;
    // The normal of the plane the curve lies in (unit), or zero when it lies
    // in no plane (a line lies in many: zero).
    virtual Base::Vector3d planeNormal() const = 0;
    // One line of text for dumps and native files: the curve's kind and data.
    virtual std::string describe() const = 0;
    // The same geometry, value for value.
    virtual bool same(const CurveGeometry& other) const = 0;

    double period() const
    {
        return last() - first();
    }

    // The direction the normal frame of the equations is built from
    // (curveReference()), computed once.
    const Base::Vector3d& reference() const;

private:
    mutable bool haveReference = false;
    mutable Base::Vector3d referenceDirection;
};

// The curve at any parameter: wrapped by the period when closed, extended
// along the end tangents when open.
AssemblyExport CurvePoint evaluateCurve(const CurveGeometry& curve, double u);

// A direction no tangent of the curve is parallel to, for the normal frame of
// the PointOnCurve equations: the plane normal of a planar curve; otherwise
// the direction the curve's tangents (sampled) point along least, the
// eigenvector of the smallest eigenvalue of sum(t t'). 'worstCosine'
// receives the largest |cos| between it and a sampled tangent: near 1, the
// curve turns through every direction and the frame has no smooth choice.
AssemblyExport Base::Vector3d curveReference(const CurveGeometry& curve, double* worstCosine = nullptr);

// The point of the curve closest to x, found by descent from the parameter
// 'from': Newton on (x - C(u)) . C'(u) = 0 with backtracking, each step at
// most an eighth of the parameter range. It finds the closest point in the
// basin of 'from' -- the projection a follower moving continuously along the
// curve keeps -- and never jumps across the curve to a point closer
// elsewhere. A closed curve's parameter comes back unwrapped.
struct CurveProjection
{
    double u = 0.0;
    CurvePoint at;
    double distance = 0.0;
    int iterations = 0;
    bool converged = false;
};
AssemblyExport CurveProjection projectOnCurve(const CurveGeometry& curve, const Base::Vector3d& x, double from);

// The closest point over the whole curve (a sampled search, then
// projectOnCurve()): for a start only, when there is no parameter to follow
// from.
AssemblyExport CurveProjection projectOnCurveGlobally(const CurveGeometry& curve, const Base::Vector3d& x);

// The two equations of a point x (frame J's origin in frame I's coordinates)
// held on the curve, or on its offset by 'offset' in the curve's plane.
//
// At the closest point C(u) of the curve to x (projectOnCurve from 'from'),
// with T the unit tangent, a = curve.reference(), N1 the unit part of a
// normal to T and N2 = T x N1 (for a planar curve N1 is the plane normal and
// N2 the normal in the plane), e = x - C(u):
//   r1 = e . N1,   r2 = e . N2 - offset.
// The roller of radius r on a planar cam is offset = +-r: its centre on the
// curve's offset by r towards N2 or away from it (the builder takes the side
// the centre starts on). Both are zero exactly when x is on the curve
// (offset) -- e is normal to the curve at its closest point, so it has no
// component along T.
//
// u is a function of x (the stationarity of |x - C(u)|), so the gradient
// includes du/dx = C' / (C'.C' - e.C''), the implicit-function derivative:
//   grad r_k = N_k + ((e - offset N2) . N_k' / D) C',  D = C'.C' - e.C''.
// It is exact wherever the projection is. The Hessian is the exact one where
// the equations hold ((e - offset N2) = 0):
//   H_k = (N_k' C'^T + C' N_k'^T - (O'.N_k') C' C'^T / D) / D
// with O' = C' + offset N2' the offset curve's derivative; elsewhere it is
// off by terms of the residual (OndselSolver uses it for its Lagrangian and
// its accelerations only).
struct PointOnCurveRows
{
    double u = 0.0;  // the closest point's parameter, unwrapped
    std::array<double, 2> value {};
    std::array<std::array<double, 3>, 2> gradient {};
    std::array<std::array<std::array<double, 3>, 3>, 2> hessian {};
    bool converged = false;
};
AssemblyExport PointOnCurveRows pointOnCurveRows(
    const CurveGeometry& curve,
    double offset,
    const Base::Vector3d& x,
    double from
);

}  // namespace Assembly::Solver
