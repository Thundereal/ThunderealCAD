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

// A surface where two parts touch, and what the contact is by the two
// surfaces' relative curvature (docs/AssemblyDynamics.md, D4b way 1): one
// rule for every pair of geometries, where steps 13 to 29 each placed the
// points of one -- a round on a round, in a bore, along a flat face, a ball.
// Plain arrays, no Chrono: the backend's flexible parts take a quadratic
// face's surface from their nodes as the modes have them, its rigid parts
// theirs from ContactShape::surfaces, and the tests take this header alone.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Assembly::Solver::LocalSurface
{

using Vec = std::array<double, 3>;
// A six-node triangle's nodes: the corners 0, 1, 2, then the nodes of the
// edges (0 1), (1 2) and (2 0), wound to face out of the part
using Face = std::array<Vec, 6>;

inline Vec add(const Vec& a, const Vec& b, double s = 1.0)
{
    return {a[0] + s * b[0], a[1] + s * b[1], a[2] + s * b[2]};
}
inline Vec scaled(const Vec& a, double s)
{
    return {s * a[0], s * a[1], s * a[2]};
}
inline double dot(const Vec& a, const Vec& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Vec cross(const Vec& a, const Vec& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double length(const Vec& a)
{
    return std::sqrt(dot(a, a));
}
inline bool normalize(Vec& a)
{
    const double l = length(a);
    if (!(l > 0.0)) {
        return false;
    }
    a = scaled(a, 1.0 / l);
    return true;
}

// A surface at a point: its outward normal, its principal curvatures,
// convex positive, k1 >= k2, and their ways, unit, in its tangent plane,
// way2 = normal x way1
struct Local
{
    Vec point {};
    Vec normal {};
    Vec way1 {};
    Vec way2 {};
    double k1 = 0.0;
    double k2 = 0.0;
};

// The surface of the quadratic face through x at area coordinates L. Its
// derivatives along s = L[1] and t = L[2]: the second ones are the face's
// own, the same all over it, the first ones where L is. False where the face
// has no tangent plane there.
inline bool localSurface(const Face& x, const std::array<double, 3>& L, Local& at)
{
    const double l0 = L[0];
    const double s = L[1];
    const double t = L[2];
    // The shape functions, their s and t derivatives and the second ones
    const std::array<double, 6> N {l0 * (2.0 * l0 - 1.0),
                                   s * (2.0 * s - 1.0),
                                   t * (2.0 * t - 1.0),
                                   4.0 * l0 * s,
                                   4.0 * s * t,
                                   4.0 * t * l0};
    const std::array<double, 6> Ns {1.0 - 4.0 * l0, 4.0 * s - 1.0, 0.0, 4.0 * (l0 - s), 4.0 * t,
                                    -4.0 * t};
    const std::array<double, 6> Nt {1.0 - 4.0 * l0, 0.0, 4.0 * t - 1.0, -4.0 * s, 4.0 * s,
                                    4.0 * (l0 - t)};
    const std::array<double, 6> Nss {4.0, 4.0, 0.0, -8.0, 0.0, 0.0};
    const std::array<double, 6> Ntt {4.0, 0.0, 4.0, 0.0, 0.0, -8.0};
    const std::array<double, 6> Nst {4.0, 0.0, 0.0, -4.0, 4.0, -4.0};
    Vec p {};
    Vec xs {};
    Vec xt {};
    Vec xss {};
    Vec xtt {};
    Vec xst {};
    for (std::size_t k = 0; k < 6; ++k) {
        p = add(p, x[k], N[k]);
        xs = add(xs, x[k], Ns[k]);
        xt = add(xt, x[k], Nt[k]);
        xss = add(xss, x[k], Nss[k]);
        xtt = add(xtt, x[k], Ntt[k]);
        xst = add(xst, x[k], Nst[k]);
    }
    Vec n = cross(xs, xt);
    const double area = length(n);
    const double reach = std::max(dot(xs, xs), dot(xt, xt));
    if (!(area > 1e-12 * reach) || !normalize(n)) {
        return false;
    }
    // The first fundamental form, and the second, convex positive: an
    // outward normal, the surface bending away from it
    const double E = dot(xs, xs);
    const double F = dot(xs, xt);
    const double G = dot(xt, xt);
    const double e = -dot(xss, n);
    const double f = -dot(xst, n);
    const double g = -dot(xtt, n);
    const double det = E * G - F * F;
    if (!(det > 0.0)) {
        return false;
    }
    // det(II - k I) = 0: k = H +- sqrt(H^2 - K)
    const double H = (e * G - 2.0 * f * F + g * E) / (2.0 * det);
    const double K = (e * g - f * f) / det;
    const double root = std::sqrt(std::max(0.0, H * H - K));
    at.point = p;
    at.normal = n;
    at.k1 = H + root;
    at.k2 = H - root;
    // k1's way: (II - k1 I) (a, b) = 0, by the row the larger
    Vec way {};
    {
        const std::array<double, 2> one {f - at.k1 * F, -(e - at.k1 * E)};
        const std::array<double, 2> two {g - at.k1 * G, -(f - at.k1 * F)};
        const auto& ab = std::hypot(one[0], one[1]) >= std::hypot(two[0], two[1]) ? one : two;
        way = add(scaled(xs, ab[0]), xt, ab[1]);
    }
    // Where the two are one (a ball's, a plane's), any way: the s way
    if (!(root > 1e-9 * (std::abs(H) + 1.0 / std::sqrt(reach))) || !normalize(way)) {
        way = xs;
        normalize(way);
    }
    // In the tangent plane exactly
    way = add(way, n, -dot(way, n));
    normalize(way);
    at.way1 = way;
    at.way2 = cross(n, way);
    return true;
}

// What two surfaces touching are, by their relative curvature: the sum of
// the two curvature tensors in A's tangent plane, its principal values k1
// >= k2 and their ways. Pressed 'depth' together, the surfaces are within it
// of each other along a principal way over a width 2 sqrt(2 depth / k) --
// none past where k is none or below. A way is flat where that width is
// more than 'reach', how far the two shapes reach to touch that way (the
// smaller's size): the contact is a point where neither is, a line along
// the flat way where one is, an area where both are. Not by a face's size:
// a rod R 5 in a bore R 5.5 pressed 50 um touches 4.7 mm round, past a 4 mm
// mesh's face, and is a line -- two points at its ends hold its force to
// 0.05% (step 29).
struct Contact
{
    enum class Kind
    {
        Point,
        Line,
        Area
    };
    Kind kind = Kind::Point;
    Vec normal {};  // A's
    Vec way1 {};
    Vec way2 {};
    double k1 = 0.0;
    double k2 = 0.0;
    double width1 = 0.0;  // along way1, infinity where flat
    double width2 = 0.0;
    // A line's: the way it runs, and its width across
    Vec along {};
    double across = 0.0;
};

// A surface's curvature along a unit way of its tangent plane (Euler's)
inline double curvatureAlong(const Local& surface, const Vec& way)
{
    const double c = dot(way, surface.way1);
    const double s = dot(way, surface.way2);
    return surface.k1 * c * c + surface.k2 * s * s;
}

inline Contact contactOf(const Local& a, const Local& b, double depth, double reach)
{
    Contact c;
    c.normal = a.normal;
    // Both tensors on A's tangent plane's ways, a.way1 and a.way2: B's
    // normal the other way, its ways into that plane
    const Vec u = a.way1;
    const Vec v = a.way2;
    auto onPlane = [&](const Local& surface) {
        // The tensor k1 w1 w1' + k2 w2 w2' as a 2 x 2 on (u, v)
        const double w1u = dot(surface.way1, u);
        const double w1v = dot(surface.way1, v);
        const double w2u = dot(surface.way2, u);
        const double w2v = dot(surface.way2, v);
        return std::array<double, 3> {surface.k1 * w1u * w1u + surface.k2 * w2u * w2u,
                                      surface.k1 * w1u * w1v + surface.k2 * w2u * w2v,
                                      surface.k1 * w1v * w1v + surface.k2 * w2v * w2v};
    };
    const auto ta = onPlane(a);
    const auto tb = onPlane(b);
    const double xx = ta[0] + tb[0];
    const double xy = ta[1] + tb[1];
    const double yy = ta[2] + tb[2];
    const double mean = 0.5 * (xx + yy);
    const double root = std::sqrt(std::max(0.0, 0.25 * (xx - yy) * (xx - yy) + xy * xy));
    c.k1 = mean + root;
    c.k2 = mean - root;
    // k1's way on (u, v)
    double angle = 0.5 * std::atan2(2.0 * xy, xx - yy);
    c.way1 = add(scaled(u, std::cos(angle)), v, std::sin(angle));
    normalize(c.way1);
    c.way2 = cross(c.normal, c.way1);
    const double flat = std::numeric_limits<double>::infinity();
    auto widthOf = [&](double k) {
        return k > 0.0 && depth > 0.0 ? 2.0 * std::sqrt(2.0 * depth / k) : flat;
    };
    c.width1 = widthOf(c.k1);
    c.width2 = widthOf(c.k2);
    const bool flat1 = !(c.width1 <= reach);
    const bool flat2 = !(c.width2 <= reach);
    if (flat1 && flat2) {
        c.kind = Contact::Kind::Area;
    }
    else if (flat2) {
        c.kind = Contact::Kind::Line;
        c.along = c.way2;
        c.across = c.width1;
    }
    else {
        c.kind = Contact::Kind::Point;
    }
    return c;
}

}  // namespace Assembly::Solver::LocalSurface
