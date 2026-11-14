// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef RENDER_MOTIONCLIP_H
#define RENDER_MOTIONCLIP_H

// An Animation's motion as a viewer can play it by itself
// (docs/Animation.md sec 9, "Streaming").
//
// The serving process steps the Animation through its range and reads,
// from the scene feed it would publish, where each object's draws are
// (MotionClipBaker). An object whose draws all moved by one rigid
// transform has a track: that transform against the first frame, sampled.
// A viewer holding the clip and a clock (MotionClock) poses the draws of
// the publish it last took by itself, so the serving process need not
// publish a frame for what the clip already says.
//
// Header only, and nothing of Qt or of a backend: the desktop bakes with
// it, the browser's viewer plays with it, and the tests read both.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Renderer.h"

namespace Render
{

namespace Motion
{

// 4x4, GL layout: element (row, col) at [col * 4 + row], the translation
// in [12..14]; a point is transformed as M p. DrawCall::model is this.

inline void identity(double m[16])
{
    for (int i = 0; i < 16; ++i)
        m[i] = (i % 5 == 0) ? 1.0 : 0.0;
}

/// out = a b: b first, then a
inline void mul(const double a[16], const double b[16], double out[16])
{
    double r[16];
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    }
    std::memcpy(out, r, sizeof(r));
}

/// The inverse of an affine matrix (last row 0 0 0 1); false when it has none
inline bool invertAffine(const double m[16], double out[16])
{
    const double a = m[0], b = m[4], c = m[8];
    const double d = m[1], e = m[5], f = m[9];
    const double g = m[2], h = m[6], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-300))
        return false;
    const double inv = 1.0 / det;
    double r[16];
    r[0] = A * inv;
    r[4] = -(b * i - c * h) * inv;
    r[8] = (b * f - c * e) * inv;
    r[1] = B * inv;
    r[5] = (a * i - c * g) * inv;
    r[9] = -(a * f - c * d) * inv;
    r[2] = C * inv;
    r[6] = -(a * h - b * g) * inv;
    r[10] = (a * e - b * d) * inv;
    for (int row = 0; row < 3; ++row) {
        r[12 + row] = -(r[row] * m[12] + r[4 + row] * m[13] + r[8 + row] * m[14]);
    }
    r[3] = r[7] = r[11] = 0.0;
    r[15] = 1.0;
    std::memcpy(out, r, sizeof(r));
    return true;
}

inline void modelOf(const DrawCall &draw, double out[16])
{
    if (draw.identity) {
        identity(out);
        return;
    }
    for (int i = 0; i < 16; ++i)
        out[i] = draw.model[i];
}

/// A rigid transform as a translation and a unit quaternion (x y z w)
struct Pose {
    double t[3] = {0.0, 0.0, 0.0};
    double q[4] = {0.0, 0.0, 0.0, 1.0};
};

inline void matrixOf(const Pose &p, double m[16])
{
    const double x = p.q[0], y = p.q[1], z = p.q[2], w = p.q[3];
    m[0] = 1.0 - 2.0 * (y * y + z * z);
    m[1] = 2.0 * (x * y + z * w);
    m[2] = 2.0 * (x * z - y * w);
    m[3] = 0.0;
    m[4] = 2.0 * (x * y - z * w);
    m[5] = 1.0 - 2.0 * (x * x + z * z);
    m[6] = 2.0 * (y * z + x * w);
    m[7] = 0.0;
    m[8] = 2.0 * (x * z + y * w);
    m[9] = 2.0 * (y * z - x * w);
    m[10] = 1.0 - 2.0 * (x * x + y * y);
    m[11] = 0.0;
    m[12] = p.t[0];
    m[13] = p.t[1];
    m[14] = p.t[2];
    m[15] = 1.0;
}

/// The pose of a matrix that is a rotation and a translation, within
/// \a tol of one; false for anything else (a scale, a shear, a mirror)
inline bool poseOf(const double m[16], Pose &p, double tol = 1e-4)
{
    // Columns of unit length and at right angles, right-handed
    for (int a = 0; a < 3; ++a) {
        for (int b = a; b < 3; ++b) {
            double dot = 0.0;
            for (int r = 0; r < 3; ++r)
                dot += m[a * 4 + r] * m[b * 4 + r];
            if (std::fabs(dot - (a == b ? 1.0 : 0.0)) > tol)
                return false;
        }
    }
    const double det = m[0] * (m[5] * m[10] - m[9] * m[6])
        - m[4] * (m[1] * m[10] - m[9] * m[2])
        + m[8] * (m[1] * m[6] - m[5] * m[2]);
    if (det < 0.0)
        return false;
    if (std::fabs(m[3]) > tol || std::fabs(m[7]) > tol || std::fabs(m[11]) > tol
        || std::fabs(m[15] - 1.0) > tol)
        return false;
    const double r00 = m[0], r11 = m[5], r22 = m[10];
    const double trace = r00 + r11 + r22;
    double x, y, z, w;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        w = 0.25 * s;
        x = (m[6] - m[9]) / s;
        y = (m[8] - m[2]) / s;
        z = (m[1] - m[4]) / s;
    }
    else if (r00 > r11 && r00 > r22) {
        const double s = std::sqrt(1.0 + r00 - r11 - r22) * 2.0;
        w = (m[6] - m[9]) / s;
        x = 0.25 * s;
        y = (m[4] + m[1]) / s;
        z = (m[8] + m[2]) / s;
    }
    else if (r11 > r22) {
        const double s = std::sqrt(1.0 + r11 - r00 - r22) * 2.0;
        w = (m[8] - m[2]) / s;
        x = (m[4] + m[1]) / s;
        y = 0.25 * s;
        z = (m[9] + m[6]) / s;
    }
    else {
        const double s = std::sqrt(1.0 + r22 - r00 - r11) * 2.0;
        w = (m[1] - m[4]) / s;
        x = (m[8] + m[2]) / s;
        y = (m[9] + m[6]) / s;
        z = 0.25 * s;
    }
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 0.0))
        return false;
    p.q[0] = x / n;
    p.q[1] = y / n;
    p.q[2] = z / n;
    p.q[3] = w / n;
    p.t[0] = m[12];
    p.t[1] = m[13];
    p.t[2] = m[14];
    return true;
}

