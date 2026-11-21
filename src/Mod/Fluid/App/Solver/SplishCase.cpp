// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_map>

#include "SplishCase.h"

using namespace Fluid::Solver;

namespace
{

Vec3 operator+(const Vec3& a, const Vec3& b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vec3 operator-(const Vec3& a, const Vec3& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 operator*(const Vec3& a, double f)
{
    return {a[0] * f, a[1] * f, a[2] * f};
}

double dot(const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double length(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}

bool unit(Vec3& a)
{
    const double l = length(a);
    if (l < 1e-300) {
        return false;
    }
    a = a * (1.0 / l);
    return true;
}

std::string num(double value)
{
    char text[40];
    std::snprintf(text, sizeof(text), "%.15g", value);
    return text;
}

std::string vec(const Vec3& v)
{
    return num(v[0]) + " " + num(v[1]) + " " + num(v[2]);
}

// Points kept apart: one is taken only when none is nearer than the
// distance.
class Apart
{
public:
    explicit Apart(double distance)
        : distance(distance)
    {}

    bool take(const Vec3& p)
    {
        const std::array<std::int64_t, 3> cell = cellOf(p);
        for (std::int64_t i = -1; i <= 1; ++i) {
            for (std::int64_t j = -1; j <= 1; ++j) {
                for (std::int64_t k = -1; k <= 1; ++k) {
                    const auto found = cells.find(keyOf({cell[0] + i, cell[1] + j, cell[2] + k}));
                    if (found == cells.end()) {
                        continue;
                    }
                    for (const Vec3& q : found->second) {
                        if (length(p - q) < distance) {
                            return false;
                        }
                    }
                }
            }
        }
        cells[keyOf(cell)].push_back(p);
        return true;
    }

private:
    std::array<std::int64_t, 3> cellOf(const Vec3& p) const
    {
        return {static_cast<std::int64_t>(std::floor(p[0] / distance)),
                static_cast<std::int64_t>(std::floor(p[1] / distance)),
                static_cast<std::int64_t>(std::floor(p[2] / distance))};
    }
    static std::uint64_t keyOf(const std::array<std::int64_t, 3>& cell)
    {
        // 21 bits a direction: two million cells each way
        const auto part = [](std::int64_t v) {
            return static_cast<std::uint64_t>(v + (1LL << 20)) & ((1ULL << 21) - 1);
        };
        return (part(cell[0]) << 42) | (part(cell[1]) << 21) | part(cell[2]);
    }

    double distance;
    std::unordered_map<std::uint64_t, std::vector<Vec3>> cells;
};

// Where an edge goes for the faces at it to end the same distance behind
// where they were: the shortest x with n . x = 1 for the faces' normals n.
// One face gives its normal, two at a right angle their sum.
Vec3 retreatOf(const std::vector<Vec3>& normals)
{
    // normals nearly the same are one face, curved: their mean stands for
    // them
    std::vector<Vec3> sums;
    for (const Vec3& n : normals) {
        bool placed = false;
        for (Vec3& sum : sums) {
            Vec3 mean = sum;
            if (unit(mean) && dot(mean, n) > 0.95) {
                sum = sum + n;
                placed = true;
                break;
            }
        }
        if (!placed) {
            sums.push_back(n);
        }
    }
    std::vector<Vec3> faces;
    for (Vec3 sum : sums) {
        if (!unit(sum)) {
            continue;
        }
        // only what the ones before do not already say
        if (faces.size() == 1 && length(cross(faces[0], sum)) < 0.05) {
            continue;
        }
        if (faces.size() == 2 && std::fabs(dot(cross(faces[0], faces[1]), sum)) < 0.05) {
            continue;
        }
        if (faces.size() < 3) {
            faces.push_back(sum);
        }
    }
    Vec3 x {0.0, 0.0, 0.0};
    if (faces.size() == 1) {
        x = faces[0];
    }
    else if (faces.size() == 2) {
        x = (faces[0] + faces[1]) * (1.0 / (1.0 + dot(faces[0], faces[1])));
    }
    else if (faces.size() == 3) {
        const double det = dot(faces[0], cross(faces[1], faces[2]));
        x = (cross(faces[1], faces[2]) + cross(faces[2], faces[0]) + cross(faces[0], faces[1]))
            * (1.0 / det);
    }
    // a knife's edge would send its corner far off
    const double l = length(x);
    return l > 3.0 ? x * (3.0 / l) : x;
}

// The distance from a point to a segment, in a plane.
double toSegment(const std::array<double, 2>& p,
                 const std::array<double, 2>& a,
                 const std::array<double, 2>& b)
{
    const double ex = b[0] - a[0];
    const double ey = b[1] - a[1];
    const double l2 = ex * ex + ey * ey;
    double t = l2 > 0.0 ? ((p[0] - a[0]) * ex + (p[1] - a[1]) * ey) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p[0] - (a[0] + t * ex), p[1] - (a[1] + t * ey));
}

// The sheet of particles behind a triangulation's faces: every face
// covered by one grid of the spacing in the plane the distance behind it,
// every crease by a row where the sheets of its two faces meet.
//
// fluidAt says whether fluid can be at a place, and decides what stays. A
// grid's particle stays when it is deep enough -- no fluid at it, nor
// within the distance of it in its plane -- and when fluid can rest
// against the wall in front of it, or within the distance of there. So a
// sheet stops the distance short of an edge the fluid goes round, where the
// next face's sheet takes over; and it runs on past an edge by the
// distance where the face ends against another part, or under one: there
// the other part's sheet is, and the two meet behind the corner the parts
// make together. A tank of plates that only touch has a slot along every
// corner without that, and lost two thirds of a dam break's water.
//
// A face no fluid can rest against -- a part touches another by it, or it
// looks out of the space -- gets nothing: nothing in front of it is fluid.
void sheetOf(const std::vector<Vec3>& points,
             const std::vector<std::array<int, 3>>& triangles,
             const Splish::Sizes& sizes,
             const std::function<bool(const Vec3&)>& fluidAt,
             std::vector<Vec3>& creases,
             std::vector<Vec3>& faces)
{
    // the same place is one corner, whichever triangle names it
    const double grain = sizes.radius * 1e-6;
    std::map<std::array<std::int64_t, 3>, int> places;
    std::vector<int> corner(points.size());
    std::vector<Vec3> at;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::array<std::int64_t, 3> key {std::llround(points[i][0] / grain),
                                                std::llround(points[i][1] / grain),
                                                std::llround(points[i][2] / grain)};
        const auto [found, fresh] = places.try_emplace(key, static_cast<int>(at.size()));
        if (fresh) {
            at.push_back(points[i]);
        }
        corner[i] = found->second;
    }

    struct Triangle
    {
        std::array<Vec3, 3> p;
        Vec3 normal;
        Vec3 middle;
    };
    std::vector<Triangle> kept;
    std::map<std::pair<int, int>, std::vector<std::size_t>> edges;
    for (const auto& t : triangles) {
        const std::array<int, 3> c {corner[static_cast<std::size_t>(t[0])],
                                    corner[static_cast<std::size_t>(t[1])],
                                    corner[static_cast<std::size_t>(t[2])]};
        const std::array<Vec3, 3> p {at[static_cast<std::size_t>(c[0])],
                                     at[static_cast<std::size_t>(c[1])],
                                     at[static_cast<std::size_t>(c[2])]};
        Vec3 n = cross(p[1] - p[0], p[2] - p[0]);
        if (c[0] == c[1] || c[1] == c[2] || c[0] == c[2] || !unit(n)) {
            continue;
        }
        for (std::size_t k = 0; k < 3; ++k) {
            const int a = c[k];
            const int b = c[(k + 1) % 3];
            edges[{std::min(a, b), std::max(a, b)}].push_back(kept.size());
        }
        kept.push_back({p, n, (p[0] + p[1] + p[2]) * (1.0 / 3.0)});
    }

    const double s = sizes.wallSpacing;
    const double b = sizes.behind;
    const double reach = b + sizes.radius;
    const double hair = 1e-6 * sizes.radius;

    // Creases: a row along every edge whose two triangles are not of one
    // flat face. Where fluid rests against both, the row is behind both.
    // Where it rests against one and the other is covered, the row is
    // behind the one and as far beyond the edge: the corner the two parts
    // make.
    for (const auto& [ends, sharing] : edges) {
        if (sharing.size() < 2) {
            continue;  // a rim: the face's own grid runs on past it
        }
        const Triangle& one = kept[sharing[0]];
        const Triangle& two = kept[sharing[1]];
        if (dot(one.normal, two.normal) > 0.9999) {
            continue;
        }
        const Vec3 from = at[static_cast<std::size_t>(ends.first)];
        const Vec3 to = at[static_cast<std::size_t>(ends.second)];
        const int steps = std::max(1, static_cast<int>(std::ceil(length(to - from) / s - 1e-9)));
        // whether fluid can rest against a triangle just beside a place on
        // the edge: asked a hair inside the triangle, a radius in front
        const auto faced = [&](const Triangle& t, const Vec3& q) {
            Vec3 in = t.middle - q;
            const double far = length(in);
            if (far > 0.0) {
                in = in * (std::min(1e-3 * sizes.radius, 0.5 * far) / far);
            }
            return fluidAt(q + in + t.normal * sizes.radius);
        };
        for (int k = 0; k <= steps; ++k) {
            const Vec3 q = from + (to - from) * (static_cast<double>(k) / steps);
            const bool first = faced(one, q);
            const bool second = faced(two, q);
            if (!first && !second) {
                continue;
            }
            const Vec3 towards = retreatOf({first ? one.normal : one.normal * -1.0,
                                            second ? two.normal : two.normal * -1.0});
            const Vec3 p = q - towards * b;
            if (fluidAt(p + towards * reach)) {
                creases.push_back(p);
            }
        }
    }

    // Faces: a grid laid in the face's plane from the plane alone, so the
    // triangles of one flat face share it; looked at over the triangle and
    // the distance around it.
    for (const Triangle& t : kept) {
        const Vec3& n = t.normal;
        std::size_t least = 0;
        for (std::size_t k = 1; k < 3; ++k) {
            if (std::fabs(n[k]) < std::fabs(n[least])) {
                least = k;
            }
        }
        Vec3 axis {0.0, 0.0, 0.0};
        axis[least] = 1.0;
        Vec3 u = cross(n, axis);
        unit(u);
        const Vec3 v = cross(n, u);
        const double height = dot(n, t.p[0]) - b;
        std::array<std::array<double, 2>, 3> q {};
        for (std::size_t k = 0; k < 3; ++k) {
            q[k] = {dot(t.p[k], u), dot(t.p[k], v)};
        }
        const double area = (q[1][0] - q[0][0]) * (q[2][1] - q[0][1])
            - (q[2][0] - q[0][0]) * (q[1][1] - q[0][1]);
        if (std::fabs(area) < 1e-300) {
            continue;
        }
        const auto [uLow, uHigh] = std::minmax({q[0][0], q[1][0], q[2][0]});
        const auto [vLow, vHigh] = std::minmax({q[0][1], q[1][1], q[2][1]});
        const double around = b * (1.0 + 1e-6);
        for (auto i = static_cast<long>(std::ceil((uLow - around) / s));
             static_cast<double>(i) * s <= uHigh + around;
             ++i) {
            for (auto j = static_cast<long>(std::ceil((vLow - around) / s));
                 static_cast<double>(j) * s <= vHigh + around;
                 ++j) {
                const std::array<double, 2> xy {static_cast<double>(i) * s,
                                                static_cast<double>(j) * s};
                // in the triangle, or within the distance of it
                bool in = true;
                for (std::size_t k = 0; k < 3 && in; ++k) {
                    const auto& e0 = q[k];
                    const auto& e1 = q[(k + 1) % 3];
                    const double side = ((e1[0] - e0[0]) * (xy[1] - e0[1])
                                         - (e1[1] - e0[1]) * (xy[0] - e0[0]))
                        / area;
                    in = side >= -1e-9;
                }
                for (std::size_t k = 0; k < 3 && !in; ++k) {
                    in = toSegment(xy, q[k], q[(k + 1) % 3]) <= around;
                }
                if (!in) {
                    continue;
                }
                const Vec3 p = n * height + u * xy[0] + v * xy[1];
                const Vec3 front = p + n * reach;
                bool deep = !fluidAt(p);
                bool reached = fluidAt(front);
                for (int du = -1; du <= 1 && deep; ++du) {
                    for (int dv = -1; dv <= 1 && deep; ++dv) {
                        if (du == 0 && dv == 0) {
                            continue;
                        }
                        const Vec3 aside = u * static_cast<double>(du) + v * static_cast<double>(dv);
                        deep = !fluidAt(p + aside * (b - hair));
                        reached = reached || fluidAt(front + aside * (b + hair));
                    }
                }
                if (deep && reached) {
                    faces.push_back(p);
                }
            }
        }
    }
}

// The space's closed sides as triangles, wound to face into the space as a
// part's faces face into the fluid.
void sidesOf(const Space& space,
             std::vector<Vec3>& points,
             std::vector<std::array<int, 3>>& triangles)
{
    for (int i = 0; i < 8; ++i) {
        points.push_back({(i & 1) ? space.max[0] : space.min[0],
                          (i & 2) ? space.max[1] : space.min[1],
                          (i & 4) ? space.max[2] : space.min[2]});
    }
    // the sides in the space's order: -x, +x, -y, +y, -z, +z; outwards
    const std::array<std::array<std::array<int, 3>, 2>, 6> outwards {
        {{{{0, 4, 6}, {0, 6, 2}}},
         {{{1, 3, 7}, {1, 7, 5}}},
         {{{0, 1, 5}, {0, 5, 4}}},
         {{{2, 6, 7}, {2, 7, 3}}},
         {{{0, 2, 3}, {0, 3, 1}}},
         {{{4, 5, 7}, {4, 7, 6}}}}};
    for (std::size_t side = 0; side < 6; ++side) {
        if (space.open[side]) {
            continue;
        }
        for (const auto& t : outwards[side]) {
            triangles.push_back({t[0], t[2], t[1]});
        }
    }
}

struct Part
{
    const Surface* surface = nullptr;
    Vec3 min {};
    Vec3 max {};
};

std::vector<Part> partsOf(const Model& model)
{
    std::vector<Part> parts;
    for (const auto& wall : model.walls) {
        Part part;
        part.surface = &wall.surface;
        if (boxOf(wall.surface, part.min, part.max)) {
            parts.push_back(part);
        }
    }
    return parts;
}

bool insideAPart(const std::vector<Part>& parts, const Vec3& p)
{
    for (const Part& part : parts) {
        bool near = true;
        for (std::size_t k = 0; k < 3 && near; ++k) {
            near = p[k] >= part.min[k] && p[k] <= part.max[k];
        }
        if (near && inside(*part.surface, p)) {
            return true;
        }
    }
    return false;
}

// The point of a triangle nearest a point.
Vec3 nearestOn(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& p)
{
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const double d1 = dot(ab, ap);
    const double d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return a;
    }
    const Vec3 bp = p - b;
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return b;
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return a + ab * (d1 / (d1 - d3));
    }
    const Vec3 cp = p - c;
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return c;
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return a + ac * (d2 / (d2 - d6));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// The way the wall at a point faces: the normal of the nearest face of a
// part or of a closed side of the space, when one is within the distance.
bool wallAt(const Model& model, const Vec3& p, double within, Vec3& normal)
{
    double best = within;
    bool found = false;
    const auto look = [&](const std::vector<Vec3>& points,
                          const std::vector<std::array<int, 3>>& triangles) {
        for (const auto& t : triangles) {
            const Vec3& a = points[static_cast<std::size_t>(t[0])];
            const Vec3& b = points[static_cast<std::size_t>(t[1])];
            const Vec3& c = points[static_cast<std::size_t>(t[2])];
            Vec3 n = cross(b - a, c - a);
            if (!unit(n)) {
                continue;
            }
            const double distance = length(p - nearestOn(a, b, c, p));
            if (distance <= best) {
                best = distance;
                normal = n;
                found = true;
            }
        }
    };
    for (const auto& wall : model.walls) {
        look(wall.surface.points, wall.surface.triangles);
    }
    std::vector<Vec3> points;
    std::vector<std::array<int, 3>> triangles;
    sidesOf(model.space, points, triangles);
    look(points, triangles);
    return found;
}

std::string bytesOf(const std::vector<Vec3>& points)
{
    std::string bytes(points.size() * 3 * sizeof(float), '\0');
    std::size_t at = 0;
    for (const Vec3& p : points) {
        const std::array<float, 3> f {static_cast<float>(p[0]),
                                      static_cast<float>(p[1]),
                                      static_cast<float>(p[2])};
        std::memcpy(bytes.data() + at, f.data(), sizeof(f));
        at += sizeof(f);
    }
    return bytes;
}

bool numberOf(const std::string& word, double& value)
{
    if (word.empty()) {
        return false;
    }
    char* end = nullptr;
    value = std::strtod(word.c_str(), &end);
    return end == word.c_str() + word.size();
}

}  // namespace

