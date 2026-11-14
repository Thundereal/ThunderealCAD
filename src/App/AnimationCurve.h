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

#ifndef APP_ANIMATIONCURVE_H
#define APP_ANIMATIONCURVE_H

/** The curves of an animation track: how a track's keys give a value at any
 * time (docs/Animation.md sections 3.5, 3.12, 3.13 and 4).
 *
 * Nothing here knows of documents or properties. A value is a list of
 * numbers whose meaning is given by a Layout -- groups of scalars,
 * directions, quaternions and colours -- which a property fills in from its
 * own type (Property::getAnimLayout()). Curves run on the numbers.
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <FCGlobal.h>

namespace Base
{
class Rotation;
}

namespace App::Anim
{

/// What a group of numbers in a value is, which decides how it blends
enum class Kind : std::uint8_t
{
    Scalar,      ///< any number of independent numbers, blended linearly
    Direction,   ///< 3 numbers, a unit vector, blended on the sphere
    Quaternion,  ///< 4 numbers x y z w, a unit quaternion, blended by slerp
    Colour,      ///< 4 numbers r g b a, blended linearly and kept in [0, 1]
};

/// How a rotation is turned into numbers (docs/Animation.md 3.13)
enum class RotationMode : std::uint8_t
{
    Shortest,  ///< a quaternion: the shortest way between keys
    Turns,     ///< an axis and an angle in degrees, the angle unwrapped
    Euler,     ///< yaw, pitch and roll in degrees
};

struct Group
{
    Kind kind = Kind::Scalar;
    int size = 1;  ///< numbers in the group: Direction 3, Quaternion and Colour 4

    bool operator==(const Group& other) const
    {
        return kind == other.kind && size == other.size;
    }
};

/// What the numbers of a value are. `rotation` is asked for by the caller
/// before a property fills in the groups.
struct AppExport Layout
{
    RotationMode rotation = RotationMode::Shortest;
    std::vector<Group> groups;

    void clear()
    {
        groups.clear();
    }
    /// Append a group; scalars next to scalars are merged
    void add(Kind kind, int size = 1);
    /// Append the groups of a rotation in this layout's rotation mode
    void addRotation();
    /// Total count of numbers
    int size() const;

    bool operator==(const Layout& other) const
    {
        return rotation == other.rotation && groups == other.groups;
    }
    bool operator!=(const Layout& other) const
    {
        return !(*this == other);
    }
};

/// A rotation as numbers in `mode`, appended to `out` (4, 4 or 3 numbers)
AppExport void rotationToNumbers(const Base::Rotation& rot, RotationMode mode,
                                 std::vector<double>& out);
/// The rotation at `nums` in `mode`; returns the count of numbers read
AppExport int rotationFromNumbers(const double* nums, RotationMode mode, Base::Rotation& rot);

/// Blend `a` to `b` by `u` (0 gives a, 1 gives b; outside [0, 1] extrapolates
/// along the same path), group by group, into `out`. The three may alias.
AppExport void blend(const Layout& layout, const double* a, const double* b, double u, double* out);

/// Normalise the direction and quaternion groups of `v`, and clamp colours
AppExport void normalise(const Layout& layout, double* v);

/// Interpolation of the span that leaves a key
enum class Interp : std::uint8_t
{
    Constant,
    Linear,
    Bezier,
    Easing,
    Python,
};

/// The easing functions, Robert Penner's set as Qt's QEasingCurve has them
enum class EaseType : std::uint8_t
{
    Quad,
    Cubic,
    Quart,
    Quint,
    Sine,
    Expo,
    Circ,
    Elastic,
    Back,
    Bounce,
};

enum class EaseMode : std::uint8_t
{
    In,
    Out,
    InOut,
    OutIn,
};

struct Easing
{
    EaseType type = EaseType::Sine;
    EaseMode mode = EaseMode::InOut;
    double amplitude = 1.0;     ///< Elastic and Bounce
    double period = 0.3;        ///< Elastic
    double overshoot = 1.70158; ///< Back

    bool operator==(const Easing& o) const
    {
        return type == o.type && mode == o.mode && amplitude == o.amplitude
            && period == o.period && overshoot == o.overshoot;
    }
};

/// The eased progress of `u`, clamped to [0, 1] first. Back and Elastic go
/// outside [0, 1] in between, as they are meant to.
AppExport double ease(const Easing& easing, double u);

enum class HandleType : std::uint8_t
{
    AutoClamped,  ///< smooth through the neighbours, flat at a peak or a trough
    Auto,         ///< smooth through the neighbours
    Vector,       ///< pointing at the neighbour: a straight span when both ends are
    Aligned,      ///< stored; the editor keeps the two handles in line
    Free,         ///< stored
};

/// One side of a key's Bezier handle: an offset in time (negative on the
/// left) and one in value per number. Only scalar and colour numbers have
/// their own handles; directions and quaternions are always smooth.
struct Handle
{
    HandleType type = HandleType::AutoClamped;
    double dt = 0;
    std::vector<double> dv;

    bool operator==(const Handle& o) const
    {
        return type == o.type && dt == o.dt && dv == o.dv;
    }
};

struct Key
{
    double t = 0;                    ///< seconds
    std::vector<double> v;           ///< the value, Layout::size() numbers
    Interp interp = Interp::Linear;  ///< for the span to the next key
    Easing easing;                   ///< Interp::Easing
    Handle left, right;              ///< Interp::Bezier
    /// Interp::Python: an expression in `u`, the span's progress from 0 to
    /// 1, giving the eased progress (docs/Animation.md sec 4)
    std::string python;

    bool operator==(const Key& o) const
    {
        return t == o.t && v == o.v && interp == o.interp && easing == o.easing
            && left == o.left && right == o.right && python == o.python;
    }
};

/// Before the first key and after the last
enum class Extrap : std::uint8_t
{
    Constant,     ///< the end key's value
    Linear,       ///< on along the line through the two end keys
    Cycle,        ///< the keys again, and again
    CycleOffset,  ///< the keys again, each time starting where the last ended
    PingPong,     ///< the keys forward, then backward
};

/// Keys closer in time than this are the same time
constexpr double TimeEpsilon = 1e-6;

/** A track's keys and how to evaluate them. Call prepare() after changing
 * anything, before evaluate().
 */
class AppExport Curve
{
public:
    Layout layout;
    std::vector<Key> keys;  ///< sorted by time, one per time
    Extrap before = Extrap::Constant;
    Extrap after = Extrap::Constant;

    /// Interp::Python: the eased progress of `u` for the span leaving `key`
    std::function<double(const Key& key, double u)> python;

    /** Sort the keys, merge keys at one time (the later given wins, at the
     * earlier's time), turn each
     * quaternion to the side of its predecessor so that every span is the
     * shortest, and compute the automatic and vector handles.
     */
    void prepare();

    /// The value at time `t` into `out`; false when there are no keys
    bool evaluate(double t, std::vector<double>& out) const;

    /// Index of the key at `t`, or -1
    int find(double t) const;

private:
    void evaluateSpan(std::size_t i, double t, double* out) const;
    double bezierParameter(std::size_t i, double t) const;
    void handles(std::size_t i, double& dtl, double& dtr) const;
};

}  // namespace App::Anim

#endif  // APP_ANIMATIONCURVE_H