/// Between two poses: the translation straight, the rotation the short way
inline Pose mix(const Pose &a, const Pose &b, double u)
{
    Pose r;
    for (int i = 0; i < 3; ++i)
        r.t[i] = a.t[i] + (b.t[i] - a.t[i]) * u;
    double dot = 0.0;
    for (int i = 0; i < 4; ++i)
        dot += a.q[i] * b.q[i];
    const double sign = dot < 0.0 ? -1.0 : 1.0;
    dot = std::fabs(dot);
    double wa = 1.0 - u, wb = u;
    if (dot < 0.9995) {
        const double angle = std::acos(dot);
        const double s = std::sin(angle);
        wa = std::sin((1.0 - u) * angle) / s;
        wb = std::sin(u * angle) / s;
    }
    double n = 0.0;
    for (int i = 0; i < 4; ++i) {
        r.q[i] = wa * a.q[i] + sign * wb * b.q[i];
        n += r.q[i] * r.q[i];
    }
    n = std::sqrt(n);
    for (int i = 0; i < 4; ++i)
        r.q[i] = n > 0.0 ? r.q[i] / n : (i == 3 ? 1.0 : 0.0);
    return r;
}

/// Move a draw by \a x: its model, and the world-space box it states
inline void moveDraw(DrawCall &draw, const double x[16])
{
    double model[16], moved[16];
    modelOf(draw, model);
    mul(x, model, moved);
    for (int i = 0; i < 16; ++i)
        draw.model[i] = float(moved[i]);
    draw.identity = false;
    if (draw.bboxMin[0] > draw.bboxMax[0])
        return;
    double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    for (int corner = 0; corner < 8; ++corner) {
        const double p[3] = {(corner & 1) ? draw.bboxMax[0] : draw.bboxMin[0],
                             (corner & 2) ? draw.bboxMax[1] : draw.bboxMin[1],
                             (corner & 4) ? draw.bboxMax[2] : draw.bboxMin[2]};
        for (int r = 0; r < 3; ++r) {
            const double v = x[r] * p[0] + x[4 + r] * p[1] + x[8 + r] * p[2] + x[12 + r];
            if (corner == 0 || v < lo[r])
                lo[r] = v;
            if (corner == 0 || v > hi[r])
                hi[r] = v;
        }
    }
    for (int r = 0; r < 3; ++r) {
        draw.bboxMin[r] = float(lo[r]);
        draw.bboxMax[r] = float(hi[r]);
    }
}

inline const char *base64Alphabet()
{
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

inline std::string base64(const uint8_t *data, size_t size)
{
    const char *abc = base64Alphabet();
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
        const uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out += abc[(v >> 18) & 63];
        out += abc[(v >> 12) & 63];
        out += i + 1 < size ? abc[(v >> 6) & 63] : '=';
        out += i + 2 < size ? abc[v & 63] : '=';
    }
    return out;
}

/// Decodes up to the closing quote (or the end); false on a stray character
inline bool unbase64(const char *text, std::vector<uint8_t> &out)
{
    static int8_t table[256];
    static bool ready = false;
    if (!ready) {
        std::memset(table, -1, sizeof(table));
        const char *abc = base64Alphabet();
        for (int i = 0; i < 64; ++i)
            table[uint8_t(abc[i])] = int8_t(i);
        ready = true;
    }
    uint32_t acc = 0;
    int bits = 0;
    for (const char *p = text; *p && *p != '"'; ++p) {
        if (*p == '=')
            break;
        const int v = table[uint8_t(*p)];
        if (v < 0)
            return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((acc >> bits) & 0xff));
        }
    }
    return true;
}

/// The number after `"name":` in a flat JSON object; \a fallback without one
inline double jsonNumber(const char *json, const char *name, double fallback)
{
    std::string key = std::string("\"") + name + "\":";
    const char *p = std::strstr(json, key.c_str());
    if (!p)
        return fallback;
    p += key.size();
    if (std::strncmp(p, "true", 4) == 0)
        return 1.0;
    if (std::strncmp(p, "false", 5) == 0)
        return 0.0;
    char *end = nullptr;
    const double v = std::strtod(p, &end);
    return end == p ? fallback : v;
}

/// The strings of `"name":["..",..]`, none escaped; false without the array
inline bool jsonStrings(const char *json, const char *name, std::vector<std::string> &out)
{
    out.clear();
    std::string key = std::string("\"") + name + "\":[";
    const char *p = std::strstr(json, key.c_str());
    if (!p)
        return false;
    p += key.size();
    while (*p && *p != ']') {
        if (*p != '"') {
            ++p;
            continue;
        }
        const char *end = std::strchr(p + 1, '"');
        if (!end)
            return false;
        out.emplace_back(p + 1, end);
        p = end + 1;
    }
    return *p == ']';
}

/// The integers of `"name":[..]`; false without the array
inline bool jsonInts(const char *json, const char *name, std::vector<long long> &out)
{
    out.clear();
    std::string key = std::string("\"") + name + "\":[";
    const char *p = std::strstr(json, key.c_str());
    if (!p)
        return false;
    p += key.size();
    while (*p && *p != ']') {
        if (*p == ',' || *p == ' ') {
            ++p;
            continue;
        }
        char *end = nullptr;
        const long long v = std::strtoll(p, &end, 10);
        if (end == p)
            return false;
        out.push_back(v);
        p = end;
    }
    return *p == ']';
}

/// The bytes of `"name":"<base64>"`; empty without it
inline bool jsonBytes(const char *json, const char *name, std::vector<uint8_t> &out)
{
    out.clear();
    std::string key = std::string("\"") + name + "\":\"";
    const char *p = std::strstr(json, key.c_str());
    return !p || unbase64(p + key.size(), out);
}

}  // namespace Motion