Splish::Sizes Splish::sizesOf(double resolution)
{
    Sizes sizes;
    sizes.radius = 0.5 * resolution;
    // Measured on still water (sec 12 S3): at 1.2 radii the layer against
    // the wall has the rest density, at 1.0 it is 12% over and the first
    // step throws the water off the wall.
    sizes.behind = 1.2 * sizes.radius;
    // the solver's own samplers use this
    sizes.wallSpacing = 1.5 * sizes.radius;
    return sizes;
}

std::vector<Vec3> Splish::boundaryOf(const Model& model)
{
    const Sizes sizes = sizesOf(model.resolution);
    const std::vector<Part> parts = partsOf(model);
    const double slack = 1e-9 * sizes.radius;
    // Whether fluid can be at a place: in the space and in no part.
    const auto fluidAt = [&](const Vec3& p) {
        for (std::size_t k = 0; k < 3; ++k) {
            if (p[k] < model.space.min[k] - slack || p[k] > model.space.max[k] + slack) {
                return false;
            }
        }
        // The space is as a rule the parts' own box, so a rim's particle
        // looks out exactly along a part's face: asked a hair nearer the
        // space's middle, on which side it is has an answer.
        Vec3 asked = p;
        for (std::size_t k = 0; k < 3; ++k) {
            const double middle = 0.5 * (model.space.min[k] + model.space.max[k]);
            asked[k] += (middle > p[k] ? 1.0 : -1.0) * 1e-3 * sizes.radius;
        }
        return !insideAPart(parts, asked);
    };

    std::vector<Vec3> creases;
    std::vector<Vec3> faces;
    for (const auto& wall : model.walls) {
        sheetOf(wall.surface.points, wall.surface.triangles, sizes, fluidAt, creases, faces);
    }
    std::vector<Vec3> points;
    std::vector<std::array<int, 3>> triangles;
    sidesOf(model.space, points, triangles);
    sheetOf(points, triangles, sizes, fluidAt, creases, faces);

    // Creases first: where a face's grid comes near one, the crease's row
    // is what stays.
    Apart apart(0.4 * sizes.wallSpacing);
    std::vector<Vec3> kept;
    for (const auto* samples : {&creases, &faces}) {
        for (const Vec3& p : *samples) {
            if (apart.take(p)) {
                kept.push_back(p);
            }
        }
    }
    return kept;
}

