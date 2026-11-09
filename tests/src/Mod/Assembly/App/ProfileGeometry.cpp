// SPDX-License-Identifier: LGPL-2.1-or-later

// A profile contact's points (docs/AssemblyDynamics.md sec 7.4's 2D path,
// Solver/ProfileGeometry.h) against brute force: pairs of profiles -- discs,
// boxes, rounded boxes, a knife, a V notch, a hole, a slot, a groove -- in
// random poses, brought together until they overlap by a depth chosen, from
// a gap within the envelope to a tenth of a millimetre, and each run both
// ways round. Brute force walks each boundary in fine steps, its distance to
// the other's exact on its lines and arcs and its side by a ray's crossings
// of them, each deepest point found again by a golden-section search. Each
// place one boundary goes into the other must have a point there at least
// as deep, each point's depth must be its own, and none deeper than the
// deepest.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include <Mod/Assembly/App/Solver/ProfileGeometry.h>

using namespace Assembly::Solver::Profile;

namespace
{

constexpr double pi = std::numbers::pi;
constexpr double envelope = 2e-5;   // m, the backend's
constexpr double tolerance = 1e-9;  // m

// A profile's loops as the backend gives them: each a closed run of pieces,
// the face to their left, keys unique to the profile
struct Shape
{
    std::string name;
    std::vector<std::vector<Profile2::Piece>> loops;
};

Profile2::Piece line(V2 from, V2 to)
{
    Profile2::Piece p;
    p.from = from;
    p.to = to;
    return p;
}

// An arc about 'centre' from angle 'start', sweeping 'sweep' (counterclockwise
// positive)
Profile2::Piece arc(V2 centre, double radius, double start, double sweep)
{
    Profile2::Piece p;
    p.centre = centre;
    p.radius = radius;
    p.start = start;
    p.sweep = sweep;
    p.from = centre + V2 {std::cos(start), std::sin(start)} * radius;
    p.to = centre + V2 {std::cos(start + sweep), std::sin(start + sweep)} * radius;
    return p;
}

std::vector<Profile2::Piece> polygon(const std::vector<V2>& points)
{
    std::vector<Profile2::Piece> loop;
    for (std::size_t k = 0; k < points.size(); ++k) {
        loop.push_back(line(points[k], points[(k + 1) % points.size()]));
    }
    return loop;
}

Shape disc(double r)
{
    return {"disc", {{arc({}, r, 0.0, 2.0 * pi)}}};
}

Shape box(double w, double h)
{
    return {"box", {polygon({{-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}, {-w / 2, h / 2}})}};
}

// A box with its corners rounded by r, tangent
Shape roundedBox(double w, double h, double r)
{
    const double x = w / 2 - r;
    const double y = h / 2 - r;
    return {"rounded box",
            {{line({-x, -h / 2}, {x, -h / 2}),
              arc({x, -y}, r, -pi / 2, pi / 2),
              line({w / 2, -y}, {w / 2, y}),
              arc({x, y}, r, 0.0, pi / 2),
              line({x, h / 2}, {-x, h / 2}),
              arc({-x, y}, r, pi / 2, pi / 2),
              line({-w / 2, y}, {-w / 2, -y}),
              arc({-x, -y}, r, pi, pi / 2)}}};
}

// A knife: its edge at the origin pointing down, 30 deg either side
Shape knife(double h)
{
    const double t = h * std::tan(pi / 6);
    return {"knife", {polygon({{0.0, 0.0}, {t, h}, {-t, h}})}};
}

// A block with a V notch in its top, 45 deg either side, its bottom at the
// origin
Shape notched(double w, double h, double depth)
{
    return {"V notch",
            {polygon({{-w / 2, -h},
                      {w / 2, -h},
                      {w / 2, 0.0},
                      {depth, 0.0},
                      {0.0, -depth},
                      {-depth, 0.0},
                      {-w / 2, 0.0}})}};
}

// A plate with a round hole at the origin: the hole's loop clockwise
Shape holed(double w, double r)
{
    return {"hole",
            {polygon({{-w / 2, -w / 2}, {w / 2, -w / 2}, {w / 2, w / 2}, {-w / 2, w / 2}}),
             {arc({}, r, 0.0, -2.0 * pi)}}};
}

// A plate with a slot at the origin, its straight sides 2 l long, its ends
// round of radius r: the slot's loop clockwise
Shape slotted(double w, double l, double r)
{
    return {"slot",
            {polygon({{-w / 2, -w / 2}, {w / 2, -w / 2}, {w / 2, w / 2}, {-w / 2, w / 2}}),
             {line({-l, r}, {l, r}),
              arc({l, 0.0}, r, pi / 2, -pi),
              line({l, -r}, {-l, -r}),
              arc({-l, 0.0}, r, -pi / 2, -pi)}}};
}

// A block with a round groove across its top, its centre at the origin, the
// groove's rims sharp corners
Shape grooved(double w, double h, double r)
{
    return {"groove",
            {{line({-w / 2, -h}, {w / 2, -h}),
              line({w / 2, -h}, {w / 2, 0.0}),
              line({w / 2, 0.0}, {r, 0.0}),
              arc({}, r, 0.0, -pi),
              line({-r, 0.0}, {-w / 2, 0.0}),
              line({-w / 2, 0.0}, {-w / 2, -h})}}};
}

V2 turned(V2 x, double angle)
{
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return {c * x.x - s * x.y, s * x.x + c * x.y};
}

// The shape turned by 'angle' and moved by 'by', as a Profile2, its keys
// from 'keyBase' on
struct Placed
{
    Shape shape;
    Profile2 profile;
    std::vector<int> keys;
};

void place(const Shape& shape, double angle, V2 by, Placed& out)
{
    out.shape = shape;
    out.profile = Profile2 {};
    std::size_t count = 0;
    for (const auto& loop : shape.loops) {
        count += loop.size();
    }
    out.keys.assign(count, 0);
    std::size_t k = 0;
    for (auto& loop : out.shape.loops) {
        const std::size_t base = out.profile.pieces.size();
        for (auto& p : loop) {
            p.from = turned(p.from, angle) + by;
            p.to = turned(p.to, angle) + by;
            p.centre = turned(p.centre, angle) + by;
            p.start += angle;
            Profile2::Piece q = p;
            q.key = &out.keys[k++];
            out.profile.pieces.push_back(q);
        }
        for (std::size_t i = 0; i < loop.size(); ++i) {
            Profile2::Piece& q = out.profile.pieces[base + i];
            q.prev = base + (i + loop.size() - 1) % loop.size();
            q.next = base + (i + 1) % loop.size();
        }
    }
    finishProfile(out.profile);
}

// Brute force, independent of the code under test

V2 pointAt(const Profile2::Piece& p, double s)
{
    if (p.sweep == 0.0) {
        return p.from + (p.to - p.from) * s;
    }
    const double a = p.start + p.sweep * s;
    return p.centre + V2 {std::cos(a), std::sin(a)} * p.radius;
}

double lengthOf(const Profile2::Piece& p)
{
    return p.sweep == 0.0 ? norm(p.to - p.from) : std::abs(p.sweep) * p.radius;
}

// Whether an arc reaches the angle, its own way
bool onArc(const Profile2::Piece& p, double angle)
{
    double u = std::fmod((angle - p.start) * (p.sweep > 0.0 ? 1.0 : -1.0), 2 * pi);
    if (u < 0.0) {
        u += 2 * pi;
    }
    return u <= std::abs(p.sweep);
}

// The exact distance from x to a piece
double distanceTo(const Profile2::Piece& p, V2 x)
{
    if (p.sweep == 0.0) {
        const V2 d = p.to - p.from;
        const double t = std::clamp(dot(x - p.from, d) / dot(d, d), 0.0, 1.0);
        return norm(x - (p.from + d * t));
    }
    const V2 r = x - p.centre;
    if (onArc(p, std::atan2(r.y, r.x))) {
        return std::abs(norm(r) - p.radius);
    }
    return std::min(norm(x - p.from), norm(x - p.to));
}

// Whether x is in a profile: a ray from it along +x crossing its boundary
// an odd number of times, exactly on its lines and arcs
bool inside(const Profile2& profile, V2 x)
{
    bool in = false;
    for (const auto& p : profile.pieces) {
        if (p.sweep == 0.0) {
            const V2 a = p.from;
            const V2 b = p.to;
            if ((a.y > x.y) != (b.y > x.y)) {
                if (a.x + (x.y - a.y) / (b.y - a.y) * (b.x - a.x) > x.x) {
                    in = !in;
                }
            }
            continue;
        }
        const double dy = x.y - p.centre.y;
        if (std::abs(dy) >= p.radius) {
            continue;
        }
        const double dx = std::sqrt(p.radius * p.radius - dy * dy);
        for (const double sx : {dx, -dx}) {
            if (p.centre.x + sx > x.x && onArc(p, std::atan2(dy, sx))) {
                in = !in;
            }
        }
    }
    return in;
}

// How deep x is in a profile: its distance to the boundary, positive inside
double depthIn(const Profile2& profile, V2 x)
{
    double d = std::numeric_limits<double>::infinity();
    for (const auto& p : profile.pieces) {
        d = std::min(d, distanceTo(p, x));
    }
    return inside(profile, x) ? d : -d;
}

// A place where one boundary goes into the other: the run of it inside, how
// deep it goes, and its bounds
struct Run
{
    double depth = -std::numeric_limits<double>::infinity();
    V2 deepest;
    double low[2] {std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::infinity()};
    double high[2] {-std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity()};
    void add(V2 x)
    {
        low[0] = std::min(low[0], x.x);
        low[1] = std::min(low[1], x.y);
        high[0] = std::max(high[0], x.x);
        high[1] = std::max(high[1], x.y);
    }
    bool holds(V2 x, double slack) const
    {
        return x.x >= low[0] - slack && x.x <= high[0] + slack && x.y >= low[1] - slack
            && x.y <= high[1] + slack;
    }
};

// X's boundary walked against Y: the runs of it in Y, and the deepest any
// point of it goes (negative, the nearest it comes, apart)
struct Walk
{
    std::vector<Run> runs;
    double deepest = -std::numeric_limits<double>::infinity();
};

Walk walk(const Profile2& x, const Profile2& y, bool refine)
{
    Walk out;
    // Each loop in order, starting at a sample outside if there is one, so
    // that a run is not cut at the loop's start
    std::vector<std::vector<std::pair<std::size_t, double>>> loops;
    std::vector<bool> seen(x.pieces.size(), false);
    for (std::size_t first = 0; first < x.pieces.size(); ++first) {
        if (seen[first]) {
            continue;
        }
        std::vector<std::pair<std::size_t, double>> samples;
        std::size_t k = first;
        do {
            seen[k] = true;
            const auto& p = x.pieces[k];
            const int n = std::max(8, int(std::ceil(lengthOf(p) / 1e-4)));
            for (int i = 0; i < n; ++i) {
                samples.emplace_back(k, double(i) / n);
            }
            k = p.next;
        } while (k != first);
        loops.push_back(std::move(samples));
    }
    auto depthAt = [&](std::size_t k, double s) {
        return depthIn(y, pointAt(x.pieces[k], s));
    };
    for (auto& samples : loops) {
        std::vector<double> depth(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            depth[i] = depthAt(samples[i].first, samples[i].second);
        }
        // Each local deepest, found again on its piece by a golden section
        auto sharpen = [&](std::size_t i) {
            const auto [k, s] = samples[i];
            const double h = 1.0 / std::max(8, int(std::ceil(lengthOf(x.pieces[k]) / 1e-4)));
            double a = std::max(0.0, s - h);
            double b = std::min(1.0, s + h);
            const double g = (std::sqrt(5.0) - 1.0) / 2.0;
            double c = b - g * (b - a);
            double d = a + g * (b - a);
            double fc = depthAt(k, c);
            double fd = depthAt(k, d);
            for (int it = 0; it < 60; ++it) {
                if (fc > fd) {
                    b = d;
                    d = c;
                    fd = fc;
                    c = b - g * (b - a);
                    fc = depthAt(k, c);
                }
                else {
                    a = c;
                    c = d;
                    fc = fd;
                    d = a + g * (b - a);
                    fd = depthAt(k, d);
                }
            }
            double best = depth[i];
            V2 at = pointAt(x.pieces[k], s);
            for (const double t : {a, b, 0.5 * (a + b), 0.0, 1.0}) {
                if (std::abs(t - s) <= h + 1e-15) {
                    const double f = depthAt(k, t);
                    if (f > best) {
                        best = f;
                        at = pointAt(x.pieces[k], t);
                    }
                }
            }
            return std::make_pair(best, at);
        };
        const std::size_t n = samples.size();
        std::size_t start = 0;
        while (start < n && depth[start] > 0.0) {
            ++start;
        }
        Run run;
        bool open = false;
        for (std::size_t j = 0; j <= n; ++j) {
            const std::size_t i = (start + j) % n;
            const bool in = j < n && depth[i] > 0.0;
            if (in) {
                open = true;
                const V2 at = pointAt(x.pieces[samples[i].first], samples[i].second);
                run.add(at);
                const double before = depth[(i + n - 1) % n];
                const double after = depth[(i + 1) % n];
                if (depth[i] >= before && depth[i] >= after) {
                    const auto [best, where] = refine ? sharpen(i) : std::make_pair(depth[i], at);
                    if (best > run.depth) {
                        run.depth = best;
                        run.deepest = where;
                    }
                }
            }
            else if (open) {
                out.runs.push_back(run);
                run = Run {};
                open = false;
            }
            if (j < n) {
                out.deepest = std::max(out.deepest, depth[i]);
            }
        }
        // Apart, the nearest it comes
        if (refine) {
            for (std::size_t i = 0; i < n; ++i) {
                if (depth[i] >= depth[(i + n - 1) % n] && depth[i] >= depth[(i + 1) % n]) {
                    out.deepest = std::max(out.deepest, sharpen(i).first);
                }
            }
        }
    }
    for (const auto& run : out.runs) {
        out.deepest = std::max(out.deepest, run.depth);
    }
    return out;
}

struct Pair
{
    Shape a;
    Shape b;
    // B's start, turned about it, and the way it comes in: from 'far',
    // apart, along 'way' to 'near', overlapping
    V2 far;
    V2 near;
    double turn = 0.0;    // the most B is turned either way from its start
    double spread = 0.5;  // the most its way in is turned off straight, about 'far'
};

// The deepest one boundary goes into the other, B placed at 'at' turned by
// 'angle'
double overlap(const Placed& pa, const Shape& b, double angle, V2 at)
{
    Placed pb;
    place(b, angle, at, pb);
    return std::max(walk(pa.profile, pb.profile, false).deepest,
                    walk(pb.profile, pa.profile, false).deepest);
}

// Where profileTouches() says A and B touch, against the brute force's
// walks of A's boundary in B and B's in A; each fault found added
void check(const Profile2& a,
           const Profile2& b,
           const Walk& aInB,
           const Walk& bInA,
           const std::string& prefix,
           std::vector<std::string>& faults,
           int& points)
{
    const double deepest = std::max(aInB.deepest, bInA.deepest);
    std::vector<ProfileTouch> touches;
    profileTouches(a, b, envelope, touches);
    points += int(touches.size());
    auto fault = [&](const char* format, auto... args) {
        char text[400];
        std::snprintf(text, sizeof text, format, args...);
        faults.emplace_back(prefix + text);
    };
    double found = -std::numeric_limits<double>::infinity();
    std::vector<double> depthOf;
    for (const auto& t : touches) {
        const double depth = dot(t.normal, t.onA - t.onB);
        depthOf.push_back(depth);
        found = std::max(found, depth);
        double onA = std::numeric_limits<double>::infinity();
        for (const auto& p : a.pieces) {
            onA = std::min(onA, distanceTo(p, t.onA));
        }
        double onB = std::numeric_limits<double>::infinity();
        for (const auto& p : b.pieces) {
            onB = std::min(onB, distanceTo(p, t.onB));
        }
        if (onA > tolerance || onB > tolerance) {
            fault("a point off its boundary: A's by %.3g, B's by %.3g", onA, onB);
        }
        if (std::abs(norm(t.normal) - 1.0) > 1e-9) {
            fault("a normal of length %.12g", norm(t.normal));
        }
        // Its depth its own point's: A's in B, or B's in A
        const double ownA = depthIn(b, t.onA);
        const double ownB = depthIn(a, t.onB);
        if (std::min(std::abs(depth - ownA), std::abs(depth - ownB)) > tolerance) {
            fault("a point %.4g deep at (%.6g, %.6g), its own %.4g or %.4g",
                  depth,
                  t.onA.x,
                  t.onA.y,
                  ownA,
                  ownB);
        }
        if (depth > deepest + tolerance) {
            fault("a point %.4g deep, the deepest %.4g", depth, deepest);
        }
    }
    // The deepest found, or the nearest within the envelope
    if (deepest > -envelope + tolerance && !(found >= deepest - tolerance)) {
        fault("the deepest %.4g, found %.4g", deepest, found);
    }
    // Each run of one in the other has a point there as deep
    auto cover = [&](const Walk& w, bool ofA, const char* which) {
        for (const auto& run : w.runs) {
            if (!(run.depth > tolerance)) {
                continue;
            }
            bool covered = false;
            for (std::size_t i = 0; i < touches.size() && !covered; ++i) {
                const V2 x = ofA ? touches[i].onA : touches[i].onB;
                covered = depthOf[i] >= run.depth - tolerance && run.holds(x, 2e-4);
            }
            if (!covered) {
                fault("%s's boundary %.4g deep at (%.6g, %.6g), no point there",
                      which,
                      run.depth,
                      run.deepest.x,
                      run.deepest.y);
            }
        }
    };
    cover(aInB, true, "A");
    cover(bInA, false, "B");
}

}  // namespace