/// What a draw looks like, as far as an Animation changes it without
/// changing its geometry: a colour, a transparency, a line's pattern or
/// width, a style. The fields of Material that MotionClipBaker compares;
/// the rest of a draw's material stays as the publish has it.
struct MotionLook {
    /// transparent 1, ontop 2, lighting 4, twoside 8, pervertexcolor 16,
    /// perfacematerial 32, depthtest 64, depthwrite 128; drawstyle << 8
    uint32_t bits = 0;
    uint32_t diffuse = 0;
    uint32_t emissive = 0;
    uint32_t specular = 0;
    uint32_t ambient = 0;
    uint32_t linecolor = 0;
    uint32_t linepattern = 0;
    float shininess = 0.0f;
    float linewidth = 0.0f;
    float pointsize = 0.0f;

    static MotionLook of(const Material &m)
    {
        MotionLook l;
        l.bits = (m.transparent ? 1U : 0U) | (m.ontop ? 2U : 0U) | (m.lighting ? 4U : 0U)
            | (m.twoside ? 8U : 0U) | (m.pervertexcolor ? 16U : 0U)
            | (m.perfacematerial ? 32U : 0U) | (m.depthtest ? 64U : 0U)
            | (m.depthwrite ? 128U : 0U) | (uint32_t(m.drawstyle) << 8);
        l.diffuse = m.diffuse;
        l.emissive = m.emissive;
        l.specular = m.specular;
        l.ambient = m.ambient;
        l.linecolor = m.linecolor;
        l.linepattern = m.linepattern;
        l.shininess = m.shininess;
        l.linewidth = m.linewidth;
        l.pointsize = m.pointsize;
        return l;
    }

    void applyTo(Material &m) const
    {
        m.transparent = (bits & 1U) != 0;
        m.ontop = (bits & 2U) != 0;
        m.lighting = (bits & 4U) != 0;
        m.twoside = (bits & 8U) != 0;
        m.pervertexcolor = (bits & 16U) != 0;
        m.perfacematerial = (bits & 32U) != 0;
        m.depthtest = (bits & 64U) != 0;
        m.depthwrite = (bits & 128U) != 0;
        m.drawstyle = uint8_t((bits >> 8) & 0xffU);
        m.diffuse = diffuse;
        m.emissive = emissive;
        m.specular = specular;
        m.ambient = ambient;
        m.linecolor = linecolor;
        m.linepattern = linepattern;
        m.shininess = shininess;
        m.linewidth = linewidth;
        m.pointsize = pointsize;
    }

    bool operator==(const MotionLook &o) const
    {
        return std::memcmp(this, &o, sizeof(MotionLook)) == 0;
    }
    bool operator!=(const MotionLook &o) const
    {
        return !(*this == o);
    }

    /// Between \a a and \a b, \a u of the way: the colours channel by
    /// channel, the widths and shininess linearly. Where the flags or
    /// patterns differ there is no between, and the nearer one is taken.
    static MotionLook mix(const MotionLook &a, const MotionLook &b, double u)
    {
        if (a.bits != b.bits || a.linepattern != b.linepattern)
            return u < 0.5 ? a : b;
        auto rgba = [u](uint32_t x, uint32_t y) {
            uint32_t out = 0;
            for (int s = 0; s < 32; s += 8) {
                const double p = double((x >> s) & 0xffU), q = double((y >> s) & 0xffU);
                out |= uint32_t(std::lround(p + (q - p) * u)) << s;
            }
            return out;
        };
        auto lin = [u](float x, float y) {
            return float(x + (y - x) * u);
        };
        MotionLook l = a;
        l.diffuse = rgba(a.diffuse, b.diffuse);
        l.emissive = rgba(a.emissive, b.emissive);
        l.specular = rgba(a.specular, b.specular);
        l.ambient = rgba(a.ambient, b.ambient);
        l.linecolor = rgba(a.linecolor, b.linecolor);
        l.shininess = lin(a.shininess, b.shininess);
        l.linewidth = lin(a.linewidth, b.linewidth);
        l.pointsize = lin(a.pointsize, b.pointsize);
        return l;
    }
};
static_assert(sizeof(MotionLook) == 40, "MotionLook rides the wire as it is laid out");

/// Which of an object's draws a look is for: enough to find it again
/// among the draws of a publish, whatever their order
struct MotionSlot {
    int32_t type = 0;
    int32_t partIndex = -1;
    int32_t indexStart = 0;
    int32_t indexCount = 0;

    static MotionSlot of(const DrawCall &d)
    {
        MotionSlot s;
        s.type = int32_t(d.material.type);
        s.partIndex = d.partIndex;
        s.indexStart = d.indexStart;
        s.indexCount = d.indexCount;
        return s;
    }
    bool operator==(const MotionSlot &o) const
    {
        return type == o.type && partIndex == o.partIndex && indexStart == o.indexStart
            && indexCount == o.indexCount;
    }
};

