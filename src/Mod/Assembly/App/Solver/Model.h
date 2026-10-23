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

// The solver-neutral model of an assembly: what the workbench asks a solver
// backend to solve, as plain data (docs/AssemblySolverInterface.md sec 4.2).
// No document objects, no solver types, no shared graph: every record refers
// to others by Id, which is the record's index in its vector. The whole model
// is serialisable as is, so a backend can live in another process.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Base/Placement.h>
#include <Base/Rotation.h>
#include <Base/Vector3D.h>

#include <Mod/Assembly/AssemblyGlobal.h>

#include "Formula.h"

namespace Assembly::Solver
{

using Id = std::uint32_t;
inline constexpr Id NoId = 0xffffffffu;

struct MassProps
{
    double mass = 1.0;  // today's placeholder (audit defect 2)
    Base::Vector3d com;  // in the body frame
    std::array<double, 3> principal {1.0, 1.0, 1.0};
    Base::Rotation principalAxes;
};

// One rigid body the solver moves: a part, or a bundle of parts held together
// (a rigid group, or Fixed joints while dragging).
struct Body
{
    Id id = NoId;
    Id source = NoId;  // index into Model::sources: the representative object
    Base::Placement placement;  // current pose, the solve's starting point
    bool grounded = false;  // set when a Ground record pins this body
    MassProps mass;
    // How hard the body resists being moved during drag steps, against the
    // others at 1; the GUI gives the dragged bodies more so they follow the
    // mouse. Static solves ignore it.
    double dragWeight = 1.0;
};

// A frame fixed on a body, given in the body frame; body == NoId puts it on
// the ground (the assembly's own frame).
struct Frame
{
    Id id = NoId;
    Id body = NoId;
    Base::Placement local;
    Id label = NoId;  // index into Model::sources: a name for dumps and native formats
};

enum class JointKind : std::uint8_t
{
    // lower pairs                                     equations
    Fixed,        // frames coincide                         6
    Revolute,     // origins coincide, zI || zJ              5
    Cylindrical,  // oJ on axis zI, zI || zJ                 4
    Prismatic,    // oJ on axis zI, rotations equal          5
    Spherical,    // origins coincide                        3
    Planar,       // oJ in plane(I) + value, zI || zJ        3
    // orientation only
    ParallelZ,       // zI || zJ                             2
    PerpendicularZ,  // zI . zJ = 0                          1
    AngleZ,          // angle(zI, zJ) = value                1
    // incidence with an offset
    PointInPlane,  // oJ in plane(I) + value                 1
    LineInPlane,   // PointInPlane and zI . zJ = 0           2
    PointOnLine,   // oJ on axis zI                          2   (unused today)
    // distances
    PointPointDistance,  // |oJ - oI| = value                1
    PointLineDistance,   // dist(oJ, axis zI) = value        1
    LineLineDistance,    // axes parallel at value           3
    // couplers (relative rotations / slides about z)
    Gear,         // value*thetaI + value2*thetaJ            1   (Belt: value2 < 0)
    CoaxialGear,  // as Gear, angles measured against carrier frame K
    RackPinion,   // slide(I along x) = value*theta(J)       1
    Screw,        // slide(I along z) = value*theta/(2 pi)   1
    // reserved: Universal, ConstantVelocity, Tangent, Slot, Cam, Path
    Count_
};

struct Joint
{
    Id id = NoId;
    Id source = NoId;  // owning document joint; one source may own several records
    JointKind kind {};
    Id frameI = NoId;
    Id frameJ = NoId;
    Id frameK = NoId;  // CoaxialGear only
    double value = 0.0;  // offset | distance | angle (rad) | radius | pitch
    double value2 = 0.0;  // second radius
};

// Pins a body to the ground: frameI is on the ground (body NoId) at the pose the
// body must keep, frameJ on the body. One record per grounded document object,
// so a body bundling several grounded objects has several.
struct Ground
{
    Id id = NoId;
    Id source = NoId;  // the grounded document object
    Id frameI = NoId;
    Id frameJ = NoId;
};

enum class LimitKind : std::uint8_t
{
    TranslationZ,  // mm, along zI
    RotationZ  // rad, about zI
};

// A bound on the relative motion of frame J against frame I. Numbers, never
// expression strings.
struct Limit
{
    Id id = NoId;
    Id source = NoId;
    Id frameI = NoId;
    Id frameJ = NoId;
    LimitKind kind {};
    std::optional<double> min;
    std::optional<double> max;
    double tolerance = 1e-9;
};

// A formula of time in the workbench's grammar (sec 4.6.1, Formula.h);
// 'initialValue' is substituted before it gets here. The text is the record;
// the tree is the builder's parse of it, shared, so a backend or the default
// simulation evaluates without parsing again. Null when nobody parsed it.
struct Formula
{
    std::string text;
    std::shared_ptr<const FormulaTree> tree;
};

enum class DriverKind : std::uint8_t
{
    RotationZ,
    TranslationZ,
    Both
};

// Prescribes the relative motion of frame J against frame I over time.
struct Driver
{
    Id id = NoId;
    Id source = NoId;
    Id frameI = NoId;
    Id frameJ = NoId;
    DriverKind kind {};
    Formula rotation;
    Formula translation;
};

struct Model
{
    std::uint32_t version = 1;
    std::vector<std::string> sources;  // names, for diagnostics, dumps and native formats
    std::vector<Body> bodies;  // in insertion order (sec 6.2: order matters)
    std::vector<Frame> frames;  // in creation order
    std::vector<Joint> joints;
    std::vector<Ground> grounds;
    std::vector<Limit> limits;
    std::vector<Driver> drivers;

    Id addSource(std::string name)
    {
        sources.push_back(std::move(name));
        return static_cast<Id>(sources.size() - 1);
    }
    Id addBody(Body body)
    {
        return add(bodies, std::move(body));
    }
    Id addFrame(Frame frame)
    {
        return add(frames, std::move(frame));
    }
    Id addJoint(Joint joint)
    {
        return add(joints, std::move(joint));
    }
    Id addGround(Ground ground)
    {
        return add(grounds, std::move(ground));
    }
    Id addLimit(Limit limit)
    {
        return add(limits, std::move(limit));
    }
    Id addDriver(Driver driver)
    {
        return add(drivers, std::move(driver));
    }

    const std::string& sourceName(Id index) const
    {
        static const std::string none;
        return index < sources.size() ? sources[index] : none;
    }

private:
    template<typename T>
    static Id add(std::vector<T>& records, T record)
    {
        record.id = static_cast<Id>(records.size());
        records.push_back(std::move(record));
        return records.back().id;
    }
};

// Whether two models are the same record for record, bit for bit, apart from
// the bodies' drag weights: the one change a session takes without a reload
// before a drag (Session::update).
AssemblyExport bool sameExceptDragWeights(const Model& a, const Model& b);

}  // namespace Assembly::Solver