TEST(ProfileGeometry, pointsAgainstBruteForce)  // NOLINT
{
    const double mm = 1e-3;
    std::vector<Pair> pairs = {
        // A disc on a box's top, and coming at its corner
        {box(60 * mm, 20 * mm), disc(5 * mm), {0, 30 * mm}, {0, 5 * mm}, pi},
        {box(60 * mm, 20 * mm), disc(5 * mm), {40 * mm, 20 * mm}, {28 * mm, 8 * mm}, pi},
        // Boxes: a corner into a side, and flat on each other
        {box(60 * mm, 20 * mm), box(10 * mm, 10 * mm), {0, 30 * mm}, {0, 5 * mm}, pi},
        {box(60 * mm, 20 * mm), box(10 * mm, 10 * mm), {0, 30 * mm}, {0, 5 * mm}, 0.0},
        // A rounded box on a box, on its corners' arcs and its sides
        {box(60 * mm, 20 * mm),
         roundedBox(20 * mm, 10 * mm, 3 * mm),
         {0, 30 * mm},
         {0, 5 * mm},
         pi},
        {roundedBox(30 * mm, 20 * mm, 4 * mm),
         roundedBox(20 * mm, 10 * mm, 3 * mm),
         {0, 30 * mm},
         {0, 5 * mm},
         pi},
        // A disc on a disc
        {disc(10 * mm), disc(5 * mm), {0, 30 * mm}, {0, 5 * mm}, pi},
        // A knife edge into a V notch, and a disc into it
        {notched(40 * mm, 20 * mm, 8 * mm), knife(10 * mm), {0, 10 * mm}, {0, -10 * mm}, 0.4},
        // The knife's edge sunk below the notch's bottom, nearest its vertex
        {notched(40 * mm, 20 * mm, 8 * mm),
         knife(10 * mm),
         {0, 10 * mm},
         {0, -10 * mm},
         0.05,
         1e-3},
        {notched(40 * mm, 20 * mm, 8 * mm), disc(4 * mm), {0, 10 * mm}, {0, -6 * mm}, pi},
        // A pin in a hole, against its wall
        {holed(40 * mm, 3.005 * mm), disc(3 * mm), {0, 0}, {0, -1 * mm}, pi},
        // A pin in a slot, against its sides and into its ends
        {slotted(40 * mm, 6 * mm, 3.005 * mm), disc(3 * mm), {0, 0}, {0, -1 * mm}, pi, 0.3},
        {slotted(40 * mm, 6 * mm, 3.005 * mm), disc(3 * mm), {0, 0}, {10 * mm, 0}, pi, 0.3},
        // A disc in a groove of nearly its radius, and a box across it
        {grooved(40 * mm, 20 * mm, 5.01 * mm), disc(5 * mm), {0, 0}, {0, -1 * mm}, pi},
        {grooved(40 * mm, 20 * mm, 5 * mm), box(20 * mm, 4 * mm), {0, 10 * mm}, {0, -2 * mm}, 0.3},
        // A knife's edge on a disc
        {disc(20 * mm), knife(10 * mm), {0, 30 * mm}, {0, 15 * mm}, 0.5},
    };
    const std::vector<double> depths = {-0.5 * envelope, 1e-7, 1e-6, 1e-5, 1e-4};
    std::mt19937 random(20261102);
    int cases = 0;
    int failed = 0;
    int points = 0;
    for (std::size_t n = 0; n < pairs.size(); ++n) {
        const Pair& pair = pairs[n];
        Placed pa;
        place(pair.a, 0.0, {}, pa);
        for (int trial = 0; trial < 6; ++trial) {
            std::uniform_real_distribution<double> unit(-1.0, 1.0);
            const double angle = pair.turn * unit(random);
            const V2 way = turned(pair.near - pair.far, pair.spread * unit(random));
            const V2 near = pair.far + way;
            for (const double target : depths) {
                // Brought in to that depth, by bisection along its way in
                double lo = 0.0;  // apart
                double hi = 1.0;  // overlapping
                if (!(overlap(pa, pair.b, angle, near) > target)) {
                    continue;
                }
                for (int it = 0; it < 40; ++it) {
                    const double mid = 0.5 * (lo + hi);
                    const V2 at = pair.far + (near - pair.far) * mid;
                    if (overlap(pa, pair.b, angle, at) > target) {
                        hi = mid;
                    }
                    else {
                        lo = mid;
                    }
                }
                const V2 at = pair.far + (near - pair.far) * hi;
                Placed pb;
                place(pair.b, angle, at, pb);
                const Walk aInB = walk(pa.profile, pb.profile, true);
                const Walk bInA = walk(pb.profile, pa.profile, true);
                const double deepest = std::max(aInB.deepest, bInA.deepest);
                std::vector<std::string> faults;
                check(pa.profile, pb.profile, aInB, bInA, "", faults, points);
                check(pb.profile, pa.profile, bInA, aInB, "B first: ", faults, points);
                ++cases;
                if (!faults.empty()) {
                    ++failed;
                    std::string text;
                    for (const auto& f : faults) {
                        text += "\n  " + f;
                    }
                    ADD_FAILURE() << pair.a.name << " / " << pair.b.name << " (pair " << n
                                  << "), turned " << angle << ", at (" << at.x << ", " << at.y
                                  << "), " << target << " deep, deepest " << deepest << ":" << text;
                }
            }
        }
    }
    std::printf("Profile contact against brute force: %d cases, %d points, %d failed\n",
                cases,
                points,
                failed);
    EXPECT_GE(cases, 300);
}
