// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 ****************************************************************************/

#include "PreCompiled.h"

#include <algorithm>
#include <cmath>

#include <Base/Rotation.h>
#include <Base/Vector3D.h>

#include "AnimationCurve.h"

using namespace App::Anim;

namespace
{

constexpr double Pi = 3.14159265358979323846;

// --------------------------------------------------------------------------
// Easing: Robert Penner's equations as Qt's QEasingCurve computes them
// (qtbase src/3rdparty/easing), constants and end cases included, so that a
// curve keyed in the old App::KeyFrame plays the same.

double inQuad(double t)
{
    return t * t;
}
double outQuad(double t)
{
    return -t * (t - 2);
}
double inOutQuad(double t)
{
    t *= 2.0;
    if (t < 1) {
        return t * t / 2.0;
    }
    --t;
    return -0.5 * (t * (t - 2) - 1);
}

double inCubic(double t)
{
    return t * t * t;
}
double outCubic(double t)
{
    t -= 1.0;
    return t * t * t + 1;
}
double inOutCubic(double t)
{
    t *= 2.0;
    if (t < 1) {
        return 0.5 * t * t * t;
    }
    t -= 2.0;
    return 0.5 * (t * t * t + 2);
}

double inQuart(double t)
{
    return t * t * t * t;
}
double outQuart(double t)
{
    t -= 1.0;
    return -(t * t * t * t - 1);
}
double inOutQuart(double t)
{
    t *= 2;
    if (t < 1) {
        return 0.5 * t * t * t * t;
    }
    t -= 2.0;
    return -0.5 * (t * t * t * t - 2);
}

double inQuint(double t)
{
    return t * t * t * t * t;
}
double outQuint(double t)
{
    t -= 1.0;
    return t * t * t * t * t + 1;
}
double inOutQuint(double t)
{
    t *= 2.0;
    if (t < 1) {
        return 0.5 * t * t * t * t * t;
    }
    t -= 2.0;
    return 0.5 * (t * t * t * t * t + 2);
}

double inSine(double t)
{
    return (t == 1.0) ? 1.0 : -std::cos(t * Pi / 2) + 1.0;
}
double outSine(double t)
{
    return std::sin(t * Pi / 2);
}
double inOutSine(double t)
{
    return -0.5 * (std::cos(Pi * t) - 1);
}

double inExpo(double t)
{
    return (t == 0 || t == 1.0) ? t : std::pow(2.0, 10 * (t - 1)) - 0.001;
}
double outExpo(double t)
{
    return (t == 1.0) ? 1.0 : 1.001 * (-std::pow(2.0, -10 * t) + 1);
}
double inOutExpo(double t)
{
    if (t == 0.0) {
        return 0.0;
    }
    if (t == 1.0) {
        return 1.0;
    }
    t *= 2.0;
    if (t < 1) {
        return 0.5 * std::pow(2.0, 10 * (t - 1)) - 0.0005;
    }
    return 0.5 * 1.0005 * (-std::pow(2.0, -10 * (t - 1)) + 2);
}

double inCirc(double t)
{
    return -(std::sqrt(1 - t * t) - 1);
}
double outCirc(double t)
{
    t -= 1.0;
    return std::sqrt(1 - t * t);
}
double inOutCirc(double t)
{
    t *= 2.0;
    if (t < 1) {
        return -0.5 * (std::sqrt(1 - t * t) - 1);
    }
    t -= 2.0;
    return 0.5 * (std::sqrt(1 - t * t) + 1);
}

double inElasticHelper(double t, double b, double c, double d, double a, double p)
{
    if (t == 0) {
        return b;
    }
    double tAdj = t / d;
    if (tAdj == 1) {
        return b + c;
    }
    double s;
    if (a < std::fabs(c)) {
        a = c;
        s = p / 4.0;
    }
    else {
        s = p / (2 * Pi) * std::asin(c / a);
    }
    tAdj -= 1.0;
    return -(a * std::pow(2.0, 10 * tAdj) * std::sin((tAdj * d - s) * (2 * Pi) / p)) + b;
}
double outElasticHelper(double t, double /*b*/, double c, double /*d*/, double a, double p)
{
    if (t == 0) {
        return 0;
    }
    if (t == 1) {
        return c;
    }
    double s;
    if (a < c) {
        a = c;
        s = p / 4.0;
    }
    else {
        s = p / (2 * Pi) * std::asin(c / a);
    }
    return a * std::pow(2.0, -10 * t) * std::sin((t - s) * (2 * Pi) / p) + c;
}
double inOutElastic(double t, double a, double p)
{
    if (t == 0) {
        return 0.0;
    }
    t *= 2.0;
    if (t == 2) {
        return 1.0;
    }
    double s;
    if (a < 1.0) {
        a = 1.0;
        s = p / 4.0;
    }
    else {
        s = p / (2 * Pi) * std::asin(1.0 / a);
    }
    if (t < 1) {
        return -.5 * (a * std::pow(2.0, 10 * (t - 1)) * std::sin((t - 1 - s) * (2 * Pi) / p));
    }
    return a * std::pow(2.0, -10 * (t - 1)) * std::sin((t - 1 - s) * (2 * Pi) / p) * .5 + 1.0;
}
double outInElastic(double t, double a, double p)
{
    if (t < 0.5) {
        return outElasticHelper(t * 2, 0, 0.5, 1.0, a, p);
    }
    return inElasticHelper(2 * t - 1.0, 0.5, 0.5, 1.0, a, p);
}

double inBack(double t, double s)
{
    return t * t * ((s + 1) * t - s);
}
double outBack(double t, double s)
{
    t -= 1.0;
    return t * t * ((s + 1) * t + s) + 1;
}
double inOutBack(double t, double s)
{
    t *= 2.0;
    if (t < 1) {
        s *= 1.525;
        return 0.5 * (t * t * ((s + 1) * t - s));
    }
    t -= 2.0;
    s *= 1.525;
    return 0.5 * (t * t * ((s + 1) * t + s) + 2);
}

double outBounceHelper(double t, double c, double a)
{
    if (t == 1.0) {
        return c;
    }
    if (t < (4 / 11.0)) {
        return c * (7.5625 * t * t);
    }
    if (t < (8 / 11.0)) {
        t -= (6 / 11.0);
        return -a * (1. - (7.5625 * t * t + .75)) + c;
    }
    if (t < (10 / 11.0)) {
        t -= (9 / 11.0);
        return -a * (1. - (7.5625 * t * t + .9375)) + c;
    }
    t -= (21 / 22.0);
    return -a * (1. - (7.5625 * t * t + .984375)) + c;
}
double outBounce(double t, double a)
{
    return outBounceHelper(t, 1, a);
}
double inBounce(double t, double a)
{
    return 1.0 - outBounceHelper(1.0 - t, 1.0, a);
}
double inOutBounce(double t, double a)
{
    if (t < 0.5) {
        return inBounce(2 * t, a) / 2;
    }
    return (t == 1.0) ? 1.0 : outBounce(2 * t - 1, a) / 2 + 0.5;
}
double outInBounce(double t, double a)
{
    if (t < 0.5) {
        return outBounceHelper(t * 2, 0.5, a);
    }
    return 1.0 - outBounceHelper(2.0 - 2 * t, 0.5, a);
}

/// The out-in form of a family: its Out to the middle, its In after
template<class Out, class In>
double outIn(double t, Out out, In in)
{
    if (t < 0.5) {
        return out(2 * t) / 2;
    }
    return in(2 * t - 1) / 2 + 0.5;
}

// --------------------------------------------------------------------------
// Quaternions as x y z w

double dot4(const double* a, const double* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

void normalise4(double* q)
{
    double len = std::sqrt(dot4(q, q));
    if (len > 0) {
        for (int i = 0; i < 4; ++i) {
            q[i] /= len;
        }
    }
    else {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
    }
}

void normalise3(double* v)
{
    double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0) {
        for (int i = 0; i < 3; ++i) {
            v[i] /= len;
        }
    }
}

/// a * b
void mul4(const double* a, const double* b, double* out)
{
    double r[4] = {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
    std::copy(r, r + 4, out);
}

/// q to the power k, for a unit q
void pow4(const double* q, double k, double* out)
{
    double w = std::clamp(q[3], -1.0, 1.0);
    double half = std::acos(w);
    double s = std::sin(half);
    if (s < 1e-12) {
        out[0] = out[1] = out[2] = 0;
        out[3] = 1;
        return;
    }
    double nh = half * k;
    double f = std::sin(nh) / s;
    out[0] = q[0] * f;
    out[1] = q[1] * f;
    out[2] = q[2] * f;
    out[3] = std::cos(nh);
}

/// slerp the shortest way; u may be outside [0, 1]
void slerp4(const double* a, const double* bIn, double u, double* out)
{
    double b[4] = {bIn[0], bIn[1], bIn[2], bIn[3]};
    double d = dot4(a, b);
    if (d < 0) {
        d = -d;
        for (double& x : b) {
            x = -x;
        }
    }
    double r[4];
    if (d > 1.0 - 1e-12) {
        for (int i = 0; i < 4; ++i) {
            r[i] = a[i] + (b[i] - a[i]) * u;
        }
    }
    else {
        double theta = std::acos(std::min(d, 1.0));
        double s = std::sin(theta);
        double fa = std::sin((1 - u) * theta) / s;
        double fb = std::sin(u * theta) / s;
        for (int i = 0; i < 4; ++i) {
            r[i] = a[i] * fa + b[i] * fb;
        }
    }
    normalise4(r);
    std::copy(r, r + 4, out);
}

/// Unit vectors a to b on the sphere; u may be outside [0, 1]
void slerp3(const double* a, const double* b, double u, double* out)
{
    Base::Vector3d va(a[0], a[1], a[2]);
    Base::Vector3d vb(b[0], b[1], b[2]);
    Base::Rotation rot(va, vb);
    Base::Vector3d axis;
    double angle;
    rot.getRawValue(axis, angle);
    rot.setValue(axis, angle * u);
    Base::Vector3d r = rot.multVec(va);
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
}

double cubic(double p0, double p1, double p2, double p3, double s)
{
    double r = 1 - s;
    return r * r * r * p0 + 3 * r * r * s * p1 + 3 * r * s * s * p2 + s * s * s * p3;
}

double cubicDerivative(double p0, double p1, double p2, double p3, double s)
{
    double r = 1 - s;
    return 3 * r * r * (p1 - p0) + 6 * r * s * (p2 - p1) + 3 * s * s * (p3 - p2);
}

}  // namespace

// --------------------------------------------------------------------------

void Layout::add(Kind kind, int size)
{
    if (kind == Kind::Direction) {
        size = 3;
    }
    else if (kind == Kind::Quaternion || kind == Kind::Colour) {
        size = 4;
    }
    if (size <= 0) {
        return;
    }
    if (kind == Kind::Scalar && !groups.empty() && groups.back().kind == Kind::Scalar) {
        groups.back().size += size;
        return;
    }
    groups.push_back({kind, size});
}

void Layout::addRotation()
{
    switch (rotation) {
        case RotationMode::Shortest:
            add(Kind::Quaternion);
            break;
        case RotationMode::Turns:
            add(Kind::Direction);
            add(Kind::Scalar, 1);
            break;
        case RotationMode::Euler:
            add(Kind::Scalar, 3);
            break;
    }
}

int Layout::size() const
{
    int n = 0;
    for (const auto& g : groups) {
        n += g.size;
    }
    return n;
}

void App::Anim::rotationToNumbers(const Base::Rotation& rot, RotationMode mode,
                                  std::vector<double>& out)
{
    switch (mode) {
        case RotationMode::Shortest: {
            double q[4];
            rot.getValue(q[0], q[1], q[2], q[3]);
            out.insert(out.end(), q, q + 4);
            break;
        }
        case RotationMode::Turns: {
            Base::Vector3d axis;
            double angle;
            rot.getValue(axis, angle);
            if (axis.Length() < 1e-12) {
                axis = Base::Vector3d(0, 0, 1);
            }
            out.insert(out.end(), {axis.x, axis.y, axis.z, angle * 180.0 / Pi});
            break;
        }
        case RotationMode::Euler: {
            double y, p, r;
            rot.getYawPitchRoll(y, p, r);
            out.insert(out.end(), {y, p, r});
            break;
        }
    }
}

int App::Anim::rotationFromNumbers(const double* nums, RotationMode mode, Base::Rotation& rot)
{
    switch (mode) {
        case RotationMode::Shortest: {
            double q[4] = {nums[0], nums[1], nums[2], nums[3]};
            normalise4(q);
            rot.setValue(q[0], q[1], q[2], q[3]);
            return 4;
        }
        case RotationMode::Turns: {
            Base::Vector3d axis(nums[0], nums[1], nums[2]);
            if (axis.Length() < 1e-12) {
                axis = Base::Vector3d(0, 0, 1);
            }
            rot.setValue(axis.Normalize(), nums[3] * Pi / 180.0);
            return 4;
        }
        case RotationMode::Euler:
            rot.setYawPitchRoll(nums[0], nums[1], nums[2]);
            return 3;
    }
    return 0;
}

void App::Anim::blend(const Layout& layout, const double* a, const double* b, double u, double* out)
{
    for (const auto& g : layout.groups) {
        switch (g.kind) {
            case Kind::Scalar:
            case Kind::Colour:
                for (int i = 0; i < g.size; ++i) {
                    out[i] = a[i] + (b[i] - a[i]) * u;
                }
                if (g.kind == Kind::Colour) {
                    for (int i = 0; i < g.size; ++i) {
                        out[i] = std::clamp(out[i], 0.0, 1.0);
                    }
                }
                break;
            case Kind::Direction:
                slerp3(a, b, u, out);
                break;
            case Kind::Quaternion:
                slerp4(a, b, u, out);
                break;
        }
        a += g.size;
        b += g.size;
        out += g.size;
    }
}

void App::Anim::normalise(const Layout& layout, double* v)
{
    for (const auto& g : layout.groups) {
        switch (g.kind) {
            case Kind::Scalar:
                break;
            case Kind::Colour:
                for (int i = 0; i < g.size; ++i) {
                    v[i] = std::clamp(v[i], 0.0, 1.0);
                }
                break;
            case Kind::Direction:
                normalise3(v);
                break;
            case Kind::Quaternion:
                normalise4(v);
                break;
        }
        v += g.size;
    }
}

double App::Anim::ease(const Easing& e, double u)
{
    double t = std::clamp(u, 0.0, 1.0);
    const double a = e.amplitude;
    const double p = e.period;
    const double s = e.overshoot;
    switch (e.type) {
        case EaseType::Quad:
            switch (e.mode) {
                case EaseMode::In: return inQuad(t);
                case EaseMode::Out: return outQuad(t);
                case EaseMode::InOut: return inOutQuad(t);
                case EaseMode::OutIn: return outIn(t, outQuad, inQuad);
            }
            break;
        case EaseType::Cubic:
            switch (e.mode) {
                case EaseMode::In: return inCubic(t);
                case EaseMode::Out: return outCubic(t);
                case EaseMode::InOut: return inOutCubic(t);
                case EaseMode::OutIn: return outIn(t, outCubic, inCubic);
            }
            break;
        case EaseType::Quart:
            switch (e.mode) {
                case EaseMode::In: return inQuart(t);
                case EaseMode::Out: return outQuart(t);
                case EaseMode::InOut: return inOutQuart(t);
                case EaseMode::OutIn: return outIn(t, outQuart, inQuart);
            }
            break;
        case EaseType::Quint:
            switch (e.mode) {
                case EaseMode::In: return inQuint(t);
                case EaseMode::Out: return outQuint(t);
                case EaseMode::InOut: return inOutQuint(t);
                case EaseMode::OutIn: return outIn(t, outQuint, inQuint);
            }
            break;
        case EaseType::Sine:
            switch (e.mode) {
                case EaseMode::In: return inSine(t);
                case EaseMode::Out: return outSine(t);
                case EaseMode::InOut: return inOutSine(t);
                case EaseMode::OutIn: return outIn(t, outSine, inSine);
            }
            break;
        case EaseType::Expo:
            switch (e.mode) {
                case EaseMode::In: return inExpo(t);
                case EaseMode::Out: return outExpo(t);
                case EaseMode::InOut: return inOutExpo(t);
                case EaseMode::OutIn: return outIn(t, outExpo, inExpo);
            }
            break;
        case EaseType::Circ:
            switch (e.mode) {
                case EaseMode::In: return inCirc(t);
                case EaseMode::Out: return outCirc(t);
                case EaseMode::InOut: return inOutCirc(t);
                case EaseMode::OutIn: return outIn(t, outCirc, inCirc);
            }
            break;
        case EaseType::Elastic:
            switch (e.mode) {
                case EaseMode::In: return inElasticHelper(t, 0, 1, 1, a, p);
                case EaseMode::Out: return outElasticHelper(t, 0, 1, 1, a, p);
                case EaseMode::InOut: return inOutElastic(t, a, p);
                case EaseMode::OutIn: return outInElastic(t, a, p);
            }
            break;
        case EaseType::Back:
            switch (e.mode) {
                case EaseMode::In: return inBack(t, s);
                case EaseMode::Out: return outBack(t, s);
                case EaseMode::InOut: return inOutBack(t, s);
                case EaseMode::OutIn:
                    return outIn(t, [s](double x) { return outBack(x, s); },
                                 [s](double x) { return inBack(x, s); });
            }
            break;
        case EaseType::Bounce:
            switch (e.mode) {
                case EaseMode::In: return inBounce(t, a);
                case EaseMode::Out: return outBounce(t, a);
                case EaseMode::InOut: return inOutBounce(t, a);
                case EaseMode::OutIn: return outInBounce(t, a);
            }
            break;
    }
    return t;
}

// --------------------------------------------------------------------------

int Curve::find(double t) const
{
    auto it = std::lower_bound(keys.begin(), keys.end(), t - TimeEpsilon,
                               [](const Key& k, double x) { return k.t < x; });
    if (it != keys.end() && std::fabs(it->t - t) < TimeEpsilon) {
        return static_cast<int>(it - keys.begin());
    }
    return -1;
}

void Curve::prepare()
{
    const int n = layout.size();

    // Sort, and of keys at one time keep the last given, at the first one's
    // time: a key set where one already is replaces it there
    std::stable_sort(keys.begin(), keys.end(),
                     [](const Key& a, const Key& b) { return a.t < b.t; });
    std::vector<Key> merged;
    merged.reserve(keys.size());
    for (auto& k : keys) {
        if (!merged.empty() && k.t - merged.back().t < TimeEpsilon) {
            double t = merged.back().t;
            merged.back() = std::move(k);
            merged.back().t = t;
        }
        else {
            merged.push_back(std::move(k));
        }
    }
    keys = std::move(merged);

    for (auto& k : keys) {
        k.v.resize(n, 0.0);
        k.left.dv.resize(n, 0.0);
        k.right.dv.resize(n, 0.0);
        normalise(layout, k.v.data());
    }

    // Each quaternion on the side of the one before, so that every span is
    // the shortest way and a Bezier through the components does not swing
    // round the far side
    int offset = 0;
    for (const auto& g : layout.groups) {
        if (g.kind == Kind::Quaternion) {
            for (std::size_t i = 1; i < keys.size(); ++i) {
                double* q = keys[i].v.data() + offset;
                if (dot4(keys[i - 1].v.data() + offset, q) < 0) {
                    for (int j = 0; j < 4; ++j) {
                        q[j] = -q[j];
                    }
                }
            }
        }
        offset += g.size;
    }

    // Handles
    const std::size_t count = keys.size();
    for (std::size_t i = 0; i < count; ++i) {
        Key& k = keys[i];
        const Key* prev = i > 0 ? &keys[i - 1] : nullptr;
        const Key* next = i + 1 < count ? &keys[i + 1] : nullptr;
        double dtl = prev ? -(k.t - prev->t) / 3.0 : 0.0;
        double dtr = next ? (next->t - k.t) / 3.0 : 0.0;

        auto computed = [](HandleType type) {
            return type == HandleType::AutoClamped || type == HandleType::Auto
                || type == HandleType::Vector;
        };
        if (computed(k.left.type)) {
            k.left.dt = dtl;
        }
        else {
            k.left.dt = std::min(0.0, k.left.dt);
        }
        if (computed(k.right.type)) {
            k.right.dt = dtr;
        }
        else {
            k.right.dt = std::max(0.0, k.right.dt);
        }

        int c = 0;
        for (const auto& g : layout.groups) {
            const bool own = g.kind == Kind::Scalar || g.kind == Kind::Colour;
            for (int j = 0; j < g.size; ++j, ++c) {
                // The slope through the neighbours; at an end, along the line
                // to the one neighbour where the curve goes on linearly past
                // it, flat where it holds
                double slope = 0.0;
                if (prev && next) {
                    slope = (next->v[c] - prev->v[c]) / (next->t - prev->t);
                }
                else if (next && before == Extrap::Linear) {
                    slope = (next->v[c] - k.v[c]) / (next->t - k.t);
                }
                else if (prev && after == Extrap::Linear) {
                    slope = (k.v[c] - prev->v[c]) / (k.t - prev->t);
                }
                auto side = [&](Handle& h, double dt, const Key* other) {
                    HandleType type = own ? h.type : HandleType::Auto;
                    switch (type) {
                        case HandleType::AutoClamped:
                            if (prev && next
                                && (k.v[c] - prev->v[c]) * (next->v[c] - k.v[c]) <= 0) {
                                h.dv[c] = 0;
                                break;
                            }
                            h.dv[c] = slope * dt;
                            break;
                        case HandleType::Auto:
                            h.dv[c] = slope * dt;
                            break;
                        case HandleType::Vector:
                            h.dv[c] = other ? (other->v[c] - k.v[c]) / 3.0 : 0.0;
                            break;
                        case HandleType::Aligned:
                        case HandleType::Free:
                            break;
                    }
                };
                side(k.left, k.left.dt, prev);
                side(k.right, k.right.dt, next);
            }
        }
    }
}

void Curve::handles(std::size_t i, double& dtl, double& dtr) const
{
    // The handles inside the span, shortened together where they reach past
    // each other, so that time runs one way along the curve
    const double len = keys[i + 1].t - keys[i].t;
    dtr = keys[i].right.dt;
    dtl = -keys[i + 1].left.dt;
    if (dtr + dtl > len && dtr + dtl > 0) {
        double f = len / (dtr + dtl);
        dtr *= f;
        dtl *= f;
    }
}

double Curve::bezierParameter(std::size_t i, double t) const
{
    double h1, h2;
    handles(i, h1, h2);
    const double x0 = keys[i].t;
    const double x3 = keys[i + 1].t;
    const double x1 = x0 + h1;
    const double x2 = x3 - h2;

    // Newton from the linear guess, kept inside a shrinking bracket
    double lo = 0, hi = 1;
    double s = (t - x0) / (x3 - x0);
    for (int iter = 0; iter < 50; ++iter) {
        double x = cubic(x0, x1, x2, x3, s) - t;
        if (std::fabs(x) < 1e-12 * std::max(1.0, std::fabs(t))) {
            break;
        }
        if (x > 0) {
            hi = s;
        }
        else {
            lo = s;
        }
        double d = cubicDerivative(x0, x1, x2, x3, s);
        double next = d > 0 ? s - x / d : -1;
        s = (next > lo && next < hi) ? next : 0.5 * (lo + hi);
    }
    return s;
}

void Curve::evaluateSpan(std::size_t i, double t, double* out) const
{
    const Key& k0 = keys[i];
    const Key& k1 = keys[i + 1];
    const double u = (t - k0.t) / (k1.t - k0.t);
    switch (k0.interp) {
        case Interp::Constant:
            std::copy(k0.v.begin(), k0.v.end(), out);
            return;
        case Interp::Linear:
            blend(layout, k0.v.data(), k1.v.data(), u, out);
            return;
        case Interp::Easing:
            blend(layout, k0.v.data(), k1.v.data(), ease(k0.easing, u), out);
            return;
        case Interp::Python:
            blend(layout, k0.v.data(), k1.v.data(), python ? python(k0, u) : u, out);
            return;
        case Interp::Bezier:
            break;
    }

    double h1, h2;
    handles(i, h1, h2);
    // The value handles shortened as their times were
    const double f1 = k0.right.dt > 0 ? h1 / k0.right.dt : 0.0;
    const double f2 = k1.left.dt < 0 ? h2 / -k1.left.dt : 0.0;
    const double s = bezierParameter(i, t);
    const int n = layout.size();
    for (int c = 0; c < n; ++c) {
        out[c] = cubic(k0.v[c], k0.v[c] + k0.right.dv[c] * f1, k1.v[c] + k1.left.dv[c] * f2,
                       k1.v[c], s);
    }
    normalise(layout, out);
}

bool Curve::evaluate(double t, std::vector<double>& out) const
{
    if (keys.empty()) {
        return false;
    }
    const int n = layout.size();
    out.resize(n);
    const Key& first = keys.front();
    const Key& last = keys.back();
    if (keys.size() == 1) {
        out = first.v;
        return true;
    }

    const double t0 = first.t;
    const double tN = last.t;
    const double period = tN - t0;

    if (t >= t0 && t < tN) {
        auto it = std::upper_bound(keys.begin(), keys.end(), t,
                                   [](double x, const Key& k) { return x < k.t; });
        std::size_t i = static_cast<std::size_t>(it - keys.begin()) - 1;
        evaluateSpan(i, t, out.data());
        return true;
    }

    const bool isBefore = t < t0;
    if (!isBefore && t == tN) {
        out = last.v;
        return true;
    }
    switch (isBefore ? before : after) {
        case Extrap::Constant:
            out = isBefore ? first.v : last.v;
            return true;
        case Extrap::Linear: {
            const Key& a = isBefore ? keys[0] : keys[keys.size() - 2];
            const Key& b = isBefore ? keys[1] : keys.back();
            blend(layout, a.v.data(), b.v.data(), (t - a.t) / (b.t - a.t), out.data());
            return true;
        }
        case Extrap::Cycle:
        case Extrap::CycleOffset:
        case Extrap::PingPong:
            break;
    }

    // Which repeat t falls in, and where in the keys
    const double cycles = std::floor((t - t0) / period);
    double local = t - cycles * period;
    if (local >= tN) {
        local = t0;
    }
    const Extrap mode = isBefore ? before : after;
    const long k = static_cast<long>(cycles);
    if (mode == Extrap::PingPong && (k % 2 != 0)) {
        local = tN - (local - t0);
    }
    if (local <= t0) {
        out = first.v;
    }
    else if (local >= tN) {
        out = last.v;
    }
    else {
        auto it = std::upper_bound(keys.begin(), keys.end(), local,
                                   [](double x, const Key& key) { return x < key.t; });
        evaluateSpan(static_cast<std::size_t>(it - keys.begin()) - 1, local, out.data());
    }
    if (mode != Extrap::CycleOffset || k == 0) {
        return true;
    }

    // Each repeat starts where the one before ended
    int c = 0;
    for (const auto& g : layout.groups) {
        switch (g.kind) {
            case Kind::Scalar:
            case Kind::Colour:
                for (int j = 0; j < g.size; ++j) {
                    out[c + j] += (last.v[c + j] - first.v[c + j]) * static_cast<double>(k);
                }
                if (g.kind == Kind::Colour) {
                    for (int j = 0; j < g.size; ++j) {
                        out[c + j] = std::clamp(out[c + j], 0.0, 1.0);
                    }
                }
                break;
            case Kind::Quaternion: {
                // delta = last * first^-1, applied k times
                const double* q0 = first.v.data() + c;
                double inv[4] = {-q0[0], -q0[1], -q0[2], q0[3]};
                double delta[4], dk[4];
                mul4(last.v.data() + c, inv, delta);
                normalise4(delta);
                pow4(delta, static_cast<double>(k), dk);
                mul4(dk, out.data() + c, out.data() + c);
                normalise4(out.data() + c);
                break;
            }
            case Kind::Direction:
                break;
        }
        c += g.size;
    }
    return true;
}
