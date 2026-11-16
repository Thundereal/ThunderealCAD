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

// The six-node face's shape functions at area coordinates L
inline std::array<double, 6> quadratic(const std::array<double, 3>& L)
{
    const double l0 = L[0];
    const double s = L[1];
    const double t = L[2];
    return {l0 * (2.0 * l0 - 1.0),
            s * (2.0 * s - 1.0),
            t * (2.0 * t - 1.0),
            4.0 * l0 * s,
            4.0 * s * t,
            4.0 * t * l0};
}

// A face's true surface (D4b way 1 step 3): a six-node face goes through
// its round's points at its nodes and stands in from it between them, 3.9
// um on R 5 at a 4 mm mesh -- a tenth of what a rod is pressed. The solid's
// own surface at nine more points of the face, as offsets from the face
// there at rest (Flexible::trueFaces), and a quartic triangle through the
// fifteen: the points L = (i, j, k) / 4 over its corners, but the six the
// face goes through, in this order. Offsets, not points: carried by the
// face as its nodes move, they are the solid's surface where the part bends
// -- a strain's turn of them is a micron's of a micron
inline constexpr std::array<std::array<int, 3>, 9> quarticPoints {{{3, 1, 0},
                                                                   {1, 3, 0},
                                                                   {0, 3, 1},
                                                                   {0, 1, 3},
                                                                   {1, 0, 3},
                                                                   {3, 0, 1},
                                                                   {2, 1, 1},
                                                                   {1, 2, 1},
                                                                   {1, 1, 2}}};
using Offsets = std::array<Vec, 9>;

// The quartic lattice's factor of a point m quarters along: prod over q < m
// of (4 l - q) / (q + 1), and its derivative in l
inline void latticeFactor(int m, double l, double& value, double& slope)
{
    value = 1.0;
    slope = 0.0;
    for (int q = 0; q < m; ++q) {
        const double f = (4.0 * l - q) / (q + 1.0);
        slope = slope * f + value * 4.0 / (q + 1.0);
        value *= f;
    }
}

// The true surface's offset from the six-node face at L, and its
// derivatives along s = L[1] and t = L[2]
inline Vec offsetAt(const Offsets& offsets,
                    const std::array<double, 3>& L,
                    Vec* ds = nullptr,
                    Vec* dt = nullptr)
{
    Vec value {};
    Vec alongS {};
    Vec alongT {};
    for (std::size_t p = 0; p < quarticPoints.size(); ++p) {
        double f[3];
        double d[3];
        for (std::size_t k = 0; k < 3; ++k) {
            latticeFactor(quarticPoints[p][k], L[k], f[k], d[k]);
        }
        const double phi = f[0] * f[1] * f[2];
        // L[0] = 1 - s - t
        const double phiS = -d[0] * f[1] * f[2] + f[0] * d[1] * f[2];
        const double phiT = -d[0] * f[1] * f[2] + f[0] * f[1] * d[2];
        value = add(value, offsets[p], phi);
        alongS = add(alongS, offsets[p], phiS);
        alongT = add(alongT, offsets[p], phiT);
    }
    if (ds) {
        *ds = alongS;
    }
    if (dt) {
        *dt = alongT;
    }
    return value;
}

// The face's point at L, on its true surface where 'offsets' are given, and
// its derivatives along s and t
inline Vec pointAt(const Face& x,
                   const Offsets* offsets,
                   const std::array<double, 3>& L,
                   Vec* ds = nullptr,
                   Vec* dt = nullptr)
{
    const double l0 = L[0];
    const double s = L[1];
    const double t = L[2];
    const auto N = quadratic(L);
    const std::array<double, 6> Ns {1.0 - 4.0 * l0, 4.0 * s - 1.0, 0.0, 4.0 * (l0 - s), 4.0 * t,
                                    -4.0 * t};
    const std::array<double, 6> Nt {1.0 - 4.0 * l0, 0.0, 4.0 * t - 1.0, -4.0 * s, 4.0 * s,
                                    4.0 * (l0 - t)};
    Vec p {};
    Vec xs {};
    Vec xt {};
    for (std::size_t k = 0; k < 6; ++k) {
        p = add(p, x[k], N[k]);
        xs = add(xs, x[k], Ns[k]);
        xt = add(xt, x[k], Nt[k]);
    }
    if (offsets) {
        Vec os {};
        Vec ot {};
        p = add(p, offsetAt(*offsets, L, &os, &ot));
        xs = add(xs, os);
        xt = add(xt, ot);
    }
    if (ds) {
        *ds = xs;
    }
    if (dt) {
        *dt = xt;
    }
    return p;
}