std::vector<Vec3> Splish::particlesOf(const Model& model)
{
    const std::vector<Part> parts = partsOf(model);
    std::vector<Vec3> particles;
    for (const auto& fluid : model.fluids) {
        for (const Vec3& p : sampled(fluid.surface, model.resolution)) {
            if (!insideAPart(parts, p)) {
                particles.push_back(p);
            }
        }
    }
    return particles;
}

bool Splish::makeCase(const Model& model, const CaseOptions& options, Case& made, std::string& why)
{
    made = Case();
    if (!(model.resolution > 0.0)) {
        why = "the resolution must be more than 0";
        return false;
    }
    if (model.fluids.empty()) {
        why = "the model has no fluid";
        return false;
    }
    const Material& liquid = model.fluids.front().material;
    for (const auto& fluid : model.fluids) {
        if (fluid.material.density != liquid.density
            || fluid.material.viscosity != liquid.viscosity) {
            why = "this backend runs one liquid: " + fluid.source + " is not of "
                + model.fluids.front().source + "'s material";
            return false;
        }
    }
    if (!(liquid.density > 0.0)) {
        why = "the liquid's density must be more than 0";
        return false;
    }
    for (std::size_t k = 0; k < 3; ++k) {
        if (!(model.space.max[k] > model.space.min[k])) {
            why = "the space has no size";
            return false;
        }
    }
    const std::vector<Vec3> particles = particlesOf(model);
    if (particles.empty()) {
        why = "there is no fluid left outside the parts at this resolution";
        return false;
    }
    const std::vector<Vec3> boundary = boundaryOf(model);
    const Sizes sizes = sizesOf(model.resolution);

    std::ostringstream text;
    text << "fluidsplish 1\n"
         << "radius " << num(sizes.radius) << "\n"
         << "gravity " << vec(model.gravity) << "\n"
         << "end " << num(model.timeEnd) << "\n"
         << "interval " << num(model.outputInterval) << "\n"
         << "density " << num(liquid.density) << "\n"
         << "viscosity " << num(liquid.viscosity / liquid.density) << "\n"
         << "threads " << std::max(options.threads, 1) << "\n"
         << "space " << vec(model.space.min) << " " << vec(model.space.max) << "\n"
         << "boundary " << boundary.size() << "\n"
         << "fluid " << particles.size() << "\n";
    for (const auto& monitor : model.monitors) {
        text << "monitor " << (monitor.kind == MonitorKind::Pressure ? "pressure " : "height ")
             << vec(monitor.point);
        // A pressure on a wall is the fluid's push on the wall there, and
        // the runner needs the way the wall faces; one in the open fluid
        // is the fluid's own.
        Vec3 normal {};
        if (monitor.kind == MonitorKind::Pressure
            && wallAt(model, monitor.point, model.resolution, normal)) {
            text << " " << vec(normal);
        }
        text << "\n";
    }
    made.files["case.txt"] = text.str();
    made.files["fluid.points"] = bytesOf(particles);
    made.files["boundary.points"] = bytesOf(boundary);
    made.particles = particles.size();
    made.boundary = boundary.size();
    return true;
}

