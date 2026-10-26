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

#include <cmath>

#include "Constraints3D.h"

namespace GCS
{

static inline double dvar(const double* p, const double* derivparam)
{
    return p == derivparam ? 1.0 : 0.0;
}

// g / (l1 l2) and its derivative.
static inline double
normalised(double g, double dg, double l1, double dl1, double l2, double dl2, double& d)
{
    double l = l1 * l2;
    if (l == 0.) {
        d = 0.;
        return 0.;
    }
    double dl = dl1 * l2 + l1 * dl2;
    d = dg / l - g * dl / (l * l);
    return g / l;
}

static inline void output(double* err, double* grad, double e, double de)
{
    if (err) {
        *err = e;
    }
    if (grad) {
        *grad = de;
    }
}

// --------------------------------------------------------
// Dir3

Dir3 Dir3::vector(const Point3& v)
{
    Dir3 d;
    d.kind = Kind::Vector;
    v.push(d.params);
    return d;
}

Dir3 Dir3::diff(const Point3& a, const Point3& b)
{
    Dir3 d;
    d.kind = Kind::Diff;
    a.push(d.params);
    b.push(d.params);
    return d;
}

Dir3 Dir3::crossDiff(const Point3& n, const Point3& a, const Point3& b)
{
    Dir3 d;
    d.kind = Kind::CrossDiff;
    n.push(d.params);
    a.push(d.params);
    b.push(d.params);
    return d;
}

Dir3 Dir3::planeNormal(const Plane3& pl)
{
    return planeAxis(pl, 2);
}

Dir3 Dir3::planeAxis(const Plane3& pl, int axis)
{
    Dir3 d;
    d.kind = Kind::QuatAxis;
    d.axis = axis;
    d.params = {pl.qw, pl.qx, pl.qy, pl.qz};
    return d;
}

DeriVector3 Dir3::eval(const VEC_pD& pvec, std::size_t i, const double* derivparam) const
{
    switch (kind) {
        case Kind::Vector:
            return {pvec, i, derivparam};
        case Kind::Diff:
            return DeriVector3(pvec, i + 3, derivparam).subtr(DeriVector3(pvec, i, derivparam));
        case Kind::CrossDiff: {
            DeriVector3 n(pvec, i, derivparam);
            DeriVector3 r =
                DeriVector3(pvec, i + 6, derivparam).subtr(DeriVector3(pvec, i + 3, derivparam));
            return n.cross(r);
        }
        case Kind::QuatAxis:
            return quaternionAxis(pvec, i, axis, derivparam);
    }
    return {};
}

DeriVector3 Dir3::current() const
{
    return eval(params, 0, nullptr);
}

// --------------------------------------------------------
// P3OnLine: pvec = P, A, B, t

ConstraintP3OnLine::ConstraintP3OnLine(const Point3& p,
                                       const Point3& a,
                                       const Point3& b,
                                       double* t,
                                       int comp)
    : comp(comp)
{
    p.push(pvec);
    a.push(pvec);
    b.push(pvec);
    pvec.push_back(t);
    origpvec = pvec;
    rescale();
}

void ConstraintP3OnLine::errorgrad(double* err, double* grad, double* param)
{
    DeriVector3 P(pvec, 0, param), A(pvec, 3, param), B(pvec, 6, param);
    double t = *pvec[9], dt = dvar(pvec[9], param);
    double dp, da, db;
    double p = P.component(comp, dp), a = A.component(comp, da), b = B.component(comp, db);
    output(err, grad, p - a - t * (b - a), dp - da - dt * (b - a) - t * (db - da));
}

// --------------------------------------------------------
// P3OnBSpline: pvec = P, t, poles, weights

ConstraintP3OnBSpline::ConstraintP3OnBSpline(const Point3& p,
                                             const BSpline3& bsp,
                                             double* t,
                                             int comp)
    : comp(comp)
    , npoles(bsp.poles.size())
    , flatknots(bsp.flatknots)
    , degree(bsp.degree)
{
    p.push(pvec);
    pvec.push_back(t);
    bsp.push(pvec);
    origpvec = pvec;
    rescale();
}

void ConstraintP3OnBSpline::errorgrad(double* err, double* grad, double* param)
{
    DeriVector3 P(pvec, 0, param);
    double t = *pvec[3], dt = dvar(pvec[3], param);
    BSpline3View bsp(pvec, 4, npoles, flatknots, degree);
    DeriVector3 C = bsp.value(t, dt, param);
    double dp, dc;
    double p = P.component(comp, dp), c = C.component(comp, dc);
    output(err, grad, p - c, dp - dc);
}

// --------------------------------------------------------
// P3OnFixedCurve: pvec = P, t

ConstraintP3OnFixedCurve::ConstraintP3OnFixedCurve(const Point3& p,
                                                   std::shared_ptr<const FixedCurve3> curve,
                                                   double* t,
                                                   int comp)
    : comp(comp)
    , curve(std::move(curve))
{
    p.push(pvec);
    pvec.push_back(t);
    origpvec = pvec;
    rescale();
}

void ConstraintP3OnFixedCurve::errorgrad(double* err, double* grad, double* param)
{
    DeriVector3 P(pvec, 0, param);
    double t = *pvec[3], dt = dvar(pvec[3], param);
    double c[3], d1[3];
    curve->value(t, c, d1);
    double dp;
    double p = P.component(comp, dp);
    output(err, grad, p - c[comp], dp - dt * d1[comp]);
}

// --------------------------------------------------------
// P3PlaneDistance: pvec = P, O, q, [D]

ConstraintP3PlaneDistance::ConstraintP3PlaneDistance(const Point3& p,
                                                     const Plane3& pl,
                                                     double* distance)
    : hasDistance(distance != nullptr)
{
    p.push(pvec);
    pl.push(pvec);
    if (distance) {
        pvec.push_back(distance);
    }
    origpvec = pvec;
    rescale();
}

void ConstraintP3PlaneDistance::errorgrad(double* err, double* grad, double* param)
{
    DeriVector3 v = DeriVector3(pvec, 0, param).subtr(DeriVector3(pvec, 3, param));
    DeriVector3 n = quaternionNormal(pvec, 6, param);
    double dd;
    double d = v.dot(n, dd);
    if (hasDistance) {
        d -= *pvec[10];
        dd -= dvar(pvec[10], param);
    }
    output(err, grad, d, dd);
}

void ConstraintP3PlaneDistance::evaluate()
{
    if (hasDistance) {
        DeriVector3 v = DeriVector3(pvec, 0, nullptr).subtr(DeriVector3(pvec, 3, nullptr));
        double dd;
        *pvec[10] = v.dot(quaternionNormal(pvec, 6, nullptr), dd);
    }
}

// --------------------------------------------------------
// Dir3Parallel, Dir3Dot, Dir3Angle: pvec = d1's params, d2's params (, s or angle)

ConstraintDir3Parallel::ConstraintDir3Parallel(const Dir3& d1, const Dir3& d2, double* s, int comp)
    : d1(d1)
    , d2(d2)
    , comp(comp)
{
    pvec = d1.params;
    pvec.insert(pvec.end(), d2.params.begin(), d2.params.end());
    pvec.push_back(s);
    origpvec = pvec;
    rescale();
}

void ConstraintDir3Parallel::errorgrad(double* err, double* grad, double* param)
{
    DeriVector3 v1 = d1.eval(pvec, 0, param), v2 = d2.eval(pvec, d1.params.size(), param);
    double* s = pvec.back();
    double d1c, d2c;
    double c1 = v1.component(comp, d1c), c2 = v2.component(comp, d2c);
    output(err, grad, c2 - *s * c1, d2c - *s * d1c - dvar(s, param) * c1);
}

ConstraintDir3Dot::ConstraintDir3Dot(const Dir3& d1, const Dir3& d2)
    : d1(d1)
    , d2(d2)
{
    pvec = d1.params;
    pvec.insert(pvec.end(), d2.params.begin(), d2.params.end());
    origpvec = pvec;
    rescale();
}

void ConstraintDir3Dot::errorgrad(double* err, double* grad, double* param)
{
    DeriVector3 v1 = d1.eval(pvec, 0, param), v2 = d2.eval(pvec, d1.params.size(), param);
    double dl1, dl2, dg, d;
    double l1 = v1.length(dl1), l2 = v2.length(dl2);
    double g = v1.dot(v2, dg);
    double e = normalised(g, dg, l1, dl1, l2, dl2, d);
    output(err, grad, e, d);
}

ConstraintDir3Angle::ConstraintDir3Angle(const Dir3& d1, const Dir3& d2, double* angle)
    : d1(d1)
    , d2(d2)
{
    pvec = d1.params;
    pvec.insert(pvec.end(), d2.params.begin(), d2.params.end());
    pvec.push_back(angle);
    origpvec = pvec;
    rescale();
}

double ConstraintDir3Angle::value(double* dvalue, const double* param)
{
    DeriVector3 v1 = d1.eval(pvec, 0, param), v2 = d2.eval(pvec, d1.params.size(), param);
    double ds, dc;
    double s = v1.cross(v2).length(ds);
    double c = v1.dot(v2, dc);
    if (dvalue) {
        double q = s * s + c * c;
        *dvalue = q == 0. ? 0. : (c * ds - s * dc) / q;
    }
    return std::atan2(s, c);
}

void ConstraintDir3Angle::errorgrad(double* err, double* grad, double* param)
{
    double* angle = pvec.back();
    double dv;
    double v = value(&dv, param);
    output(err, grad, v - *angle, dv - dvar(angle, param));
}

void ConstraintDir3Angle::evaluate()
{
    *pvec.back() = value(nullptr, nullptr);
}

// --------------------------------------------------------
// P3Distance: pvec = P, Q, D

ConstraintP3Distance::ConstraintP3Distance(const Point3& p, const Point3& q, double* distance)
{
    p.push(pvec);
    q.push(pvec);
    pvec.push_back(distance);
    origpvec = pvec;
    rescale();
}

void ConstraintP3Distance::errorgrad(double* err, double* grad, double* param)
{
    DeriVector3 v = DeriVector3(pvec, 0, param).subtr(DeriVector3(pvec, 3, param));
    double dl;
    double l = v.length(dl);
    output(err, grad, l - *pvec[6], dl - dvar(pvec[6], param));
}

void ConstraintP3Distance::evaluate()
{
    *pvec[6] = DeriVector3(pvec, 0, nullptr).subtr(DeriVector3(pvec, 3, nullptr)).length();
}

// --------------------------------------------------------
// P3EqualDistance: pvec = A, B, C, D

ConstraintP3EqualDistance::ConstraintP3EqualDistance(const Point3& a,
                                                     const Point3& b,
                                                     const Point3& c,
                                                     const Point3& d)
{
    a.push(pvec);
    b.push(pvec);
    c.push(pvec);
    d.push(pvec);
    origpvec = pvec;
    rescale();
}

void ConstraintP3EqualDistance::errorgrad(double* err, double* grad, double* param)
{
    DeriVector3 u = DeriVector3(pvec, 3, param).subtr(DeriVector3(pvec, 0, param));
    DeriVector3 v = DeriVector3(pvec, 9, param).subtr(DeriVector3(pvec, 6, param));
    double du, dv;
    double lu = u.length(du), lv = v.length(dv);
    output(err, grad, lu - lv, du - dv);
}

// --------------------------------------------------------
// P3LineDistance: pvec = P, A, B, D

ConstraintP3LineDistance::ConstraintP3LineDistance(const Point3& p,
                                                   const Point3& a,
                                                   const Point3& b,
                                                   double* distance)
{
    p.push(pvec);
    a.push(pvec);
    b.push(pvec);
    pvec.push_back(distance);
    origpvec = pvec;
    rescale();
}

double ConstraintP3LineDistance::value(double* dvalue, const double* param)
{
    DeriVector3 A(pvec, 3, param);
    DeriVector3 u = DeriVector3(pvec, 6, param).subtr(A);
    DeriVector3 w = DeriVector3(pvec, 0, param).subtr(A);
    double dc, du;
    double c = w.cross(u).length(dc);
    double l = u.length(du);
    if (l == 0.) {
        if (dvalue) {
            *dvalue = 0.;
        }
        return 0.;
    }
    if (dvalue) {
        *dvalue = (dc * l - c * du) / (l * l);
    }
    return c / l;
}

void ConstraintP3LineDistance::errorgrad(double* err, double* grad, double* param)
{
    double dv;
    double v = value(&dv, param);
    output(err, grad, v - *pvec[9], dv - dvar(pvec[9], param));
}

void ConstraintP3LineDistance::evaluate()
{
    *pvec[9] = value(nullptr, nullptr);
}

// --------------------------------------------------------
// P3Midpoint: pvec = P, A, B

ConstraintP3Midpoint::ConstraintP3Midpoint(const Point3& p,
                                           const Point3& a,
                                           const Point3& b,
                                           int comp)
    : comp(comp)
{
    p.push(pvec);
    a.push(pvec);
    b.push(pvec);
    origpvec = pvec;
    rescale();
}

void ConstraintP3Midpoint::errorgrad(double* err, double* grad, double* param)
{
    DeriVector3 P(pvec, 0, param), A(pvec, 3, param), B(pvec, 6, param);
    double dp, da, db;
    double p = P.component(comp, dp), a = A.component(comp, da), b = B.component(comp, db);
    output(err, grad, p - 0.5 * (a + b), dp - 0.5 * (da + db));
}

// --------------------------------------------------------
// Vec3Unit: pvec = n; QuatUnit: pvec = q

ConstraintVec3Unit::ConstraintVec3Unit(const Point3& n)
{
    n.push(pvec);
    origpvec = pvec;
    rescale();
}

void ConstraintVec3Unit::errorgrad(double* err, double* grad, double* param)
{
    double dl;
    double l = DeriVector3(pvec, 0, param).length(dl);
    output(err, grad, l - 1., dl);
}

ConstraintQuatUnit::ConstraintQuatUnit(const Plane3& pl)
{
    pvec = {pl.qw, pl.qx, pl.qy, pl.qz};
    origpvec = pvec;
    rescale();
}

void ConstraintQuatUnit::errorgrad(double* err, double* grad, double* param)
{
    double s = 0., ds = 0.;
    for (auto* q : pvec) {
        s += *q * *q;
        ds += 2. * *q * dvar(q, param);
    }
    double l = std::sqrt(s);
    output(err, grad, l - 1., l == 0. ? 0. : ds / (2. * l));
}

///////////////////////////////////////
// Adders
///////////////////////////////////////

static int add(System& sys, Constraint* c, int tag, bool driving)
{
    c->setTag(tag);
    c->setDriving(driving);
    return sys.addConstraint(c);
}

static double* coord(Point3& p, int axis)
{
    return axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
}

int addConstraintCircle3Rules(System& sys, Circle3& c, int tag, bool driving)
{
    return add(sys, new ConstraintVec3Unit(c.normal), tag, driving);
}

int addConstraintArc3Rules(System& sys, Arc3& a, int tag, bool driving)
{
    // |n| = 1, |s - c| = |e - c|, s and e in the plane through c normal to n.
    int i = add(sys, new ConstraintVec3Unit(a.normal), tag, driving);
    add(sys, new ConstraintP3EqualDistance(a.center, a.start, a.center, a.end), tag, driving);
    add(sys,
        new ConstraintDir3Dot(Dir3::diff(a.center, a.start), Dir3::vector(a.normal)),
        tag,
        driving);
    add(sys,
        new ConstraintDir3Dot(Dir3::diff(a.center, a.end), Dir3::vector(a.normal)),
        tag,
        driving);
    return i;
}

int addConstraintPlane3Rules(System& sys, Plane3& pl, int tag, bool driving)
{
    return add(sys, new ConstraintQuatUnit(pl), tag, driving);
}

int addConstraintP3Coincident(System& sys, Point3& p, Point3& q, int tag, bool driving)
{
    int i = sys.addConstraintEqual(p.x, q.x, tag, driving);
    sys.addConstraintEqual(p.y, q.y, tag, driving);
    sys.addConstraintEqual(p.z, q.z, tag, driving);
    return i;
}

int addConstraintP3AlongAxis(System& sys, Point3& a, Point3& b, int axis, int tag, bool driving)
{
    int i = -1;
    for (int k = 0; k < 3; ++k) {
        if (k != axis) {
            int j = sys.addConstraintEqual(coord(a, k), coord(b, k), tag, driving);
            if (i < 0) {
                i = j;
            }
        }
    }
    return i;
}

int addConstraintP3OnLine(System& sys,
                          Point3& p,
                          Point3& a,
                          Point3& b,
                          double* t,
                          int tag,
                          bool driving)
{
    int i = add(sys, new ConstraintP3OnLine(p, a, b, t, 0), tag, driving);
    add(sys, new ConstraintP3OnLine(p, a, b, t, 1), tag, driving);
    add(sys, new ConstraintP3OnLine(p, a, b, t, 2), tag, driving);
    return i;
}

int addConstraintP3OnCircle(System& sys, Point3& p, Circle3& c, int tag, bool driving)
{
    int i = add(sys, new ConstraintP3Distance(p, c.center, c.rad), tag, driving);
    add(sys, new ConstraintDir3Dot(Dir3::diff(c.center, p), Dir3::vector(c.normal)), tag, driving);
    return i;
}

int addConstraintP3OnArc(System& sys, Point3& p, Arc3& a, int tag, bool driving)
{
    int i = add(sys, new ConstraintP3EqualDistance(a.center, p, a.center, a.start), tag, driving);
    add(sys, new ConstraintDir3Dot(Dir3::diff(a.center, p), Dir3::vector(a.normal)), tag, driving);
    return i;
}

int addConstraintP3OnBSpline(System& sys,
                             Point3& p,
                             BSpline3& bsp,
                             double* t,
                             int tag,
                             bool driving)
{
    int i = add(sys, new ConstraintP3OnBSpline(p, bsp, t, 0), tag, driving);
    add(sys, new ConstraintP3OnBSpline(p, bsp, t, 1), tag, driving);
    add(sys, new ConstraintP3OnBSpline(p, bsp, t, 2), tag, driving);
    return i;
}

int addConstraintP3OnFixedCurve(System& sys,
                                Point3& p,
                                std::shared_ptr<const FixedCurve3> curve,
                                double* t,
                                int tag,
                                bool driving)
{
    int i = add(sys, new ConstraintP3OnFixedCurve(p, curve, t, 0), tag, driving);
    add(sys, new ConstraintP3OnFixedCurve(p, curve, t, 1), tag, driving);
    add(sys, new ConstraintP3OnFixedCurve(p, curve, t, 2), tag, driving);
    return i;
}

int addConstraintP3OnPlane(System& sys, Point3& p, Plane3& pl, int tag, bool driving)
{
    return add(sys, new ConstraintP3PlaneDistance(p, pl, nullptr), tag, driving);
}

double initialParallelRatio(const Dir3& d1, const Dir3& d2)
{
    DeriVector3 v1 = d1.current(), v2 = d2.current();
    double dd;
    double l1 = v1.length(), l2 = v2.length();
    if (l1 == 0.) {
        return 1.;
    }
    return v1.dot(v2, dd) < 0. ? -l2 / l1 : l2 / l1;
}

int addConstraintParallel3(System& sys,
                           const Dir3& d1,
                           const Dir3& d2,
                           double* s,
                           int tag,
                           bool driving)
{
    int i = add(sys, new ConstraintDir3Parallel(d1, d2, s, 0), tag, driving);
    add(sys, new ConstraintDir3Parallel(d1, d2, s, 1), tag, driving);
    add(sys, new ConstraintDir3Parallel(d1, d2, s, 2), tag, driving);
    return i;
}

int addConstraintPerpendicular3(System& sys, const Dir3& d1, const Dir3& d2, int tag, bool driving)
{
    return add(sys, new ConstraintDir3Dot(d1, d2), tag, driving);
}

int addConstraintLineParallelPlane3(System& sys,
                                    Point3& a,
                                    Point3& b,
                                    Plane3& pl,
                                    int tag,
                                    bool driving)
{
    return add(sys, new ConstraintDir3Dot(Dir3::diff(a, b), Dir3::planeNormal(pl)), tag, driving);
}

int addConstraintCollinear3(System& sys,
                            Point3& a1,
                            Point3& b1,
                            Point3& a2,
                            Point3& b2,
                            double* t1,
                            double* t2,
                            int tag,
                            bool driving)
{
    int i = addConstraintP3OnLine(sys, a2, a1, b1, t1, tag, driving);
    addConstraintP3OnLine(sys, b2, a1, b1, t2, tag, driving);
    return i;
}

int addConstraintEqualLength3(System& sys,
                              Point3& a1,
                              Point3& b1,
                              Point3& a2,
                              Point3& b2,
                              int tag,
                              bool driving)
{
    return add(sys, new ConstraintP3EqualDistance(a1, b1, a2, b2), tag, driving);
}

int addConstraintP3Midpoint(System& sys, Point3& p, Point3& a, Point3& b, int tag, bool driving)
{
    int i = add(sys, new ConstraintP3Midpoint(p, a, b, 0), tag, driving);
    add(sys, new ConstraintP3Midpoint(p, a, b, 1), tag, driving);
    add(sys, new ConstraintP3Midpoint(p, a, b, 2), tag, driving);
    return i;
}

int addConstraintTangentLineArc3(System& sys,
                                 Point3& a,
                                 Point3& b,
                                 Arc3& arc,
                                 Point3& s,
                                 int tag,
                                 bool driving)
{
    // The line is perpendicular to the radius at S and lies in the arc's plane.
    int i =
        add(sys, new ConstraintDir3Dot(Dir3::diff(a, b), Dir3::diff(arc.center, s)), tag, driving);
    add(sys, new ConstraintDir3Dot(Dir3::diff(a, b), Dir3::vector(arc.normal)), tag, driving);
    return i;
}

int addConstraintTangent3(System& sys,
                          const Dir3& t1,
                          const Dir3& t2,
                          double* s,
                          int tag,
                          bool driving)
{
    return addConstraintParallel3(sys, t1, t2, s, tag, driving);
}

int addConstraintP3Distance(System& sys,
                            Point3& p,
                            Point3& q,
                            double* distance,
                            int tag,
                            bool driving)
{
    return add(sys, new ConstraintP3Distance(p, q, distance), tag, driving);
}

int addConstraintP3AxisDistance(System& sys,
                                Point3& p,
                                Point3& q,
                                int axis,
                                double* distance,
                                int tag,
                                bool driving)
{
    // q - p along the axis.
    return sys.addConstraintDifference(coord(p, axis), coord(q, axis), distance, tag, driving);
}

int addConstraintP3PlaneDistance(System& sys,
                                 Point3& p,
                                 Plane3& pl,
                                 double* distance,
                                 int tag,
                                 bool driving)
{
    return add(sys, new ConstraintP3PlaneDistance(p, pl, distance), tag, driving);
}

int addConstraintP3LineDistance(System& sys,
                                Point3& p,
                                Point3& a,
                                Point3& b,
                                double* distance,
                                int tag,
                                bool driving)
{
    return add(sys, new ConstraintP3LineDistance(p, a, b, distance), tag, driving);
}

int addConstraintAngle3(System& sys,
                        const Dir3& d1,
                        const Dir3& d2,
                        double* angle,
                        int tag,
                        bool driving)
{
    return add(sys, new ConstraintDir3Angle(d1, d2, angle), tag, driving);
}

int addConstraintCircle3Radius(System& sys, Circle3& c, double* radius, int tag, bool driving)
{
    return sys.addConstraintEqual(c.rad, radius, tag, driving);
}

int addConstraintArc3Radius(System& sys, Arc3& a, double* radius, int tag, bool driving)
{
    return add(sys, new ConstraintP3Distance(a.start, a.center, radius), tag, driving);
}

}  // namespace GCS
