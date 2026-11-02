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

#include <algorithm>
#include <limits>

#include "ProfileGeometry.h"

namespace Assembly::Solver::Profile
{

void finishProfile(Profile2& profile)
{
    profile.low[0] = profile.low[1] = std::numeric_limits<double>::infinity();
    profile.high[0] = profile.high[1] = -std::numeric_limits<double>::infinity();
    for (auto& p : profile.pieces) {
        if (p.sweep != 0.0) {
            p.low[0] = p.centre.x - p.radius;
            p.low[1] = p.centre.y - p.radius;
            p.high[0] = p.centre.x + p.radius;
            p.high[1] = p.centre.y + p.radius;
        }
        else {
            p.low[0] = std::min(p.from.x, p.to.x);
            p.low[1] = std::min(p.from.y, p.to.y);
            p.high[0] = std::max(p.from.x, p.to.x);
            p.high[1] = std::max(p.from.y, p.to.y);
        }
        for (int d = 0; d < 2; ++d) {
            profile.low[d] = std::min(profile.low[d], p.low[d]);
            profile.high[d] = std::max(profile.high[d], p.high[d]);
        }
    }
    profile.corner.resize(profile.pieces.size());
    profile.vertexNormal.resize(profile.pieces.size());
    for (std::size_t k = 0; k < profile.pieces.size(); ++k) {
        const Profile2::Piece& p = profile.pieces[k];
        const V2 in = profile.pieces[p.prev].endTangent();
        const V2 outward = p.startTangent();
        // The approximation's arcs meet within 0.05 rad
        profile.corner[k] = cross(in, outward) > std::sin(0.06) || dot(in, outward) < 0.0;
        profile.vertexNormal[k] = unit(rightOf(in) + rightOf(outward));
    }
}

OnProfile nearestOn(const Profile2& profile, V2 x)
{
    OnProfile best;
    auto atVertex = [&](std::size_t k) {
        const Profile2::Piece& p = profile.pieces[k];
        const double d = norm(x - p.from);
        if (d < best.distance) {
            best.distance = d;
            best.point = p.from;
            best.normal = profile.vertexNormal[k];
            best.pieces = {profile.pieces[p.prev].key, p.key};
        }
    };
    for (std::size_t k = 0; k < profile.pieces.size(); ++k) {
        const Profile2::Piece& p = profile.pieces[k];
        if (p.sweep == 0.0) {
            const V2 d = p.to - p.from;
            const double l2 = dot(d, d);
            const double t = l2 > 0.0 ? dot(x - p.from, d) / l2 : 0.0;
            if (t <= 0.0) {
                atVertex(k);
                continue;
            }
            if (t >= 1.0) {
                atVertex(p.next);
                continue;
            }
            const V2 q = p.from + d * t;
            const double dist = norm(x - q);
            if (dist < best.distance) {
                best.distance = dist;
                best.point = q;
                best.normal = rightOf(unit(d));
                best.pieces = {p.key, nullptr};
            }
            continue;
        }
        const V2 r = x - p.centre;
        const double angle = std::atan2(r.y, r.x);
        if (norm(r) > 0.0 && p.reaches(angle)) {
            const V2 radial {std::cos(angle), std::sin(angle)};
            const V2 q = p.centre + radial * p.radius;
            const double dist = norm(x - q);
            if (dist < best.distance) {
                best.distance = dist;
                best.point = q;
                // Out of a convex arc's circle, into a hollow one's
                best.normal = p.sweep > 0.0 ? radial : radial * -1.0;
                best.pieces = {p.key, nullptr};
            }
            continue;
        }
        atVertex(k);
        atVertex(p.next);
    }
    return best;
}

void nearestOnLine(const Profile2::Piece& line, V2 x, V2& y, V2& out)
{
    const V2 d = line.to - line.from;
    const double l2 = dot(d, d);
    const double t = l2 > 0.0 ? std::clamp(dot(x - line.from, d) / l2, 0.0, 1.0) : 0.0;
    y = line.from + d * t;
    out = rightOf(unit(d));
}

bool farthestOn(const Profile2::Piece& arc, V2 direction, V2& x)
{
    direction = unit(direction);
    if (norm(direction) == 0.0 || !arc.reaches(std::atan2(direction.y, direction.x))) {
        return false;
    }
    x = arc.centre + direction * arc.radius;
    return true;
}

V2 farthestOnOrEnd(const Profile2::Piece& arc, V2 direction)
{
    V2 x;
    if (farthestOn(arc, direction, x)) {
        return x;
    }
    return dot(arc.from, direction) >= dot(arc.to, direction) ? arc.from : arc.to;
}

void profileTouches(const Profile2& a,
                    const Profile2& b,
                    double envelope,
                    std::vector<ProfileTouch>& out)
{
    out.clear();
    for (int d = 0; d < 2; ++d) {
        if (a.low[d] > b.high[d] + envelope || b.low[d] > a.high[d] + envelope) {
            return;
        }
    }
    auto overlap = [&](const Profile2::Piece& p, const Profile2::Piece& q) {
        for (int d = 0; d < 2; ++d) {
            if (p.low[d] > q.high[d] + envelope || q.low[d] > p.high[d] + envelope) {
                return false;
            }
        }
        return true;
    };
    auto keep = [&](const ProfileTouch& touch) {
        for (const auto& other : out) {
            if (norm(other.onA - touch.onA) < 1e-12 && norm(other.onB - touch.onB) < 1e-12) {
                return;
            }
        }
        out.push_back(touch);
    };
    using Kind = ProfileFound::Kind;
    // How a point was found, for following it within a substep: an arc's
    // farthest point across a line, an arc's against an arc on the line of
    // their centres ('centres', the two arcs'; the sides it was found on
    // kept), or a corner's against the piece of the other's nearest it --
    // only where the other's nearest is on that line or arc, not at a
    // corner, or on a piece beside it, from which that point is not the
    // one. The corner is a point of its material, but the other's nearest
    // slides along its piece: fixed, a knife-edge follower's force on a
    // disk cam read 4.5% off.
    auto followed = [](ProfileFound found,
                       const OnProfile& y,
                       V2 x,
                       V2 normal,
                       const std::array<V2, 2>& centres) {
        if (found.kind == Kind::CornerA || found.kind == Kind::CornerB) {
            found.across = y.pieces[0];
        }
        if (y.pieces[1] || y.pieces[0] != found.across) {
            found.kind = Kind::None;
        }
        if (found.kind == Kind::ArcArc) {
            const V2 u = centres[1] - centres[0];
            auto sign = [](double v) {
                return v < 0.0 ? -1.0 : 1.0;
            };
            found.sides = {sign(dot(x - centres[0], u)),
                           sign(dot(y.point - centres[1], u)),
                           sign(dot(normal, u))};
        }
        return found;
    };
    // A's point x against B: in B, or within the envelope of it
    auto fromA = [&](V2 x,
                     std::array<const void*, 2> pieces,
                     const ProfileFound& found,
                     const std::array<V2, 2>& centres = {}) {
        const OnProfile y = nearestOn(b, x);
        const V2 d = x - y.point;
        const double signedDistance = dot(d, y.normal);
        if (!(signedDistance < envelope)) {
            return;
        }
        // Out of B where they meet, exactly along x - y off a corner
        const V2 out = norm(d) > 1e-12 ? unit(d) * (signedDistance < 0.0 ? -1.0 : 1.0) : y.normal;
        keep(
            {x, y.point, out * -1.0, pieces, y.pieces, followed(found, y, x, out * -1.0, centres)});
    };
    // B's point x against A, the same way round
    auto fromB = [&](V2 x, std::array<const void*, 2> pieces, const ProfileFound& found) {
        const OnProfile y = nearestOn(a, x);
        const V2 d = x - y.point;
        const double signedDistance = dot(d, y.normal);
        if (!(signedDistance < envelope)) {
            return;
        }
        const V2 out = norm(d) > 1e-12 ? unit(d) * (signedDistance < 0.0 ? -1.0 : 1.0) : y.normal;
        keep({y.point, x, out, y.pieces, pieces, followed(found, y, x, out, {})});
    };
    auto near = [&](V2 x, const Profile2& other) {
        return x.x > other.low[0] - envelope && x.x < other.high[0] + envelope
            && x.y > other.low[1] - envelope && x.y < other.high[1] + envelope;
    };
    for (std::size_t k = 0; k < a.pieces.size(); ++k) {
        const Profile2::Piece& p = a.pieces[k];
        if (a.corner[k] && near(p.from, b)) {
            fromA(p.from, {a.pieces[p.prev].key, p.key}, {Kind::CornerA, p.key, nullptr});
        }
    }
    for (std::size_t k = 0; k < b.pieces.size(); ++k) {
        const Profile2::Piece& q = b.pieces[k];
        if (b.corner[k] && near(q.from, a)) {
            fromB(q.from, {b.pieces[q.prev].key, q.key}, {Kind::CornerB, q.key, nullptr});
        }
    }
    for (const auto& p : a.pieces) {
        for (const auto& q : b.pieces) {
            if (!overlap(p, q)) {
                continue;
            }
            V2 x;
            if (p.sweep != 0.0) {
                if (q.sweep == 0.0) {
                    // Farthest across q's line into B
                    if (farthestOn(p, rightOf(unit(q.to - q.from)) * -1.0, x)) {
                        fromA(x, {p.key, nullptr}, {Kind::ArcA, p.key, q.key});
                    }
                }
                else {
                    // On the line of the centres, either way
                    const V2 c = q.centre - p.centre;
                    const ProfileFound found {Kind::ArcArc, p.key, q.key};
                    if (farthestOn(p, c, x)) {
                        fromA(x, {p.key, nullptr}, found, {p.centre, q.centre});
                    }
                    if (farthestOn(p, c * -1.0, x)) {
                        fromA(x, {p.key, nullptr}, found, {p.centre, q.centre});
                    }
                }
            }
            else if (q.sweep != 0.0) {
                // B's arc farthest across p's line into A
                if (farthestOn(q, rightOf(unit(p.to - p.from)) * -1.0, x)) {
                    fromB(x, {q.key, nullptr}, {Kind::ArcB, q.key, p.key});
                }
            }
        }
    }
}

}  // namespace Assembly::Solver::Profile