// The surface of the quadratic face through x at area coordinates L. Its
// derivatives along s = L[1] and t = L[2]: the second ones are the face's
// own, the same all over it, the first ones where L is. Of its true surface
// where 'offsets' are given (offsetAt()), the second ones by the first's
// differences. False where the face has no tangent plane there.
inline bool localSurface(const Face& x,
                         const std::array<double, 3>& L,
                         Local& at,
                         const Offsets* offsets = nullptr)
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
    if (offsets) {
        // The offsets' second derivatives by their first ones' central
        // differences
        const double h = 1e-4;
        Vec os;
        Vec ot;
        p = add(p, offsetAt(*offsets, L, &os, &ot));
        xs = add(xs, os);
        xt = add(xt, ot);
        Vec sp;
        Vec tp;
        Vec sm;
        Vec tm;
        offsetAt(*offsets, {l0 - h, s + h, t}, &sp, &tp);
        offsetAt(*offsets, {l0 + h, s - h, t}, &sm, &tm);
        xss = add(xss, scaled(add(sp, sm, -1.0), 0.5 / h));
        const Vec stS = scaled(add(tp, tm, -1.0), 0.5 / h);
        offsetAt(*offsets, {l0 - h, s, t + h}, &sp, &tp);
        offsetAt(*offsets, {l0 + h, s, t - h}, &sm, &tm);
        xtt = add(xtt, scaled(add(tp, tm, -1.0), 0.5 / h));
        const Vec stT = scaled(add(sp, sm, -1.0), 0.5 / h);
        xst = add(xst, scaled(add(stS, stT), 0.5));
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

// The face's point, on its true surface where 'offsets' are given, in the
// plane through 'c' square to the unit 'way', farthest along the unit 'out'
// -- a line's point at a station along it, where the surface touches a flat
// face of normal -out lying along 'way' (thinOut()). From L, and L the
// point. False where it is beyond the face: 'leaving' the edge it goes out
// by, (0 1) 0, (1 2) 1, (2 0) 2, L where the plane crosses that edge, for
// the face beyond to go on from; -1 where none is found -- the plane misses
// the face, or the cut curves away from 'out' all along it
inline bool farthestAcross(const Face& x,
                           const Offsets* offsets,
                           const Vec& out,
                           const Vec& c,
                           const Vec& way,
                           std::array<double, 3>& L,
                           int& leaving)
{
    leaving = -1;
    double s = L[1];
    double t = L[2];
    auto coordinates = [](double s, double t) {
        return std::array<double, 3> {1.0 - s - t, s, t};
    };
    // Onto the plane, along the face's steepest way across it
    auto ontoPlane = [&](double& s, double& t) {
        for (int k = 0; k < 8; ++k) {
            Vec ps;
            Vec pt;
            const Vec p = pointAt(x, offsets, coordinates(s, t), &ps, &pt);
            const double g = dot(add(p, c, -1.0), way);
            const double gs = dot(ps, way);
            const double gt = dot(pt, way);
            const double g2 = gs * gs + gt * gt;
            if (!(g2 > 0.0)) {
                return false;
            }
            s -= g * gs / g2;
            t -= g * gt / g2;
            if (std::abs(g) < 1e-12 * (1.0 + std::sqrt(g2))) {
                return true;
            }
        }
        return true;
    };
    auto height = [&](double s, double t) {
        return dot(pointAt(x, offsets, coordinates(s, t)), out);
    };
    // Where the cut leaves the face, the edge and its point there: along
    // the cut from (s, t) by (ds, dt), the first edge met
    auto edgeMet = [&](double s0, double t0, double ds, double dt, double& step) {
        int edge = -1;
        step = std::numeric_limits<double>::infinity();
        auto meet = [&](double from, double rate, int e) {
            // from + rate * h = 0, h > 0
            if (rate < 0.0 && from >= 0.0) {
                const double h = -from / rate;
                if (h < step) {
                    step = h;
                    edge = e;
                }
            }
        };
        meet(t0, dt, 0);
        meet(1.0 - s0 - t0, -ds - dt, 1);
        meet(s0, ds, 2);
        return edge;
    };
    // On edge e, where the plane crosses it: by the secant along the edge
    // between its corners, the nearer end where it does not
    auto onEdge = [&](int e, double& s, double& t) {
        // The edge's corners in (s, t): (0 1) from L0 to L1, (1 2) from L1
        // to L2, (2 0) from L2 to L0
        static const double ends[3][2][2] {{{0.0, 0.0}, {1.0, 0.0}},
                                           {{1.0, 0.0}, {0.0, 1.0}},
                                           {{0.0, 1.0}, {0.0, 0.0}}};
        const auto& a = ends[e][0];
        const auto& b = ends[e][1];
        auto planeAt = [&](double u) {
            const Vec p = pointAt(x,
                                  offsets,
                                  coordinates(a[0] + u * (b[0] - a[0]), a[1] + u * (b[1] - a[1])));
            return dot(add(p, c, -1.0), way);
        };
        double u0 = 0.0;
        double u1 = 1.0;
        double g0 = planeAt(u0);
        double g1 = planeAt(u1);
        double u = std::abs(g0) < std::abs(g1) ? 0.0 : 1.0;
        if ((g0 <= 0.0) != (g1 <= 0.0)) {
            for (int k = 0; k < 60; ++k) {
                u = u0 - g0 * (u1 - u0) / (g1 - g0);
                const double g = planeAt(u);
                if (std::abs(g) < 1e-13) {
                    break;
                }
                // Illinois: the end kept twice halves its value
                if ((g <= 0.0) == (g1 <= 0.0)) {
                    u1 = u;
                    g1 = g;
                    g0 *= 0.5;
                }
                else {
                    u0 = u;
                    g0 = g;
                    g1 *= 0.5;
                }
            }
        }
        s = a[0] + u * (b[0] - a[0]);
        t = a[1] + u * (b[1] - a[1]);
    };
    // Out of the face already: by the edge it is farthest out of, on it
    // where the plane crosses it
    auto beyond = [&](double& s, double& t) {
        const double by[3] = {-t, s + t - 1.0, -s};
        int edge = -1;
        double most = 1e-9;
        for (int e = 0; e < 3; ++e) {
            if (by[e] > most) {
                most = by[e];
                edge = e;
            }
        }
        if (edge < 0) {
            return false;
        }
        onEdge(edge, s, t);
        L = coordinates(s, t);
        leaving = edge;
        return true;
    };
    // From the middle of the plane's cut through the face, where it crosses
    // two of its edges: projected from L, a cut microns long by a corner --
    // a plane square to a cone's side by its rim -- was missed, the point
    // left by an edge the cut does not reach
    {
        auto planeAt = [&](double s, double t) {
            return dot(add(pointAt(x, offsets, coordinates(s, t)), c, -1.0), way);
        };
        std::array<double, 3> gs {planeAt(0.0, 0.0), planeAt(1.0, 0.0), planeAt(0.0, 1.0)};
        double ms = 0.0;
        double mt = 0.0;
        int crossings = 0;
        for (int e = 0; e < 3 && crossings < 2; ++e) {
            const double ga = gs[static_cast<std::size_t>(e)];
            const double gb = gs[static_cast<std::size_t>((e + 1) % 3)];
            if ((ga <= 0.0) == (gb <= 0.0)) {
                continue;
            }
            double es = 0.0;
            double et = 0.0;
            onEdge(e, es, et);
            ms += es;
            mt += et;
            ++crossings;
        }
        if (crossings == 2) {
            s = 0.5 * ms;
            t = 0.5 * mt;
        }
    }
    if (!ontoPlane(s, t) || beyond(s, t)) {
        return false;
    }
    const double eps = 1e-4;
    for (int iteration = 0; iteration < 40; ++iteration) {
        Vec ps;
        Vec pt;
        pointAt(x, offsets, coordinates(s, t), &ps, &pt);
        // Along the cut: square to the plane's way in (s, t)
        double ts = -dot(pt, way);
        double tt = dot(ps, way);
        const double norm = std::hypot(ts, tt);
        if (!(norm > 0.0)) {
            return false;
        }
        ts /= norm;
        tt /= norm;
        // How the height goes along it: by the cut's points either side
        double sp = s + eps * ts;
        double tp = t + eps * tt;
        double sm = s - eps * ts;
        double tm = t - eps * tt;
        if (!ontoPlane(sp, tp) || !ontoPlane(sm, tm)) {
            return false;
        }
        const double h0 = height(s, t);
        const double hp = height(sp, tp);
        const double hm = height(sm, tm);
        const double d1 = (hp - hm) / (2.0 * eps);
        const double d2 = (hp - 2.0 * h0 + hm) / (eps * eps);
        double step = d2 < 0.0 ? -d1 / d2 : (d1 >= 0.0 ? 0.25 : -0.25);
        step = std::clamp(step, -0.25, 0.25);
        const double ds = step * ts;
        const double dt = step * tt;
        // Beyond the face: to the edge, and out by it
        double toEdge = 0.0;
        const int edge = edgeMet(s, t, ds, dt, toEdge);
        if (edge >= 0 && toEdge < 1.0) {
            onEdge(edge, s, t);
            L = coordinates(s, t);
            leaving = edge;
            return false;
        }
        s += ds;
        t += dt;
        if (!ontoPlane(s, t) || beyond(s, t)) {
            return false;
        }
        if (std::abs(step) < 1e-9) {
            break;
        }
    }
    L = coordinates(s, t);
    return true;
}

// The point of triangle abc nearest p, as its weights on the corners
inline std::array<double, 3>
nearestOnTriangle(const Vec& p, const Vec& a, const Vec& b, const Vec& c)
{
    // Ericson, Real-Time Collision Detection, 5.1.5
    const Vec ab = add(b, a, -1.0);
    const Vec ac = add(c, a, -1.0);
    const Vec ap = add(p, a, -1.0);
    const double d1 = dot(ab, ap);
    const double d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return {1.0, 0.0, 0.0};
    }
    const Vec bp = add(p, b, -1.0);
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return {0.0, 1.0, 0.0};
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double v = d1 / (d1 - d3);
        return {1.0 - v, v, 0.0};
    }
    const Vec cp = add(p, c, -1.0);
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return {0.0, 0.0, 1.0};
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double w = d2 / (d2 - d6);
        return {1.0 - w, 0.0, w};
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {0.0, 1.0 - w, w};
    }
    const double denom = 1.0 / (va + vb + vc);
    const double v = vb * denom;
    const double w = vc * denom;
    return {1.0 - v - w, v, w};
}

