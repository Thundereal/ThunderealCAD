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

// The 3D layer of PlaneGCS (docs/Sketch3D.md section 4). The solver core works on a
// vector of parameter pointers and does not care about dimension; what is 2D is the
// geometry and the constraints. This header adds the 3D geometry beside the 2D one,
// and Constraints3D.h the constraints that use it.

#include <memory>

#include "../../SketcherGlobal.h"
#include "Util.h"

namespace GCS
{

/// A point in 3D: three parameter pointers, as Point holds two.
class SketcherExport Point3
{
public:
    Point3() = default;
    Point3(double* px, double* py, double* pz)
        : x(px)
        , y(py)
        , z(pz)
    {}
    double* x {nullptr};
    double* y {nullptr};
    double* z {nullptr};

    void push(VEC_pD& pvec) const
    {
        pvec.push_back(x);
        pvec.push_back(y);
        pvec.push_back(z);
    }
};

/// A 3D vector and its derivative by one parameter, as DeriVector2 is in 2D: x, y, z
/// is the value, dx, dy, dz its derivative by the parameter the gradient is being
/// computed for.
class SketcherExport DeriVector3
{
public:
    DeriVector3() = default;
    DeriVector3(double x, double y, double z)
        : x(x)
        , y(y)
        , z(z)
    {}
    DeriVector3(double x, double y, double z, double dx, double dy, double dz)
        : x(x)
        , dx(dx)
        , y(y)
        , dy(dy)
        , z(z)
        , dz(dz)
    {}
    /// Reads the point's values; each derivative is 1 where that coordinate's pointer is
    /// derivparam.
    DeriVector3(const Point3& p, const double* derivparam);
    /// The same from three consecutive entries of a parameter vector.
    DeriVector3(const VEC_pD& pvec, std::size_t i, const double* derivparam);

    double x {0}, dx {0};
    double y {0}, dy {0};
    double z {0}, dz {0};

    double length() const;
    /// The length, and its derivative into dlength.
    double length(double& dlength) const;
    double dot(const DeriVector3& v, double& ddot) const;
    DeriVector3 cross(const DeriVector3& v) const;
    /// Component i (0, 1, 2), and its derivative into dc.
    double component(int i, double& dc) const;

    DeriVector3 sum(const DeriVector3& v) const
    {
        return {x + v.x, y + v.y, z + v.z, dx + v.dx, dy + v.dy, dz + v.dz};
    }
    DeriVector3 subtr(const DeriVector3& v) const
    {
        return {x - v.x, y - v.y, z - v.z, dx - v.dx, dy - v.dy, dz - v.dz};
    }
    DeriVector3 mult(double val) const
    {
        return {x * val, y * val, z * val, dx * val, dy * val, dz * val};
    }
    /// Multiplies by a value that has a derivative of its own.
    DeriVector3 multD(double val, double dval) const
    {
        return {x * val,
                y * val,
                z * val,
                dx * val + x * dval,
                dy * val + y * dval,
                dz * val + z * dval};
    }
};

/// The unit normal of a frame given by a quaternion (w, x, y, z): the frame's Z axis.
/// Computed as R(q) (0, 0, 1) / |q|^2, so it is a unit vector even while the solver has
/// q off the unit sphere. The four pointers start at pvec[i].
SketcherExport DeriVector3 quaternionNormal(const VEC_pD& pvec,
                                            std::size_t i,
                                            const double* derivparam);

/// A workplane: an origin and an orientation as a quaternion. Its normal is the
/// orientation's Z axis (quaternionNormal). Fixed in v1 (its parameters are not
/// declared unknown), constrainable in v2 (docs/Sketch3D.md section 6).
class SketcherExport Plane3
{
public:
    Point3 origin;
    double* qw {nullptr};
    double* qx {nullptr};
    double* qy {nullptr};
    double* qz {nullptr};

    void push(VEC_pD& pvec) const
    {
        origin.push(pvec);
        pvec.push_back(qw);
        pvec.push_back(qx);
        pvec.push_back(qy);
        pvec.push_back(qz);
    }
};

/// A circle: centre, normal (kept unit by an internal constraint) and radius.
class SketcherExport Circle3
{
public:
    Point3 center;
    Point3 normal;
    double* rad {nullptr};
};

/// An arc: centre, normal, start and end, 8 DOF after its four internal equations
/// (addConstraintArc3Rules). It runs counterclockwise about the normal from start to
/// end; its radius is |start - centre|.
class SketcherExport Arc3
{
public:
    Point3 center;
    Point3 normal;
    Point3 start;
    Point3 end;
};

/// A non-periodic rational B-spline with a fixed degree and fixed knots. The poles and
/// weights are parameters; the flattened knot vector is a value.
class SketcherExport BSpline3
{
public:
    std::vector<Point3> poles;
    VEC_pD weights;
    VEC_D flatknots;  // size = poles + degree + 1, clamped
    int degree {3};

    void push(VEC_pD& pvec) const;
    /// The number of parameters push() adds.
    std::size_t paramCount() const
    {
        return poles.size() * 4;
    }
};

/// The geometry of a BSpline3 read from a parameter vector, as Constraint evaluates it:
/// poles from pvec[i], then the weights.
class SketcherExport BSpline3View
{
public:
    BSpline3View(const VEC_pD& pvec,
                 std::size_t i,
                 std::size_t npoles,
                 const VEC_D& flatknots,
                 int degree)
        : pvec(pvec)
        , first(i)
        , npoles(npoles)
        , flatknots(flatknots)
        , degree(degree)
    {}

    /// The point at parameter u, with its derivative by derivparam. du is the derivative
    /// of u itself by derivparam (1 when u is the parameter being differentiated), so
    /// the result carries du * C'(u) as well.
    DeriVector3 value(double u, double du, const double* derivparam) const;
    /// The tangent C'(u) (its derivative by derivparam excluding u's own variation).
    DeriVector3 tangent(double u, const double* derivparam) const;

private:
    const VEC_pD& pvec;
    std::size_t first;
    std::size_t npoles;
    const VEC_D& flatknots;
    int degree;

    // Index of the knot span containing u.
    std::size_t span(double u) const;
    // Rational value and first derivative by u, without derivatives by parameters, plus
    // the basis function values N and derivatives dN in the span.
    void eval(double u, std::size_t k, VEC_D& N, VEC_D& dN) const;
};

/// A curve that is not solved for -- a model edge. The 3D sketch's solver facade wraps
/// an OCCT curve in one; the solver only needs a point and the first derivative.
class SketcherExport FixedCurve3
{
public:
    virtual ~FixedCurve3() = default;
    virtual void value(double u, double p[3], double d1[3]) const = 0;
};

}  // namespace GCS
