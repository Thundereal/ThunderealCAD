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

// Profile contact's geometry (docs/AssemblyDynamics.md sec 7.4's 2D path):
// two planar profiles, each a face's boundary of segments and arcs with the
// face to their left, in one plane's coordinates, and where they touch.
// The Chrono backend's profile contact finds its points here
// (ChronoBackend.cpp); nothing here knows Chrono, so a test checks it
// against brute force.

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace Assembly::Solver::Profile
{

// How profileTouches() found a profile contact's point, to find it again
// with the bodies moved (ContactHandover's ProfileFollow::place()): at an
// arc of A's or B's ('at'), its point farthest across a line of the
// other's ('across'), the other point the nearest on that line; or at an
// arc of A's against one of B's (ArcArc), each point on the line of their
// centres, 'sides' the signs along it, from A's centre to B's, of A's point
// from its centre, B's from its, and the normal; or at a convex corner of
// A's or B's, the 'from' of piece 'at', against a piece of the other's, the
// other point the nearest on it (CornerA, CornerB). None for the others,
// not found again (profileTouches()). 'pair', the profiles' pair of faces;
// 'level', the height through their overlap it was taken at
// (ProfileFollow::height()).
struct ProfileFound
{
    enum class Kind
    {
        None,
        ArcA,
        ArcB,
        ArcArc,
        CornerA,
        CornerB
    };
    Kind kind = Kind::None;
    const void* at = nullptr;
    const void* across = nullptr;
    std::array<double, 3> sides {1.0, 1.0, 1.0};
    std::size_t pair = 0;
    double level = 0.0;
};

struct V2
{
    double x = 0.0;
    double y = 0.0;
};
inline V2 operator+(V2 a, V2 b)
{
    return {a.x + b.x, a.y + b.y};
}
inline V2 operator-(V2 a, V2 b)
{
    return {a.x - b.x, a.y - b.y};
}
inline V2 operator*(V2 a, double s)
{
    return {a.x * s, a.y * s};
}
inline double dot(V2 a, V2 b)
{
    return a.x * b.x + a.y * b.y;
}
inline double cross(V2 a, V2 b)
{
    return a.x * b.y - a.y * b.x;
}
inline double norm(V2 a)
{
    return std::hypot(a.x, a.y);
}
inline V2 unit(V2 a)
{
    const double l = norm(a);
    return l > 0.0 ? a * (1.0 / l) : V2 {};
}
// To the right of a direction: away from the material on its left
inline V2 rightOf(V2 t)
{
    return {t.y, -t.x};
}

struct Profile2
{
    struct Piece
    {
        V2 from;
        V2 to;
        V2 centre;
        double sweep = 0.0;  // 0: a segment; an arc's, positive counterclockwise
        double radius = 0.0;
        double start = 0.0;  // an arc's angle at 'from'
        std::size_t prev = 0;
        std::size_t next = 0;
        const void* key = nullptr;  // the model's piece, for the patches
        // Where it lies, an arc's whole circle's
        double low[2] {};
        double high[2] {};
        V2 tangentAt(double angle) const
        {
            return V2 {-std::sin(angle), std::cos(angle)} * (sweep > 0.0 ? 1.0 : -1.0);
        }
        V2 startTangent() const
        {
            return sweep == 0.0 ? unit(to - from) : tangentAt(start);
        }
        V2 endTangent() const
        {
            return sweep == 0.0 ? unit(to - from) : tangentAt(start + sweep);
        }
        // Whether an arc reaches the angle
        bool reaches(double angle) const
        {
            double u =
                std::fmod((angle - start) * (sweep > 0.0 ? 1.0 : -1.0), 2.0 * std::numbers::pi);
            if (u < 0.0) {
                u += 2.0 * std::numbers::pi;
            }
            return u <= std::abs(sweep) + 1e-12;
        }
    };
    std::vector<Piece> pieces;
    // At each piece's 'from': whether a convex corner is there -- turning
    // left, the face to the left, by more than the arcs' approximation's
    // kinks -- and the outward normal there, its pieces' between
    std::vector<bool> corner;
    std::vector<V2> vertexNormal;
    double low[2] {};
    double high[2] {};
};

// A profile's bounds, its pieces' and its own, and at each piece's 'from'
// whether a convex corner is there and the outward normal there; its
// pieces and their links, prev and next, set
AssemblyExport void finishProfile(Profile2& profile);

// The nearest point of a profile's boundary to x: the point, the outward
// normal there -- a corner's, its pieces' between -- and the pieces it is
// on, one or a corner's two
struct OnProfile
{
    V2 point;
    V2 normal;
    double distance = std::numeric_limits<double>::infinity();
    std::array<const void*, 2> pieces {};
};

AssemblyExport OnProfile nearestOn(const Profile2& profile, V2 x);

// The nearest point to x on a line piece, its ends included, and its
// outward normal: past an end, x is measured from the line, not from the end
// itself -- a Newton iterate 10 mm off, read off an end as nearestOn() reads
// it off a corner, was 10 mm deep where the line had it 10 mm clear, and the
// iterations ran off
AssemblyExport void nearestOnLine(const Profile2::Piece& line, V2 x, V2& y, V2& out);

// An arc's point farthest along a direction, x; whether the arc reaches
// that way
AssemblyExport bool farthestOn(const Profile2::Piece& arc, V2 direction, V2& x);

// The same, or where the arc does not reach that way the end of it farther
// along it: moving smoothly as the direction turns past the arc's end
AssemblyExport V2 farthestOnOrEnd(const Profile2::Piece& arc, V2 direction);

// Where two profiles touch, or are within 'envelope' (m) of it: point
// pairs, A's on A's boundary and B's on B's, and the normal from A to B,
// each where one's boundary goes deepest into the other -- a convex
// corner's, an arc's farthest across a piece of the other's, along the
// line of the centres for two arcs -- B's point the nearest on B's boundary
// to A's, or the other way round, so that A's point is in B as deep as it
// goes, and the pieces each is on. Exact on lines and arcs: no tessellation's
// facets, and a pin rolling along a slot's wall is as round as it is.
struct ProfileTouch
{
    V2 onA;
    V2 onB;
    V2 normal;
    std::array<const void*, 2> piecesA {};
    std::array<const void*, 2> piecesB {};
    ProfileFound found;  // not its pair
};

AssemblyExport void profileTouches(const Profile2& a,
                                   const Profile2& b,
                                   double envelope,
                                   std::vector<ProfileTouch>& out);

}  // namespace Assembly::Solver::Profile
