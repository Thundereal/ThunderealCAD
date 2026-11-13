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

}  // namespace Motion

/// The tracks of one bake: for each objectKey a pose per frame, against
/// the first frame
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
    /// Every change the sweep made to the feed is in a track: nothing
    /// appeared, went, was rebuilt, changed its look or moved other
    /// than rigidly. Only then may a publish be held for what the
    /// Animation writes.
    bool exact = false;

    bool empty() const
    {
        return !id || keys.empty() || frames < 2 || !(step > 0.0);
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
    ///  "keys":["<hex>",..],"data":"<base64 of the float32s>"}. \a doc is
    /// written as given: already quoted and escaped.
    std::string toJson(const std::string &quotedDoc) const
    {
        char buf[160];
        std::string json = "{\"cmd\":\"clip\",\"doc\":" + quotedDoc;
        std::snprintf(buf, sizeof(buf), ",\"id\":%u,\"start\":%.9g,\"step\":%.9g,\"frames\":%u",
                      id, start, step, empty() ? 0U : frames);
        json += buf;
        json += ",\"keys\":[";
        if (!empty()) {
            for (size_t i = 0; i < keys.size(); ++i) {
                std::snprintf(buf, sizeof(buf), "%s\"%016llx\"", i ? "," : "",
                              static_cast<unsigned long long>(keys[i]));
                json += buf;
            }
        }
        json += "],\"data\":\"";
        if (!empty()) {
            json += Motion::base64(reinterpret_cast<const uint8_t *>(data.data()),
                                   data.size() * sizeof(float));
        }
        json += "\"}";
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
        if (const char *p = std::strstr(json, "\"keys\":[")) {
            p += 8;
            while (*p && *p != ']') {
                if (*p != '"') {
                    ++p;
                    continue;
                }
                char *end = nullptr;
                const uint64_t key = std::strtoull(p + 1, &end, 16);
                if (!end || *end != '"')
                    break;
                keys.push_back(key);
                p = end + 1;
            }
        }
        std::vector<uint8_t> bytes;
        const char *d = std::strstr(json, "\"data\":\"");
        const bool read = d && Motion::unbase64(d + 8, bytes);
        const size_t want = keys.size() * size_t(frames) * 7;
        if (!read || bytes.size() != want * sizeof(float)) {
            const uint32_t keep = id;
            *this = MotionClip();
            id = keep;
            return want == 0;
        }
        data.resize(want);
        if (want)
            std::memcpy(data.data(), bytes.data(), bytes.size());
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

/// Reads the feed frame by frame and says which objects moved rigidly
class MotionClipBaker
{
public:
    /// The feed at the next frame; the first one is what the rest are
    /// measured against
    void frame(const DrawCallList &draws)
    {
        std::unordered_map<uint64_t, std::vector<const DrawCall *>> byKey;
        for (const auto &draw : draws) {
            if (draw.objectKey)
                byKey[draw.objectKey].push_back(&draw);
        }
        if (count == 0) {
            for (const auto &entry : byKey) {
                Object &obj = objects[entry.first];
                obj.draws.reserve(entry.second.size());
                for (const DrawCall *draw : entry.second)
                    obj.draws.push_back(shapeOf(*draw));
                order.push_back(entry.first);
            }
            std::sort(order.begin(), order.end());
        }
        else {
            // Something the first frame did not have
            for (const auto &entry : byKey) {
                if (!objects.count(entry.first))
                    exact = false;
            }
        }
        for (auto &entry : objects) {
            Object &obj = entry.second;
            if (obj.dropped)
                continue;
            auto it = byKey.find(entry.first);
            Motion::Pose pose;
            if (it == byKey.end() || !measure(obj, it->second, pose)) {
                obj.dropped = true;
                exact = false;
                continue;
            }
            if (!obj.poses.empty()) {
                // On the side of the one before, so a viewer turns the
                // short way between them
                const Motion::Pose &last = obj.poses.back();
                double dot = 0.0;
                for (int i = 0; i < 4; ++i)
                    dot += last.q[i] * pose.q[i];
                if (dot < 0.0) {
                    for (int i = 0; i < 4; ++i)
                        pose.q[i] = -pose.q[i];
                }
            }
            obj.poses.push_back(pose);
        }
        ++count;
    }

    /// The clip of the frames read: a track for each object that moved
    MotionClip finish(uint32_t id, double start, double step) const
    {
        MotionClip clip;
        clip.id = id;
        clip.start = start;
        clip.step = step;
        clip.frames = count;
        clip.exact = exact;
        for (uint64_t key : order) {
            const Object &obj = objects.at(key);
            if (obj.dropped || obj.poses.size() != count || !moved(obj))
                continue;
            clip.keys.push_back(key);
            for (const Motion::Pose &pose : obj.poses) {
                for (int i = 0; i < 3; ++i)
                    clip.data.push_back(float(pose.t[i]));
                for (int i = 0; i < 4; ++i)
                    clip.data.push_back(float(pose.q[i]));
            }
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
        /// The material as far as one colour and one style say it
        uint64_t look = 0;
    };
    struct Object {
        std::vector<Shape> draws;
        std::vector<Motion::Pose> poses;
        bool dropped = false;
    };

    /// A digest of what a material draws with, apart from what rides
    /// the mesh: enough to tell a colour, a transparency or a style that
    /// the Animation changes from a move
    static uint64_t lookOf(const Material &m)
    {
        uint64_t h = 1469598103934665603ULL;
        auto add = [&h](uint64_t v) {
            h = (h ^ v) * 1099511628211ULL;
        };
        auto bits = [](float f) {
            uint32_t u = 0;
            std::memcpy(&u, &f, sizeof(u));
            return uint64_t(u);
        };
        add(m.type);
        add(m.drawstyle);
        add((m.transparent ? 1U : 0U) | (m.ontop ? 2U : 0U) | (m.lighting ? 4U : 0U)
            | (m.twoside ? 8U : 0U) | (m.pervertexcolor ? 16U : 0U)
            | (m.perfacematerial ? 32U : 0U) | (m.depthtest ? 64U : 0U)
            | (m.depthwrite ? 128U : 0U));
        add(m.diffuse);
        add(m.emissive);
        add(m.specular);
        add(m.ambient);
        add(m.linecolor);
        add(m.linepattern);
        add(bits(m.shininess));
        add(bits(m.linewidth));
        add(bits(m.pointsize));
        return h;
    }

    static Shape shapeOf(const DrawCall &draw)
    {
        Shape s;
        Motion::modelOf(draw, s.model);
        s.mesh = draw.mesh ? draw.mesh->cacheId : 0;
        s.partIndex = draw.partIndex;
        s.indexStart = draw.indexStart;
        s.indexCount = draw.indexCount;
        s.look = lookOf(draw.material);
        return s;
    }

    /// The one rigid transform that takes the object's first-frame draws
    /// to \a now; false when there is none
    static bool measure(const Object &obj, const std::vector<const DrawCall *> &now,
                        Motion::Pose &pose)
    {
        if (now.size() != obj.draws.size() || now.empty())
            return false;
        double first[16], back[16], move[16];
        Motion::modelOf(*now[0], first);
        if (!Motion::invertAffine(obj.draws[0].model, back))
            return false;
        Motion::mul(first, back, move);
        if (!Motion::poseOf(move, pose))
            return false;
        for (size_t i = 0; i < now.size(); ++i) {
            const Shape &was = obj.draws[i];
            const Shape is = shapeOf(*now[i]);
            if (is.mesh != was.mesh || is.partIndex != was.partIndex
                || is.indexStart != was.indexStart || is.indexCount != was.indexCount
                || is.look != was.look)
                return false;
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

    std::unordered_map<uint64_t, Object> objects;
    std::vector<uint64_t> order;
    uint32_t count = 0;
    bool exact = true;
};

}  // namespace Render

#endif  // RENDER_MOTIONCLIP_H