/// The tracks of one bake: for each objectKey a pose per frame, against
/// the first frame it was shown in.
///
/// Three more kinds of what an Animation does to the feed ride along
/// (docs/Animation.md sec 9.1):
/// - a track's object may be hidden at some frames (`shown`). Its draws
///   at the frame its poses are measured from are a blob (`refBlob`),
///   so a viewer holding a publish taken while it was hidden can still
///   show it;
/// - a track's object may change how it looks -- a colour, a
///   transparency -- with its geometry moved rigidly or not at all: its
///   looks at every frame, a record a draw (`looks`), and a viewer sets
///   them on the publish's draws;
/// - an object that does not move rigidly -- rebuilt, its draws moving
///   apart -- is a sequence: its draws at every frame are a blob each,
///   and a viewer shows the one of the frame nearest.
///
/// A blob is a self-contained scene snapshot (SceneDump) holding the
/// object's draws, addressed by the SHA-1 of its bytes and served by
/// the scene server like any other chunk.
struct MotionClip {
    /// Which bake this is; 0 = none. A viewer plays only the clip its
    /// clock names.
    uint32_t id = 0;
    double start = 0.0;  ///< the first frame's time, seconds
    double step = 0.0;   ///< between frames
    uint32_t frames = 0;
    std::vector<uint64_t> keys;
    /// keys.size() x frames x 7: tx ty tz qx qy qz qw
    std::vector<float> data;
    /// keys.size() x frames: 1 where the track's object is shown; empty
    /// when every one is shown throughout
    std::vector<uint8_t> shown;
    /// Per track, with `shown`: the frame the poses are measured from and
    /// the blob of the draws there, -1 for none
    std::vector<int32_t> refFrame;
    std::vector<int32_t> refBlob;
    /// The sequences: seqKeys.size() x frames, the blob of the object's
    /// draws at each frame, -1 where it is not shown
    std::vector<uint64_t> seqKeys;
    std::vector<int32_t> seq;
    /// What the blob numbers above name: content keys, and their sizes
    std::vector<std::string> blobs;
    std::vector<uint32_t> blobSizes;
    /// The tracks whose objects change how they look, by track number:
    /// row r is track lookTracks[r], whose draws lookSlots name --
    /// lookCount[r] of them, the rows' slots one after another. lookAt is
    /// lookTracks.size() x frames: where in `looks` that frame's records
    /// start, one a slot. Frames that look alike share their records.
    std::vector<uint32_t> lookTracks;
    std::vector<uint32_t> lookCount;
    std::vector<MotionSlot> lookSlots;
    std::vector<int32_t> lookAt;
    std::vector<MotionLook> looks;
    /// Every change the sweep made to the feed is in the clip: nothing
    /// changed that a track or a sequence does not say. Only then may a
    /// publish be held for what the Animation writes.
    bool exact = false;

    bool empty() const
    {
        return !id || (keys.empty() && seqKeys.empty()) || frames < 2 || !(step > 0.0);
    }

    /// The frame nearest \a t, clamped to the range
    uint32_t frameAt(double t) const
    {
        if (frames < 2 || !(step > 0.0))
            return 0;
        const double u = std::floor((t - start) / step + 0.5);
        return uint32_t(std::max(0.0, std::min(u, double(frames - 1))));
    }

    double timeOf(uint32_t frame) const
    {
        return start + frame * step;
    }

    /// Whether the track's object is shown at \a t
    bool shownAt(size_t track, double t) const
    {
        return shown.empty() || shown[track * frames + frameAt(t)] != 0;
    }

    /// The blob of sequence \a index at \a t; -1 where it is not shown
    int32_t blobAt(size_t index, double t) const
    {
        return seq[index * frames + frameAt(t)];
    }

    /// Where row \a row's slots start in lookSlots
    size_t lookSlotStart(size_t row) const
    {
        size_t at = 0;
        for (size_t r = 0; r < row; ++r)
            at += lookCount[r];
        return at;
    }

    /// Row \a row's looks at \a t, one a slot: between the two frames
    /// about it (MotionLook::mix), clamped to the range
    void looksAt(size_t row, double t, std::vector<MotionLook> &out) const
    {
        const size_t n = lookCount[row];
        out.resize(n);
        if (!n || frames == 0)
            return;
        double u = frames > 1 && step > 0.0 ? (t - start) / step : 0.0;
        u = std::max(0.0, std::min(u, double(frames - 1)));
        const uint32_t a = frames > 1 ? std::min(uint32_t(u), frames - 2) : 0;
        const uint32_t b = frames > 1 ? a + 1 : 0;
        const MotionLook *la = &looks[size_t(lookAt[row * frames + a])];
        const MotionLook *lb = &looks[size_t(lookAt[row * frames + b])];
        for (size_t i = 0; i < n; ++i)
            out[i] = MotionLook::mix(la[i], lb[i], u - a);
    }

    Motion::Pose poseAt(size_t track, uint32_t frame) const
    {
        Motion::Pose p;
        const float *v = &data[(track * frames + frame) * 7];
        for (int i = 0; i < 3; ++i)
            p.t[i] = v[i];
        for (int i = 0; i < 4; ++i)
            p.q[i] = v[3 + i];
        return p;
    }

    /// The track's transform at \a t, clamped to the range
    void matrixAt(size_t track, double t, double out[16]) const
    {
        double u = (t - start) / step;
        u = std::max(0.0, std::min(u, double(frames - 1)));
        const uint32_t a = std::min(uint32_t(u), frames - 2);
        Motion::matrixOf(Motion::mix(poseAt(track, a), poseAt(track, a + 1), u - a), out);
    }

    /// What moves a draw taken at \a base to where it is at \a t
    bool moveBetween(size_t track, double base, double t, double out[16]) const
    {
        double at[16], from[16], back[16];
        matrixAt(track, t, at);
        matrixAt(track, base, from);
        if (!Motion::invertAffine(from, back))
            return false;
        Motion::mul(at, back, out);
        return true;
    }

