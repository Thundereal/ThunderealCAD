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

#pragma once

// The 3D constraints of PlaneGCS (docs/Sketch3D.md section 4.4).
//
// Each class is one equation; a constraint that removes n degrees of freedom is added
// as n of them by the addConstraint*3 functions at the end, so the solver's QR
// diagnosis counts it right (section 4.3: "rank-exact"). Every class keeps all its
// parameters in pvec and reads them from there on each evaluation, so the solver's
// parameter redirection needs no geometry pointers rebuilt.

#include "Constraints.h"
#include "GCS.h"
#include "Geo3D.h"

namespace GCS
{

/// A direction a constraint compares, built from the geometry's parameters:
/// - vector(v):        v itself (a circle's or arc's normal)
/// - diff(a, b):       b - a (a line, or a centre-to-point radius)
/// - crossDiff(n,a,b): n x (b - a) (an arc's tangent at b, a = centre)
/// - planeNormal(pl):  a workplane's normal, from its quaternion
class SketcherExport Dir3
{
public:
    enum class Kind
    {
        Vector,
        Diff,
        CrossDiff,
        QuatNormal
    };

    static Dir3 vector(const Point3& v);
    static Dir3 diff(const Point3& a, const Point3& b);
    static Dir3 crossDiff(const Point3& n, const Point3& a, const Point3& b);
    static Dir3 planeNormal(const Plane3& pl);

    Kind kind {Kind::Vector};
    VEC_pD params;