Splish::Event Splish::parseLine(const std::string& line)
{
    Event event;
    std::istringstream in(line);
    std::string word;
    if (!(in >> word)) {
        return event;
    }
    const auto rest = [&] {
        std::string text;
        std::getline(in, text);
        const auto first = text.find_first_not_of(" \t");
        const auto last = text.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };
    if (word == "error") {
        event.kind = Event::Error;
        event.text = rest();
    }
    else if (word == "version") {
        event.kind = Event::Version;
        event.text = rest();
    }
    else if (word == "frame") {
        std::string index;
        std::string time;
        std::string count;
        double i = 0.0;
        double n = 0.0;
        if ((in >> index >> time >> count) && numberOf(index, i) && numberOf(time, event.time)
            && numberOf(count, n) && i >= 0.0 && n >= 0.0) {
            event.kind = Event::Frame;
            event.index = static_cast<int>(i);
            event.count = static_cast<std::size_t>(n);
        }
    }
    else if (word == "sample") {
        std::string time;
        if ((in >> time) && numberOf(time, event.time)) {
            event.kind = Event::Sample;
            std::string value;
            while (in >> value) {
                double v = 0.0;
                if (!numberOf(value, v)) {
                    event = Event();
                    break;
                }
                event.values.push_back(v);
            }
        }
    }
    return event;
}

bool Splish::pointsOf(const std::string& bytes, std::size_t count, std::vector<Vec3>& points)
{
    if (bytes.size() != count * 3 * sizeof(float)) {
        return false;
    }
    points.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::array<float, 3> f {};
        std::memcpy(f.data(), bytes.data() + i * sizeof(f), sizeof(f));
        points[i] = {f[0], f[1], f[2]};
    }
    return true;
}
