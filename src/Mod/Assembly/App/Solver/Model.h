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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Base/Placement.h>
#include <Base/Rotation.h>
#include <Base/Vector3D.h>

#include <Mod/Assembly/AssemblyGlobal.h>

#include "Curve.h"
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
    // The centre and the inertia are given (a group of bodies, sec 6.22), and
    // centredOnJoints keeps them instead of taking them from the joints.
    bool sized = false;
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
    // a point on a curve (sec 6.18)
    PointOnCurve,  // oJ on the curve (Joint::curve, in frame I) offset by value
                   // in its plane                              2
    // reserved: Universal, ConstantVelocity, Tangent, Slot, Path
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
    Id curve = NoId;  // PointOnCurve only: the curve, in Model::curves
};

// A curve a joint holds a point on (JointKind::PointOnCurve), given in the
// coordinates of that joint's frame I and evaluated exactly (Curve.h). Shared:
// the geometry is not copied with the model. A record of its own, so that a
// later lift-off cam can reference the same curve (docs/AssemblyDynamics.md
// sec 6.1).
struct Curve
{
    Id id = NoId;
    Id source = NoId;
    std::shared_ptr<const CurveGeometry> geometry;
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

// The value and first two time derivatives of a formula at a time.
using FormulaFunction = std::function<FormulaValue(double time)>;

// A formula of time in the workbench's grammar (sec 4.6.1, Formula.h);
// 'initialValue' is substituted before it gets here. The text is the record;
// the tree is the builder's parse of it, shared, so a backend or the default
// simulation evaluates without parsing again. Null when nobody parsed it.
// 'function' is the formula as a callable, which a backend that takes
// functions rather than text calls (the Ondsel adapter hands it to
// OndselSolver as an ExternalFunction, so the solver neither parses nor
// differentiates the formula); the builder backs it with the tree.
struct Formula
{
    std::string text;
    std::shared_ptr<const FormulaTree> tree;
    FormulaFunction function;

    static FormulaFunction functionOf(std::shared_ptr<const FormulaTree> tree)
    {
        if (!tree) {
            return {};
        }
        return [tree = std::move(tree)](double time) {
            return tree->evaluate(time);
        };
    }