    /// {"cmd":"clip","doc":..,"id":..,"start":..,"step":..,"frames":..,
    ///  "keys":["<hex>",..],"data":"<base64 of the float32s>",
    ///  "shown":"<base64, a byte a track a frame>","refs":[frame,blob,..],
    ///  "seqKeys":["<hex>",..],"seq":"<base64 of the int32s>",
    ///  "blobs":["<sha1>",..],"sizes":[..],
    ///  "lookTracks":[..],"lookCount":[..],"lookSlots":[type,part,start,count,..],
    ///  "lookAt":"<base64 of the int32s>","looks":"<base64 of the records>"}.
    /// \a doc is written as given: already quoted and escaped.
    std::string toJson(const std::string &quotedDoc) const
    {
        char buf[160];
        const bool none = empty();
        std::string json = "{\"cmd\":\"clip\",\"doc\":" + quotedDoc;
        std::snprintf(buf, sizeof(buf), ",\"id\":%u,\"start\":%.9g,\"step\":%.9g,\"frames\":%u",
                      id, start, step, none ? 0U : frames);
        json += buf;
        auto hexList = [&](const char *name, const std::vector<uint64_t> &list) {
            json += ",\"";
            json += name;
            json += "\":[";
            for (size_t i = 0; !none && i < list.size(); ++i) {
                std::snprintf(buf, sizeof(buf), "%s\"%016llx\"", i ? "," : "",
                              static_cast<unsigned long long>(list[i]));
                json += buf;
            }
            json += "]";
        };
        auto bytes = [&](const char *name, const void *p, size_t size) {
            json += ",\"";
            json += name;
            json += "\":\"";
            if (!none && size)
                json += Motion::base64(static_cast<const uint8_t *>(p), size);
            json += "\"";
        };
        auto intList = [&](const char *name, size_t count, const auto &at) {
            json += ",\"";
            json += name;
            json += "\":[";
            for (size_t i = 0; !none && i < count; ++i) {
                std::snprintf(buf, sizeof(buf), "%s%lld", i ? "," : "",
                              static_cast<long long>(at(i)));
                json += buf;
            }
            json += "]";
        };
        hexList("keys", keys);
        bytes("data", data.data(), data.size() * sizeof(float));
        bytes("shown", shown.data(), shown.size());
        intList("refs", shown.empty() ? 0 : refFrame.size() * 2, [this](size_t i) {
            return (i % 2) ? refBlob[i / 2] : refFrame[i / 2];
        });
        hexList("seqKeys", seqKeys);
        bytes("seq", seq.data(), seq.size() * sizeof(int32_t));
        json += ",\"blobs\":[";
        for (size_t i = 0; !none && i < blobs.size(); ++i) {
            json += i ? ",\"" : "\"";
            json += blobs[i];
            json += "\"";
        }
        json += "]";
        intList("sizes", blobSizes.size(), [this](size_t i) { return blobSizes[i]; });
        intList("lookTracks", lookTracks.size(), [this](size_t i) { return lookTracks[i]; });
        intList("lookCount", lookCount.size(), [this](size_t i) { return lookCount[i]; });
        intList("lookSlots", lookSlots.size() * 4, [this](size_t i) {
            const MotionSlot &s = lookSlots[i / 4];
            const int32_t v[4] = {s.type, s.partIndex, s.indexStart, s.indexCount};
            return v[i % 4];
        });
        bytes("lookAt", lookAt.data(), lookAt.size() * sizeof(int32_t));
        bytes("looks", looks.data(), looks.size() * sizeof(MotionLook));
        json += "}";
        return json;
    }