// The face's point, on its true surface where 'offsets' are given,
// farthest along the unit 'out', from L, by Newton's steps on its height
// (its second derivatives by differences): L the point. False where it is
// beyond the face, 'leaving' the edge it goes out by as farthestAcross()
// has it, L on that edge; -1 where the face curves away from 'out' there
inline bool farthestOn(const Face& x,
                       const Offsets* offsets,
                       const Vec& out,
                       std::array<double, 3>& L,
                       int& leaving)
{
    leaving = -1;
    double s = L[1];
    double t = L[2];
    auto slopes = [&](double s, double t, double& hs, double& ht) {
        Vec ps;
        Vec pt;
        pointAt(x, offsets, {1.0 - s - t, s, t}, &ps, &pt);
        hs = dot(ps, out);
        ht = dot(pt, out);
    };
    const double eps = 1e-5;
    for (int iteration = 0; iteration < 40; ++iteration) {
        double hs;
        double ht;
        slopes(s, t, hs, ht);
        double a;
        double b;
        double c;
        double d;
        slopes(s + eps, t, a, b);
        slopes(s, t + eps, c, d);
        // The height's Hessian, symmetric
        const double hss = (a - hs) / eps;
        const double htt = (d - ht) / eps;
        const double hst = 0.5 * ((b - ht) / eps + (c - hs) / eps);
        const double det = hss * htt - hst * hst;
        double ds;
        double dt;
        if (hss < 0.0 && det > 0.0) {
            ds = -(htt * hs - hst * ht) / det;
            dt = -(hss * ht - hst * hs) / det;
        }
        else {
            // Not curved to it both ways: up its slope
            const double g = std::hypot(hs, ht);
            if (!(g > 0.0)) {
                return false;
            }
            ds = 0.25 * hs / g;
            dt = 0.25 * ht / g;
        }
        const double step = std::hypot(ds, dt);
        if (step > 0.25) {
            ds *= 0.25 / step;
            dt *= 0.25 / step;
        }
        const double ns = s + ds;
        const double nt = t + dt;
        const double by[3] = {-nt, ns + nt - 1.0, -ns};
        int edge = -1;
        double most = 1e-12;
        for (int e = 0; e < 3; ++e) {
            if (by[e] > most) {
                most = by[e];
                edge = e;
            }
        }
        if (edge >= 0) {
            // Along the step to that edge
            double h = 1.0;
            if (edge == 0 && dt < 0.0) {
                h = -t / dt;
            }
            else if (edge == 2 && ds < 0.0) {
                h = -s / ds;
            }
            else if (edge == 1 && ds + dt > 0.0) {
                h = (1.0 - s - t) / (ds + dt);
            }
            s = std::clamp(s + h * ds, 0.0, 1.0);
            t = std::clamp(t + h * dt, 0.0, 1.0 - s);
            L = {1.0 - s - t, s, t};
            leaving = edge;
            return false;
        }
        s = ns;
        t = nt;
        if (step < 1e-10) {
            break;
        }
    }
    L = {1.0 - s - t, s, t};
    return true;
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