    // 'function', else the tree's, else a parse of the text's; empty when the
    // text does not parse.
    FormulaFunction evaluator() const
    {
        if (function) {
            return function;
        }
        return functionOf(tree ? tree : FormulaTree::parse(text));
    }
};

enum class DriverKind : std::uint8_t
{
    RotationZ,
    TranslationZ,
    Both
};

// Prescribes the relative motion of frame J against frame I over time.
// What a driver's formula gives (docs/AssemblyDynamics.md sec 6.1):
// Position, the angle (rad) or the distance (mm); Speed, their rate; Effort,
// the torque (mN mm) or the force (mN). Speed and Effort are a dynamic run's.
enum class MotorControl : std::uint8_t
{
    Position,
    Speed,
    Effort
};

struct Driver
{
    Id id = NoId;
    Id source = NoId;
    Id frameI = NoId;
    Id frameJ = NoId;
    DriverKind kind {};
    Formula rotation;
    Formula translation;
    MotorControl rotationControl = MotorControl::Position;
    MotorControl translationControl = MotorControl::Position;
};

// Physical mass properties of a body (a bundle of parts), for dynamics only
// (docs/AssemblyDynamics.md sec 6.1). Kinematic solves keep Body::mass, the
// placeholder (audit defect 2), and never read this.
struct Inertia
{
    Id body = NoId;
    double mass = 0.0;   // kg
    Base::Vector3d com;  // mm, in the body's frame
    // kg mm^2, about com, along the body's axes: the tensor's xx yy zz xy xz
    // yz (xy is minus the integral of x y dm)
    std::array<double, 6> inertia {};
    enum class Origin : std::uint8_t
    {
        Shape,          // from the shape and its material's density
        Override,       // given (a mass override)
        AssumedDensity  // from the shape, at the default material's density
    } origin {};
};

// The inertia in another frame: toOther takes the points of the record's
// frame into the other's (its placement there). Mass and origin kept.
AssemblyExport Inertia moved(const Inertia& inertia, const Base::Placement& toOther);
// b added to a, both in the same frame: the masses summed, the inertias
// moved to the common centre of mass by the parallel-axis theorem. The
// origin: AssumedDensity if either is, else Override if either is.
AssemblyExport void add(Inertia& a, const Inertia& b);

// A force element of a dynamic analysis (sec 6.1), between two frames
enum class ForceKind : std::uint8_t
{
    // Along the line from oI to oJ: f - k (l - rest) - c dl/dt, a positive
    // force pushing the two apart (Chrono's ChLinkTSDA)
    SpringDamper,
    // About zI, on a revolute or cylindrical joint's frames: t - k (a - a0 -
    // rest) - c da/dt, a the turn of xJ about zI from xI (a limit's angle),
    // a0 its value at the start; a positive torque turning a up (Chrono's
    // ChLinkRSDA)
    TorsionSpring,
    // Applied loads: on frame I's body a force at oI, or a torque, of
    // magnitude(t) along 'direction', in frame I's axes (followsFrame) or
    // the assembly's; its opposite on frame J's body (the ground's when J
    // is on none), at where oI was at the start (Chrono's ChLoadBodyBody)
    AppliedForce,
    AppliedTorque,
    // On frame I's body, in its axes: preload6 - k6 s - c6 ds/dt, s minus
    // the pose of frame J in frame I (J's origin, then its turn as a
    // rotation vector: small turns), the two frames coinciding at the start;
    // the opposite on J's body. A force acts at oI, a torque is about it
    // (Chrono's ChLoadBodyBodyBushingGeneric).
    Bushing
};

struct ForceElement
{
    Id id = NoId;
    Id source = NoId;  // the document's element
    ForceKind kind {};
    Id frameI = NoId;
    Id frameJ = NoId;
    double k = 0.0;        // mN/mm; torsion: mN mm / rad
    double rest = 0.0;     // mm, 0: the length at the start; torsion: rad from the start
    double preload = 0.0;  // mN, pushing apart; torsion: mN mm
    double c = 0.0;        // mN s / mm (kg/s); torsion: mN mm s / rad
    // Applied loads: a unit vector, in frame I's axes when followsFrame,
    // else the assembly's; the magnitude over time, mN or mN mm
    Base::Vector3d direction;
    bool followsFrame = false;
    Formula magnitude;
    // A bushing's: along x, y, z (mN/mm, mN s/mm, mN), then about them (mN
    // mm / rad, mN mm s / rad, mN mm)
    std::array<double, 6> k6 {};
    std::array<double, 6> c6 {};
    std::array<double, 6> preload6 {};
};

// An initial velocity of a dynamic run (sec 6.1), set on the start
// assembly and then made consistent with the joints by the least
// mass-weighted change: a joint's rate on its frames I and J, as a Speed
// driver of the joint of that kind gives it; or a body's velocity, the
// velocity v of frame I's origin and the angular velocity w, in the
// assembly's axes
struct InitialVelocity
{
    enum class Kind : std::uint8_t
    {
        RotationZ,     // rad/s, xJ about zI
        TranslationZ,  // mm/s, along zI
        Body
    };
    Id source = NoId;  // the document's condition
    Kind kind {};
    Id frameI = NoId;
    Id frameJ = NoId;  // NoId for a body's
    double rate = 0.0;
    Base::Vector3d v;  // mm/s
    Base::Vector3d w;  // rad/s
};

// Contact (sec 6.1, 7; phase D2). The law of a contact between two shapes,
// d their penetration (the Adams/SOLIDWORKS impact law and friction, P7):
//   normal  Fn = k d^e + STEP(d, 0, 0, dmax, cmax) dd/dt, never pulling;
//   friction mu(v) Fn against the sliding velocity v, mu rising from 0 at
//            rest to muStatic at vStiction and going to muDynamic at
//            vFriction, Adams' STEP between them.
// The damping as given, the STEP's, or by a coefficient of restitution r,
// Hunt and Crossley's: Fn = k d^e (1 + a / v0 dd/dt), a exactly r's for any
// e (huntCrossley()), v0 the approach speed the contact started at.
// k and e as given, or Hertz's from the shapes' materials: k = 4/3 E* R^0.5
// and e = 1.5, 1/E* the sum of the two shapes' 1 / ContactShape::modulus,
// and R the contact's radius -- from the curvatures where the shapes touch,
// Johnson's for a line, or from the two shapes' sizes; or, at e, the k at
// which the lighter body of each pair sinks the allowance under its weight
// (Drake's penetration allowance).
struct ContactLaw
{
    enum class Stiffness : std::uint8_t
    {
        Value,       // stiffness and exponent
        Materials,   // Hertz's
        Penetration  // the allowance's, at exponent
    };
    enum class Radius : std::uint8_t
    {
        Patch,  // 1 / (the two curvatures), at most the patch's extent
        Part    // 1/R = 1/RA + 1/RB, each shape's the ball's of its volume
    };
    enum class Damping : std::uint8_t
    {
        Value,       // maxDamping reached at penetration
        Restitution  // restitution's
    };
    Stiffness from = Stiffness::Value;
    Radius radius = Radius::Patch;
    Damping damping = Damping::Value;
    double restitution = 0.6;  // r
    double stiffness = 1e8;     // k, mN / mm^e (1e5 N/mm at e = 1)
    double exponent = 1.5;      // e
    double maxDamping = 1e4;    // cmax, mN s / mm (10 N s/mm)
    double penetration = 0.01;  // dmax, mm
    double allowance = 0.01;    // mm, for Stiffness::Penetration
    double muStatic = 0.3;
    double muDynamic = 0.25;
    double vStiction = 0.1;    // mm/s
    double vFriction = 10.16;  // mm/s
};

// A piece of a planar profile (ContactShape::Kind::Profile), in its frame's
// xy plane (z 0): a segment from 'from' to 'to', or, 'sweep' not 0, an arc
// about 'centre' turning 'sweep' radians from 'from' to 'to', positive
// counterclockwise. The material is on its left: a counterclockwise arc is
// convex, a clockwise one hollow.
struct ProfilePiece
{
    Base::Vector3d from;
    Base::Vector3d to;
    Base::Vector3d centre;
    double sweep = 0.0;
};

// A solid's contact geometry (sec 7.1, the ladder's first rungs): a
// primitive the solid is, centred on its frame along its axes; the convex
// hull of points on it; or its closed mesh, in its frame. Or a planar face's
// profile (rung 5, sec 7.4's 2D path): its boundary's loops in its frame's
// xy plane, the face on the side of +z. On a frame, as every force element
// is, so that the reductions that move frames move it.
struct ContactShape
{
    enum class Kind : std::uint8_t
    {
        Sphere,    // size.x the radius
        Box,       // size the edges along x, y, z
        Cylinder,  // along z: size.x the radius, size.z the height
        ConvexHull,
        Mesh,    // points, and the triangles on them, wound outward
        Profile  // pieces, closed loops, the face to their left
    };
    Id frame = NoId;
    Kind kind {};
    Base::Vector3d size;
    std::vector<Base::Vector3d> points;
    // A mesh's; a hull's mesh's, its points', for its curvature
    std::vector<std::array<int, 3>> triangles;
    // A profile's pieces, each loop's in order, and where each loop starts;
    // its size.x and size.y the lowest and highest its solid reaches along
    // z, for the height its force acts at
    std::vector<ProfilePiece> pieces;
    std::vector<int> loops;
    // Its material's E / (1 - nu^2), kPa (mN/mm^2), for ContactLaw's
    // Materials; and its solid's volume, mm^3
    double modulus = 0.0;
    double volume = 0.0;
};

// A contact set: the shapes of group A touching those of group B, or, B
// empty, those of A touching one another; never two shapes on one body.
// Or a cam that lifts off (a CamContact, sec 7.4): its PointOnCurve joint,
// which a dynamic or static run does not hold, the follower (frame J's
// origin, offset by the joint's value, a roller's radius) pushed off the
// curve by the law instead -- on its 'side' of it, +1 the side the joint's
// equations' N2 points to, -1 the other. Its groups empty. Or a profile
// contact (sec 7.4's 2D path): its groups the profiles, A's of one body,
// B's of another, each of A's touching each of B's in A's plane.
struct ContactPair
{
    Id id = NoId;
    Id source = NoId;        // the document's set
    std::vector<Id> groupA;  // ContactShape indices
    std::vector<Id> groupB;
    ContactLaw law;
    Id cam = NoId;  // the joint, in Model::joints
    double side = 1.0;
};

// A sensor (sec 5.2, P15): one of the values a run reports (Backend.h's
// Channel) -- a vector's component, or its length -- watched after each
// step. It trips when the value passes 'value', rising above it or, 'below',
// falling under it: it stops the run there, the time located, or, 'stop'
// false, marks the time and the run goes on. In the model's units, as the
// run reports the value; scaled() leaves it.
struct Sensor
{
    Id source = NoId;   // the document's sensor
    Id watched = NoId;  // what reports the value: a joint, a spring, a contact set
    std::string quantity;  // the Channel's
    int component = -1;    // a vector's x, y or z (0, 1, 2); -1 its length, a scalar itself
    bool below = false;
    double value = 0.0;
    bool stop = true;
};

// How a dynamic or static run stops a limit (sec 5.2's JointStop, P10). A
// limit with none is Rigid at the study's restitution. Rigid: held at its
// bound by a link, the impact plastic or, 'restitution' not negative, at
// that coefficient. Penalty: pushed back by a spring and a damper on how far
// past the bound the joint's value is, k d + c dd/dt and never pulling --
// along zI for a TranslationZ limit, about it for a RotationZ one. Off: not
// stopped at all. k in mN/mm or mN mm per radian, c in mN s/mm or mN mm s
// per radian, as the limit's kind is.
struct Stop
{
    enum class Mode : std::uint8_t
    {
        Rigid,
        Penalty,
        Off
    };
    Id source = NoId;  // the document's JointStop
    Id limit = NoId;   // in Model::limits
    Mode mode {};
    double restitution = -1.0;  // Rigid: negative for the study's
    double k = 0.0;
    double c = 0.0;
};

// What a dynamic analysis adds to the model (sec 6.1); a kinematic build has
// none. Grows with phase D1: forces, tables, contact, stops, initial
// velocities, sensors.
struct Dynamics
{
    Base::Vector3d gravity;         // mm/s^2, in the assembly's frame; zero: off
    std::vector<Inertia> inertias;  // one per body that has mass, by Inertia::body
    std::vector<ForceElement> forces;
    std::vector<InitialVelocity> initial;
    std::vector<ContactShape> contactShapes;
    std::vector<ContactPair> contacts;
    std::vector<Sensor> sensors;
    std::vector<Stop> stops;  // at most one per limit
};

struct Model
{
    // 2: the dynamics block (sec 6.1)
    std::uint32_t version = 2;
    std::vector<std::string> sources;  // names, for diagnostics, dumps and native formats
    std::vector<Body> bodies;  // in insertion order (sec 6.2: order matters)
    std::vector<Frame> frames;  // in creation order
    std::vector<Joint> joints;
    std::vector<Ground> grounds;
    std::vector<Limit> limits;
    std::vector<Driver> drivers;
    std::vector<Curve> curves;
    std::optional<Dynamics> dynamics;  // empty for every kinematic build

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
    Id addCurve(Curve curve)
    {
        return add(curves, std::move(curve));
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

// Whether two models hold the same mechanism (docs/AssemblyDynamics.md sec
// 8.4, the stale flag): record for record, positions and directions within
// 'tolerance' (relative, at least 1 mm or 1) and the rest exactly, apart from
// the bodies' placements -- where the parts stand, which a run's playback
// moves, and from which a frame on a part is recomputed in its last bits.
AssemblyExport bool sameMechanism(const Model& a, const Model& b, double tolerance);

/** The model with its lengths times `length` (docs/AssemblyDynamics.md sec
 * 8.1: Solver::SI::length takes it to SI): positions, frames, the joints'
 * offsets, distances, radii and pitches, the translation limits, the
 * drivers' formulas that give a length, a force (a force scales as a length
 * when mass and time stay) or a torque (as a length squared), and the
 * dynamics block's centres, inertias (times length^2), gravity and force
 * elements, and the curves (scaledCurve()). Masses, times and angles stay.
 */
AssemblyExport Model scaled(const Model& model, double length);

/// The formula times a factor: its value, rate and acceleration; the text,
/// the record, kept
AssemblyExport Formula scaledFormula(const Formula& formula, double factor);

}  // namespace Assembly::Solver