    /// Its current value, without derivatives.
    DeriVector3 current() const;
    /// Evaluated from pvec[i], where its params were pushed.
    DeriVector3 eval(const VEC_pD& pvec, std::size_t i, const double* derivparam) const;
};

// P_k - (A_k + t (B_k - A_k)): one component of a point on a line, t unknown.
class SketcherExport ConstraintP3OnLine: public Constraint
{
    int comp;

public:
    ConstraintP3OnLine(const Point3& p, const Point3& a, const Point3& b, double* t, int comp);
    ConstraintType getTypeId() override
    {
        return P3OnLine;
    }
    void errorgrad(double* err, double* grad, double* param) override;
};

// P_k - C(t)_k: one component of a point on a B-spline at parameter t (unknown, or
// a value for a through-point at its knot).
class SketcherExport ConstraintP3OnBSpline: public Constraint
{
    int comp;
    std::size_t npoles;
    VEC_D flatknots;
    int degree;

public:
    ConstraintP3OnBSpline(const Point3& p, const BSpline3& bsp, double* t, int comp);
    ConstraintType getTypeId() override
    {
        return P3OnBSpline;
    }
    void errorgrad(double* err, double* grad, double* param) override;
};

// P_k - C(t)_k for a curve that is not solved for (a model edge).
class SketcherExport ConstraintP3OnFixedCurve: public Constraint
{
    int comp;
    std::shared_ptr<const FixedCurve3> curve;

public:
    ConstraintP3OnFixedCurve(const Point3& p,
                             std::shared_ptr<const FixedCurve3> curve,
                             double* t,
                             int comp);
    ConstraintType getTypeId() override
    {
        return P3OnFixedCurve;
    }
    void errorgrad(double* err, double* grad, double* param) override;
};

// (P - O) . n - D: a point's signed distance from a workplane; D null means 0, the
// point on the plane.
class SketcherExport ConstraintP3PlaneDistance: public Constraint
{
    bool hasDistance;

public:
    ConstraintP3PlaneDistance(const Point3& p, const Plane3& pl, double* distance);
    ConstraintType getTypeId() override
    {
        return P3PlaneDistance;
    }
    void errorgrad(double* err, double* grad, double* param) override;
    void evaluate() override;
};

// (d2 - s d1)_k, s unknown: one component of two directions being parallel. Three of
// them and the one unknown remove exactly 2 DOF whatever the directions' orientation
// (section 4.3), the way point on line uses its t; s < 0 is antiparallel.
class SketcherExport ConstraintDir3Parallel: public Constraint
{
    Dir3 d1, d2;
    int comp;

public:
    ConstraintDir3Parallel(const Dir3& d1, const Dir3& d2, double* s, int comp);
    ConstraintType getTypeId() override
    {
        return Dir3Parallel;
    }
    void errorgrad(double* err, double* grad, double* param) override;
};

// d1 . d2 / (|d1| |d2|): zero makes two directions perpendicular.
class SketcherExport ConstraintDir3Dot: public Constraint
{
    Dir3 d1, d2;

public:
    ConstraintDir3Dot(const Dir3& d1, const Dir3& d2);
    ConstraintType getTypeId() override
    {
        return Dir3Dot;
    }
    void errorgrad(double* err, double* grad, double* param) override;
};

// atan2(|d1 x d2|, d1 . d2) - theta: the unsigned angle between two directions, in
// [0, pi], whether or not their lines meet. Parallel (0) and antiparallel (pi) have
// no gradient through it; Parallel is the constraint for those.
class SketcherExport ConstraintDir3Angle: public Constraint
{
    Dir3 d1, d2;
    double value(double* dvalue, const double* param);

public:
    ConstraintDir3Angle(const Dir3& d1, const Dir3& d2, double* angle);
    ConstraintType getTypeId() override
    {
        return Dir3Angle;
    }
    void errorgrad(double* err, double* grad, double* param) override;
    void evaluate() override;
};

// |P - Q| - D.
class SketcherExport ConstraintP3Distance: public Constraint
{
public:
    ConstraintP3Distance(const Point3& p, const Point3& q, double* distance);
    ConstraintType getTypeId() override
    {
        return P3Distance;
    }
    void errorgrad(double* err, double* grad, double* param) override;
    void evaluate() override;
};

// |B - A| - |D - C|: equal lengths, equal radii, a point at an arc's radius.
class SketcherExport ConstraintP3EqualDistance: public Constraint
{
public:
    ConstraintP3EqualDistance(const Point3& a, const Point3& b, const Point3& c, const Point3& d);
    ConstraintType getTypeId() override
    {
        return P3EqualDistance;
    }
    void errorgrad(double* err, double* grad, double* param) override;
};

// |(P - A) x (B - A)| / |B - A| - D: a point's distance from a line. Not for D = 0,
// which has no gradient there; that is point on line.
class SketcherExport ConstraintP3LineDistance: public Constraint
{
    double value(double* dvalue, const double* param);

public:
    ConstraintP3LineDistance(const Point3& p, const Point3& a, const Point3& b, double* distance);
    ConstraintType getTypeId() override
    {
        return P3LineDistance;
    }
    void errorgrad(double* err, double* grad, double* param) override;
    void evaluate() override;
};

// P_k - (A_k + B_k) / 2.
class SketcherExport ConstraintP3Midpoint: public Constraint
{
    int comp;

public:
    ConstraintP3Midpoint(const Point3& p, const Point3& a, const Point3& b, int comp);
    ConstraintType getTypeId() override
    {
        return P3Midpoint;
    }
    void errorgrad(double* err, double* grad, double* param) override;
};

// |n| - 1: a circle's or arc's normal.
class SketcherExport ConstraintVec3Unit: public Constraint
{
public:
    explicit ConstraintVec3Unit(const Point3& n);
    ConstraintType getTypeId() override
    {
        return Vec3Unit;
    }
    void errorgrad(double* err, double* grad, double* param) override;
};

// |q| - 1: a workplane's orientation (v2, when a workplane is solved for).
class SketcherExport ConstraintQuatUnit: public Constraint
{
public:
    explicit ConstraintQuatUnit(const Plane3& pl);
    ConstraintType getTypeId() override
    {
        return QuatUnit;
    }
    void errorgrad(double* err, double* grad, double* param) override;
};

///////////////////////////////////////
// Adding them: one call per sketch constraint, as many equations as the DOF it removes.
// Each returns the index of its first equation in the system.
///////////////////////////////////////

// Entity rules (internal equations).
SketcherExport int
addConstraintCircle3Rules(System& sys, Circle3& c, int tag = 0, bool driving = true);
SketcherExport int addConstraintArc3Rules(System& sys, Arc3& a, int tag = 0, bool driving = true);
SketcherExport int
addConstraintPlane3Rules(System& sys, Plane3& pl, int tag = 0, bool driving = true);

// Coincident (3), along the frame's axis 0/1/2 (2), point on line (2, t unknown).
SketcherExport int
addConstraintP3Coincident(System& sys, Point3& p, Point3& q, int tag = 0, bool driving = true);
SketcherExport int addConstraintP3AlongAxis(System& sys,
                                            Point3& a,
                                            Point3& b,
                                            int axis,
                                            int tag = 0,
                                            bool driving = true);
SketcherExport int addConstraintP3OnLine(System& sys,
                                         Point3& p,
                                         Point3& a,
                                         Point3& b,
                                         double* t,
                                         int tag = 0,
                                         bool driving = true);
SketcherExport int
addConstraintP3OnCircle(System& sys, Point3& p, Circle3& c, int tag = 0, bool driving = true);
SketcherExport int
addConstraintP3OnArc(System& sys, Point3& p, Arc3& a, int tag = 0, bool driving = true);
SketcherExport int addConstraintP3OnBSpline(System& sys,
                                            Point3& p,
                                            BSpline3& bsp,
                                            double* t,
                                            int tag = 0,
                                            bool driving = true);
SketcherExport int addConstraintP3OnFixedCurve(System& sys,
                                               Point3& p,
                                               std::shared_ptr<const FixedCurve3> curve,
                                               double* t,
                                               int tag = 0,
                                               bool driving = true);
SketcherExport int
addConstraintP3OnPlane(System& sys, Point3& p, Plane3& pl, int tag = 0, bool driving = true);

// Directions: parallel (2), perpendicular (1), a line parallel to a plane (1),
// collinear (4, two t unknowns).
/// s: the ratio d2 = s d1, an unknown the caller allocates (initialParallelRatio).
SketcherExport int addConstraintParallel3(System& sys,
                                          const Dir3& d1,
                                          const Dir3& d2,
                                          double* s,
                                          int tag = 0,
                                          bool driving = true);
/// The s to start from: |d2| / |d1|, negative when the directions face apart -- the
/// ratio a parallel pair of their lengths has, on the side they are nearer. Not the
/// projection d1 . d2 / |d1|^2, which is 0 for perpendicular directions, where the
/// equations then have no gradient to turn them.
SketcherExport double initialParallelRatio(const Dir3& d1, const Dir3& d2);
SketcherExport int addConstraintPerpendicular3(System& sys,
                                               const Dir3& d1,
                                               const Dir3& d2,
                                               int tag = 0,
                                               bool driving = true);
SketcherExport int addConstraintLineParallelPlane3(System& sys,
                                                   Point3& a,
                                                   Point3& b,
                                                   Plane3& pl,
                                                   int tag = 0,
                                                   bool driving = true);
SketcherExport int addConstraintCollinear3(System& sys,
                                           Point3& a1,
                                           Point3& b1,
                                           Point3& a2,
                                           Point3& b2,
                                           double* t1,
                                           double* t2,
                                           int tag = 0,
                                           bool driving = true);
SketcherExport int addConstraintEqualLength3(System& sys,
                                             Point3& a1,
                                             Point3& b1,
                                             Point3& a2,
                                             Point3& b2,
                                             int tag = 0,
                                             bool driving = true);
SketcherExport int addConstraintP3Midpoint(System& sys,
                                           Point3& p,
                                           Point3& a,
                                           Point3& b,
                                           int tag = 0,
                                           bool driving = true);

// Tangency at a shared end S (the coincidence is a constraint of its own): a line A-B
// and an arc (2), and two directions in general -- arc-arc, spline-line, spline-arc (2).
SketcherExport int addConstraintTangentLineArc3(System& sys,
                                                Point3& a,
                                                Point3& b,
                                                Arc3& arc,
                                                Point3& s,
                                                int tag = 0,
                                                bool driving = true);
SketcherExport int addConstraintTangent3(System& sys,
                                         const Dir3& t1,
                                         const Dir3& t2,
                                         double* s,
                                         int tag = 0,
                                         bool driving = true);

// Dimensions (1 each).
SketcherExport int addConstraintP3Distance(System& sys,
                                           Point3& p,
                                           Point3& q,
                                           double* distance,
                                           int tag = 0,
                                           bool driving = true);
SketcherExport int addConstraintP3AxisDistance(System& sys,
                                               Point3& p,
                                               Point3& q,
                                               int axis,
                                               double* distance,
                                               int tag = 0,
                                               bool driving = true);
SketcherExport int addConstraintP3PlaneDistance(System& sys,
                                                Point3& p,
                                                Plane3& pl,
                                                double* distance,
                                                int tag = 0,
                                                bool driving = true);
SketcherExport int addConstraintP3LineDistance(System& sys,
                                               Point3& p,
                                               Point3& a,
                                               Point3& b,
                                               double* distance,
                                               int tag = 0,
                                               bool driving = true);
SketcherExport int addConstraintAngle3(System& sys,
                                       const Dir3& d1,
                                       const Dir3& d2,
                                       double* angle,
                                       int tag = 0,
                                       bool driving = true);
SketcherExport int addConstraintCircle3Radius(System& sys,
                                              Circle3& c,
                                              double* radius,
                                              int tag = 0,
                                              bool driving = true);
SketcherExport int
addConstraintArc3Radius(System& sys, Arc3& a, double* radius, int tag = 0, bool driving = true);

}  // namespace GCS