    /// Reads what toJson() wrote; false, and empty, when it is not that
    bool fromJson(const char *json)
    {
        *this = MotionClip();
        id = uint32_t(Motion::jsonNumber(json, "id", 0.0));
        start = Motion::jsonNumber(json, "start", 0.0);
        step = Motion::jsonNumber(json, "step", 0.0);
        frames = uint32_t(Motion::jsonNumber(json, "frames", 0.0));
        auto hexKeys = [json](const char *name, std::vector<uint64_t> &out) {
            std::vector<std::string> text;
            Motion::jsonStrings(json, name, text);
            for (const std::string &s : text) {
                char *end = nullptr;
                const uint64_t key = std::strtoull(s.c_str(), &end, 16);
                if (s.empty() || !end || *end)
                    return false;
                out.push_back(key);
            }
            return true;
        };
        bool ok = hexKeys("keys", keys) && hexKeys("seqKeys", seqKeys);
        std::vector<uint8_t> bytes, seqBytes;
        ok = ok && Motion::jsonBytes(json, "data", bytes)
            && Motion::jsonBytes(json, "shown", shown)
            && Motion::jsonBytes(json, "seq", seqBytes);
        std::vector<long long> refs, sizes;
        Motion::jsonInts(json, "refs", refs);
        Motion::jsonInts(json, "sizes", sizes);
        Motion::jsonStrings(json, "blobs", blobs);
        std::vector<long long> rows, counts, slots;
        std::vector<uint8_t> atBytes, lookBytes;
        Motion::jsonInts(json, "lookTracks", rows);
        Motion::jsonInts(json, "lookCount", counts);
        Motion::jsonInts(json, "lookSlots", slots);
        ok = ok && Motion::jsonBytes(json, "lookAt", atBytes)
            && Motion::jsonBytes(json, "looks", lookBytes);
        const size_t want = keys.size() * size_t(frames) * 7;
        const size_t cells = keys.size() * size_t(frames);
        ok = ok && bytes.size() == want * sizeof(float)
            && (shown.empty() || (shown.size() == cells && refs.size() == keys.size() * 2))
            && seqBytes.size() == seqKeys.size() * size_t(frames) * sizeof(int32_t)
            && sizes.size() == blobs.size();
        if (ok) {
            data.resize(want);
            if (want)
                std::memcpy(data.data(), bytes.data(), bytes.size());
            seq.resize(seqBytes.size() / sizeof(int32_t));
            if (!seq.empty())
                std::memcpy(seq.data(), seqBytes.data(), seqBytes.size());
            for (size_t i = 0; i + 1 < refs.size(); i += 2) {
                refFrame.push_back(int32_t(refs[i]));
                refBlob.push_back(int32_t(refs[i + 1]));
            }
            for (long long s : sizes)
                blobSizes.push_back(uint32_t(s));
            // The looks: every row a track, every frame's records inside
            size_t slotTotal = 0;
            for (long long c : counts)
                slotTotal += c > 0 ? size_t(c) : 0;
            ok = counts.size() == rows.size() && slots.size() == slotTotal * 4
                && atBytes.size() == rows.size() * size_t(frames) * sizeof(int32_t)
                && lookBytes.size() % sizeof(MotionLook) == 0
                && std::all_of(rows.begin(), rows.end(), [this](long long r) {
                       return r >= 0 && size_t(r) < keys.size();
                   })
                && std::all_of(counts.begin(), counts.end(), [](long long c) {
                       return c > 0;
                   });
            if (ok) {
                for (long long r : rows)
                    lookTracks.push_back(uint32_t(r));
                for (long long c : counts)
                    lookCount.push_back(uint32_t(c));
                for (size_t i = 0; i + 3 < slots.size(); i += 4) {
                    MotionSlot s;
                    s.type = int32_t(slots[i]);
                    s.partIndex = int32_t(slots[i + 1]);
                    s.indexStart = int32_t(slots[i + 2]);
                    s.indexCount = int32_t(slots[i + 3]);
                    lookSlots.push_back(s);
                }
                lookAt.resize(atBytes.size() / sizeof(int32_t));
                if (!lookAt.empty())
                    std::memcpy(lookAt.data(), atBytes.data(), atBytes.size());
                looks.resize(lookBytes.size() / sizeof(MotionLook));
                if (!looks.empty())
                    std::memcpy(static_cast<void *>(looks.data()), lookBytes.data(),
                                lookBytes.size());
                for (size_t r = 0; ok && r < lookTracks.size(); ++r) {
                    for (uint32_t f = 0; f < frames; ++f) {
                        const int32_t at = lookAt[r * frames + f];
                        if (at < 0 || size_t(at) + lookCount[r] > looks.size()) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
            const auto blobOk = [this](int32_t b) {
                return b >= -1 && b < int32_t(blobs.size());
            };
            ok = ok && std::all_of(seq.begin(), seq.end(), blobOk)
                && std::all_of(refBlob.begin(), refBlob.end(), blobOk)
                && std::all_of(refFrame.begin(), refFrame.end(), [this](int32_t f) {
                       return f >= -1 && f < int32_t(frames);
                   });
        }
        if (!ok) {
            const uint32_t keep = id;
            const bool nothing = want == 0 && seqKeys.empty();
            *this = MotionClip();
            id = keep;
            return nothing;
        }
        return true;
    }
};

/// The serving process's clock as it tells it: enough for a viewer to
/// say what time the Animation is at without being told each frame.
struct MotionClock {
    enum LoopMode { Once = 0, Loop = 1, PingPong = 2 };

    uint32_t clip = 0;     ///< the clip this is the clock of
    bool active = false;   ///< an Animation of the document is posing it
    bool playing = false;
    double time = 0.0;     ///< the Animation's Time when this was said
    double raw = 0.0;      ///< the player's own count, before Loop folds it
    double speed = 1.0;
    double start = 0.0;
    double end = 0.0;
    int loop = Once;
    /// The publish this describes, and the pose it was taken in
    uint64_t version = 0;
    bool baseActive = false;
    double baseTime = 0.0;

    /// The Animation's Time \a elapsed seconds after this was said: the
    /// player's own arithmetic (Gui::AnimationPlayer::tick)
    double timeAfter(double elapsed) const
    {
        if (!playing)
            return time;
        const double length = end - start;
        if (!(length > 0.0))
            return time;
        double t = raw + elapsed * speed;
        switch (loop) {
            case Loop:
                if (t > end)
                    t = start + std::fmod(t - start, length);
                break;
            case PingPong: {
                const double u = (t - start) / length;
                const double turn = std::floor(u);
                const double frac = u - turn;
                t = (static_cast<long>(turn) % 2) ? end - frac * length : start + frac * length;
                break;
            }
            default:
                t = std::min(t, end);
                break;
        }
        return t;
    }

    std::string toJson(const std::string &quotedDoc) const
    {
        char buf[400];
        std::snprintf(buf, sizeof(buf),
                      ",\"clip\":%u,\"active\":%s,\"playing\":%s,\"t\":%.9g,\"raw\":%.9g,"
                      "\"speed\":%.9g,\"start\":%.9g,\"end\":%.9g,\"loop\":%d,"
                      "\"v\":%llu,\"baseActive\":%s,\"base\":%.9g}",
                      clip, active ? "true" : "false", playing ? "true" : "false", time, raw,
                      speed, start, end, loop, static_cast<unsigned long long>(version),
                      baseActive ? "true" : "false", baseTime);
        return "{\"cmd\":\"time\",\"doc\":" + quotedDoc + buf;
    }

    void fromJson(const char *json)
    {
        using Motion::jsonNumber;
        clip = uint32_t(jsonNumber(json, "clip", 0.0));
        active = jsonNumber(json, "active", 0.0) != 0.0;
        playing = jsonNumber(json, "playing", 0.0) != 0.0;
        time = jsonNumber(json, "t", 0.0);
        raw = jsonNumber(json, "raw", time);
        speed = jsonNumber(json, "speed", 1.0);
        start = jsonNumber(json, "start", 0.0);
        end = jsonNumber(json, "end", 0.0);
        loop = int(jsonNumber(json, "loop", 0.0));
        version = uint64_t(jsonNumber(json, "v", 0.0));
        baseActive = jsonNumber(json, "baseActive", 0.0) != 0.0;
        baseTime = jsonNumber(json, "base", 0.0);
    }
};

/// Reads the feed frame by frame and says which objects moved rigidly,
/// which were hidden when, and which changed in some other way
class MotionClipBaker
{
public:
    /// Keeps the draws of one object at one frame where a viewer can
    /// fetch them, and says their blob number; -1 when it will not (no
    /// room left). Without one, an object that is not moved rigidly is
    /// left out of the clip, which is then not exact.
    using Store = std::function<int32_t(const DrawCallList &)>;

    explicit MotionClipBaker(Store store = {})
        : store(std::move(store))
    {}

    /// The feed at the next frame
    void frame(const DrawCallList &draws)
    {
        std::unordered_map<uint64_t, std::vector<const DrawCall *>> byKey;
        for (const auto &draw : draws) {
            if (draw.objectKey)
                byKey[draw.objectKey].push_back(&draw);
        }
        for (const auto &entry : byKey) {
            if (objects.count(entry.first))
                continue;
            // Not there before this frame: hidden until now
            Object &obj = objects[entry.first];
            obj.poses.resize(count);
            obj.shown.assign(count, 0);
            obj.lookFrames.resize(count);
            order.push_back(entry.first);
        }
        for (auto &entry : objects) {
            Object &obj = entry.second;
            if (obj.dropped)
                continue;
            auto it = byKey.find(entry.first);
            if (it == byKey.end()) {
                hide(obj);
                continue;
            }
            const std::vector<const DrawCall *> &now = it->second;
            if (obj.refFrame < 0) {
                obj.refFrame = int32_t(count);
                std::vector<MotionLook> looks;
                for (const DrawCall *draw : now) {
                    obj.ref.push_back(shapeOf(*draw));
                    obj.refDraws.push_back(*draw);
                    looks.push_back(obj.ref.back().look);
                }
                obj.poses.emplace_back();
                obj.shown.push_back(1);
                obj.lookFrames.push_back(std::move(looks));
                continue;
            }
            if (obj.sequence) {
                keep(obj, now);
                continue;
            }
            Motion::Pose pose;
            std::vector<MotionLook> looks;
            if (!measure(obj, now, pose, looks)) {
                becomeSequence(obj);
                if (!obj.dropped)
                    keep(obj, now);
                continue;
            }
            if (looks != obj.lookFrames[size_t(obj.refFrame)])
                obj.looked = true;
            obj.lookFrames.push_back(std::move(looks));
            // On the side of the last shown, so a viewer turns the short
            // way between them
            for (size_t i = obj.shown.size(); i-- > 0;) {
                if (!obj.shown[i])
                    continue;
                const Motion::Pose &last = obj.poses[i];
                double dot = 0.0;
                for (int k = 0; k < 4; ++k)
                    dot += last.q[k] * pose.q[k];
                if (dot < 0.0) {
                    for (int k = 0; k < 4; ++k)
                        pose.q[k] = -pose.q[k];
                }
                break;
            }
            obj.poses.push_back(pose);
            obj.shown.push_back(1);
        }
        ++count;
    }

    /// The clip of the frames read
    MotionClip finish(uint32_t id, double start, double step)
    {
        MotionClip clip;
        clip.id = id;
        clip.start = start;
        clip.step = step;
        clip.frames = count;
        clip.exact = exact;
        std::sort(order.begin(), order.end());
        bool anyHidden = false;
        std::vector<uint64_t> tracks;
        for (uint64_t key : order) {
            Object &obj = objects.at(key);
            if (obj.dropped || obj.refFrame < 0)
                continue;
            if (obj.sequence) {
                clip.seqKeys.push_back(key);
                clip.seq.insert(clip.seq.end(), obj.blobs.begin(), obj.blobs.end());
                continue;
            }
            const bool gaps = hidden(obj);
            if (!gaps && !moved(obj) && !obj.looked)
                continue;
            if (gaps && obj.refBlob < 0) {
                // A viewer holding a publish taken while it was hidden
                // needs its draws from somewhere
                obj.refBlob = store ? store(obj.refDraws) : -1;
                if (obj.refBlob < 0)
                    clip.exact = false;
            }
            anyHidden = anyHidden || gaps;
            tracks.push_back(key);
        }
        for (uint64_t key : tracks) {
            const Object &obj = objects.at(key);
            clip.keys.push_back(key);
            for (const Motion::Pose &pose : obj.poses) {
                for (int i = 0; i < 3; ++i)
                    clip.data.push_back(float(pose.t[i]));
                for (int i = 0; i < 4; ++i)
                    clip.data.push_back(float(pose.q[i]));
            }
            if (anyHidden) {
                clip.shown.insert(clip.shown.end(), obj.shown.begin(), obj.shown.end());
                clip.refFrame.push_back(obj.refFrame);
                clip.refBlob.push_back(obj.refBlob);
            }
            if (obj.looked)
                addLooks(clip, uint32_t(clip.keys.size() - 1), obj);
        }
        return clip;
    }

    uint32_t frames() const
    {
        return count;
    }

private:
    /// What a draw is apart from where
    struct Shape {
        double model[16];
        uint64_t mesh = 0;
        int partIndex = -1;
        int indexStart = 0;
        int indexCount = 0;
        int type = 0;
        /// The material as far as an Animation changes it in place
        MotionLook look;
    };
    struct Object {
        /// The draws at refFrame, what the poses are measured from
        std::vector<Shape> ref;
        DrawCallList refDraws;
        int32_t refFrame = -1;
        int32_t refBlob = -1;
        /// One a frame: the move from refFrame, the last one held where
        /// it is hidden
        std::vector<Motion::Pose> poses;
        std::vector<uint8_t> shown;
        /// Not moved rigidly: the draws of every frame are kept instead
        bool sequence = false;
        std::vector<int32_t> blobs;
        /// Cannot be played: the clip leaves it to the publishes
        bool dropped = false;
        /// One a frame, with the poses: how each of the draws looks, in
        /// the order of `ref`; empty before refFrame
        std::vector<std::vector<MotionLook>> lookFrames;
        /// The looks change: a colour, a transparency
        bool looked = false;
    };

    /// The object's looks as a row of the clip: each frame's records,
    /// frames that look alike sharing theirs
    static void addLooks(MotionClip &clip, uint32_t track, const Object &obj)
    {
        const std::vector<MotionLook> &first = obj.lookFrames[size_t(obj.refFrame)];
        clip.lookTracks.push_back(track);
        clip.lookCount.push_back(uint32_t(first.size()));
        for (const DrawCall &draw : obj.refDraws)
            clip.lookSlots.push_back(MotionSlot::of(draw));
        std::unordered_map<std::string, int32_t> seen;
        for (uint32_t f = 0; f < clip.frames; ++f) {
            const std::vector<MotionLook> &set =
                obj.lookFrames[f].empty() ? first : obj.lookFrames[f];
            const std::string bytes(reinterpret_cast<const char *>(set.data()),
                                    set.size() * sizeof(MotionLook));
            auto it = seen.find(bytes);
            if (it == seen.end()) {
                it = seen.emplace(bytes, int32_t(clip.looks.size())).first;
                clip.looks.insert(clip.looks.end(), set.begin(), set.end());
            }
            clip.lookAt.push_back(it->second);
        }
    }

    void hide(Object &obj)
    {
        obj.poses.push_back(obj.poses.empty() ? Motion::Pose() : obj.poses.back());
        obj.shown.push_back(0);
        obj.lookFrames.push_back(obj.lookFrames.empty() ? std::vector<MotionLook>()
                                                        : obj.lookFrames.back());
        if (obj.sequence)
            obj.blobs.push_back(-1);
    }

    void drop(Object &obj)
    {
        obj.dropped = true;
        exact = false;
    }

    void keep(Object &obj, const std::vector<const DrawCall *> &now)
    {
        DrawCallList list;
        list.reserve(now.size());
        for (const DrawCall *draw : now)
            list.push_back(*draw);
        const int32_t blob = store(list);
        if (blob < 0) {
            drop(obj);
            return;
        }
        obj.blobs.push_back(blob);
        obj.poses.emplace_back();
        obj.shown.push_back(1);
        obj.lookFrames.emplace_back();
    }

    /// The frames so far, from the draws they were moves of
    void becomeSequence(Object &obj)
    {
        if (!store) {
            drop(obj);
            return;
        }
        obj.sequence = true;
        for (uint32_t i = 0; i < count; ++i) {
            if (!obj.shown[i]) {
                obj.blobs.push_back(-1);
                continue;
            }
            double x[16];
            Motion::matrixOf(obj.poses[i], x);
            DrawCallList moved = obj.refDraws;
            const std::vector<MotionLook> &looks = obj.lookFrames[i];
            for (size_t d = 0; d < moved.size(); ++d) {
                Motion::moveDraw(moved[d], x);
                // As it looked then, not as it did at refFrame
                if (d < looks.size())
                    looks[d].applyTo(moved[d].material);
            }
            const int32_t blob = store(moved);
            if (blob < 0) {
                drop(obj);
                return;
            }
            obj.blobs.push_back(blob);
        }
    }

    /// The same geometry under another cache id: a colour or a
    /// transparency changed builds the object's caches again, the arrays
    /// as they were
    static bool sameMesh(const MeshData *a, const MeshData *b)
    {
        if (a == b)
            return true;
        if (!a || !b)
            return false;
        auto same = [](const void *x, const void *y, size_t bytes) {
            if (x == y)
                return true;
            if (!x || !y)
                return false;
            return std::memcmp(x, y, bytes) == 0;
        };
        const size_t nv = size_t(std::max(a->numVertices, 0));
        return a->numVertices == b->numVertices
            && a->numTriangleIndices == b->numTriangleIndices
            && a->numLineIndices == b->numLineIndices
            && a->numPointIndices == b->numPointIndices
            && same(a->positions, b->positions, nv * 3 * sizeof(float))
            && same(a->normals, b->normals, nv * 3 * sizeof(float))
            && same(a->colors, b->colors, nv * 4)
            && same(a->materials, b->materials, nv * MeshData::MaterialStride)
            && same(a->triangleIndices, b->triangleIndices,
                    size_t(std::max(a->numTriangleIndices, 0)) * sizeof(int32_t))
            && same(a->lineIndices, b->lineIndices,
                    size_t(std::max(a->numLineIndices, 0)) * sizeof(int32_t))
            && same(a->pointIndices, b->pointIndices,
                    size_t(std::max(a->numPointIndices, 0)) * sizeof(int32_t));
    }

    static Shape shapeOf(const DrawCall &draw)
    {
        Shape s;
        Motion::modelOf(draw, s.model);
        s.mesh = draw.mesh ? draw.mesh->cacheId : 0;
        s.partIndex = draw.partIndex;
        s.indexStart = draw.indexStart;
        s.indexCount = draw.indexCount;
        s.type = draw.material.type;
        s.look = MotionLook::of(draw.material);
        return s;
    }

    /// The one rigid transform that takes the object's draws at refFrame
    /// to \a now, and how each of them looks there; false when there is
    /// none. The looks may differ from refFrame's: a colour or a
    /// transparency the Animation changes rides the track.
    static bool measure(const Object &obj, const std::vector<const DrawCall *> &now,
                        Motion::Pose &pose, std::vector<MotionLook> &looks)
    {
        looks.clear();
        if (now.size() != obj.ref.size() || now.empty())
            return false;
        double first[16], back[16], move[16];
        Motion::modelOf(*now[0], first);
        if (!Motion::invertAffine(obj.ref[0].model, back))
            return false;
        Motion::mul(first, back, move);
        if (!Motion::poseOf(move, pose))
            return false;
        for (size_t i = 0; i < now.size(); ++i) {
            const Shape &was = obj.ref[i];
            const Shape is = shapeOf(*now[i]);
            if ((is.mesh != was.mesh
                 && !sameMesh(now[i]->mesh.get(), obj.refDraws[i].mesh.get()))
                || is.partIndex != was.partIndex || is.indexStart != was.indexStart
                || is.indexCount != was.indexCount || is.type != was.type)
                return false;
            looks.push_back(is.look);
            double expect[16];
            Motion::mul(move, was.model, expect);
            // In float the feed holds about seven digits of a coordinate
            const double reach = 1.0 + std::fabs(expect[12]) + std::fabs(expect[13])
                + std::fabs(expect[14]);
            for (int k = 0; k < 16; ++k) {
                const double tol = (k >= 12 && k < 15) ? 1e-4 * reach : 1e-4;
                if (std::fabs(expect[k] - is.model[k]) > tol)
                    return false;
            }
        }
        return true;
    }

    static bool hidden(const Object &obj)
    {
        return std::find(obj.shown.begin(), obj.shown.end(), uint8_t(0)) != obj.shown.end();
    }

    static bool moved(const Object &obj)
    {
        const Motion::Pose &first = obj.poses.front();
        for (const Motion::Pose &pose : obj.poses) {
            for (int i = 0; i < 3; ++i) {
                if (std::fabs(pose.t[i] - first.t[i]) > 1e-6)
                    return true;
            }
            for (int i = 0; i < 4; ++i) {
                if (std::fabs(pose.q[i] - first.q[i]) > 1e-7)
                    return true;
            }
        }
        return false;
    }

    Store store;
    std::unordered_map<uint64_t, Object> objects;
    std::vector<uint64_t> order;
    uint32_t count = 0;
    bool exact = true;
};

}  // namespace Render

#endif  // RENDER_MOTIONCLIP_H
