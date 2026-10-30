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

// The Chrono backend (docs/AssemblySolverInterface.md sec 6.15, 6.17).
//
// The model: a ChBody per Model::Body (mass and inertia set so that Chrono's
// mass matrix is the metric of the solve, below); a body pinned by a Ground
// record is SetFixed() where its first Ground holds it, as the Reference
// does; a fixed world body carries the frames that are on no body. Each joint
// becomes one or two of Chrono's links, frame J on the first body and frame I
// on the second, since Chrono's links measure frame 1 in frame 2 and the
// neutral joints measure J in I:
//
//   Fixed                   ChLinkMateFix
//   Revolute                ChLinkMateRevolute
//   Cylindrical             ChLinkMateCylindrical
//   Prismatic               ChLinkMatePrismatic
//   Spherical               ChLinkMateSpherical
//   Planar(v)               ChLinkMatePlanar, SetDistance(v)
//   ParallelZ               ChLinkMateParallel
//   PerpendicularZ          ChLinkMateOrthogonal
//   AngleZ(pi/2)            ChLinkMateOrthogonal
//   PointInPlane(v)         ChLinkMateDistanceZ, SetDistance(v)
//   LineInPlane(v)          ChLinkMateDistanceZ and ChLinkMateOrthogonal
//   PointOnLine             ChLinkMateGeneric(x, y)
//   PointPointDistance(v)   ChLinkDistance
//   Gear (r1, r2 > 0)       ChLinkLockGear with its phase enforced
//   drivers                 ChLinkMotorRotationAngle, ChLinkMotorLinearPosition
//
// and every other kind -- AngleZ other than a right angle, PointLineDistance,
// LineLineDistance, a belt, a gear between coaxial frames or shafts that are
// not parallel, CoaxialGear, RackPinion, Screw, PointOnCurve -- a
// NeutralLink: a link of our own holding the neutral layer's equations
// (JointEquations.h), the Reference's, with their analytic Jacobian in
// Chrono's coordinates.
//
// PointOnCurve (a cam) is a NeutralLink rather than ChLinkLockPointSpline:
// that link finds its nearest point by sampling the whole ChLine on every
// update (FindNearestLinePoint: it can jump across a cam to a closer
// stretch), to a tolerance of 1e-6, takes the tangent and the curvature by
// finite differences (a step of 1e-4 of the parameter), and holds the point
// by moving its marker onto the curve at each update, like ChLinkLockGear's
// phase. The NeutralLink evaluates the exact OCCT curve through the neutral
// projector, follows the closest point from the last one, and holds the
// equations with their analytic Jacobian.
// Chrono::nativeLink() decides; Chrono::refusal() says why a joint cannot be
// held at all (a gear without a ratio).
//
// Branches. OndselSolver writes a parallel axis as two direction cosines,
// which hold zJ = zI and zJ = -zI alike, and a fixed rotation as three, which
// hold four rotations (the identity and the half turns about x, y and z).
// Chrono's mates hold components of the relative quaternion, which only the
// identity (or a turn about z) zeroes. So frame J is turned, when the link is
// made, onto the solution the start is nearest: the half turn about x when zJ
// starts against zI (any joint holding the axes parallel), the nearest of the
// four for Fixed and Prismatic. The joint is the same; Chrono only holds the
// branch OndselSolver would converge to from there.
//
// Solve. The pose closest to a target -- the start for a static solve, the
// drag targets for a drag step -- in the metric |dx|^2 + |dtheta|^2 / 4 per
// body (the Reference's), subject to Chrono's constraint equations. Chrono's
// own assembly analysis (ChAssemblyAnalysis) is a Newton iteration whose
// steps are the KKT solve [M Cq'; Cq 0] [dx; -l] = [0; -C], i.e. the step of
// least M-norm onto the linearised constraints; with mass 1 and inertia 1/4 M
// is the metric above. Here that iteration, with backtracking on |C|,
// restores the constraints (restoration); then the same KKT solve with the
// right-hand side [M (target - x); -C] gives the step to the point of the
// linearised constraints closest to the target, which is taken, restored onto
// the constraints and kept while it brings the poses closer (the pull). Both
// solve with Eigen's SparseLU, and SparseQR where that does not solve the
// system (the rank-deficient KKT systems redundant joints make; LUThenQR).
//
// Limits are an active set: a bound a solution violates gets a Chrono link
// holding it there (ChLinkMateDistanceZ for a slide, a constant
// ChLinkMotorRotationAngle for a turn) and the solve repeats.
//
// Diagnostics: Chrono's constraint Jacobian (ChSystem::
// GetConstraintJacobianMatrix), its rank by column-pivoted QR, and which rows
// depend on the rows before them in OndselSolver's order (its categories,
// then model order), by Gram-Schmidt -- the Reference's analysis on Chrono's
// equations.

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include <Eigen/Dense>

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionShapeBox.h>
#include <chrono/collision/ChCollisionShapeConvexHull.h>
#include <chrono/collision/ChCollisionShapeCylinder.h>
#include <chrono/collision/ChCollisionShapeMeshTriangle.h>
#include <chrono/collision/ChCollisionShapeSphere.h>
#include <chrono/collision/ChCollisionShapeTriangleMesh.h>
#include <chrono/collision/ChCollisionSystem.h>
#include <chrono/collision/bullet/BulletCollision/CollisionDispatch/cbtActivatingCollisionAlgorithm.h>
#include <chrono/collision/bullet/BulletCollision/CollisionDispatch/cbtCollisionDispatcher.h>
#include <chrono/collision/bullet/BulletCollision/CollisionDispatch/cbtCollisionObjectWrapper.h>
#include <chrono/collision/bullet/BulletCollision/CollisionDispatch/cbtManifoldResult.h>
#include <chrono/collision/bullet/BulletCollision/CollisionShapes/cbtBoxShape.h>
#include <chrono/collision/bullet/BulletCollision/CollisionShapes/cbtCEtriangleShape.h>
#include <chrono/collision/bullet/BulletCollision/CollisionShapes/cbtCompoundShape.h>
#include <chrono/collision/bullet/BulletCollision/CollisionShapes/cbtConvexHullShape.h>
#include <chrono/collision/bullet/BulletCollision/CollisionShapes/cbtConvexPolyhedron.h>
#include <chrono/collision/bullet/BulletCollision/NarrowPhaseCollision/cbtPolyhedralContactClipping.h>
#include <chrono/collision/bullet/ChCollisionModelBullet.h>
#include <chrono/collision/bullet/ChCollisionSystemBullet.h>
#include <chrono/core/ChFrame.h>
#include <chrono/functions/ChFunctionBase.h>
#include <chrono/geometry/ChTriangleMeshConnected.h>
#include <chrono/functions/ChFunctionConst.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChContactMaterialNSC.h>
#include <chrono/physics/ChLinkDistance.h>
#include <chrono/physics/ChLinkLockGear.h>
#include <chrono/physics/ChLinkMate.h>
#include <chrono/physics/ChLinkRSDA.h>
#include <chrono/physics/ChLinkTSDA.h>
#include <chrono/physics/ChLoadContainer.h>
#include <chrono/physics/ChLoadsBody.h>
#include <chrono/physics/ChLinkMotorLinearForce.h>
#include <chrono/physics/ChLinkMotorLinearPosition.h>
#include <chrono/physics/ChLinkMotorLinearSpeed.h>
#include <chrono/physics/ChLinkMotorRotationAngle.h>
#include <chrono/physics/ChLinkMotorRotationSpeed.h>
#include <chrono/physics/ChLinkMotorRotationTorque.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/physics/ChLinkBase.h>
#include <chrono/serialization/ChArchiveJSON.h>
#include <chrono/solver/ChConstraintNgeneric.h>
#include <chrono/solver/ChDirectSolverLS.h>
#include <chrono/solver/ChSystemDescriptor.h>
#include <chrono/timestepper/ChTimestepperHHT.h>

#include <Base/Console.h>

#include "ChronoBackend.h"
#include "JointEquations.h"
#include "ReferenceBackend.h"
#include "Units.h"

using namespace Assembly::Solver;
namespace ch = chrono;

namespace
{

constexpr double pi = std::numbers::pi;
constexpr double twoPi = 2.0 * std::numbers::pi;
constexpr double notANumber = std::numeric_limits<double>::quiet_NaN();

// The Reference's thresholds, so that the two analyses answer alike.
constexpr double conflictTolerance = 1.0e-6;
constexpr double rankTolerance = 1.0e-8;
constexpr double coaxialTolerance = 1.0e-9;
constexpr double rotationWeight = 0.25;
// The largest turn of a driven angle between two solves of a kinematic run.
constexpr double maxDriverStep = 0.25;

using MatX = Eigen::MatrixXd;
using VecX = Eigen::VectorXd;

// The KKT solves: Eigen's SparseLU, and SparseQR where that does not solve
// the system: a factorization that fails, or a solution off the constraint
// rows (Cq dx = -C, the rows after the variables'; relative to C, with a floor
// at the system's scale, for the steps along the constraints where C is all
// but zero) or off the variables' rows. Redundant joints make the system
// singular: an LU of it solves the
// constraint rows still, but its multipliers come to 1e19 and their rounding
// leaves the variables' rows hundreds off (a cam's roller follower), the step
// with them; conflicting joints make it inconsistent, which wants the least
// squares step. SparseQR alone took the
// rank-deficient systems, but on regular ones it returned steps far off the
// system: on a chain of 100 bodies |Cq dx + C| / |C| came to 3e-3, then
// 2e-2, then 1, where SparseLU gives 1e-13, and the restoration stopped short
// of the constraints.
class LUThenQR: public ch::ChDirectSolverLS
{
public:
    Type GetType() const override
    {
        return Type::SPARSE_LU;
    }
    bool Setup(ch::ChSystemDescriptor& sysd, bool analyze) override
    {
        constraints = sysd.CountActiveConstraints();
        return ChDirectSolverLS::Setup(sysd, analyze);
    }

private:
    bool FactorizeMatrix(bool analyze) override
    {
        // Nothing to move (a run of parts no joint ties to the ground): Eigen's
        // SparseLU divides by the matrix's size, and a 0 x 0 one crashed
        if (m_mat.rows() == 0) {
            factored = true;
            qrReady = false;
            return true;
        }
        if (analyze) {
            lu.analyzePattern(m_mat);
        }
        lu.factorize(m_mat);
        factored = lu.info() == Eigen::Success;
        qrReady = false;
        return true;
    }
    bool SolveSystem() override
    {
        if (m_mat.rows() == 0) {
            m_sol.resize(0);
            return true;
        }
        if (factored) {
            m_sol = lu.solve(m_rhs);
            if (lu.info() == Eigen::Success && m_sol.allFinite()) {
                const Eigen::Index n = std::min<Eigen::Index>(constraints, m_rhs.size());
                const ch::ChVectorDynamic<> residual = m_mat * m_sol - m_rhs;
                const Eigen::Index nv = m_rhs.size() - n;
                const double scale =
                    std::max({1.0, m_rhs.head(nv).norm(), m_sol.head(nv).norm()});
                if (residual.tail(n).norm() <= 1e-10 * m_rhs.tail(n).norm() + 1e-12 * scale
                    && residual.head(nv).norm() <= 1e-8 * scale) {
                    return true;
                }
            }
        }
        if (!qrReady) {
            qr.compute(m_mat);
            qrReady = true;
        }
        m_sol = qr.solve(m_rhs);
        return qr.info() == Eigen::Success && m_sol.allFinite();
    }
    void PrintErrorMessage() override
    {}

    Eigen::SparseLU<ch::ChSparseMatrix, Eigen::COLAMDOrdering<int>> lu;
    Eigen::SparseQR<ch::ChSparseMatrix, Eigen::COLAMDOrdering<int>> qr;
    unsigned int constraints = 0;
    bool factored = false;
    bool qrReady = false;
};

enum class Category : std::uint8_t
{
    Essential,
    Displacement,
    Perpendicular
};

enum class Owner : std::uint8_t
{
    Joint,
    Driver,
    Limit
};

const char* kindName(JointKind kind)
{
    switch (kind) {
        case JointKind::Fixed:
            return "Fixed";
        case JointKind::Revolute:
            return "Revolute";
        case JointKind::Cylindrical:
            return "Cylindrical";
        case JointKind::Prismatic:
            return "Prismatic";
        case JointKind::Spherical:
            return "Spherical";
        case JointKind::Planar:
            return "Planar";
        case JointKind::ParallelZ:
            return "ParallelZ";
        case JointKind::PerpendicularZ:
            return "PerpendicularZ";
        case JointKind::AngleZ:
            return "AngleZ";
        case JointKind::PointInPlane:
            return "PointInPlane";
        case JointKind::LineInPlane:
            return "LineInPlane";
        case JointKind::PointOnLine:
            return "PointOnLine";
        case JointKind::PointPointDistance:
            return "PointPointDistance";
        case JointKind::PointLineDistance:
            return "PointLineDistance";
        case JointKind::LineLineDistance:
            return "LineLineDistance";
        case JointKind::Gear:
            return "Gear";
        case JointKind::CoaxialGear:
            return "CoaxialGear";
        case JointKind::RackPinion:
            return "RackPinion";
        case JointKind::Screw:
            return "Screw";
        case JointKind::PointOnCurve:
            return "PointOnCurve";
        case JointKind::Count_:
            break;
    }
    return "?";
}

bool isRightAngle(double angle)
{
    return std::abs(std::abs(std::remainder(angle, twoPi)) - pi / 2) <= 1e-12;
}

// ---------------------------------------------------------------- conversions

ch::ChVector3d toCh(const Base::Vector3d& v)
{
    return {v.x, v.y, v.z};
}

ch::ChQuaterniond toCh(const Base::Rotation& r)
{
    double x, y, z, w;
    r.getValue(x, y, z, w);
    return {w, x, y, z};
}

ch::ChFramed toCh(const Base::Placement& p)
{
    return ch::ChFramed(toCh(p.getPosition()), toCh(p.getRotation()));
}

Base::Placement toPlacement(const ch::ChVector3d& pos, const ch::ChQuaterniond& q)
{
    return Base::Placement(Base::Vector3d(pos.x(), pos.y(), pos.z()),
                           Base::Rotation(q.e1(), q.e2(), q.e3(), q.e0()));
}

double wrapAngle(double a)
{
    a = std::remainder(a, twoPi);
    return a <= -pi ? a + twoPi : a;
}

// OndselSolver's first reading of an angle (the Reference's initialAngle).
double initialAngle(double s, double c)
{
    if (c > 0.0) {
        return std::atan2(s, c);
    }
    return s >= 0.0 ? std::atan2(s, c) : twoPi + std::atan2(s, c);
}

// The rotation vector taking q0 to q1 in q0's frame, angle in [0, pi].
ch::ChVector3d rotationFrom(const ch::ChQuaterniond& q0, const ch::ChQuaterniond& q1)
{
    ch::ChQuaterniond rel = q0.GetConjugate() * q1;
    if (rel.e0() < 0.0) {
        rel = -rel;
    }
    return rel.GetRotVec();
}

// ---------------------------------------------------------------- motions

// A motion's formula as a Chrono function of time: the neutral evaluator's
// value and its exact first and second derivatives, never Chrono's finite
// differences. The evaluator has no third derivative.
class FormulaChFunction: public ch::ChFunction
{
public:
    explicit FormulaChFunction(FormulaFunction function)
        : function(std::move(function))
    {}

    ch::ChFunction* Clone() const override
    {
        return new FormulaChFunction(*this);
    }
    double GetVal(double x) const override
    {
        return function(x).value;
    }
    double GetDer(double x) const override
    {
        return function(x).rate;
    }
    double GetDer2(double x) const override
    {
        return function(x).acceleration;
    }
    double GetDer3(double) const override
    {
        return notANumber;
    }

private:
    FormulaFunction function;
};

// An applied load (ForceKind::AppliedForce / AppliedTorque): Chrono's
// body-body wrench, on body B a force at frame I's origin or a torque, and
// its opposite on body A, each at where frame I was at the start. The
// magnitude is the formula at the time the load was last updated to, which
// the integrator does for each of its stages.
class AppliedLoad: public ch::ChLoadBodyBody
{
public:
    AppliedLoad(std::shared_ptr<ch::ChBody> reaction,
                std::shared_ptr<ch::ChBody> loaded,
                const ch::ChFramed& frame,
                FormulaFunction magnitude,
                const ch::ChVector3d& direction,
                bool followsFrame,
                bool torque)
        : ch::ChLoadBodyBody(std::move(reaction), std::move(loaded), frame)
        , magnitude(std::move(magnitude))
        , direction(direction)
        , followsFrame(followsFrame)
        , torque(torque)
    {}

    AppliedLoad* Clone() const override
    {
        return new AppliedLoad(*this);
    }
    void Update(double t, ch::UpdateFlags flags) override
    {
        time = t;
        ch::ChLoadBodyBody::Update(t, flags);
    }
    void ComputeBodyBodyForceTorque(const ch::ChFrameMoving<>& /*rel_AB*/,
                                    ch::ChVector3d& force,
                                    ch::ChVector3d& moment) override
    {
        // In the axes of B's application frame, frame I's: a direction in
        // the assembly's taken there at B's pose now (ComputeQ's frame_Bw)
        const ch::ChVector3d along =
            followsFrame ? direction : frame_Bw.TransformDirectionParentToLocal(direction);
        const ch::ChVector3d value = along * magnitude(time).value;
        force = torque ? ch::ChVector3d(0, 0, 0) : value;
        moment = torque ? value : ch::ChVector3d(0, 0, 0);
    }
    bool IsStiff() override
    {
        return false;
    }

private:
    FormulaFunction magnitude;
    ch::ChVector3d direction;
    bool followsFrame = false;
    bool torque = false;
    double time = 0.0;
};

// A bushing (ForceKind::Bushing): Chrono's generic one, A frame J's body, B
// frame I's. Chrono's force on B, in B's application frame, is K s' + C
// ds'/dt - the neutral force, s' the pose of A's frame in B's: minus the
// record's s, so the neutral force is minus the preload.
class Bushing: public ch::ChLoadBodyBodyBushingGeneric
{
public:
    using ch::ChLoadBodyBodyBushingGeneric::ChLoadBodyBodyBushingGeneric;

    Bushing* Clone() const override
    {
        return new Bushing(*this);
    }
    // Chrono's numerical jacobian leaves the load at its last perturbed
    // state: put back the one at the state
    void ComputeJacobian(ch::ChState* state_x, ch::ChStateDelta* state_w) override
    {
        ch::ChLoadBodyBodyBushingGeneric::ComputeJacobian(state_x, state_w);
        ComputeQ(state_x, state_w);
    }
    // The force on B and the torque about its application point now,
    // absolute
    ch::ChWrenchd wrenchNow()
    {
        ComputeQ(nullptr, nullptr);
        return {frame_Bw.TransformDirectionLocalToParent(locB_force),
                frame_Bw.TransformDirectionLocalToParent(locB_torque)};
    }
};

// ---------------------------------------------------------------- contact

// Adams' STEP: h0 below x0, h1 above x1, a cubic between them
double step(double x, double x0, double h0, double x1, double h1)
{
    if (x <= x0) {
        return h0;
    }
    if (x >= x1) {
        return h1;
    }
    const double u = (x - x0) / (x1 - x0);
    return h0 + (h1 - h0) * u * u * (3.0 - 2.0 * u);
}

// The contact between two bodies a contact set lets touch (ContactPair,
// docs/AssemblyDynamics.md sec 7, phase D2): the points collision detection
// found at the step's start, each a point on either body and the normal
// from A to B there, the normal kept for the step. At every state the
// integrator asks for -- each Newton iteration's -- the gap is the normal's
// component of B's point from A's, the penetration its opposite, and the
// impact law and friction give the force on B at its point, its opposite on
// A at A's. Chrono's numerical jacobian of that makes the contact implicit,
// so a part resting on another needs no step shorter than the contact's.
//
// The law is a patch's, not a point's: the points of one stretch of
// surface touching another (patchesOf()) push as one point would at the
// patch's depth, each its share by its own spring, so the deeper carry more
// and a tilt is pushed back. The patch's spring is its points' springs
// averaged, each weighted by itself: a face resting flat pushes as one
// point, however many its clipping or its mesh gave it, and a corner
// coming down beside a deep one adds as little as it presses. Per point, a
// box's four corners were four times as stiff as a sphere's one, and a
// mesh's ring of points as stiff as it was fine. Each patch's stiffness is
// the law's, or Hertz's from the shapes' materials (patchStiffness()).
class ContactLoad: public ch::ChLoadBodyBody
{
public:
    struct Point
    {
        ch::ChVector3d onA;     // in A's frame
        ch::ChVector3d onB;     // in B's frame
        ch::ChVector3d normal;  // absolute, from A to B
        // The shapes it is on, for patchesOf(), and the patch it is in
        const ch::ChCollisionShape* shapeA = nullptr;
        const ch::ChCollisionShape* shapeB = nullptr;
        std::size_t patch = 0;
    };

    ContactLoad(std::shared_ptr<ch::ChBody> a,
                std::shared_ptr<ch::ChBody> b,
                const ContactLaw& law,
                std::size_t set)
        : ch::ChLoadBodyBody(a, b, ch::ChFramed(b->GetPos(), b->GetRot()))
        , law(law)
        , set(set)
    {
        if (law.from == ContactLaw::Stiffness::Materials) {
            this->law.exponent = 1.5;
        }
    }

    ContactLoad* Clone() const override
    {
        return new ContactLoad(*this);
    }
    bool IsStiff() override
    {
        return true;
    }
    void ComputeBodyBodyForceTorque(const ch::ChFrameMoving<>&,
                                    ch::ChVector3d& force,
                                    ch::ChVector3d& moment) override
    {
        force = ch::VNULL;
        moment = ch::VNULL;
    }
    void ComputeQ(ch::ChState* state_x, ch::ChStateDelta* state_w) override
    {
        auto bodyA = std::dynamic_pointer_cast<ch::ChBody>(loadables[0]);
        auto bodyB = std::dynamic_pointer_cast<ch::ChBody>(loadables[1]);
        ch::ChFrameMoving<> a;
        ch::ChFrameMoving<> b;
        if (state_x) {
            a.SetCoordsys(state_x->segment(0, 7));
            b.SetCoordsys(state_x->segment(7, 7));
        }
        else {
            a.SetCoordsys(bodyA->GetCoordsys());
            b.SetCoordsys(bodyB->GetCoordsys());
        }
        if (state_w) {
            a.SetPosDt(state_w->segment(0, 3));
            a.SetAngVelLocal(state_w->segment(3, 3));
            b.SetPosDt(state_w->segment(6, 3));
            b.SetAngVelLocal(state_w->segment(9, 3));
        }
        else {
            a.SetCoordsysDt(bodyA->GetCoordsysDt());
            b.SetCoordsysDt(bodyB->GetCoordsysDt());
        }
        load_Q.setZero();
        onB = ch::VNULL;
        count = 0;
        // Each patch's springs and their squares: a point's share is the
        // patch's spring, sum(s^2) / sum(s), over the sum. The deepest
        // point's spring, the first try, has a kink where the deepest
        // changes, and the Newton iterations of a cube landing flat on a
        // mesh ran off on it, the cube thrown off at 3 km/s.
        depths.resize(points.size());
        squares.assign(patches, 0.0);
        sum.assign(patches, 0.0);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const Point& point = points[i];
            const ch::ChVector3d pA = a.TransformPointLocalToParent(point.onA);
            const ch::ChVector3d pB = b.TransformPointLocalToParent(point.onB);
            depths[i] = -ch::Vdot(point.normal, pB - pA);
            if (depths[i] > 0.0) {
                const double spring = std::pow(depths[i], law.exponent);
                squares[point.patch] += spring * spring;
                sum[point.patch] += spring;
            }
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            const Point& point = points[i];
            const double depth = depths[i];
            if (!(depth > 0.0)) {
                continue;
            }
            const ch::ChVector3d pA = a.TransformPointLocalToParent(point.onA);
            const ch::ChVector3d pB = b.TransformPointLocalToParent(point.onB);
            const ch::ChVector3d relative =
                b.PointSpeedLocalToParent(point.onB) - a.PointSpeedLocalToParent(point.onA);
            const double rate = -ch::Vdot(point.normal, relative);
            const double total = sum[point.patch];
            const double share = total > 0.0 ? squares[point.patch] / total / total : 1.0;
            double normal = share
                * (stiffness[point.patch] * std::pow(depth, law.exponent)
                   + step(depth, 0.0, 0.0, law.penetration, law.maxDamping) * rate);
            if (!(normal > 0.0)) {
                continue;
            }
            ch::ChVector3d f = point.normal * normal;
            const ch::ChVector3d sliding =
                relative - point.normal * ch::Vdot(point.normal, relative);
            const double speed = sliding.Length();
            if (speed > 0.0 && law.muStatic > 0.0) {
                const double mu = speed < law.vStiction
                    ? step(speed, 0.0, 0.0, law.vStiction, law.muStatic)
                    : step(speed, law.vStiction, law.muStatic, law.vFriction, law.muDynamic);
                f -= sliding * (mu * normal / speed);
            }
            // f on B at pB, -f on A at pA; the torques in each body's axes
            load_Q.segment(6, 3) += f.eigen();
            load_Q.segment(9, 3) += b.GetRot().RotateBack(ch::Vcross(pB - b.GetPos(), f)).eigen();
            load_Q.segment(0, 3) -= f.eigen();
            load_Q.segment(3, 3) -= a.GetRot().RotateBack(ch::Vcross(pA - a.GetPos(), f)).eigen();
            onB += f;
            ++count;
        }
    }
    // Chrono's numerical jacobian leaves the load at its last perturbed
    // state: put back the one at the state
    void ComputeJacobian(ch::ChState* state_x, ch::ChStateDelta* state_w) override
    {
        ch::ChLoadBodyBody::ComputeJacobian(state_x, state_w);
        ComputeQ(state_x, state_w);
    }

    ContactLaw law;
    std::size_t set = 0;  // the ContactPair's index
    std::vector<Point> points;
    std::size_t patches = 0;
    std::vector<double> stiffness;  // each patch's k
    // At the last state ComputeQ was given: the force on B, absolute, and
    // the points pressing
    ch::ChVector3d onB;
    int count = 0;

private:
    std::vector<double> depths, squares, sum;
};

// What a shape on a body meets its neighbours by: a mesh triangle its
// corners, any other shape itself
int keysOf(const ch::ChCollisionShape* shape, const void* keys[3])
{
    if (shape && shape->GetType() == ch::ChCollisionShape::Type::MESHTRIANGLE) {
        const auto* triangle = static_cast<const ch::ChCollisionShapeMeshTriangle*>(shape);
        keys[0] = triangle->V1;
        keys[1] = triangle->V2;
        keys[2] = triangle->V3;
        return 3;
    }
    keys[0] = shape;
    return 1;
}

// A load's points into patches, the stretches of surface touching: two
// points are in one where both bodies' shapes under them meet -- the same
// shape, or mesh triangles with a corner in common -- and their normals are
// within 45 deg. A ball in a V-groove and a box in a corner touch in two,
// and a table's legs on the floor one each. A mesh's triangles share their
// corners and edges, and each one's pair gives the points on them: each
// point is kept once, the first of its copies, in their patch. Each point's
// patch is set, and the load's count. Only points sharing a key on both
// bodies (keysOf()) are compared, those sorted together: comparing all of
// them, a torus's 1560 points before the copies went took 18 ms a query.
void patchesOf(ContactLoad& load)
{
    auto& points = load.points;
    std::vector<std::size_t> root(points.size());
    std::vector<bool> copy(points.size(), false);
    for (std::size_t i = 0; i < root.size(); ++i) {
        root[i] = i;
    }
    auto find = [&](std::size_t i) {
        while (root[i] != i) {
            root[i] = root[root[i]];
            i = root[i];
        }
        return i;
    };
    using Entry = std::tuple<const void*, const void*, std::size_t>;
    std::vector<Entry> entries;
    entries.reserve(points.size() * 3);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const void* keysA[3];
        const void* keysB[3];
        const int na = keysOf(points[i].shapeA, keysA);
        const int nb = keysOf(points[i].shapeB, keysB);
        for (int x = 0; x < na; ++x) {
            for (int y = 0; y < nb; ++y) {
                entries.emplace_back(keysA[x], keysB[y], i);
            }
        }
    }
    std::sort(entries.begin(), entries.end());
    const double within = std::cos(pi / 4.0);
    for (std::size_t first = 0; first < entries.size();) {
        std::size_t last = first + 1;
        while (last < entries.size() && std::get<0>(entries[last]) == std::get<0>(entries[first])
               && std::get<1>(entries[last]) == std::get<1>(entries[first])) {
            ++last;
        }
        for (std::size_t m = first + 1; m < last; ++m) {
            const std::size_t i = std::get<2>(entries[m]);
            for (std::size_t k = first; k < m; ++k) {
                const std::size_t j = std::get<2>(entries[k]);  // j < i
                const double turn = ch::Vdot(points[i].normal, points[j].normal);
                if (turn > 1.0 - 1e-9 && (points[i].onA - points[j].onA).Length2() < 1e-18) {
                    copy[i] = true;
                }
                if (turn > within && find(i) != find(j)) {
                    root[find(i)] = find(j);
                }
            }
        }
        first = last;
    }
    std::vector<std::size_t> number(points.size(), points.size());
    std::size_t patches = 0;
    std::vector<ContactLoad::Point> once;
    once.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!copy[i]) {
            std::size_t& patch = number[find(i)];
            if (patch == points.size()) {
                patch = patches++;
            }
            once.push_back(points[i]);
            once.back().patch = patch;
        }
    }
    points.swap(once);
    load.patches = patches;
}

// A box's or a hull's polyhedral features as the separating axis test reads
// them: the vertices, the faces' planes, and the edges with the faces either
// side and the cross product of those faces' normals (right x left), in the
// shape's frame. A mesh triangle's (satOfTriangle()) has its own face first,
// the one with a polygon, and its neighbours' after it.
struct SatPolyhedron
{
    struct Edge
    {
        int tail = 0;
        int head = 0;
        int left = 0;
        int right = 0;
    };
    std::vector<cbtVector3> vertices;
    std::vector<cbtVector3> normals;
    std::vector<cbtScalar> offsets;  // a face's plane: normal . x + offset = 0
    std::vector<Edge> edges;
    std::vector<cbtVector3> arcs;
    std::vector<std::vector<int>> polygons;  // each face's vertices, in order
    std::vector<std::vector<int>> facesAt;   // each vertex's faces
    bool triangle = false;                   // a mesh's, one-sided
    cbtScalar grown = 0;                     // a triangle's, along its normal
};

SatPolyhedron satOf(const cbtConvexPolyhedron& polyhedron)
{
    SatPolyhedron sat;
    for (int i = 0; i < polyhedron.m_vertices.size(); ++i) {
        sat.vertices.push_back(polyhedron.m_vertices[i]);
    }
    // Each edge by its ends, and the faces either side
    std::map<std::pair<int, int>, std::pair<int, int>> sides;
    sat.facesAt.resize(sat.vertices.size());
    for (int f = 0; f < polyhedron.m_faces.size(); ++f) {
        const cbtFace& face = polyhedron.m_faces[f];
        sat.normals.emplace_back(face.m_plane[0], face.m_plane[1], face.m_plane[2]);
        sat.offsets.push_back(face.m_plane[3]);
        const int n = face.m_indices.size();
        std::vector<int>& polygon = sat.polygons.emplace_back();
        for (int k = 0; k < n; ++k) {
            polygon.push_back(face.m_indices[k]);
            sat.facesAt[face.m_indices[k]].push_back(f);
        }
        for (int k = 0; k < n; ++k) {
            const int u = face.m_indices[k];
            const int v = face.m_indices[(k + 1) % n];
            auto [it, added] = sides.emplace(std::make_pair(std::min(u, v), std::max(u, v)),
                                             std::make_pair(f, -1));
            if (!added) {
                it->second.second = f;
            }
        }
    }
    for (const auto& [ends, faces] : sides) {
        if (faces.second >= 0) {
            sat.edges.push_back({ends.first, ends.second, faces.first, faces.second});
            sat.arcs.push_back(sat.normals[faces.second].cross(sat.normals[faces.first]));
        }
    }
    return sat;
}

// A mesh triangle's features (sec 7.1's rung 3a), in its body's frame: its
// face, and each edge between that face and the neighbour's across it, so
// that the edges' arcs are the mesh's, with the neighbour's face. A flat
// edge's arc is a point and a concave one's none: neither is an edge of the
// mesh's hull there, and neither makes an axis -- where an isolated
// triangle's would, a part sliding over a flat mesh bumped into the edges
// between its triangles. Grown along its normal by the envelope, as Bullet's
// margin grows it, and a neighbour's plane along that one's. No features for
// a triangle without an area.
SatPolyhedron satOfTriangle(const cbtCEtriangleShape& triangle, cbtScalar grow)
{
    SatPolyhedron sat;
    auto bt = [](const ch::ChVector3d& v) {
        return cbtVector3(static_cast<cbtScalar>(v.x()),
                          static_cast<cbtScalar>(v.y()),
                          static_cast<cbtScalar>(v.z()));
    };
    const cbtVector3 p[3] = {bt(*triangle.get_p1()),
                             bt(*triangle.get_p2()),
                             bt(*triangle.get_p3())};
    // Each edge's wing: the far corner of its neighbour, or of this triangle
    // where it has none
    const cbtVector3 wing[3] = {bt(*triangle.get_e1()),
                                bt(*triangle.get_e2()),
                                bt(*triangle.get_e3())};
    cbtVector3 n = (p[1] - p[0]).cross(p[2] - p[0]);
    if (!(n.length() > 0)) {
        return sat;
    }
    n.normalize();
    for (const auto& corner : p) {
        sat.vertices.push_back(corner + n * grow);
    }
    sat.normals.push_back(n);
    sat.offsets.push_back(-n.dot(sat.vertices[0]));
    sat.polygons.push_back({0, 1, 2});
    sat.facesAt.assign(3, {0});
    sat.triangle = true;
    sat.grown = grow;
    for (int k = 0; k < 3; ++k) {
        const cbtVector3& u = p[k];
        const cbtVector3& v = p[(k + 1) % 3];
        const cbtVector3& w = wing[k];
        // Convex: the wing behind the face
        if (!(n.dot(w - p[0]) < -1e-9 * (w - p[0]).length())) {
            continue;
        }
        // The neighbour, wound outward as this one: v, u, w
        cbtVector3 m = (u - v).cross(w - v);
        if (!(m.length() > 0)) {
            continue;
        }
        m.normalize();
        const int face = static_cast<int>(sat.normals.size());
        sat.normals.push_back(m);
        sat.offsets.push_back(-(m.dot(u) + grow));
        sat.edges.push_back({k, (k + 1) % 3, 0, face});
        sat.arcs.push_back(m.cross(n));
    }
    return sat;
}

// Whether an edge between faces of normals a and b (bxa = b x a) and one
// between faces of normals c and d (dxc = d x c) make a face of the two
// shapes' Minkowski difference: the arcs a-b and -c to -d cross on the Gauss
// map. Only such a pair's cross product can separate them (Gregorius, "The
// Separating Axis Test between Convex Polyhedra", GDC 2013); most pairs are
// passed over after two products.
bool minkowskiFace(const cbtVector3& a,
                   const cbtVector3& b,
                   const cbtVector3& bxa,
                   const cbtVector3& c,
                   const cbtVector3& d,
                   const cbtVector3& dxc)
{
    const cbtScalar cba = -c.dot(bxa);
    const cbtScalar dba = -d.dot(bxa);
    if (!(cba * dba < 0)) {
        return false;
    }
    const cbtScalar adc = a.dot(dxc);
    const cbtScalar bdc = b.dot(dxc);
    return adc * bdc < 0 && cba * bdc > 0;
}

// The axis separating two polyhedra most, in A's frame from A to B: a face
// of A's (i), of B's (j), or an edge of each (i, j); or a direction that
// proves a mesh triangle apart from the other (satApart())
struct SatAxis
{
    enum Kind
    {
        None,
        FaceA,
        FaceB,
        Edges,
        Apart
    };
    Kind kind = None;
    int i = -1;
    int j = -1;
    cbtScalar separation = -std::numeric_limits<cbtScalar>::max();
    cbtVector3 normal {0, 0, 1};
};

// B's features where A's frame has them
struct SatPlaced
{
    std::vector<cbtVector3> vertices;
    std::vector<cbtVector3> normals;
    std::vector<cbtScalar> offsets;
    std::vector<cbtVector3> arcs;
};

SatPlaced placed(const SatPolyhedron& b, const cbtTransform& bInA)
{
    SatPlaced out;
    out.vertices.reserve(b.vertices.size());
    for (const auto& v : b.vertices) {
        out.vertices.push_back(bInA(v));
    }
    out.normals.reserve(b.normals.size());
    out.offsets.reserve(b.normals.size());
    for (std::size_t f = 0; f < b.normals.size(); ++f) {
        const cbtVector3 n = bInA.getBasis() * b.normals[f];
        out.normals.push_back(n);
        out.offsets.push_back(b.offsets[f] - n.dot(bInA.getOrigin()));
    }
    out.arcs.reserve(b.arcs.size());
    for (const auto& arc : b.arcs) {
        out.arcs.push_back(bInA.getBasis() * arc);
    }
    return out;
}

// One axis's separation in a search, its normal from A to B. An edge pair's
// by its edges alone: they are a Minkowski face, which the search tested. A
// kept axis's pair may no longer be one, and its edges' gap then says
// nothing of the shapes': satRemeasure() measures the shapes' extents.
void satMeasure(const SatPolyhedron& a, const SatPolyhedron& b, const SatPlaced& bp, SatAxis& axis)
{
    switch (axis.kind) {
        case SatAxis::FaceA: {
            const cbtVector3& n = a.normals[axis.i];
            cbtScalar least = std::numeric_limits<cbtScalar>::max();
            for (const auto& v : bp.vertices) {
                least = std::min(least, n.dot(v) + a.offsets[axis.i]);
            }
            axis.separation = least;
            axis.normal = n;
            break;
        }
        case SatAxis::FaceB: {
            const cbtVector3& n = bp.normals[axis.j];
            cbtScalar least = std::numeric_limits<cbtScalar>::max();
            for (const auto& v : a.vertices) {
                least = std::min(least, n.dot(v) + bp.offsets[axis.j]);
            }
            axis.separation = least;
            axis.normal = -n;
            break;
        }
        case SatAxis::Edges: {
            const SatPolyhedron::Edge& ea = a.edges[axis.i];
            const SatPolyhedron::Edge& eb = b.edges[axis.j];
            const cbtVector3& p = a.vertices[ea.tail];
            const cbtVector3 e1 = a.vertices[ea.head] - p;
            const cbtVector3 e2 = bp.vertices[eb.head] - bp.vertices[eb.tail];
            cbtVector3 n = e1.cross(e2);
            const cbtScalar length = n.length();
            // Parallel edges: their faces' axes cover them
            if (!(length > 0.005 * std::sqrt(e1.length2() * e2.length2()))) {
                axis.separation = -std::numeric_limits<cbtScalar>::max();
                break;
            }
            n /= length;
            // Out of A: within the edge's arc, so along the faces' normals --
            // a triangle has no inside to point away from
            if (n.dot(a.normals[ea.left] + a.normals[ea.right]) < 0) {
                n = -n;
            }
            axis.separation = n.dot(bp.vertices[eb.tail] - p);
            axis.normal = n;
            break;
        }
        case SatAxis::Apart:
        case SatAxis::None:
            break;
    }
}

// The vertex farthest along dir
int supportOf(const std::vector<cbtVector3>& vertices, const cbtVector3& dir)
{
    int best = 0;
    cbtScalar most = -std::numeric_limits<cbtScalar>::max();
    for (int i = 0; i < static_cast<int>(vertices.size()); ++i) {
        const cbtScalar d = dir.dot(vertices[i]);
        if (d > most) {
            most = d;
            best = i;
        }
    }
    return best;
}

// A kept axis's separation now, B where bInA has it, without placing all of
// B's features in A's frame as a search does: one direction turned into
// B's frame, then one pass over the vertices. The shapes' extents along it,
// so it holds for any axis (satMeasure()'s projected).
void satRemeasure(const SatPolyhedron& a,
                  const SatPolyhedron& b,
                  const cbtTransform& bInA,
                  SatAxis& axis)
{
    const cbtMatrix3x3& turn = bInA.getBasis();
    const cbtVector3& shift = bInA.getOrigin();
    // The least of n . x over B, n in A's frame
    auto leastB = [&](const cbtVector3& n) {
        const cbtVector3 inB = turn.transpose() * n;
        return inB.dot(b.vertices[supportOf(b.vertices, -inB)]) + n.dot(shift);
    };
    switch (axis.kind) {
        case SatAxis::FaceA: {
            const cbtVector3& n = a.normals[axis.i];
            axis.separation = leastB(n) + a.offsets[axis.i];
            axis.normal = n;
            break;
        }
        case SatAxis::FaceB: {
            const cbtVector3 n = turn * b.normals[axis.j];
            const cbtScalar offset = b.offsets[axis.j] - n.dot(shift);
            axis.separation = n.dot(a.vertices[supportOf(a.vertices, -n)]) + offset;
            axis.normal = -n;
            break;
        }
        case SatAxis::Edges: {
            const SatPolyhedron::Edge& ea = a.edges[axis.i];
            const SatPolyhedron::Edge& eb = b.edges[axis.j];
            const cbtVector3& p = a.vertices[ea.tail];
            const cbtVector3 e1 = a.vertices[ea.head] - p;
            const cbtVector3 e2 = turn * (b.vertices[eb.head] - b.vertices[eb.tail]);
            cbtVector3 n = e1.cross(e2);
            const cbtScalar length = n.length();
            if (!(length > 0.005 * std::sqrt(e1.length2() * e2.length2()))) {
                axis.separation = -std::numeric_limits<cbtScalar>::max();
                break;
            }
            n /= length;
            // Out of A: within the edge's arc, so along the faces' normals --
            // a triangle has no inside to point away from
            if (n.dot(a.normals[ea.left] + a.normals[ea.right]) < 0) {
                n = -n;
            }
            axis.separation = leastB(n) - n.dot(a.vertices[supportOf(a.vertices, n)]);
            axis.normal = n;
            break;
        }
        case SatAxis::Apart: {
            const cbtVector3& d = axis.normal;
            axis.separation = std::max(leastB(d) - d.dot(a.vertices[supportOf(a.vertices, d)]),
                                       leastB(-d) + d.dot(a.vertices[supportOf(a.vertices, -d)]));
            break;
        }
        case SatAxis::None:
            break;
    }
}

// A point of a pair's clipping, as Bullet's result takes one: the normal on
// B, pointing to A, the point on B, and how far apart (negative, how deep)
struct ClipPoint
{
    cbtVector3 normalOnB;
    cbtVector3 onB;
    cbtScalar depth;
};

// The pair's points along the axis, as Bullet's clipping gives them: the
// reference face, the one the axis is the normal of (an edge
// pair's: the face of either most along it, at the support vertex), and the
// incident face of the other, the one most against it among the faces at the
// other's support vertex; the incident face clipped to the reference face's
// sides, each point kept within threshold of its plane. Bullet's own looks
// for both faces among all of them, 11558 of an ellipsoid's on each query.
void clipAlong(const SatPolyhedron& a,
               const SatPolyhedron& b,
               const cbtTransform& ta,
               const cbtTransform& tb,
               const SatAxis& axis,
               cbtScalar threshold,
               std::vector<ClipPoint>& out)
{
    const cbtVector3 n = ta.getBasis() * axis.normal;  // from A to B
    // An edge of a mesh triangle's on an edge of the other's: the edges'
    // closest points. A triangle's one face is no reference for it, up to a
    // right angle off the axis: its normal taken for the point's pushed a
    // part sideways, as deep as the face was wide.
    if (axis.kind == SatAxis::Edges && (a.triangle || b.triangle)) {
        const SatPolyhedron::Edge& ea = a.edges[axis.i];
        const SatPolyhedron::Edge& eb = b.edges[axis.j];
        // A triangle's edge grown toward the other, as a ball grows it, not
        // along its own normal
        const cbtVector3 shiftA =
            a.triangle ? n * a.grown - ta.getBasis() * a.normals[0] * a.grown : cbtVector3(0, 0, 0);
        const cbtVector3 shiftB = b.triangle ? -n * b.grown - tb.getBasis() * b.normals[0] * b.grown
                                             : cbtVector3(0, 0, 0);
        const cbtVector3 p1 = ta(a.vertices[ea.tail]) + shiftA;
        const cbtVector3 d1 = ta(a.vertices[ea.head]) - ta(a.vertices[ea.tail]);
        const cbtVector3 p2 = tb(b.vertices[eb.tail]) + shiftB;
        const cbtVector3 d2 = tb(b.vertices[eb.head]) - tb(b.vertices[eb.tail]);
        // Each one's parameter along its edge where the lines come closest:
        // outside either edge they touch elsewhere, a face's pair's -- an
        // axis kept from an earlier query put a part's corner on an edge
        // 20 mm off
        const cbtVector3 r = p1 - p2;
        const cbtScalar aa = d1.dot(d1);
        const cbtScalar ee = d2.dot(d2);
        const cbtScalar f = d2.dot(r);
        const cbtScalar c = d1.dot(r);
        const cbtScalar bb = d1.dot(d2);
        const cbtScalar denom = aa * ee - bb * bb;
        if (!(aa > 0) || !(ee > 0) || !(denom > 0)) {
            return;
        }
        const cbtScalar s1 = (bb * f - c * ee) / denom;
        const cbtScalar s2 = (bb * s1 + f) / ee;
        if (!(s1 >= 0 && s1 <= 1 && s2 >= 0 && s2 <= 1)) {
            return;
        }
        const cbtVector3 onA = p1 + d1 * s1;
        const cbtVector3 onB = p2 + d2 * s2;
        const cbtScalar depth = n.dot(onB - onA);
        if (depth <= threshold) {
            out.push_back({-n, onB, depth});
        }
        return;
    }
    // The face of a shape most along dir among those at its support vertex
    // that way, dir in the world
    auto faceAt = [](const SatPolyhedron& shape, const cbtTransform& at, const cbtVector3& dir) {
        const cbtVector3 local = at.getBasis().transpose() * dir;
        int best = -1;
        cbtScalar most = -std::numeric_limits<cbtScalar>::max();
        for (int f : shape.facesAt[supportOf(shape.vertices, local)]) {
            const cbtScalar d = shape.normals[f].dot(local);
            if (d > most) {
                most = d;
                best = f;
            }
        }
        return std::make_pair(best, most);
    };
    bool referenceA = axis.kind == SatAxis::FaceA;
    int reference = axis.i;
    if (axis.kind == SatAxis::FaceB) {
        reference = axis.j;
    }
    else if (axis.kind == SatAxis::Edges) {
        const auto [fa, da] = faceAt(a, ta, n);
        const auto [fb, db] = faceAt(b, tb, -n);
        referenceA = da >= db;
        reference = referenceA ? fa : fb;
    }
    const SatPolyhedron& ref = referenceA ? a : b;
    const cbtTransform& refAt = referenceA ? ta : tb;
    const SatPolyhedron& inc = referenceA ? b : a;
    const cbtTransform& incAt = referenceA ? tb : ta;
    // A mesh triangle's neighbour's face: its points are that one's pair's
    if (reference < 0 || reference >= static_cast<int>(ref.polygons.size())) {
        return;
    }
    const cbtVector3 refNormal = refAt.getBasis() * ref.normals[reference];
    const int incident = faceAt(inc, incAt, -refNormal).first;
    if (incident < 0) {
        return;
    }
    std::vector<cbtVector3> sides;
    for (int v : ref.polygons[reference]) {
        sides.push_back(refAt(ref.vertices[v]));
    }
    // An incident triangle grown toward the reference face, as a ball grows
    // it: grown along its own normal, a wall's top edge under a part's face
    // stood an envelope below the top beside it, and the part sank that far
    const cbtVector3 shift = inc.triangle
        ? -refNormal * inc.grown - incAt.getBasis() * inc.normals[0] * inc.grown
        : cbtVector3(0, 0, 0);
    std::vector<cbtVector3> points;
    for (int v : inc.polygons[incident]) {
        points.push_back(incAt(inc.vertices[v]) + shift);
    }
    cbtVector3 centre(0, 0, 0);
    for (const auto& v : sides) {
        centre += v;
    }
    centre /= static_cast<cbtScalar>(sides.size());
    std::vector<cbtVector3> kept;
    for (std::size_t k = 0; k < sides.size() && !points.empty(); ++k) {
        const cbtVector3& p0 = sides[k];
        const cbtVector3& p1 = sides[(k + 1) % sides.size()];
        cbtVector3 out = (p1 - p0).cross(refNormal);
        if (out.dot(centre - p0) > 0) {
            out = -out;
        }
        kept.clear();
        for (std::size_t i = 0; i < points.size(); ++i) {
            const cbtVector3& cur = points[i];
            const cbtVector3& prev = points[(i + points.size() - 1) % points.size()];
            const cbtScalar dc = out.dot(cur - p0);
            const cbtScalar dp = out.dot(prev - p0);
            if ((dc <= 0) != (dp <= 0)) {
                kept.push_back(prev + (cur - prev) * (dp / (dp - dc)));
            }
            if (dc <= 0) {
                kept.push_back(cur);
            }
        }
        points.swap(kept);
    }
    for (const auto& p : points) {
        const cbtScalar depth = refNormal.dot(p - sides[0]);
        if (depth > threshold) {
            continue;
        }
        // Bullet's normal on B points to A, its point is on B
        if (referenceA) {
            out.push_back({-refNormal, p, depth});
        }
        else {
            out.push_back({refNormal, p - refNormal * depth, depth});
        }
    }
}

// Whether a pair with a mesh triangle is apart by more than threshold, by
// the plain test of two convex shapes: the triangle as the flat thing it is,
// each face's normal and the cross product of each edge of one with each of
// the other, its flat edges too, and both shapes' extents along them. The
// mesh's axes satSearch() takes a contact normal from are not enough to tell:
// a channel's wall and a part's side, 7 mm apart, the one axis between them
// the cross product of their triangles' diagonals, were taken to overlap.
SatAxis
satApart(const SatPolyhedron& a, const SatPolyhedron& b, const SatPlaced& bp, cbtScalar threshold)
{
    SatAxis apart;
    apart.kind = SatAxis::Apart;
    // Either way along d
    auto offer = [&](cbtVector3 d) {
        const cbtScalar length = d.length();
        if (!(length > 0)) {
            return false;
        }
        d /= length;
        cbtScalar lowA = std::numeric_limits<cbtScalar>::max();
        cbtScalar highA = -lowA;
        cbtScalar lowB = lowA;
        cbtScalar highB = -lowA;
        for (const auto& v : a.vertices) {
            lowA = std::min(lowA, d.dot(v));
            highA = std::max(highA, d.dot(v));
        }
        for (const auto& v : bp.vertices) {
            lowB = std::min(lowB, d.dot(v));
            highB = std::max(highB, d.dot(v));
        }
        const cbtScalar gap = std::max(lowB - highA, lowA - highB);
        if (gap > apart.separation) {
            apart.separation = gap;
            apart.normal = d;
        }
        return gap > threshold;
    };
    auto ends = [](const SatPolyhedron& shape, const std::vector<cbtVector3>& vertices) {
        std::vector<cbtVector3> out;
        if (shape.triangle) {
            for (int k = 0; k < 3; ++k) {
                out.push_back(vertices[(k + 1) % 3] - vertices[k]);
            }
        }
        else {
            for (const auto& e : shape.edges) {
                out.push_back(vertices[e.head] - vertices[e.tail]);
            }
        }
        return out;
    };
    for (const auto& n : a.normals) {
        if (offer(n)) {
            return apart;
        }
    }
    for (const auto& n : bp.normals) {
        if (offer(n)) {
            return apart;
        }
    }
    const std::vector<cbtVector3> edgesA = ends(a, a.vertices);
    const std::vector<cbtVector3> edgesB = ends(b, bp.vertices);
    // A triangle's sides, square to its edges in its plane: two triangles in
    // one plane are apart along one of those or not at all
    if (a.triangle) {
        for (const auto& e : edgesA) {
            if (offer(a.normals[0].cross(e))) {
                return apart;
            }
        }
    }
    if (b.triangle) {
        for (const auto& e : edgesB) {
            if (offer(bp.normals[0].cross(e))) {
                return apart;
            }
        }
    }
    for (const auto& ea : edgesA) {
        for (const auto& eb : edgesB) {
            const cbtVector3 d = ea.cross(eb);
            // Parallel edges: the faces' normals cover them
            if (d.length2() > 2.5e-5 * ea.length2() * eb.length2() && offer(d)) {
                return apart;
            }
        }
    }
    return apart;
}

// The full search: every face of each, and the edge pairs that make
// Minkowski faces. An edge pair's axis is taken over a face's only when it
// separates them clearly more, so parallel faces resting on each other keep
// a face's normal.
SatAxis
satSearch(const SatPolyhedron& a, const SatPolyhedron& b, const SatPlaced& bp, cbtScalar threshold)
{
    // A mesh triangle stands for the solid behind it: an axis along which
    // that solid reaches on without end -- out of its back -- is no contact
    // normal of the pair's. Any axis still proves the triangles apart. Two
    // triangles facing the same way, side by side in one plane, touch only
    // along the edge between them: a part resting flush with the side of
    // another, the two faces taken to overlap by the part's width along the
    // plane, threw it off. Their neighbours' faces give the axis they touch
    // along, and the neighbours their points.
    constexpr cbtScalar slack = 1e-3;
    auto valid = [&](const SatAxis& axis) {
        return (!a.triangle || axis.normal.dot(a.normals[0]) >= -slack)
            && (!b.triangle || axis.normal.dot(bp.normals[0]) <= slack);
    };
    SatAxis best;   // of all
    SatAxis touch;  // of the valid ones
    auto offer = [&](const SatAxis& axis, cbtScalar bias) {
        if (axis.separation > best.separation + bias) {
            best = axis;
        }
        if (axis.separation > touch.separation + bias && valid(axis)) {
            touch = axis;
        }
    };
    for (int i = 0; i < static_cast<int>(a.normals.size()); ++i) {
        SatAxis axis;
        axis.kind = SatAxis::FaceA;
        axis.i = i;
        satMeasure(a, b, bp, axis);
        offer(axis, 0);
    }
    for (int j = 0; j < static_cast<int>(b.normals.size()); ++j) {
        SatAxis axis;
        axis.kind = SatAxis::FaceB;
        axis.j = j;
        satMeasure(a, b, bp, axis);
        offer(axis, 0);
    }
    constexpr cbtScalar edgeBias = 1e-7;  // m
    for (int i = 0; i < static_cast<int>(a.edges.size()); ++i) {
        const SatPolyhedron::Edge& ea = a.edges[i];
        for (int j = 0; j < static_cast<int>(b.edges.size()); ++j) {
            const SatPolyhedron::Edge& eb = b.edges[j];
            if (!minkowskiFace(a.normals[ea.left],
                               a.normals[ea.right],
                               a.arcs[i],
                               bp.normals[eb.left],
                               bp.normals[eb.right],
                               bp.arcs[j])) {
                continue;
            }
            SatAxis axis;
            axis.kind = SatAxis::Edges;
            axis.i = i;
            axis.j = j;
            satMeasure(a, b, bp, axis);
            offer(axis, edgeBias);
        }
    }
    if (best.separation > threshold) {
        return best;
    }
    if (a.triangle || b.triangle) {
        SatAxis apart = satApart(a, b, bp, threshold);
        if (apart.separation > threshold) {
            return apart;
        }
    }
    // Touching the least out of a triangle's back, the touch is its
    // neighbours': no points, unless a valid axis touches as little. A side
    // of a cube landing on a plate, tilted past the slack, its triangles
    // meeting the base at a corner only, touched least along the plate's top
    // face, out of their backs, and the valid axis left was the plate's
    // bottom face, 20 mm deep: the cube was driven through the plate.
    if (!valid(best) && touch.separation < best.separation - 1e-6) {
        return SatAxis();
    }
    return touch;
}

// Contact between two boxes or hulls, or a box and a hull, in place of
// Bullet's convex pair's, and between a mesh's triangle and any of those or
// another mesh's: the separating axis by satSearch(), then clipping face
// against face along it (clipAlong()) into the pair's manifolds.
// Bullet's own SAT projects both shapes on every pair of edge directions: two
// 256-sided prisms, 129 directions each, one dropped across the other, took
// 55 s over a tenth of a second that GJK's axis did in 1.7 s, and GJK's axis
// put them a millimetre wrong. The axis is kept for the next query and
// measured again there: still separating them beyond the contact threshold,
// it proves them apart however they moved; otherwise it is taken while the
// pair has moved less than 10 um and 1e-3 rad apart since its search -- a
// resting or impacting pair's substeps are microseconds long. The prisms
// take 1.4 s so. A full search of theirs is 1.5 ms (768 edges each, 33000
// pairs of them Minkowski faces, cap rim against cap rim); searching at all
// 108000 of the run's queries took 180 s.
class PolyhedralAlgorithm: public cbtActivatingCollisionAlgorithm
{
public:
    // By a box's or a hull's polyhedron, a mesh triangle's shape
    using Polyhedra = std::map<const void*, SatPolyhedron>;

    static const void* keyOf(const cbtCollisionShape* shape)
    {
        if (shape->getShapeType() == CH_TRIANGLE_SHAPE_PROXYTYPE) {
            return shape;
        }
        return static_cast<const cbtPolyhedralConvexShape*>(shape)->getConvexPolyhedron();
    }

    PolyhedralAlgorithm(const cbtCollisionAlgorithmConstructionInfo& ci,
                        const cbtCollisionObjectWrapper* body0Wrap,
                        const cbtCollisionObjectWrapper* body1Wrap,
                        const Polyhedra* polyhedra)
        : cbtActivatingCollisionAlgorithm(ci, body0Wrap, body1Wrap)
        , manifold(ci.m_manifold)
        , polyhedra(polyhedra)
    {}
    ~PolyhedralAlgorithm() override
    {
        if (ownManifold && manifold) {
            m_dispatcher->releaseManifold(manifold);
        }
        for (cbtPersistentManifold* more : extra) {
            m_dispatcher->releaseManifold(more);
        }
    }

    void processCollision(const cbtCollisionObjectWrapper* body0Wrap,
                          const cbtCollisionObjectWrapper* body1Wrap,
                          const cbtDispatcherInfo&,
                          cbtManifoldResult* resultOut) override
    {
        if (!manifold) {
            manifold = m_dispatcher->getNewManifold(body0Wrap->getCollisionObject(),
                                                    body1Wrap->getCollisionObject());
            ownManifold = true;
        }
        resultOut->setPersistentManifold(manifold);
        const void* keyA = keyOf(body0Wrap->getCollisionShape());
        const void* keyB = keyOf(body1Wrap->getCollisionShape());
        auto a = keyA ? polyhedra->find(keyA) : polyhedra->end();
        auto b = keyB ? polyhedra->find(keyB) : polyhedra->end();
        if (a == polyhedra->end() || b == polyhedra->end() || a->second.normals.empty()
            || b->second.normals.empty()) {
            if (ownManifold) {
                resultOut->refreshContactPoints();
            }
            return;
        }
        const cbtTransform& ta = body0Wrap->getWorldTransform();
        const cbtTransform& tb = body1Wrap->getWorldTransform();
        const cbtTransform bInA = ta.inverse() * tb;
        const cbtScalar threshold =
            manifold->getContactBreakingThreshold() + resultOut->m_closestPointDistanceThreshold;
        bool kept = false;
        if (axis.kind != SatAxis::None) {
            satRemeasure(a->second, b->second, bInA, axis);
            const cbtScalar moved = (bInA.getOrigin() - searchedAt.getOrigin()).length();
            const cbtQuaternion turn = searchedAt.getRotation().inverse() * bInA.getRotation();
            const cbtScalar angle = 2 * std::acos(std::min<cbtScalar>(1, std::abs(turn.getW())));
            kept = axis.separation > threshold
                || (moved < 1e-5 && angle < 1e-3 && axis.kind != SatAxis::Apart
                    && axis.separation > -std::numeric_limits<cbtScalar>::max());
        }
        if (!kept) {
            axis = satSearch(a->second, b->second, placed(b->second, bInA), threshold);
            searchedAt = bInA;
        }
        clipped.clear();
        if (axis.kind != SatAxis::None && axis.separation <= threshold) {
            clipAlong(a->second, b->second, ta, tb, axis, threshold, clipped);
        }
        if (!ownManifold) {
            for (const ClipPoint& point : clipped) {
                resultOut->addContactPoint(point.normalOnB, point.onB, point.depth);
            }
            return;
        }
        handOver(resultOut);
    }

    cbtScalar calculateTimeOfImpact(cbtCollisionObject*,
                                    cbtCollisionObject*,
                                    const cbtDispatcherInfo&,
                                    cbtManifoldResult*) override
    {
        return 1;
    }

    void getAllContactManifolds(cbtManifoldArray& manifoldArray) override
    {
        if (manifold && ownManifold) {
            manifoldArray.push_back(manifold);
            for (cbtPersistentManifold* more : extra) {
                manifoldArray.push_back(more);
            }
        }
    }

    struct CreateFunc: cbtCollisionAlgorithmCreateFunc
    {
        const Polyhedra* polyhedra = nullptr;
        cbtCollisionAlgorithm*
        CreateCollisionAlgorithm(cbtCollisionAlgorithmConstructionInfo& ci,
                                 const cbtCollisionObjectWrapper* body0Wrap,
                                 const cbtCollisionObjectWrapper* body1Wrap) override
        {
            // Not from the dispatcher's pool: its elements are sized for
            // Bullet's own algorithms (184 bytes, this one 240), and it hands
            // one out whatever size is asked (the check is an assert) -- a
            // mesh's thousands of pairs overran them. The dispatcher frees
            // what is not the pool's with cbtAlignedFree.
            void* memory = cbtAlignedAlloc(sizeof(PolyhedralAlgorithm), 16);
            return new (memory) PolyhedralAlgorithm(ci, body0Wrap, body1Wrap, polyhedra);
        }
    };

private:
    // The result's shape identifiers, which Chrono's report takes a
    // compound's child by: protected, named through a class of its own
    struct Identifiers: cbtManifoldResult
    {
        static constexpr int cbtManifoldResult::*partId0 = &Identifiers::m_partId0;
        static constexpr int cbtManifoldResult::*partId1 = &Identifiers::m_partId1;
        static constexpr int cbtManifoldResult::*index0 = &Identifiers::m_index0;
        static constexpr int cbtManifoldResult::*index1 = &Identifiers::m_index1;
    };

    // The clipping's points, all of them: a manifold holds four, a fifth
    // replacing one of them, and a new point near one, within the breaking
    // threshold, replaces that. A hexagonal prism resting on a plate kept
    // four of its corners, off its centre, and rested tilted on them (2e-3
    // rad under a soft law). Four to a manifold, then, the pair's own and as
    // many more as it takes, each emptied first: the clipping is the whole
    // contact, nothing to keep from the last query. As Bullet's result adds
    // one otherwise.
    void handOver(cbtManifoldResult* resultOut)
    {
        const cbtCollisionObject* body0 = resultOut->getBody0Wrap()->getCollisionObject();
        const cbtCollisionObject* body1 = resultOut->getBody1Wrap()->getCollisionObject();
        const bool swapped = manifold->getBody0() != body0;
        const int partId0 = resultOut->*Identifiers::partId0;
        const int partId1 = resultOut->*Identifiers::partId1;
        const int index0 = resultOut->*Identifiers::index0;
        const int index1 = resultOut->*Identifiers::index1;
        manifold->clearManifold();
        for (cbtPersistentManifold* more : extra) {
            more->clearManifold();
        }
        std::size_t added = 0;
        for (const ClipPoint& c : clipped) {
            if (c.depth > manifold->getContactBreakingThreshold()) {
                continue;
            }
            const std::size_t which = added++ / MANIFOLD_CACHE_SIZE;
            if (which > extra.size()) {
                extra.push_back(
                    m_dispatcher->getNewManifold(manifold->getBody0(), manifold->getBody1()));
            }
            cbtPersistentManifold* into = which ? extra[which - 1] : manifold;
            const cbtVector3 pointA = c.onB + c.normalOnB * c.depth;
            const cbtVector3 localA =
                (swapped ? body1 : body0)->getWorldTransform().invXform(pointA);
            const cbtVector3 localB =
                (swapped ? body0 : body1)->getWorldTransform().invXform(c.onB);
            cbtManifoldPoint point(localA, localB, c.normalOnB, c.depth);
            point.m_positionWorldOnA = pointA;
            point.m_positionWorldOnB = c.onB;
            point.m_partId0 = swapped ? partId1 : partId0;
            point.m_partId1 = swapped ? partId0 : partId1;
            point.m_index0 = swapped ? index1 : index0;
            point.m_index1 = swapped ? index0 : index1;
            into->addManifoldPoint(point);
        }
    }

    cbtPersistentManifold* manifold = nullptr;
    bool ownManifold = false;
    std::vector<cbtPersistentManifold*> extra;
    std::vector<ClipPoint> clipped;
    const Polyhedra* polyhedra = nullptr;
    SatAxis axis;
    cbtTransform searchedAt;
};

// Bullet's collision detection, with several points per pair at once where
// a face rests on a face: a convex pair's GJK gives one point a query, and
// its manifold gathers more only over the following steps, so a hexagonal
// prism landing flat first touched at one corner -- a spin of 100 rad/s from
// a fall of a millimetre. A pair of boxes or hulls is clipped face against
// face (addPolyhedralFeatures(), PolyhedralAlgorithm), and so is a mesh's
// triangle against a box, a hull or another mesh's triangle, its axes the
// mesh's (satOfTriangle()); a cylinder's cap or side on a box's or a hull's
// face is clipped the same way (cylinderPoints()). Not Bullet's perturbation
// of a pair's orientation for more points: they were up to 20 um off on a
// flat face, and a ball's bounce on a plate lost half a percent of its
// height to them.
class ContactCollisionSystem: public ch::ChCollisionSystemBullet
{
public:
    ContactCollisionSystem()
    {
        polyhedral.polyhedra = &polyhedra;
        bt_dispatcher->registerCollisionCreateFunc(CONVEX_HULL_SHAPE_PROXYTYPE,
                                                   CONVEX_HULL_SHAPE_PROXYTYPE,
                                                   &polyhedral);
        bt_dispatcher->registerCollisionCreateFunc(CONVEX_HULL_SHAPE_PROXYTYPE,
                                                   BOX_SHAPE_PROXYTYPE,
                                                   &polyhedral);
        bt_dispatcher->registerCollisionCreateFunc(BOX_SHAPE_PROXYTYPE,
                                                   CONVEX_HULL_SHAPE_PROXYTYPE,
                                                   &polyhedral);
        // A mesh's triangles, against a box, a hull or another mesh's (in
        // place of Chrono's triangle pair, made for an FEA mesh's thickness)
        for (int other : {BOX_SHAPE_PROXYTYPE, CONVEX_HULL_SHAPE_PROXYTYPE}) {
            bt_dispatcher->registerCollisionCreateFunc(CH_TRIANGLE_SHAPE_PROXYTYPE,
                                                       other,
                                                       &polyhedral);
            bt_dispatcher->registerCollisionCreateFunc(other,
                                                       CH_TRIANGLE_SHAPE_PROXYTYPE,
                                                       &polyhedral);
        }
        bt_dispatcher->registerCollisionCreateFunc(CH_TRIANGLE_SHAPE_PROXYTYPE,
                                                   CH_TRIANGLE_SHAPE_PROXYTYPE,
                                                   &polyhedral);
    }
    void addPolyhedralFeatures(
        ch::ChCollisionModel& model,
        std::map<const ch::ChCollisionShape*, const cbtConvexPolyhedron*>& features);

    // Chrono runs collision detection as a step starts, and advance() runs
    // it after each substep to see what the substep found: at the same
    // state, so the next substep's may keep those results. Set it just
    // before a step whose state collision detection last saw; the next run
    // takes it down.
    bool reuse = false;
    bool reused = false;  // by the last run
    void Run() override
    {
        reused = reuse;
        reuse = false;
        if (!reused) {
            ch::ChCollisionSystemBullet::Run();
        }
    }

private:
    PolyhedralAlgorithm::Polyhedra polyhedra;
    PolyhedralAlgorithm::CreateFunc polyhedral;
};

// A model's boxes and hulls with their polyhedral features, for Bullet to
// clip a pair of them face against face: all the points of a face resting
// on another at once, and exact. The perturbation's points were up to 20 um
// off on a flat face, so a hexagonal prism landing flat took its impact on
// one corner and the contact's Newton iterations ran off to NaN. The
// features are of the shape Chrono reports contacts on, the true one grown
// by the envelope: a box's vertices are that. A hull's are shrunk by the
// margin Chrono gives back as Bullet's rounding, and Chrono's hull of its
// points merges any within a millimetre of each other, so its features are
// the exact hull of its points, each face moved out by the envelope and
// each vertex onto its faces' planes.
void ContactCollisionSystem::addPolyhedralFeatures(
    ch::ChCollisionModel& model,
    std::map<const ch::ChCollisionShape*, const cbtConvexPolyhedron*>& features)
{
    if (!model.HasImplementation()) {
        return;
    }
    // Its Bullet object, the one whose user pointer is the model's Bullet
    // half
    cbtCollisionShape* top = nullptr;
    const auto& objects = bt_collision_world->getCollisionObjectArray();
    for (int i = 0; i < objects.size() && !top; ++i) {
        if (objects[i]->getUserPointer() == model.GetImplementation()) {
            top = objects[i]->getCollisionShape();
        }
    }
    if (!top) {
        return;
    }
    auto* compound = top->getShapeType() == COMPOUND_SHAPE_PROXYTYPE
        ? static_cast<cbtCompoundShape*>(top)
        : nullptr;
    // The Bullet shapes in the order the instances made them: one each, a
    // mesh's one a triangle
    std::vector<cbtCollisionShape*> children;
    if (compound) {
        for (int i = 0; i < compound->getNumChildShapes(); ++i) {
            children.push_back(compound->getChildShape(i));
        }
    }
    else {
        children.push_back(top);
    }
    const auto& instances = model.GetShapeInstances();
    std::size_t next = 0;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        if (instances[i].shape->GetType() == ch::ChCollisionShape::Type::TRIANGLEMESH) {
            while (next < children.size()
                   && children[next]->getShapeType() == CH_TRIANGLE_SHAPE_PROXYTYPE) {
                auto* triangle = static_cast<cbtCEtriangleShape*>(children[next++]);
                polyhedra[triangle] =
                    satOfTriangle(*triangle, static_cast<cbtScalar>(model.GetEnvelope()));
            }
            continue;
        }
        if (next >= children.size()) {
            return;
        }
        cbtCollisionShape* shape = children[next++];
        switch (instances[i].shape->GetType()) {
            case ch::ChCollisionShape::Type::BOX: {
                auto* box = static_cast<cbtBoxShape*>(shape);
                if (box->initializePolyhedralFeatures()) {
                    features[instances[i].shape.get()] = box->getConvexPolyhedron();
                    polyhedra[box->getConvexPolyhedron()] = satOf(*box->getConvexPolyhedron());
                }
                break;
            }
            case ch::ChCollisionShape::Type::CONVEXHULL: {
                auto hull =
                    std::static_pointer_cast<ch::ChCollisionShapeConvexHull>(instances[i].shape);
                cbtConvexHullShape exact;
                for (const auto& v : hull->GetPoints()) {
                    exact.addPoint(cbtVector3(static_cast<cbtScalar>(v.x()),
                                              static_cast<cbtScalar>(v.y()),
                                              static_cast<cbtScalar>(v.z())),
                                   false);
                }
                exact.setMargin(0);
                if (exact.getNumPoints() < 4 || !exact.initializePolyhedralFeatures()) {
                    break;
                }
                cbtConvexPolyhedron grown = *exact.getConvexPolyhedron();
                const cbtScalar grow = static_cast<cbtScalar>(model.GetEnvelope());
                std::vector<ch::ChMatrix33d> normals(grown.m_vertices.size(),
                                                     ch::ChMatrix33d::Zero());
                std::vector<ch::ChVector3d> sums(grown.m_vertices.size(), ch::VNULL);
                for (int f = 0; f < grown.m_faces.size(); ++f) {
                    cbtFace& face = grown.m_faces[f];
                    const ch::ChVector3d n(face.m_plane[0], face.m_plane[1], face.m_plane[2]);
                    for (int k = 0; k < face.m_indices.size(); ++k) {
                        const auto v = static_cast<std::size_t>(face.m_indices[k]);
                        normals[v] += ch::TensorProduct(n, n);
                        sums[v] += n;
                    }
                    face.m_plane[3] -= grow;
                }
                // Each vertex by the least squares offset whose component
                // along each of its faces' normals is the growth. On a
                // curved surface a vertex's faces are nearly parallel and
                // their normals all but span a single direction: what they
                // do not span is left out (singular values under a
                // thousandth of the largest), not the vertex's features --
                // an ellipsoid without them went through the plate.
                for (int v = 0; v < grown.m_vertices.size(); ++v) {
                    const ch::ChMatrix33d& m = normals[static_cast<std::size_t>(v)];
                    Eigen::JacobiSVD<Eigen::Matrix3d> svd(m,
                                                          Eigen::ComputeFullU
                                                              | Eigen::ComputeFullV);
                    svd.setThreshold(1e-3);
                    const Eigen::Vector3d rhs = (sums[static_cast<std::size_t>(v)] * grow).eigen();
                    const Eigen::Vector3d offset = svd.solve(rhs);
                    grown.m_vertices[v] += cbtVector3(static_cast<cbtScalar>(offset.x()),
                                                      static_cast<cbtScalar>(offset.y()),
                                                      static_cast<cbtScalar>(offset.z()));
                }
                grown.m_uniqueEdges.clear();
                grown.initialize();
                auto* convex = static_cast<cbtConvexHullShape*>(shape);
                convex->setPolyhedralFeatures(grown);
                features[instances[i].shape.get()] = convex->getConvexPolyhedron();
                polyhedra[convex->getConvexPolyhedron()] = satOf(*convex->getConvexPolyhedron());
                break;
            }
            default:
                break;
        }
    }
}

// A contact shape where it is: on its body at its frame, a cylinder's
// radius and half length, a box's or a hull's polyhedral features
struct ShapeOn
{
    ch::ChBody* body = nullptr;
    ch::ChFramed frame;
    double radius = 0.0;  // a cylinder's, 0 for any other
    double half = 0.0;
    const cbtConvexPolyhedron* features = nullptr;
    double envelope = 0.0;
    // For Hertz's law (patchStiffness()): a sphere's radius, the material's
    // E / (1 - nu^2), the radius of the ball of the solid's volume, and the
    // surface's curvature at each vertex of its mesh, a mesh's or a hull's,
    // 1/m -- a hull's at its points, in its frame. Not of a hull's features:
    // Bullet's merges faces all but in one plane, and the vertices it kept
    // about an ellipsoid's pole curved it as 1/8 mm where it is 1/12.
    double ball = 0.0;
    double modulus = 0.0;
    double size = 0.0;
    std::vector<double> bends;
    std::vector<ch::ChVector3d> hull;
    // A mesh's vertices, in its body's frame: its triangles point into them
    const ch::ChVector3d* vertices = nullptr;
    std::size_t count = 0;
};

// A surface's curvature at each vertex of its faces, the most it curves
// there, convex positive: the larger eigenvalue of the curvature tensor of
// the height quadric h = A x^2/2 + B x y + C y^2/2 + D x + E y fitted by
// least squares to its neighbours, in the tangent plane of the vertex's
// normal. The linear terms take up that normal's error: an average of the
// faces around, it was 0.004 rad off on an ellipsoid's pole, and over a
// tenth of a millimetre's edge the circle through each edge's ends with it
// read the pole's 1/12 mm as anything from 1/30 to 1/6. The normal is the
// faces' at the vertex that are within 40 deg of the edge's face: a turn of
// 40 deg or more is one of the solid's edges, not a curve's tessellation --
// a box's, a prism's -- and a flat face up to it stays flat; its edges are
// no neighbours. Fewer than five neighbours, the edges' circles fit the
// tensor alone: -2 n . (v - u) / |v - u|^2 each, exact on a polygon of a
// circle (the angle between faces over their centres' spacing made a
// torus's half-quad triangles half as round again).
std::vector<double> vertexBends(const std::vector<ch::ChVector3d>& vertices,
                                const std::vector<std::vector<int>>& faces)
{
    const double sharp = std::cos(40.0 * pi / 180.0);
    std::vector<ch::ChVector3d> normals;
    std::vector<std::vector<std::size_t>> around(vertices.size());
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const auto& face = faces[f];
        ch::ChVector3d n = ch::VNULL;
        for (std::size_t k = 1; k + 1 < face.size(); ++k) {
            n += ch::Vcross(vertices[face[k]] - vertices[face[0]],
                            vertices[face[k + 1]] - vertices[face[0]]);
        }
        const double length = n.Length();
        normals.push_back(length > 0.0 ? n / length : ch::VNULL);
        for (int v : face) {
            around[static_cast<std::size_t>(v)].push_back(f);
        }
    }
    // Each edge's faces, to tell the solid's edges
    std::map<std::pair<int, int>, std::vector<std::size_t>> sides;
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const auto& face = faces[f];
        for (std::size_t k = 0; k < face.size(); ++k) {
            sides[std::minmax(face[k], face[(k + 1) % face.size()])].push_back(f);
        }
    }
    auto normalAt = [&](std::size_t v, std::size_t f) {
        ch::ChVector3d n = ch::VNULL;
        for (std::size_t g : around[v]) {
            if (ch::Vdot(normals[g], normals[f]) > sharp) {
                n += normals[g];
            }
        }
        const double length = n.Length();
        return length > 0.0 ? n / length : normals[f];
    };
    // Each vertex's edges: the normal there, the edge's direction, and the
    // curvature along it
    struct Along
    {
        ch::ChVector3d normal;
        ch::ChVector3d direction;
        double bend;
    };
    std::vector<std::vector<Along>> edges(vertices.size());
    for (const auto& [edge, two] : sides) {
        if (two.size() != 2 || !(ch::Vdot(normals[two[0]], normals[two[1]]) > sharp)) {
            continue;
        }
        const auto u = static_cast<std::size_t>(edge.first);
        const auto v = static_cast<std::size_t>(edge.second);
        const ch::ChVector3d d = vertices[v] - vertices[u];
        const double length2 = d.Length2();
        if (!(length2 > 0.0)) {
            continue;
        }
        for (const auto& [from, to] : {std::pair(u, d), std::pair(v, -d)}) {
            const ch::ChVector3d n = normalAt(from, two[0]);
            edges[from].push_back({n, to, -2.0 * ch::Vdot(n, to) / length2});
        }
    }
    std::vector<double> bends(vertices.size(), 0.0);
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        const auto& at = edges[v];
        if (at.empty()) {
            continue;
        }
        ch::ChVector3d n = ch::VNULL;
        for (const Along& e : at) {
            n += e.normal;
        }
        n.Normalize();
        // The tangent plane's axes
        ch::ChVector3d t1 = ch::Vcross(n, std::abs(n.x()) < 0.9 ? ch::VECT_X : ch::VECT_Y);
        t1.Normalize();
        const ch::ChVector3d t2 = ch::Vcross(n, t1);
        // The larger eigenvalue of [[a, b], [b, c]], convex positive
        auto largest = [](double a, double b, double c) {
            const double mean = 0.5 * (a + c);
            const double half = std::sqrt(0.25 * (a - c) * (a - c) + b * b);
            return std::max(mean + half, 0.0);
        };
        if (at.size() >= 5) {
            // The height quadric: h = A x^2/2 + B x y + C y^2/2 + D x + E y,
            // the curvature -[[A, B], [B, C]], the normal outward
            Eigen::MatrixXd rows(static_cast<Eigen::Index>(at.size()), 5);
            Eigen::VectorXd heights(static_cast<Eigen::Index>(at.size()));
            for (std::size_t i = 0; i < at.size(); ++i) {
                const double x = ch::Vdot(at[i].direction, t1);
                const double y = ch::Vdot(at[i].direction, t2);
                const auto row = static_cast<Eigen::Index>(i);
                rows.row(row) << 0.5 * x * x, x * y, 0.5 * y * y, x, y;
                heights[row] = ch::Vdot(at[i].direction, n);
            }
            const Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(rows);
            if (qr.rank() == 5) {
                const Eigen::VectorXd fit = qr.solve(heights);
                bends[v] = largest(-fit[0], -fit[1], -fit[2]);
                continue;
            }
        }
        // k(theta) = a cos^2 + 2 b cos sin + c sin^2, from the edges' circles
        Eigen::Matrix3d m = Eigen::Matrix3d::Zero();
        Eigen::Vector3d r = Eigen::Vector3d::Zero();
        double most = 0.0;
        for (const Along& e : at) {
            const double x = ch::Vdot(e.direction, t1);
            const double y = ch::Vdot(e.direction, t2);
            const double length2 = x * x + y * y;
            if (!(length2 > 0.0)) {
                continue;
            }
            const Eigen::Vector3d row(x * x / length2, 2.0 * x * y / length2, y * y / length2);
            m += row * row.transpose();
            r += row * e.bend;
            most = std::max(most, e.bend);
        }
        const Eigen::FullPivLU<Eigen::Matrix3d> lu(m);
        if (at.size() < 3 || lu.rank() < 3) {
            bends[v] = most;
            continue;
        }
        const Eigen::Vector3d abc = lu.solve(r);
        bends[v] = largest(abc[0], abc[1], abc[2]);
    }
    return bends;
}

// A mesh's curvature at each vertex
std::vector<double> meshBends(const std::vector<ch::ChVector3d>& vertices,
                              const std::vector<ch::ChVector3i>& triangles)
{
    std::vector<std::vector<int>> faces;
    faces.reserve(triangles.size());
    for (const auto& t : triangles) {
        faces.push_back({t.x(), t.y(), t.z()});
    }
    return vertexBends(vertices, faces);
}

// The contact shapes by Chrono's shapes, and a mesh's triangle by the mesh
// its corners are in
struct ShapesOn
{
    const std::map<const ch::ChCollisionShape*, ShapeOn>* byShape = nullptr;
    std::vector<const ShapeOn*> meshes;

    const ShapeOn* of(const ch::ChCollisionShape* shape) const
    {
        if (!shape) {
            return nullptr;
        }
        if (shape->GetType() == ch::ChCollisionShape::Type::MESHTRIANGLE) {
            const auto* triangle = static_cast<const ch::ChCollisionShapeMeshTriangle*>(shape);
            for (const ShapeOn* mesh : meshes) {
                if (triangle->V1 >= mesh->vertices && triangle->V1 < mesh->vertices + mesh->count) {
                    return mesh;
                }
            }
            return nullptr;
        }
        auto it = byShape->find(shape);
        return it == byShape->end() ? nullptr : &it->second;
    }

    // The curvature of the shape where a point of its body touches ('at', in
    // the body's frame), the surface there facing 'outward' (absolute): a
    // mesh's at the triangle's corner nearest, a hull's at its point farthest
    // out that way, where a convex shape touches, a box's none
    double bendAt(const ch::ChCollisionShape* shape,
                  const ch::ChBody& body,
                  const ch::ChVector3d& at,
                  const ch::ChVector3d& outward) const
    {
        const ShapeOn* on = of(shape);
        if (!on) {
            return 0.0;
        }
        if (on->vertices) {
            const auto* triangle = static_cast<const ch::ChCollisionShapeMeshTriangle*>(shape);
            double nearest = std::numeric_limits<double>::infinity();
            double bend = 0.0;
            for (const ch::ChVector3d* corner : {triangle->V1, triangle->V2, triangle->V3}) {
                const auto index = static_cast<std::size_t>(corner - on->vertices);
                const double apart = (*corner - at).Length2();
                if (index < on->bends.size() && apart < nearest) {
                    nearest = apart;
                    bend = on->bends[index];
                }
            }
            return bend;
        }
        if (on->ball > 0.0) {
            return 1.0 / on->ball;
        }
        const ch::ChVector3d local =
            on->frame.GetRot().RotateBack(body.GetRot().RotateBack(outward));
        if (on->radius > 0.0) {
            // Its cap is flat, its side as round as the cylinder
            return std::abs(local.z()) > 0.5 ? 0.0 : 1.0 / on->radius;
        }
        std::size_t best = on->hull.size();
        double most = -std::numeric_limits<double>::infinity();
        for (std::size_t v = 0; v < on->hull.size(); ++v) {
            const double along = ch::Vdot(on->hull[v], local);
            if (along > most) {
                most = along;
                best = v;
            }
        }
        return best < on->bends.size() ? on->bends[best] : 0.0;
    }
};

// Each patch's stiffness (ContactLaw): the law's, or Hertz's from the shapes'
// materials, k = 4/3 E* R^0.5, 1/E* the sum of the two shapes'
// 1 / (E / (1 - nu^2)). R is the contact's radius: from the curvatures where
// the patch touches, 1 / (kA + kB), each side's its points' mean; two flat
// faces, the patch's extent -- the farthest of its points from their
// centre, a face resting flat on a face its size -- and a single point on
// two flat faces, the part sizes'. Or from the parts' sizes alone,
// 1/R = 1/RA + 1/RB, each the radius of the ball of the solid's volume.
// The extent is no cap on a curve's radius: its points are all those within
// the contact's margin, and an ellipsoid resting on its pole, R 12 mm, had
// them on the triangle there, half a millimetre across.
void patchStiffness(ContactLoad& load, const ShapesOn& shapes)
{
    const ContactLaw& law = load.law;
    load.stiffness.assign(load.patches, law.stiffness);
    if (law.from != ContactLaw::Stiffness::Materials || load.points.empty()) {
        return;
    }
    const auto& points = load.points;
    const auto bodyA = load.GetBodyA();
    const auto bodyB = load.GetBodyB();
    struct Patch
    {
        std::size_t count = 0;
        double bendA = 0.0;
        double bendB = 0.0;
        ch::ChVector3d centre;
        double extent = 0.0;
        const ShapeOn* onA = nullptr;
        const ShapeOn* onB = nullptr;
    };
    std::vector<Patch> patches(load.patches);
    for (const auto& point : points) {
        Patch& patch = patches[point.patch];
        ++patch.count;
        patch.bendA += shapes.bendAt(point.shapeA, *bodyA, point.onA, point.normal);
        patch.bendB += shapes.bendAt(point.shapeB, *bodyB, point.onB, -point.normal);
        patch.centre += point.onA;
        if (!patch.onA) {
            patch.onA = shapes.of(point.shapeA);
            patch.onB = shapes.of(point.shapeB);
        }
    }
    for (auto& patch : patches) {
        patch.centre /= static_cast<double>(std::max<std::size_t>(patch.count, 1));
    }
    for (const auto& point : points) {
        Patch& patch = patches[point.patch];
        patch.extent = std::max(patch.extent, (point.onA - patch.centre).Length());
    }
    for (std::size_t p = 0; p < patches.size(); ++p) {
        const Patch& patch = patches[p];
        if (!patch.onA || !patch.onB || !(patch.onA->modulus > 0.0)
            || !(patch.onB->modulus > 0.0)) {
            continue;
        }
        const double modulus = 1.0 / (1.0 / patch.onA->modulus + 1.0 / patch.onB->modulus);
        double sizes = 0.0;
        for (const ShapeOn* on : {patch.onA, patch.onB}) {
            sizes += on->size > 0.0 ? 1.0 / on->size : 0.0;
        }
        const double part = sizes > 0.0 ? 1.0 / sizes : 0.0;
        double radius = part;
        if (law.radius == ContactLaw::Radius::Patch) {
            const double bend = (patch.bendA + patch.bendB) / static_cast<double>(patch.count);
            radius = bend > 0.0 ? 1.0 / bend : patch.extent > 0.0 ? patch.extent : part;
        }
        if (radius > 0.0) {
            load.stiffness[p] = 4.0 / 3.0 * modulus * std::sqrt(radius);
        }
    }
}

// A point of a cylinder's contact, on it and on the other shape
struct CylinderPoint
{
    ch::ChVector3d onCylinder;
    ch::ChVector3d onOther;
};

// The points of a cylinder's contact with a box's or a hull's face, the
// normal from that face to the cylinder in normal: its cap (its normal
// within 60 degrees of the cylinder's axis) as a polygon of its rim, or its
// side as the segment between its caps' points nearest the face, clipped to
// the face, each point within reach of it. Bullet's GJK gives a cylinder one
// point a query, a different one of its rim each time it lands flat on its
// cap. False when there is no such face: the pair keeps GJK's points.
bool cylinderPoints(const ShapeOn& cylinder,
                    const ShapeOn& other,
                    const ch::ChVector3d& toOther,
                    double reach,
                    std::vector<CylinderPoint>& points,
                    ch::ChVector3d& normal)
{
    points.clear();
    if (!other.features || other.features->m_faces.size() == 0) {
        return false;
    }
    const ch::ChFramed o = other.frame >> *other.body;
    const ch::ChMatrix33d& r = o.GetRotMat();
    cbtTransform where;
    where.setOrigin(cbtVector3(static_cast<cbtScalar>(o.GetPos().x()),
                               static_cast<cbtScalar>(o.GetPos().y()),
                               static_cast<cbtScalar>(o.GetPos().z())));
    where.setBasis(cbtMatrix3x3(static_cast<cbtScalar>(r(0, 0)),
                                static_cast<cbtScalar>(r(0, 1)),
                                static_cast<cbtScalar>(r(0, 2)),
                                static_cast<cbtScalar>(r(1, 0)),
                                static_cast<cbtScalar>(r(1, 1)),
                                static_cast<cbtScalar>(r(1, 2)),
                                static_cast<cbtScalar>(r(2, 0)),
                                static_cast<cbtScalar>(r(2, 1)),
                                static_cast<cbtScalar>(r(2, 2))));
    // The face, the one turned most against toOther; its plane is grown by
    // the envelope, as the features are
    const cbtConvexPolyhedron& hull = *other.features;
    int closest = 0;
    cbtScalar least = std::numeric_limits<cbtScalar>::max();
    for (int f = 0; f < hull.m_faces.size(); ++f) {
        const cbtScalar* plane = hull.m_faces[f].m_plane;
        const cbtVector3 n = where.getBasis() * cbtVector3(plane[0], plane[1], plane[2]);
        const cbtScalar d = n.x() * toOther.x() + n.y() * toOther.y() + n.z() * toOther.z();
        if (d < least) {
            least = d;
            closest = f;
        }
    }
    const cbtScalar* plane = hull.m_faces[closest].m_plane;
    const cbtVector3 faceNormal = where.getBasis() * cbtVector3(plane[0], plane[1], plane[2]);
    normal = ch::ChVector3d(faceNormal.x(), faceNormal.y(), faceNormal.z());
    // The cylinder's feature toward the face, by the face's normal, not
    // GJK's, which a flat contact leaves to chance and a manifold keeps from
    // an older step
    const ch::ChVector3d into = -normal;
    const ch::ChFramed c = cylinder.frame >> *cylinder.body;
    const ch::ChVector3d axis = c.GetRotMat().GetAxisZ();
    const double along = ch::Vdot(axis, into);
    const ch::ChVector3d side = into - axis * along;
    const double sideLength = side.Length();
    cbtVertexArray polygon;
    auto add = [&](const ch::ChVector3d& p) {
        polygon.push_back(cbtVector3(static_cast<cbtScalar>(p.x()),
                                     static_cast<cbtScalar>(p.y()),
                                     static_cast<cbtScalar>(p.z())));
    };
    if (std::abs(along) >= 0.5) {
        const ch::ChVector3d center =
            c.GetPos() + axis * (along > 0.0 ? cylinder.half : -cylinder.half);
        // From the rim's point nearest the face: exact where the cylinder
        // stands tilted on it
        const ch::ChVector3d u = sideLength > 1e-9 ? side / sideLength : c.GetRotMat().GetAxisX();
        const ch::ChVector3d v = ch::Vcross(axis, u);
        constexpr int rim = 16;
        for (int k = 0; k < rim; ++k) {
            const double angle = 2.0 * pi * k / rim;
            add(center + (u * std::cos(angle) + v * std::sin(angle)) * cylinder.radius);
        }
    }
    else {
        if (!(sideLength > 1e-9)) {
            return false;
        }
        const ch::ChVector3d out = side / sideLength * cylinder.radius;
        add(c.GetPos() + axis * cylinder.half + out);
        add(c.GetPos() - axis * cylinder.half + out);
    }
    const cbtVector3 separating(static_cast<cbtScalar>(into.x()),
                                static_cast<cbtScalar>(into.y()),
                                static_cast<cbtScalar>(into.z()));
    struct Collect: cbtDiscreteCollisionDetectorInterface::Result
    {
        std::vector<std::pair<cbtVector3, cbtScalar>> found;
        void setShapeIdentifiersA(int, int) override
        {}
        void setShapeIdentifiersB(int, int) override
        {}
        void addContactPoint(const cbtVector3&, const cbtVector3& point, cbtScalar depth) override
        {
            found.emplace_back(point, depth);
        }
    } collect;
    cbtVertexArray scratch;
    cbtPolyhedralContactClipping::clipFaceAgainstHull(
        separating,
        hull,
        where,
        polygon,
        scratch,
        -std::numeric_limits<cbtScalar>::max(),
        static_cast<cbtScalar>(reach - other.envelope),
        collect);
    for (const auto& [point, depth] : collect.found) {
        const ch::ChVector3d p(point.x(), point.y(), point.z());
        // A segment's clipped end comes twice
        bool again = false;
        for (const auto& known : points) {
            again = again || (known.onCylinder - p).Length2() < 1e-18;
        }
        if (again) {
            continue;
        }
        // Its gap from the true face, the envelope inside the grown one
        const double gap = static_cast<double>(depth) + other.envelope;
        points.push_back({p, p - normal * gap});
    }
    return !points.empty();
}

// ---------------------------------------------------------------- NeutralLink

namespace eq = Assembly::Solver::Equations;

// A joint held by the neutral layer's own equations (JointEquations.h, the
// Reference's), for the kinds Chrono has no link for: the same residuals,
// in OndselSolver's forms, with the same analytic gradients, the same
// constants and the same turn counting. Up to three bodies (a coaxial gear's
// carrier is the third), one ChConstraintNgeneric per equation.
//
// The gradients are the Reference's: per body d/dx and d/dtheta, theta the
// spatial rotation vector (q' = exp(dtheta) q). Chrono's velocity
// coordinates of a body are its position and its angular velocity in the
// body's own frame (ChBody::IntStateIncrement: q' = q exp(dw)), so dtheta =
// R dw and the rotation block of a row becomes g R: Cq = [g_x, g_theta R].
//
// Turns. A coupler's angles are unwrapped against the angles last committed,
// as the Reference does: Update() reads them (measured), commit() takes the
// reading at the current poses once the backend accepts them, and the
// backend's states carry the committed angles, so a rejected step and a
// finite-difference probe leave the count as it was. A PointOnCurve's curve
// parameter is kept the same way (its one slot): the closest point is
// projected from the committed one, and a closed cam's turns are counted.
class NeutralLink: public ch::ChLinkBase
{
public:
    // A frame of the joint: on 'body' (null: a constant frame -- the ground,
    // or a body Chrono holds fixed) at 'local' in it.
    struct FrameRef
    {
        ch::ChBody* body = nullptr;
        ch::ChFramed local;
    };

    NeutralLink(const Joint& joint,
                const FrameRef& i,
                const FrameRef& j,
                const FrameRef* k,
                std::shared_ptr<const CurveGeometry> curve = nullptr)
        : joint(joint)
        , curve(std::move(curve))
        , frames {i, j, k ? *k : FrameRef {}}
        , hasCarrier(k != nullptr)
    {
        // The variables: the distinct moving bodies, in frame order.
        for (std::size_t f = 0; f < 3; ++f) {
            slot[f] = -1;
            if (f == 2 && !hasCarrier) {
                continue;
            }
            ch::ChBody* body = frames[f].body;
            if (!body || body->IsFixed()) {
                continue;
            }
            const auto at = std::find(bodies.begin(), bodies.end(), body);
            slot[f] = static_cast<int>(at - bodies.begin());
            if (at == bodies.end()) {
                bodies.push_back(body);
            }
        }
        // What the joint keeps from the start: the coaxial test, the first
        // reading of its angles and its constant.
        const auto fs = frameStates();
        bool coaxial = false;
        if (joint.kind == JointKind::Gear || joint.kind == JointKind::CoaxialGear) {
            coaxial = eq::framesAreCoaxial(fs[0], fs[1]);
        }
        state = eq::startState(joint,
                               coaxial,
                               fs[0],
                               fs[1],
                               hasCarrier ? &fs[2] : nullptr,
                               this->curve.get());
        measured = state.angles;
        // The rows, as many as the equations at the start.
        std::vector<eq::Equation> rows;
        evaluate(rows, measured);
        categories.reserve(rows.size());
        for (const auto& row : rows) {
            categories.push_back(row.category);
        }
        kept.reserve(rows.size());
        for (std::size_t r = 0; r < rows.size(); ++r) {
            kept.push_back(r);
        }
        std::vector<ch::ChVariables*> variables;
        for (ch::ChBody* body : bodies) {
            variables.push_back(&body->Variables());
        }
        constraints.resize(rows.size());
        for (auto& c : constraints) {
            c.SetVariables(variables);
        }
        C.setZero(static_cast<Eigen::Index>(rows.size()));
        reactions.setZero(static_cast<Eigen::Index>(rows.size()));
        load(rows);
    }

    NeutralLink* Clone() const override
    {
        return new NeutralLink(*this);
    }

    // The equations' categories, in row order (OndselSolver's, for the rank
    // analysis).
    const std::vector<eq::Category>& rowCategories() const
    {
        return categories;
    }

    // Takes out the rows 'drop' marks, by their index among the rows held
    // now (a redundant joint's, docs/AssemblyDynamics.md sec 6.1): the link
    // no longer holds them, and reports what the rest carry. Before the
    // system is set up again.
    void dropRows(const std::vector<char>& drop)
    {
        std::vector<std::size_t> keep;
        std::vector<eq::Category> keepCategories;
        for (std::size_t r = 0; r < kept.size(); ++r) {
            if (r < drop.size() && drop[r]) {
                continue;
            }
            keep.push_back(kept[r]);
            keepCategories.push_back(categories[r]);
        }
        kept = std::move(keep);
        categories = std::move(keepCategories);
        constraints.resize(kept.size());
        C.setZero(static_cast<Eigen::Index>(kept.size()));
        reactions.setZero(static_cast<Eigen::Index>(kept.size()));
        std::vector<eq::Equation> rows;
        evaluate(rows, measured);
        load(rows);
    }

    // The committed angles, and setting them back (a rejected step).
    const std::array<double, 2>& angles() const
    {
        return state.angles;
    }
    void setAngles(const std::array<double, 2>& angles)
    {
        state.angles = angles;
    }
    // Takes the angles the current poses read, their turns counted from the
    // last committed ones.
    void commit()
    {
        if (eq::jointAngleSlots(joint.kind) == 0) {
            return;
        }
        std::vector<eq::Equation> rows;
        std::array<double, 2> reading {};
        evaluate(rows, reading);
        state.angles = reading;
    }
    // The largest speed ratio the joint couples, for the kinematic run's step.
    double speedRatio() const
    {
        if ((joint.kind == JointKind::Gear || joint.kind == JointKind::CoaxialGear)
            && joint.value != 0.0 && joint.value2 != 0.0) {
            const double ratio = std::abs(joint.value / joint.value2);
            return std::max(ratio, 1.0 / ratio);
        }
        return 1.0;
    }

    unsigned int GetNumConstraintsBilateral() override
    {
        return static_cast<unsigned int>(constraints.size());
    }
    unsigned int GetNumAffectedCoords() override
    {
        return static_cast<unsigned int>(6 * bodies.size());
    }
    ch::ChVectorDynamic<> GetConstraintViolation() const override
    {
        return C;
    }
    ch::ChFramed GetFrame1Abs() const override
    {
        return absolute(frames[1]);
    }
    ch::ChFramed GetFrame2Abs() const override
    {
        return absolute(frames[0]);
    }
    // The wrench the rows put on 'body', absolute: the force, and the torque
    // about the absolute point 'about'. Cq^T times the multipliers, the
    // force on the variables (HHT's residual adds Cq^T L to f); the angular
    // block is the body's own axes'. A body the rows do not move -- the
    // ground, a fixed one -- takes the opposite of what they put on the
    // others: a joint's rows read relative poses only, so they balance.
    ch::ChWrenchd wrenchOn(const ch::ChBody* body, const ch::ChVector3d& about)
    {
        ch::ChVector3d sumForce;
        ch::ChVector3d sumTorque;
        for (std::size_t b = 0; b < bodies.size(); ++b) {
            ch::ChVector3d force;
            ch::ChVector3d local;
            for (std::size_t r = 0; r < constraints.size(); ++r) {
                const double l = reactions(static_cast<Eigen::Index>(r));
                auto row = constraints[r].Get_Cq_N(b);
                force += l * ch::ChVector3d(row(0), row(1), row(2));
                local += l * ch::ChVector3d(row(3), row(4), row(5));
            }
            const ch::ChVector3d torque = bodies[b]->GetRotMat() * local
                + ch::Vcross(bodies[b]->GetPos() - about, force);
            if (bodies[b] == body) {
                return {force, torque};
            }
            sumForce += force;
            sumTorque += torque;
        }
        return {-sumForce, -sumTorque};
    }

    // Kinematics only: no reactions are reported (wrenchOn() reads them).
    ch::ChWrenchd GetReaction1() const override
    {
        return {ch::VNULL, ch::VNULL};
    }
    ch::ChWrenchd GetReaction2() const override
    {
        return {ch::VNULL, ch::VNULL};
    }

    void Update(double time, ch::UpdateFlags flags) override
    {
        ch::ChPhysicsItem::Update(time, flags);
        std::vector<eq::Equation> rows;
        evaluate(rows, measured);
        load(rows);
    }

    // ------------------------------------------------ state bookkeeping

    void IntStateGatherReactions(const unsigned int off_L, ch::ChVectorDynamic<>& L) override
    {
        if (IsActive()) {
            L.segment(off_L, reactions.size()) = reactions;
        }
    }
    void IntStateScatterReactions(const unsigned int off_L,
                                  const ch::ChVectorDynamic<>& L) override
    {
        if (IsActive()) {
            reactions = L.segment(off_L, reactions.size());
        }
    }
    void IntLoadResidual_CqL(const unsigned int off_L,
                             ch::ChVectorDynamic<>& R,
                             const ch::ChVectorDynamic<>& L,
                             const double c) override
    {
        if (!IsActive()) {
            return;
        }
        for (std::size_t r = 0; r < constraints.size(); ++r) {
            constraints[r].AddJacobianTransposedTimesScalarInto(
                R,
                L(off_L + static_cast<unsigned int>(r)) * c);
        }
    }
    void IntLoadConstraint_C(const unsigned int off_L,
                             ch::ChVectorDynamic<>& Qc,
                             const double c,
                             bool do_clamp,
                             double recovery_clamp) override
    {
        if (!IsActive()) {
            return;
        }
        for (Eigen::Index r = 0; r < C.size(); ++r) {
            const double v = c * C(r);
            Qc(off_L + r) += do_clamp ? std::clamp(v, -recovery_clamp, recovery_clamp) : v;
        }
    }
    // Scleronomic: the equations do not depend on time, Ct is zero.
    void IntLoadConstraint_Ct(const unsigned int, ch::ChVectorDynamic<>&, const double) override
    {}
    void IntToDescriptor(const unsigned int,
                         const ch::ChStateDelta&,
                         const ch::ChVectorDynamic<>&,
                         const unsigned int off_L,
                         const ch::ChVectorDynamic<>& L,
                         const ch::ChVectorDynamic<>& Qc) override
    {
        if (!IsActive()) {
            return;
        }
        for (std::size_t r = 0; r < constraints.size(); ++r) {
            const unsigned int i = off_L + static_cast<unsigned int>(r);
            constraints[r].SetLagrangeMultiplier(L(i));
            constraints[r].SetRightHandSide(Qc(i));
        }
    }
    void IntFromDescriptor(const unsigned int,
                           ch::ChStateDelta&,
                           const unsigned int off_L,
                           ch::ChVectorDynamic<>& L) override
    {
        if (!IsActive()) {
            return;
        }
        for (std::size_t r = 0; r < constraints.size(); ++r) {
            L(off_L + static_cast<unsigned int>(r)) = constraints[r].GetLagrangeMultiplier();
        }
    }

    // ------------------------------------------------ solver interface

    void InjectConstraints(ch::ChSystemDescriptor& descriptor) override
    {
        if (!IsActive()) {
            return;
        }
        for (auto& c : constraints) {
            descriptor.InsertConstraint(&c);
        }
    }
    void ConstraintsBiReset() override
    {
        for (auto& c : constraints) {
            c.SetRightHandSide(0.0);
        }
    }
    void ConstraintsBiLoad_C(double factor, double recovery_clamp, bool do_clamp) override
    {
        if (!IsActive()) {
            return;
        }
        for (std::size_t r = 0; r < constraints.size(); ++r) {
            const double v = factor * C(static_cast<Eigen::Index>(r));
            constraints[r].SetRightHandSide(
                constraints[r].GetRightHandSide()
                + (do_clamp ? std::clamp(v, -recovery_clamp, recovery_clamp) : v));
        }
    }
    void LoadConstraintJacobians() override
    {
        // Loaded by Update().
    }
    void ConstraintsFetch_react(double factor) override
    {
        for (std::size_t r = 0; r < constraints.size(); ++r) {
            reactions(static_cast<Eigen::Index>(r)) =
                constraints[r].GetLagrangeMultiplier() * factor;
        }
    }

    void ArchiveOut(ch::ChArchiveOut& archive_out) override
    {
        archive_out.VersionWrite<NeutralLink>();
        ch::ChLinkBase::ArchiveOut(archive_out);
        int kind = static_cast<int>(joint.kind);
        double value = joint.value;
        double value2 = joint.value2;
        double constant = state.constant;
        archive_out << ch::make_ChNameValue("kind", kind);
        archive_out << ch::make_ChNameValue("value", value);
        archive_out << ch::make_ChNameValue("value2", value2);
        archive_out << ch::make_ChNameValue("constant", constant);
        if (curve) {
            std::string text = curve->describe();
            archive_out << ch::make_ChNameValue("curve", text);
        }
    }

private:
    ch::ChFramed absolute(const FrameRef& f) const
    {
        return f.body ? f.local >> *f.body : f.local;
    }

    std::array<eq::FrameState, 3> frameStates() const
    {
        std::array<eq::FrameState, 3> out;
        for (std::size_t f = 0; f < 3; ++f) {
            const FrameRef& ref = frames[f];
            const ch::ChFramed local = ref.local;
            const ch::ChVector3d lp = local.GetPos();
            const ch::ChMatrix33<> lr = local.GetRotMat();
            eq::FrameState& s = out[f];
            s.body = slot[f];
            if (!ref.body) {
                s.o = eq::Vec3(lp.x(), lp.y(), lp.z());
                s.base = s.o;
                s.A = eq::Mat3(lr);
                continue;
            }
            const ch::ChVector3d bp = ref.body->GetPos();
            const eq::Mat3 br = ref.body->GetRotMat();
            const eq::Mat3 lrm = lr;
            s.base = eq::Vec3(bp.x(), bp.y(), bp.z());
            s.o = s.base + br * eq::Vec3(lp.x(), lp.y(), lp.z());
            s.A = br * lrm;
        }
        return out;
    }

    void evaluate(std::vector<eq::Equation>& rows, std::array<double, 2>& reading) const
    {
        const auto fs = frameStates();
        rows.clear();
        eq::jointRows(joint,
                      fs[0],
                      fs[1],
                      hasCarrier ? &fs[2] : nullptr,
                      state,
                      false,
                      rows,
                      reading,
                      curve.get());
    }

    // C and Cq from the equations: the rotation block of each gradient from
    // the spatial rotation vector to the body's angular velocity in its own
    // frame.
    void load(const std::vector<eq::Equation>& rows)
    {
        const std::size_t n = std::min(kept.size(), constraints.size());
        for (std::size_t r = 0; r < n; ++r) {
            if (kept[r] >= rows.size()) {
                continue;
            }
            const eq::Equation& row = rows[kept[r]];
            C(static_cast<Eigen::Index>(r)) = row.s.v;
            ch::ChConstraintNgeneric& c = constraints[r];
            for (std::size_t b = 0; b < bodies.size(); ++b) {
                c.Get_Cq_N(b).setZero();
            }
            const eq::Grad& g = row.s.g;
            for (int k = 0; k < g.n; ++k) {
                const int b = g.body[k];
                if (b < 0 || static_cast<std::size_t>(b) >= bodies.size()) {
                    continue;
                }
                const eq::Mat3 R = bodies[b]->GetRotMat();
                auto row = c.Get_Cq_N(static_cast<std::size_t>(b));
                row.segment(0, 3) = g.g[k].leftCols<3>();
                row.segment(3, 3) = g.g[k].rightCols<3>() * R;
            }
        }
    }

    Joint joint;
    std::shared_ptr<const CurveGeometry> curve;  // a PointOnCurve's
    std::array<FrameRef, 3> frames;
    bool hasCarrier = false;
    std::array<int, 3> slot {-1, -1, -1};
    std::vector<ch::ChBody*> bodies;
    eq::JointState state;
    std::array<double, 2> measured {};
    std::vector<eq::Category> categories;
    // The equations held, by index among those evaluate() gives: all of
    // them, unless a redundant joint's were taken out
    std::vector<std::size_t> kept;
    std::vector<ch::ChConstraintNgeneric> constraints;
    ch::ChVectorDynamic<> C;
    ch::ChVectorDynamic<> reactions;
};

// Named in Chrono's class factory, so that its JSON archive (exportNative)
// says what the link is.
const ch::ChClassRegistration<NeutralLink> neutralLinkRegistration("AssemblyNeutralLink");

// ---------------------------------------------------------------- the model

struct Pose
{
    ch::ChVector3d p;
    ch::ChQuaterniond q;
};

struct LinkRecord
{
    std::shared_ptr<ch::ChLinkBase> link;
    Owner owner = Owner::Joint;
    Id id = NoId;
    std::vector<Category> rows;  // one per constraint row, in Chrono's row order
};

// A constraint row's owner and category (the rank analysis)
struct RowInfo
{
    Owner owner = Owner::Limit;
    Id id = NoId;
    Category category = Category::Essential;
};

std::vector<Category> mateRows(const ch::ChLinkMateGeneric& mate)
{
    std::vector<Category> rows;
    const bool flags[6] = {
        mate.IsConstrainedX(),
        mate.IsConstrainedY(),
        mate.IsConstrainedZ(),
        mate.IsConstrainedRx(),
        mate.IsConstrainedRy(),
        mate.IsConstrainedRz(),
    };
    for (int k = 0; k < 6; ++k) {
        if (flags[k]) {
            rows.push_back(k < 3 ? Category::Displacement : Category::Perpendicular);
        }
    }
    return rows;
}

// The half turn about x, y or z as a quaternion (k = 0, 1, 2), or the identity.
ch::ChQuaterniond halfTurn(int k)
{
    switch (k) {
        case 0:
            return {0.0, 1.0, 0.0, 0.0};
        case 1:
            return {0.0, 0.0, 1.0, 0.0};
        case 2:
            return {0.0, 0.0, 0.0, 1.0};
        default:
            return {1.0, 0.0, 0.0, 0.0};
    }
}

// The rotation that puts frame J on the branch of OndselSolver's solutions
// the start is nearest (the file's header): 'rel' is J's rotation in I's frame.
ch::ChQuaterniond branchOf(JointKind kind, const ch::ChMatrix33<>& rel)
{
    switch (kind) {
        case JointKind::Revolute:
        case JointKind::Cylindrical:
        case JointKind::Planar:
        case JointKind::ParallelZ:
            return rel(2, 2) < 0.0 ? halfTurn(0) : halfTurn(-1);
        case JointKind::Fixed:
        case JointKind::Prismatic: {
            // The diagonal sign matrix of determinant 1 nearest to rel.
            const double d0 = rel(0, 0), d1 = rel(1, 1), d2 = rel(2, 2);
            const std::array<double, 4> score {d0 + d1 + d2,
                                               d0 - d1 - d2,
                                               -d0 + d1 - d2,
                                               -d0 - d1 + d2};
            const auto best = std::max_element(score.begin(), score.end()) - score.begin();
            return halfTurn(static_cast<int>(best) - 1);
        }
        default:
            return halfTurn(-1);
    }
}

}  // namespace

// ---------------------------------------------------------------- refusals

std::string Chrono::refusal(const Joint& joint)
{
    switch (joint.kind) {
        case JointKind::Gear:
        case JointKind::CoaxialGear:
            // The ratio is value / value2 (the Reference fails the same).
            if (joint.value2 == 0.0) {
                return "a gear radius is zero";
            }
            return {};
        case JointKind::Count_:
            return "not a joint kind";
        default:
            return {};
    }
}

bool Chrono::nativeLink(const Joint& joint)
{
    switch (joint.kind) {
        case JointKind::AngleZ:
            return isRightAngle(joint.value);
        case JointKind::PointLineDistance:
        case JointKind::LineLineDistance:
        case JointKind::CoaxialGear:
        case JointKind::RackPinion:
        case JointKind::Screw:
        case JointKind::PointOnCurve:  // ChLinkLockPointSpline: see the header
        case JointKind::Count_:
            return false;
        case JointKind::Gear:
            // Between parallel shafts that are not coaxial, decided at load.
            return joint.value > 0.0 && joint.value2 > 0.0;
        default:
            return true;
    }
}

JointKindSet Chrono::supportedKinds()
{
    JointKindSet kinds;
    for (std::size_t k = 0; k < static_cast<std::size_t>(JointKind::Count_); ++k) {
        Joint probe;
        probe.kind = static_cast<JointKind>(k);
        probe.value = 1.0;
        probe.value2 = 1.0;
        if (refusal(probe).empty()) {
            kinds.set(k);
        }
    }
    return kinds;
}

FormulaValue Chrono::motionFunction(const Formula& formula, double time)
{
    auto evaluate = formula.evaluator();
    if (!evaluate) {
        return {notANumber, notANumber, notANumber};
    }
    const FormulaChFunction function(evaluate);
    const ch::ChFunction& f = function;
    return {f.GetVal(time), f.GetDer(time), f.GetDer2(time)};
}

// ---------------------------------------------------------------- backend

std::string ChronoBackend::name() const
{
    return "Chrono";
}

std::string ChronoBackend::label() const
{
    return "Chrono (Project Chrono 10)";
}

Capabilities ChronoBackend::capabilities() const
{
    Capabilities caps;
    caps.joints = Chrono::supportedKinds();
    caps.limits = true;
    caps.drivers = true;
    caps.drag = true;
    caps.kinematics = true;
    caps.dynamics = true;
    caps.redundancy = true;
    caps.conflicts = true;
    caps.nativeFormat = "chrono-json";
    return caps;
}

std::unique_ptr<Session> ChronoBackend::createSession()
{
    return std::make_unique<ChronoSession>();
}

// ---------------------------------------------------------------- session

struct ChronoSession::Impl
{
    Model model;
    bool loaded = false;
    std::string refused;
    std::vector<Id> unsupported;

    std::unique_ptr<ch::ChSystemNSC> sys;
    std::shared_ptr<ch::ChBody> world;
    std::vector<std::shared_ptr<ch::ChBody>> bodies;
    std::vector<char> pinned;
    // Per body, the weight of the solve and the farthest of its frames.
    std::vector<double> weights;
    std::vector<double> lever;
    std::vector<LinkRecord> links;
    // The joints held by the neutral equations (NeutralLink), in model order.
    std::vector<std::shared_ptr<NeutralLink>> neutral;
    // Joints whose two frames are on one rigid thing (one body, or two fixed
    // ones): no link; the checker evaluates them.
    std::vector<Id> unlinked;
    // Per frame, the branch turn applied when it is a joint's frame J.
    std::vector<ch::ChQuaterniond> branch;
    // The largest speed ratio of a gear: a kinematic run's step is shortened
    // so that the faster gear also turns less than maxDriverStep.
    double gearRatio = 1.0;

    // Limits: each RotationZ limit's angle, unwrapped across solves, and the
    // links holding active bounds.
    std::vector<double> limitAngle;
    std::vector<std::shared_ptr<ch::ChLinkBase>> boundLinks;

    std::vector<Id> dragBodies;
    int iterations = 0;

    double tight = 1e-11;
    double loose = 1e-9;
    double stepTolerance = 1e-11;
    int maxNewton = 50;
    int maxRounds = 200;
    // Rounds of the plain step before the reduced Newton step is tried, and
    // the most freedoms it is tried with (each costs two Jacobians).
    int newtonAfter = 6;
    Eigen::Index maxNewtonFreedoms = 120;

    // ------------------------------------------------------------ frames

    std::shared_ptr<ch::ChBody> bodyOf(Id frame) const
    {
        const Id body = model.frames[frame].body;
        return body == NoId ? world : bodies[body];
    }

    bool validFrame(Id frame) const
    {
        if (frame >= model.frames.size()) {
            return false;
        }
        const Id body = model.frames[frame].body;
        return body == NoId || body < bodies.size();
    }

    ch::ChFramed localOf(Id frame) const
    {
        return toCh(model.frames[frame].local);
    }

    ch::ChFramed globalOf(Id frame) const
    {
        return localOf(frame) >> *bodyOf(frame);
    }

    // Frame J as a driver or limit sees it: turned with its joint's branch
    // when that keeps J's x axis (the half turn about x), so thetaZ is the
    // same angle and Chrono's motor measures it about a common z.
    ch::ChFramed driverLocalOf(Id frame) const
    {
        ch::ChFramed local = localOf(frame);
        const ch::ChQuaterniond& b = branch[frame];
        if (std::abs(b.e1()) == 1.0) {
            local.SetRot(local.GetRot() * b);
        }
        return local;
    }

    bool isRigid(Id fi, Id fj) const
    {
        auto bi = bodyOf(fi);
        auto bj = bodyOf(fj);
        return bi == bj || (bi->IsFixed() && bj->IsFixed());
    }

    // thetaZ(I, J): the angle of xJ about zI from xI, (sin, cos).
    std::pair<double, double> rawAngleZ(Id fi, Id fj) const
    {
        const ch::ChFramed i = globalOf(fi);
        const ch::ChFramed j = globalOf(fj);
        const ch::ChVector3d xj = j.GetRotMat().GetAxisX();
        return {ch::Vdot(i.GetRotMat().GetAxisY(), xj), ch::Vdot(i.GetRotMat().GetAxisX(), xj)};
    }

    double slideZ(Id fi, Id fj) const
    {
        const ch::ChFramed i = globalOf(fi);
        const ch::ChFramed j = globalOf(fj);
        return ch::Vdot(j.GetPos() - i.GetPos(), i.GetRotMat().GetAxisZ());
    }

    // ------------------------------------------------------------ building

    template<typename T>
    std::shared_ptr<T> mate(Id fi, Id fj, const ch::ChQuaterniond& turn)
    {
        auto link = std::make_shared<T>();
        ch::ChFramed fj1 = localOf(fj);
        fj1.SetRot(fj1.GetRot() * turn);
        link->Initialize(bodyOf(fj), bodyOf(fi), true, fj1, localOf(fi));
        sys->AddLink(link);
        return link;
    }

    void record(const std::shared_ptr<ch::ChLinkBase>& link,
                Owner owner,
                Id id,
                std::vector<Category> rows)
    {
        links.push_back({link, owner, id, std::move(rows)});
    }

    template<typename T>
    void mateJoint(const Joint& joint, const ch::ChQuaterniond& turn)
    {
        auto link = mate<T>(joint.frameI, joint.frameJ, turn);
        record(link, Owner::Joint, joint.id, mateRows(*link));
    }

    void orthogonal(const Joint& joint)
    {
        auto link = std::make_shared<ch::ChLinkMateOrthogonal>();
        const ch::ChFramed fi = localOf(joint.frameI);
        const ch::ChFramed fj = localOf(joint.frameJ);
        link->Initialize(bodyOf(joint.frameJ),
                         bodyOf(joint.frameI),
                         true,
                         fj.GetPos(),
                         fi.GetPos(),
                         fj.GetRotMat().GetAxisZ(),
                         fi.GetRotMat().GetAxisZ());
        sys->AddLink(link);
        record(link, Owner::Joint, joint.id, {Category::Perpendicular});
    }

    void distanceZ(Id fi, Id fj, double value, Owner owner, Id id)
    {
        auto link = std::make_shared<ch::ChLinkMateDistanceZ>();
        link->Initialize(bodyOf(fj), bodyOf(fi), true, localOf(fj), localOf(fi));
        link->SetDistance(value);
        sys->AddLink(link);
        record(link,
               owner,
               id,
               {owner == Owner::Joint ? Category::Displacement : Category::Essential});
    }

    // A joint held by the neutral layer's own equations (NeutralLink); the
    // reason it cannot be, or empty.
    std::string neutralJoint(const Joint& joint)
    {
        auto ref = [&](Id frame) {
            NeutralLink::FrameRef r;
            const Id body = model.frames[frame].body;
            r.body = body == NoId ? nullptr : bodies[body].get();
            r.local = localOf(frame);
            return r;
        };
        const NeutralLink::FrameRef fi = ref(joint.frameI);
        const NeutralLink::FrameRef fj = ref(joint.frameJ);
        NeutralLink::FrameRef fk;
        const bool carrier = validFrame(joint.frameK);
        if (carrier) {
            fk = ref(joint.frameK);
        }
        std::shared_ptr<const CurveGeometry> curve;
        if (joint.kind == JointKind::PointOnCurve) {
            if (joint.curve < model.curves.size()) {
                curve = model.curves[joint.curve].geometry;
            }
            if (!curve) {
                return "it holds a point on a curve the model does not have";
            }
        }
        auto link =
            std::make_shared<NeutralLink>(joint, fi, fj, carrier ? &fk : nullptr, std::move(curve));
        if (link->rowCategories().empty()) {
            // The Reference fails the same.
            return "its frames are coaxial and it has no carrier frame to measure the turns from";
        }
        sys->AddLink(link);
        std::vector<Category> rows;
        for (eq::Category c : link->rowCategories()) {
            rows.push_back(static_cast<Category>(c));
        }
        record(link, Owner::Joint, joint.id, std::move(rows));
        neutral.push_back(link);
        gearRatio = std::max(gearRatio, link->speedRatio());
        return {};
    }

    // The rows of a Gear. Ondsel's relation, orbit(J, I) + ratio orbit(I,
    // J) = c with ratio = value / value2, is ChLinkLockGear's a1 + a2 / tau
    // = phase with shaft 1 = J and tau = value2 / value: both measure each
    // gear's turn from the line between the shafts. Between coaxial frames
    // (no such line) or shafts that are not parallel (ChLinkLockGear takes
    // them for a bevel gear), the neutral equations hold it.
    std::string gear(const Joint& joint)
    {
        const ch::ChFramed i = globalOf(joint.frameI);
        const ch::ChFramed j = globalOf(joint.frameJ);
        const ch::ChVector3d zi = i.GetRotMat().GetAxisZ();
        const ch::ChVector3d zj = j.GetRotMat().GetAxisZ();
        const ch::ChVector3d d = j.GetPos() - i.GetPos();
        if (1.0 - std::abs(ch::Vdot(zi, zj)) <= coaxialTolerance
            && ch::Vcross(d, zi).Length() <= coaxialTolerance) {
            return neutralJoint(joint);
        }
        if (std::abs(ch::Vdot(zi, zj)) <= 0.96) {
            return neutralJoint(joint);
        }
        const double tau = joint.value2 / joint.value;
        auto link = std::make_shared<ch::ChLinkLockGear>();
        link->Initialize(bodyOf(joint.frameJ),
                         bodyOf(joint.frameI),
                         true,
                         localOf(joint.frameJ),
                         localOf(joint.frameI));
        link->SetFrameShaft1(localOf(joint.frameJ));
        link->SetFrameShaft2(localOf(joint.frameI));
        link->SetTransmissionRatio(tau);
        link->SetEpicyclic(false);
        link->SetEnforcePhase(true);
        link->SetPhaseAutomatically();
        sys->AddLink(link);
        record(link, Owner::Joint, joint.id, {Category::Essential});
        gearRatio = std::max({gearRatio, std::abs(tau), std::abs(1.0 / tau)});
        return {};
    }

    // A joint as Chrono's links; the reason it cannot be, or empty.
    std::string addJoint(const Joint& joint)
    {
        if (std::string why = Chrono::refusal(joint); !why.empty()) {
            return why;
        }
        if (!validFrame(joint.frameI) || !validFrame(joint.frameJ)) {
            return {};
        }
        if (isRigid(joint.frameI, joint.frameJ)) {
            unlinked.push_back(joint.id);
            return {};
        }
        if (!Chrono::nativeLink(joint)) {
            return neutralJoint(joint);
        }
        const ch::ChQuaterniond relq =
            globalOf(joint.frameI).GetRot().GetConjugate() * globalOf(joint.frameJ).GetRot();
        const ch::ChQuaterniond turn = branchOf(joint.kind, ch::ChMatrix33<>(relq));
        branch[joint.frameJ] = turn;
        switch (joint.kind) {
            case JointKind::Fixed:
                mateJoint<ch::ChLinkMateFix>(joint, turn);
                break;
            case JointKind::Revolute:
                mateJoint<ch::ChLinkMateRevolute>(joint, turn);
                break;
            case JointKind::Cylindrical:
                mateJoint<ch::ChLinkMateCylindrical>(joint, turn);
                break;
            case JointKind::Prismatic:
                mateJoint<ch::ChLinkMatePrismatic>(joint, turn);
                break;
            case JointKind::Spherical:
                mateJoint<ch::ChLinkMateSpherical>(joint, turn);
                break;
            case JointKind::Planar: {
                auto link = mate<ch::ChLinkMatePlanar>(joint.frameI, joint.frameJ, turn);
                link->SetDistance(joint.value);
                record(link, Owner::Joint, joint.id, mateRows(*link));
                break;
            }
            case JointKind::ParallelZ:
                mateJoint<ch::ChLinkMateParallel>(joint, turn);
                break;
            case JointKind::PerpendicularZ:
            case JointKind::AngleZ:  // a right angle (nativeLink())
                orthogonal(joint);
                break;
            case JointKind::PointInPlane:
                distanceZ(joint.frameI, joint.frameJ, joint.value, Owner::Joint, joint.id);
                break;
            case JointKind::LineInPlane:
                distanceZ(joint.frameI, joint.frameJ, joint.value, Owner::Joint, joint.id);
                orthogonal(joint);
                break;
            case JointKind::PointOnLine: {
                auto link =
                    std::make_shared<ch::ChLinkMateGeneric>(true, true, false, false, false, false);
                link->Initialize(bodyOf(joint.frameJ),
                                 bodyOf(joint.frameI),
                                 true,
                                 localOf(joint.frameJ),
                                 localOf(joint.frameI));
                sys->AddLink(link);
                record(link, Owner::Joint, joint.id, mateRows(*link));
                break;
            }
            case JointKind::PointPointDistance: {
                auto link = std::make_shared<ch::ChLinkDistance>();
                link->Initialize(bodyOf(joint.frameJ),
                                 bodyOf(joint.frameI),
                                 true,
                                 localOf(joint.frameJ).GetPos(),
                                 localOf(joint.frameI).GetPos(),
                                 false,
                                 joint.value);
                sys->AddLink(link);
                record(link, Owner::Joint, joint.id, {Category::Displacement});
                break;
            }
            case JointKind::Gear:
                return gear(joint);
            default:
                break;
        }
        return {};
    }

    // The motors of the drivers (a kinematic run only).
    std::vector<std::shared_ptr<ch::ChLinkBase>> motors;

    void addMotors()
    {
        for (const auto& driver : model.drivers) {
            if (!validFrame(driver.frameI) || !validFrame(driver.frameJ)
                || isRigid(driver.frameI, driver.frameJ)) {
                continue;
            }
            const ch::ChFramed f1 = driverLocalOf(driver.frameJ);
            const ch::ChFramed f2 = localOf(driver.frameI);
            auto drives = [](const Formula& formula) {
                return !(formula.text.empty() && !formula.tree && !formula.function)
                    && static_cast<bool>(formula.evaluator());
            };
            if (driver.kind != DriverKind::TranslationZ && drives(driver.rotation)) {
                auto motor = std::make_shared<ch::ChLinkMotorRotationAngle>();
                motor->SetSpindleConstraint(ch::ChLinkMotorRotation::SpindleConstraint::FREE);
                motor->Initialize(bodyOf(driver.frameJ), bodyOf(driver.frameI), true, f1, f2);
                motor->SetMotorFunction(
                    std::make_shared<FormulaChFunction>(driver.rotation.evaluator()));
                sys->AddLink(motor);
                record(motor, Owner::Driver, driver.id, {Category::Essential});
                motors.push_back(motor);
            }
            if (driver.kind != DriverKind::RotationZ && drives(driver.translation)) {
                auto motor = std::make_shared<ch::ChLinkMotorLinearPosition>();
                motor->SetGuideConstraint(ch::ChLinkMotorLinear::GuideConstraint::FREE);
                motor->Initialize(bodyOf(driver.frameJ), bodyOf(driver.frameI), true, f1, f2);
                motor->SetMotorFunction(
                    std::make_shared<FormulaChFunction>(driver.translation.evaluator()));
                sys->AddLink(motor);
                record(motor, Owner::Driver, driver.id, {Category::Essential});
                motors.push_back(motor);
            }
        }
        prepare();
    }

    void removeMotors()
    {
        for (const auto& motor : motors) {
            sys->RemoveLink(motor);
        }
        std::erase_if(links, [](const LinkRecord& r) {
            return r.owner == Owner::Driver;
        });
        motors.clear();
        prepare();
    }

    // ------------------------------------------------------------ state

    // After any change of links: offsets, counts, the descriptor.
    void prepare()
    {
        sys->Setup();
        sys->Update(ch::UpdateFlags::UPDATE_ALL);
        sys->DescriptorPrepareInject();
    }

    // A state of the system: Chrono's, and the angles the neutral links
    // have committed there (their turns counted).
    struct State
    {
        ch::ChState x;
        ch::ChStateDelta v;
        double t = 0.0;
        std::vector<std::array<double, 2>> angles;
    };

    State gather() const
    {
        State s;
        s.x.setZero(sys->GetNumCoordsPosLevel(), sys.get());
        s.v.setZero(sys->GetNumCoordsVelLevel(), sys.get());
        sys->StateGather(s.x, s.v, s.t);
        // Kinematics: no velocities are kept.
        s.v.setZero(sys->GetNumCoordsVelLevel(), sys.get());
        s.angles = linkAngles();
        return s;
    }

    void scatter(const State& s)
    {
        setLinkAngles(s.angles);
        sys->StateScatter(s.x, s.v, s.t, ch::UpdateFlags::UPDATE_ALL);
    }

    std::vector<std::array<double, 2>> linkAngles() const
    {
        std::vector<std::array<double, 2>> out;
        out.reserve(neutral.size());
        for (const auto& link : neutral) {
            out.push_back(link->angles());
        }
        return out;
    }

    void setLinkAngles(const std::vector<std::array<double, 2>>& angles)
    {
        for (std::size_t k = 0; k < neutral.size() && k < angles.size(); ++k) {
            neutral[k]->setAngles(angles[k]);
        }
    }

    // The neutral links take the angles the current poses read: the poses
    // are accepted (the Reference's commit).
    void commitAngles()
    {
        for (const auto& link : neutral) {
            link->commit();
        }
    }

    State stepped(const State& s, const ch::ChStateDelta& dx, double alpha) const
    {
        State t = s;
        ch::ChStateDelta step(dx.size(), sys.get());
        step = dx * alpha;
        sys->StateIncrementX(t.x, s.x, step);
        return t;
    }

    ch::ChVectorDynamic<> constraints() const
    {
        ch::ChVectorDynamic<> qc(sys->GetNumConstraints());
        qc.setZero();
        sys->LoadConstraint_C(qc, 1.0);
        return qc;
    }

    static double infNorm(const ch::ChVectorDynamic<>& v)
    {
        return v.size() ? v.lpNorm<Eigen::Infinity>() : 0.0;
    }

    // [M Cq'; Cq 0] [dx; -l] = [R; -qc], by Chrono's solver.
    bool kkt(ch::ChStateDelta& dx,
             const ch::ChVectorDynamic<>& R,
             const ch::ChVectorDynamic<>& qc,
             const State& s)
    {
        dx.setZero(sys->GetNumCoordsVelLevel(), sys.get());
        if (dx.size() == 0) {
            return true;
        }
        ch::ChVectorDynamic<> L(sys->GetNumConstraints());
        L.setZero();
        const bool ok = sys->StateSolveCorrection(dx,
                                                  L,
                                                  R,
                                                  qc,
                                                  1.0,
                                                  0.0,
                                                  0.0,
                                                  s.x,
                                                  s.v,
                                                  s.t,
                                                  false,
                                                  ch::UpdateFlags::UPDATE_ALL_NO_VISUAL,
                                                  true,
                                                  true);
        return ok && dx.allFinite();
    }

    std::vector<Pose> current() const
    {
        std::vector<Pose> out;
        out.reserve(bodies.size());
        for (const auto& b : bodies) {
            out.push_back({b->GetPos(), b->GetRot()});
        }
        return out;
    }

    void setPoses(const std::vector<Pose>& poses)
    {
        for (std::size_t b = 0; b < bodies.size() && b < poses.size(); ++b) {
            if (!bodies[b]->IsFixed()) {
                bodies[b]->SetPos(poses[b].p);
                bodies[b]->SetRot(poses[b].q);
            }
        }
        sys->Update(ch::UpdateFlags::UPDATE_ALL);
    }

    // The drag weight
    double weightOf(std::size_t b) const
    {
        return b < weights.size() ? weights[b] : 1.0;
    }

    // The body's mass: 1, or the number of bodies of a group (sec 6.22).
    double massOf(std::size_t b) const
    {
        return b < model.bodies.size() ? model.bodies[b].mass.mass : 1.0;
    }

    // The body's inertia: 1 unless centredOnJoints gave it a size (sec 6.20);
    // a turn weighs rotationWeight times that, as in the Reference.
    double inertiaOf(std::size_t b) const
    {
        return b < model.bodies.size() ? model.bodies[b].mass.principal[0] : 1.0;
    }

    void setWeights(const std::vector<double>& w)
    {
        weights.assign(bodies.size(), 1.0);
        for (std::size_t b = 0; b < bodies.size() && b < w.size(); ++b) {
            if (w[b] > 0.0) {
                weights[b] = w[b];
            }
        }
        applyMetric(false);
    }

    // The mass matrix of the KKT solves: the metric of the solve (mass w,
    // inertia w / 4); while restoring, a turn weighs as much as moving the
    // body's farthest frame by as much (the Reference's restoration metric),
    // or a far start turns a box through radians to touch a plane.
    void applyMetric(bool restoring)
    {
        for (std::size_t b = 0; b < bodies.size(); ++b) {
            const double w = b < weights.size() ? weights[b] : 1.0;
            const double turn = rotationWeight * inertiaOf(b);
            const double i = w * (restoring ? std::max(turn, lever[b] * lever[b]) : turn);
            bodies[b]->SetMass(w * massOf(b));
            bodies[b]->SetInertiaXX(ch::ChVector3d(i, i, i));
        }
    }

    // The largest turn a step makes of any body.
    double largestTurn(const ch::ChStateDelta& dx) const
    {
        double turn = 0.0;
        for (const auto& body : bodies) {
            if (!body->IsFixed()) {
                turn = std::max(turn, dx.segment(body->GetOffset_w() + 3, 3).norm());
            }
        }
        return turn;
    }

    // The metric of a solve from the target, the bodies weighted.
    double distance(const std::vector<Pose>& target) const
    {
        double sum = 0.0;
        for (std::size_t b = 0; b < bodies.size(); ++b) {
            if (bodies[b]->IsFixed()) {
                continue;
            }
            const ch::ChVector3d dp = target[b].p - bodies[b]->GetPos();
            const ch::ChVector3d dw = rotationFrom(bodies[b]->GetRot(), target[b].q);
            sum += weightOf(b)
                * (massOf(b) * dp.Length2() + rotationWeight * inertiaOf(b) * dw.Length2());
        }
        return sum;
    }

    // The target less the current poses, in Chrono's velocity coordinates.
    ch::ChStateDelta towards(const std::vector<Pose>& target) const
    {
        ch::ChStateDelta w(sys->GetNumCoordsVelLevel(), sys.get());
        w.setZero(sys->GetNumCoordsVelLevel(), sys.get());
        for (std::size_t b = 0; b < bodies.size(); ++b) {
            if (bodies[b]->IsFixed()) {
                continue;
            }
            const unsigned int off = bodies[b]->GetOffset_w();
            const ch::ChVector3d dp = target[b].p - bodies[b]->GetPos();
            const ch::ChVector3d dw = rotationFrom(bodies[b]->GetRot(), target[b].q);
            for (int k = 0; k < 3; ++k) {
                w(off + k) = dp[k];
                w(off + 3 + k) = dw[k];
            }
        }
        return w;
    }

    // ------------------------------------------------------------ solving

    // Newton onto the constraints, least M-norm steps in the restoration
    // metric, no body turned more than a radian per step, backtracking on |C|.
    bool restore()
    {
        applyMetric(true);
        const bool ok = restoreSteps();
        applyMetric(false);
        return ok;
    }

    // The neutral links' angles are committed at every pose accepted, as the
    // Reference commits them.
    bool restoreSteps()
    {
        const bool ok = newtonSteps();
        if (ok) {
            commitAngles();
        }
        return ok;
    }

    bool newtonSteps()
    {
        State s = gather();
        ch::ChVectorDynamic<> c = constraints();
        double cn = infNorm(c);
        if (!(cn == cn)) {
            return false;
        }
        for (int it = 0; it < maxNewton && cn > tight; ++it) {
            ch::ChStateDelta dx;
            ch::ChVectorDynamic<> R(sys->GetNumCoordsVelLevel());
            R.setZero();
            if (!kkt(dx, R, c, s)) {
                scatter(s);
                return false;
            }
            ++iterations;
            const double c2 = c.norm();
            double alpha = std::min(1.0, 1.0 / std::max(largestTurn(dx), 1e-300));
            for (;;) {
                State t = stepped(s, dx, alpha);
                scatter(t);
                ch::ChVectorDynamic<> c1 = constraints();
                const double n1 = infNorm(c1);
                if (n1 == n1 && (c1.norm() < c2 * (1.0 - 1e-4 * alpha) || n1 <= tight)) {
                    commitAngles();
                    s = t;
                    s.angles = linkAngles();
                    c = c1;
                    cn = n1;
                    break;
                }
                alpha *= 0.5;
                if (alpha < 1e-6) {
                    scatter(s);
                    return cn <= loose;
                }
            }
            if (alpha * infNorm(dx) < 1e-15) {
                break;
            }
        }
        return cn <= loose;
    }

    // Chrono's constraint Jacobian at the current state, dense.
    MatX jacobian()
    {
        ch::ChSparseMatrix Cq;
        sys->GetConstraintJacobianMatrix(Cq);
        return MatX(Cq);
    }

    // Cq' lambda at the current state, Cq kept sparse.
    VecX jacobianTransposeTimes(const VecX& lambda)
    {
        ch::ChSparseMatrix Cq;
        sys->GetConstraintJacobianMatrix(Cq);
        if (Cq.rows() != lambda.size()) {
            return VecX();
        }
        return Cq.transpose() * lambda;
    }

    // M (x - target), the gradient of the metric, in velocity coordinates.
    VecX metricGradient(const std::vector<Pose>& target)
    {
        ch::ChVectorDynamic<> R(sys->GetNumCoordsVelLevel());
        R.setZero();
        if (R.size()) {
            sys->LoadResidual_Mv(R, towards(target), 1.0);
        }
        return -R;
    }

    // The reduced Newton step towards the target along the constraints: the
    // null space Z of Chrono's Jacobian, the multipliers that make the
    // gradient of the Lagrangian normal to it, its Hessian on Z by forward
    // differences of that gradient (Chrono's Jacobian at the perturbed
    // states), and the step Z (Z' H Z)^-1 Z' g when Z' H Z is positive
    // definite. False when it is not, or Z is too large to be worth it.
    bool newtonStep(const std::vector<Pose>& target, const State& s, ch::ChStateDelta& dx)
    {
        const MatX J = jacobian();
        const Eigen::Index n = J.cols();
        if (n == 0) {
            return false;
        }
        Eigen::ColPivHouseholderQR<MatX> qr(J.transpose());
        qr.setThreshold(rankTolerance);
        const Eigen::Index rank = J.rows() ? qr.rank() : 0;
        const Eigen::Index free = n - rank;
        if (free <= 0 || free > maxNewtonFreedoms) {
            return false;
        }
        const MatX Q = qr.householderQ();
        const MatX Z = Q.rightCols(free);
        const VecX g0 = metricGradient(target);
        if (g0.size() != n) {
            return false;
        }
        VecX lambda = VecX::Zero(J.rows());
        if (J.rows()) {
            lambda = qr.solve(-g0);
        }
        auto lagrangian = [&](const State& y) {
            scatter(y);
            VecX g = metricGradient(target);
            const VecX jl = jacobianTransposeTimes(lambda);
            if (jl.size() == g.size()) {
                g += jl;
            }
            return g;
        };
        // Forward differences: one state per free direction, not two; each is
        // an update of the whole system, and with a hundred free directions
        // (a chain of 200 parts) the central ones made a drag step take 2 s.
        // The Hessian only orients the step, which is restored and taken while
        // it brings the poses closer.
        const double h = 1e-6;
        const VecX base = lagrangian(s);
        MatX HZ(n, free);
        for (Eigen::Index k = 0; k < free; ++k) {
            ch::ChStateDelta zk(n, sys.get());
            zk = Z.col(k);
            HZ.col(k) = (lagrangian(stepped(s, zk, h)) - base) / h;
        }
        scatter(s);
        MatX Hr = Z.transpose() * HZ;
        Hr = 0.5 * (Hr + Hr.transpose());
        Eigen::LLT<MatX> llt(Hr);
        if (llt.info() != Eigen::Success) {
            return false;
        }
        const VecX gr = Z.transpose() * (g0 + J.transpose() * lambda);
        const VecX step = Z * llt.solve(-gr);
        if (!step.allFinite()) {
            return false;
        }
        dx.setZero(n, sys.get());
        dx = step;
        return true;
    }

    // From a pose satisfying the constraints, towards the target along them:
    // the step to the point of the linearised constraints closest to the
    // target, and after newtonAfter rounds the reduced Newton step, each
    // restored onto the constraints and taken while it brings the poses
    // closer.
    void pull(const std::vector<Pose>& target)
    {
        double dist = distance(target);
        for (int round = 0; round < maxRounds; ++round) {
            State s = gather();
            ch::ChStateDelta dx;
            bool newton = round >= newtonAfter && newtonStep(target, s, dx);
            if (!newton) {
                ch::ChVectorDynamic<> c = constraints();
                ch::ChVectorDynamic<> R(sys->GetNumCoordsVelLevel());
                R.setZero();
                if (R.size()) {
                    sys->LoadResidual_Mv(R, towards(target), 1.0);
                }
                if (!kkt(dx, R, c, s)) {
                    scatter(s);
                    return;
                }
            }
            ++iterations;
            const double size = infNorm(dx);
            if (!(size > stepTolerance)) {
                return;
            }
            bool taken = false;
            double alpha = std::min(1.0, 1.0 / std::max(largestTurn(dx), 1e-300));
            for (; alpha >= 1e-6; alpha *= 0.5) {
                scatter(stepped(s, dx, alpha));
                if (restore()) {
                    const double d = distance(target);
                    if (d < dist - 1e-15 * std::max(1.0, dist)) {
                        dist = d;
                        taken = true;
                        break;
                    }
                }
                scatter(s);
            }
            if (!taken || alpha * size <= stepTolerance) {
                return;
            }
        }
    }

    // ------------------------------------------------------------ limits

    // Each RotationZ limit's angle, read OndselSolver's way at load.
    void readLimits()
    {
        limitAngle.assign(model.limits.size(), 0.0);
        for (const auto& limit : model.limits) {
            if (limit.kind == LimitKind::RotationZ && validFrame(limit.frameI)
                && validFrame(limit.frameJ)) {
                auto [s, c] = rawAngleZ(limit.frameI, limit.frameJ);
                limitAngle[limit.id] = initialAngle(s, c);
            }
        }
    }

    double limitValue(const Limit& limit, std::vector<double>& angles) const
    {
        if (limit.kind == LimitKind::TranslationZ) {
            return slideZ(limit.frameI, limit.frameJ);
        }
        auto [s, c] = rawAngleZ(limit.frameI, limit.frameJ);
        const double a = angles[limit.id] + wrapAngle(std::atan2(s, c) - angles[limit.id]);
        angles[limit.id] = a;
        return a;
    }

    // A link holding a limit's value at a bound, added to the system: a
    // stop, or a violated bound held.
    std::shared_ptr<ch::ChLinkBase> boundLink(const Limit& limit, double bound)
    {
        if (limit.kind == LimitKind::TranslationZ) {
            auto link = std::make_shared<ch::ChLinkMateDistanceZ>();
            link->Initialize(bodyOf(limit.frameJ),
                             bodyOf(limit.frameI),
                             true,
                             localOf(limit.frameJ),
                             localOf(limit.frameI));
            link->SetDistance(bound);
            sys->AddLink(link);
            return link;
        }
        // Chrono's motor holds the angle modulo a turn; the bound is the
        // turn the angle is counted in.
        auto link = std::make_shared<ch::ChLinkMotorRotationAngle>();
        link->SetSpindleConstraint(ch::ChLinkMotorRotation::SpindleConstraint::FREE);
        link->Initialize(bodyOf(limit.frameJ),
                         bodyOf(limit.frameI),
                         true,
                         driverLocalOf(limit.frameJ),
                         localOf(limit.frameI));
        link->SetMotorFunction(std::make_shared<ch::ChFunctionConst>(bound));
        sys->AddLink(link);
        return link;
    }

    // Holds every bound the current poses violate; false when none is.
    bool holdViolatedBounds(std::vector<double>& angles)
    {
        bool held = false;
        for (const auto& limit : model.limits) {
            if (!validFrame(limit.frameI) || !validFrame(limit.frameJ)
                || isRigid(limit.frameI, limit.frameJ)) {
                continue;
            }
            const double value = limitValue(limit, angles);
            std::optional<double> bound;
            if (limit.min && value < *limit.min - limit.tolerance) {
                bound = limit.min;
            }
            else if (limit.max && value > *limit.max + limit.tolerance) {
                bound = limit.max;
            }
            if (!bound) {
                continue;
            }
            boundLinks.push_back(boundLink(limit, *bound));
            held = true;
        }
        if (held) {
            prepare();
        }
        return held;
    }

    void releaseBounds()
    {
        if (boundLinks.empty()) {
            return;
        }
        for (const auto& link : boundLinks) {
            sys->RemoveLink(link);
        }
        boundLinks.clear();
        prepare();
    }

    // ------------------------------------------------------------ rank

    // Each constraint row's owner and category, from its link's offset
    // (after prepare()).
    std::vector<RowInfo> rowInfo(Eigen::Index rows) const
    {
        std::vector<RowInfo> info(static_cast<std::size_t>(rows));
        for (const auto& record : links) {
            const unsigned int offset = record.link->GetOffset_L();
            for (std::size_t k = 0; k < record.rows.size(); ++k) {
                const std::size_t r = offset + k;
                if (r < info.size()) {
                    info[r] = {record.owner, record.id, record.rows[k]};
                }
            }
        }
        return info;
    }

    // Which rows of J depend on none of the rows before them in OndselSolver's
    // order (its categories, then model order), by Gram-Schmidt with
    // reorthogonalisation (the Reference's); 'kept' counts them.
    static std::vector<char>
    independentRows(const MatX& J, const std::vector<RowInfo>& info, int& kept)
    {
        const Eigen::Index rows = J.rows();
        const Eigen::Index cols = J.cols();
        std::vector<int> order(static_cast<std::size_t>(rows));
        for (Eigen::Index i = 0; i < rows; ++i) {
            order[i] = static_cast<int>(i);
        }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return static_cast<int>(info[a].category) < static_cast<int>(info[b].category);
        });
        std::vector<char> independent(static_cast<std::size_t>(rows), 0);
        kept = 0;
        if (cols > 0) {
            MatX Q(cols, std::min(cols, rows) + 1);
            VecX v(cols);
            for (int i : order) {
                if (kept >= cols) {
                    break;
                }
                v = J.row(i).transpose();
                const double norm0 = v.norm();
                if (!(norm0 > rankTolerance)) {
                    continue;
                }
                for (int pass = 0; pass < 2 && kept > 0; ++pass) {
                    v -= Q.leftCols(kept) * (Q.leftCols(kept).transpose() * v);
                }
                const double rest = v.norm();
                if (rest > rankTolerance * std::max(1.0, norm0)) {
                    Q.col(kept++) = v / rest;
                    independent[i] = 1;
                }
            }
        }
        return independent;
    }

    // What removeRedundant() did: the rows it took out, and per joint whether
    // its reaction is no longer unique -- the joint's own rows taken out, or
    // rows a taken out one repeated. 'conflicting': joints with a dependent
    // row they do not satisfy; nothing is taken out then.
    struct Redundancy
    {
        int removed = 0;
        std::vector<char> notUnique;
        std::vector<Id> conflicting;
    };

    // A dynamic or static run's redundant joints (docs/AssemblyDynamics.md
    // sec 6.1): the joint rows that depend on the rows before them, in
    // OndselSolver's order as analyse() finds them, are taken out of the
    // system -- a mate's coordinate, a neutral link's equation, a link with
    // no row left -- so that the choice is the same on every backend, and
    // the solve is regular: Chrono's LU of a singular system leaves the
    // multipliers at 1e19. The motion is the same; the reactions the rows
    // shared are not unique. 'length' is a metre in the model's lengths
    // (the conflict test's scale).
    Redundancy removeRedundant(double length)
    {
        Redundancy out;
        out.notUnique.assign(model.joints.size(), 0);
        prepare();
        ch::ChSparseMatrix Cq;
        sys->GetConstraintJacobianMatrix(Cq);
        const MatX J(Cq);
        const ch::ChVectorDynamic<> c = constraints();
        const std::vector<RowInfo> info = rowInfo(J.rows());
        int kept = 0;
        const std::vector<char> independent = independentRows(J, info, kept);
        if (kept == J.rows()) {
            return out;
        }
        // The rows each dependent one repeats: its combination of the
        // independent rows
        std::vector<Eigen::Index> basis;
        for (Eigen::Index i = 0; i < J.rows(); ++i) {
            if (independent[i]) {
                basis.push_back(i);
            }
        }
        MatX B(J.cols(), static_cast<Eigen::Index>(basis.size()));
        for (std::size_t k = 0; k < basis.size(); ++k) {
            B.col(static_cast<Eigen::Index>(k)) = J.row(basis[k]).transpose();
        }
        const Eigen::ColPivHouseholderQR<MatX> qr(B);
        for (Eigen::Index i = 0; i < J.rows(); ++i) {
            const RowInfo& row = info[i];
            if (independent[i] || row.owner != Owner::Joint || row.id >= model.joints.size()) {
                continue;
            }
            const double tolerance =
                conflictTolerance * (row.category == Category::Displacement ? length : 1.0);
            if (!(std::abs(c(i)) <= tolerance)) {
                if (std::find(out.conflicting.begin(), out.conflicting.end(), row.id)
                    == out.conflicting.end()) {
                    out.conflicting.push_back(row.id);
                }
                continue;
            }
            out.notUnique[row.id] = 1;
            const double norm = J.row(i).norm();
            if (basis.empty() || !(norm > rankTolerance)) {
                continue;
            }
            const VecX a = qr.solve(VecX(J.row(i).transpose()));
            for (std::size_t k = 0; k < basis.size(); ++k) {
                const auto at = static_cast<Eigen::Index>(k);
                const RowInfo& other = info[basis[k]];
                if (other.owner == Owner::Joint && other.id < model.joints.size()
                    && std::abs(a(at)) * B.col(at).norm() > 1e-8 * norm) {
                    out.notUnique[other.id] = 1;
                }
            }
        }
        if (!out.conflicting.empty()) {
            return out;
        }
        for (auto& record : links) {
            if (record.owner != Owner::Joint) {
                continue;
            }
            const unsigned int offset = record.link->GetOffset_L();
            std::vector<char> drop(record.rows.size(), 0);
            int dropped = 0;
            for (std::size_t k = 0; k < record.rows.size(); ++k) {
                const std::size_t r = offset + k;
                if (r < independent.size() && !independent[r]) {
                    drop[k] = 1;
                    ++dropped;
                }
            }
            if (dropped == 0) {
                continue;
            }
            if (static_cast<std::size_t>(dropped) == record.rows.size()) {
                // Its reaction reads zero from then on
                sys->RemoveLink(record.link);
                record.rows.clear();
            }
            else if (auto mate = std::dynamic_pointer_cast<ch::ChLinkMateGeneric>(record.link)) {
                std::array<bool, 6> flags {mate->IsConstrainedX(),
                                           mate->IsConstrainedY(),
                                           mate->IsConstrainedZ(),
                                           mate->IsConstrainedRx(),
                                           mate->IsConstrainedRy(),
                                           mate->IsConstrainedRz()};
                std::size_t k = 0;
                for (bool& flag : flags) {
                    if (flag) {
                        flag = !(k < drop.size() && drop[k]);
                        ++k;
                    }
                }
                mate->SetConstrainedCoords(flags[0],
                                           flags[1],
                                           flags[2],
                                           flags[3],
                                           flags[4],
                                           flags[5]);
                record.rows = mateRows(*mate);
            }
            else if (auto neutralLink = std::dynamic_pointer_cast<NeutralLink>(record.link)) {
                neutralLink->dropRows(drop);
                record.rows.clear();
                for (eq::Category category : neutralLink->rowCategories()) {
                    record.rows.push_back(static_cast<Category>(category));
                }
            }
            else {
                // No other link has more than one row
                continue;
            }
            out.removed += dropped;
        }
        prepare();
        return out;
    }

    // ------------------------------------------------------------ runs

    std::vector<BodyPose> posesNow() const
    {
        std::vector<BodyPose> out;
        out.reserve(bodies.size());
        for (std::size_t b = 0; b < bodies.size(); ++b) {
            out.push_back(
                {static_cast<Id>(b), toPlacement(bodies[b]->GetPos(), bodies[b]->GetRot())});
        }
        return out;
    }

    // The pose closest to the target satisfying the joints and the limits.
    Result run(const std::vector<Pose>& target, const std::vector<double>& weights)
    {
        Result result;
        if (!loaded) {
            result.message = "no model loaded";
            return result;
        }
        if (!refused.empty()) {
            result.status = Status::Unsupported;
            result.message = refused;
            return result;
        }
        const std::vector<BodyPose> before = posesNow();
        const std::vector<Pose> start = current();
        const auto startAngles = linkAngles();
        std::vector<double> angles = limitAngle;
        iterations = 0;
        setWeights(weights);
        bool ok = false;
        try {
            for (int round = 0; round < 8; ++round) {
                ok = restore();
                if (!ok) {
                    break;
                }
                pull(target);
                std::vector<double> trial = angles;
                if (!holdViolatedBounds(trial)) {
                    angles = trial;
                    break;
                }
            }
        }
        catch (const std::exception& e) {
            ok = false;
            result.message = e.what();
        }
        releaseBounds();
        setWeights({});
        if (ok) {
            limitAngle = angles;
            result.status = Status::Ok;
        }
        else {
            setLinkAngles(startAngles);
            setPoses(start);
            result.status = Status::NotConverged;
            if (result.message.empty()) {
                result.message = "Chrono's assembly did not converge";
            }
        }
        result.iterations = iterations;
        result.residual = infNorm(constraints());
        const std::vector<BodyPose> after = posesNow();
        for (std::size_t b = 0; b < after.size(); ++b) {
            if (!after[b].placement.isSame(before[b].placement)) {
                result.moved.push_back(after[b]);
            }
        }
        return result;
    }

    // ------------------------------------------------------------ kinematics

    // Velocities that keep the constraints as time goes on: Cq v = -Ct, least
    // M-norm. The motors' Ct are their functions' exact rates.
    bool velocity(ch::ChStateDelta& v)
    {
        const State s = gather();
        ch::ChVectorDynamic<> qc(sys->GetNumConstraints());
        qc.setZero();
        sys->LoadConstraint_Ct(qc, 1.0);
        ch::ChVectorDynamic<> R(sys->GetNumCoordsVelLevel());
        R.setZero();
        return kkt(v, R, qc, s);
    }

    bool driversFinite(double time) const
    {
        for (const auto& driver : model.drivers) {
            for (const Formula* formula : {&driver.rotation, &driver.translation}) {
                if (formula->text.empty() && !formula->tree && !formula->function) {
                    continue;
                }
                auto evaluate = formula->evaluator();
                if (evaluate && !std::isfinite(evaluate(time).value)) {
                    return false;
                }
            }
        }
        return true;
    }

    // How many steps from t0 to t1 keep every driven angle (and the gears it
    // turns) within maxDriverStep per step; 0 when a driver leaves its domain.
    int substeps(double t0, double t1) const
    {
        std::vector<FormulaFunction> angles;
        for (const auto& driver : model.drivers) {
            if (driver.kind != DriverKind::TranslationZ) {
                if (auto f = driver.rotation.evaluator()) {
                    angles.push_back(f);
                }
            }
        }
        const double bound = maxDriverStep / gearRatio;
        for (int n = 1; n <= 4096; n *= 2) {
            bool fine = true;
            for (const auto& f : angles) {
                double previous = f(t0).value;
                for (int k = 1; k <= n; ++k) {
                    const double value = f(t0 + (t1 - t0) * k / n).value;
                    if (!std::isfinite(value)) {
                        return 0;
                    }
                    if (std::abs(value - previous) > bound) {
                        fine = false;
                    }
                    previous = value;
                }
            }
            if (fine) {
                return n;
            }
        }
        return 4096;
    }
};

ChronoSession::ChronoSession()
    : d(std::make_unique<Impl>())
{}

ChronoSession::~ChronoSession() = default;

Status ChronoSession::load(const Model& model)
{
    d->model = model;
    d->loaded = false;
    d->refused.clear();
    d->unsupported.clear();
    d->links.clear();
    d->neutral.clear();
    d->unlinked.clear();
    d->motors.clear();
    d->boundLinks.clear();
    d->dragBodies.clear();
    d->gearRatio = 1.0;
    d->bodies.clear();
    d->world.reset();
    d->sys = std::make_unique<ch::ChSystemNSC>();
    try {
        ch::ChSystemNSC& sys = *d->sys;
        sys.SetGravitationalAcceleration(ch::ChVector3d(0, 0, 0));
        sys.SetSolver(std::make_shared<LUThenQR>());

        d->world = std::make_shared<ch::ChBody>();
        d->world->SetFixed(true);
        sys.AddBody(d->world);
        for (const auto& body : d->model.bodies) {
            auto b = std::make_shared<ch::ChBody>();
            const ch::ChFramed f = toCh(body.placement);
            b->SetPos(f.GetPos());
            b->SetRot(f.GetRot());
            sys.AddBody(b);
            d->bodies.push_back(b);
        }
        d->lever.assign(d->bodies.size(), 1.0);
        for (const auto& frame : d->model.frames) {
            if (frame.body < d->bodies.size()) {
                d->lever[frame.body] =
                    std::max(d->lever[frame.body], frame.local.getPosition().Length());
            }
        }
        d->setWeights({});
        d->branch.assign(d->model.frames.size(), halfTurn(-1));

        // A body a Ground pins is fixed where its first Ground holds it (left
        // as it is when it is there within 1e-12).
        d->pinned.assign(d->model.bodies.size(), 0);
        for (const auto& ground : d->model.grounds) {
            if (!d->validFrame(ground.frameI) || !d->validFrame(ground.frameJ)) {
                continue;
            }
            const Id body = d->model.frames[ground.frameJ].body;
            if (body == NoId || body >= d->bodies.size()
                || d->model.frames[ground.frameI].body != NoId || d->pinned[body]) {
                continue;
            }
            d->pinned[body] = 1;
            const Base::Placement want = d->model.frames[ground.frameI].local
                * d->model.frames[ground.frameJ].local.inverse();
            const Base::Placement have =
                toPlacement(d->bodies[body]->GetPos(), d->bodies[body]->GetRot());
            if (!want.isSame(have, 1e-12)) {
                const ch::ChFramed f = toCh(want);
                d->bodies[body]->SetPos(f.GetPos());
                d->bodies[body]->SetRot(f.GetRot());
            }
            d->bodies[body]->SetFixed(true);
        }

        std::ostringstream why;
        for (const auto& joint : d->model.joints) {
            const std::string reason = d->addJoint(joint);
            if (!reason.empty()) {
                d->unsupported.push_back(joint.id);
                why << (why.tellp() > 0 ? "; " : "") << "the joint "
                    << d->model.sourceName(joint.source) << " (" << kindName(joint.kind)
                    << "): " << reason;
            }
        }
        if (!d->unsupported.empty()) {
            d->refused = "the Chrono backend cannot solve this assembly: " + why.str();
        }

        // Chrono's initialisation (assembly setup), with no iteration.
        sys.DoAssembly(static_cast<int>(ch::AssemblyAnalysis::Level::POSITION), 0);
        d->prepare();
        d->readLimits();
    }
    catch (const std::exception& e) {
        Base::Console().error("Assembly: the Chrono backend could not build the model: %s\n",
                              e.what());
        return Status::Failed;
    }
    d->loaded = true;
    return Status::Ok;
}

Status ChronoSession::update(const Model& model)
{
    if (!d->loaded || !sameExceptDragWeights(d->model, model)) {
        return Status::NeedsReload;
    }
    for (std::size_t i = 0; i < d->model.bodies.size(); ++i) {
        d->model.bodies[i].dragWeight = model.bodies[i].dragWeight;
    }
    return Status::Ok;
}

Result ChronoSession::solve(const SolveOptions& options)
{
    if (options.maxIterations > 0) {
        d->maxNewton = options.maxIterations;
    }
    if (options.tolerance > 0.0) {
        d->loose = std::max(options.tolerance, d->tight);
    }
    Result result = d->run(d->current(), {});
    if (result.status == Status::Ok && options.diagnostics) {
        result.diagnostics = analyse();
    }
    return result;
}

Status ChronoSession::beginDrag(const std::vector<Id>& bodies, const DragOptions& options)
{
    (void)options;
    if (!d->loaded) {
        return Status::Failed;
    }
    if (!d->refused.empty()) {
        return Status::Unsupported;
    }
    d->dragBodies = bodies;
    return Status::Ok;
}

Result ChronoSession::dragStep(const std::vector<BodyPose>& targets)
{
    if (!d->loaded || !d->refused.empty()) {
        return d->run({}, {});
    }
    // The dragged bodies pulled to their targets with their drag weights;
    // the others held where they are with weight 1.
    std::vector<Pose> target = d->current();
    std::vector<double> weight(target.size(), 1.0);
    for (const auto& t : targets) {
        if (t.body >= target.size()) {
            continue;
        }
        const ch::ChFramed f = toCh(t.placement);
        target[t.body] = {f.GetPos(), f.GetRot()};
        const double w = d->model.bodies[t.body].dragWeight;
        weight[t.body] = w > 0.0 ? w : 1.0;
    }
    Result result = d->run(target, weight);
    if (result.status != Status::Ok) {
        // Half the way, as OndselSolver retries a step that does not converge.
        std::vector<Pose> half = d->current();
        for (const auto& t : targets) {
            if (t.body >= half.size()) {
                continue;
            }
            const ch::ChFramed goal = toCh(t.placement);
            Pose& p = half[t.body];
            const ch::ChVector3d w = rotationFrom(p.q, goal.GetRot());
            ch::ChQuaterniond dq;
            dq.SetFromRotVec(w * 0.5);
            p.p = (p.p + goal.GetPos()) * 0.5;
            p.q = p.q * dq;
        }
        Result retry = d->run(half, weight);
        if (retry.status == Status::Ok) {
            return retry;
        }
    }
    return result;
}

Status ChronoSession::setPoses(const std::vector<BodyPose>& poses)
{
    if (!d->loaded || !d->sys) {
        return Status::Failed;
    }
    std::vector<Pose> now = d->current();
    for (const auto& pose : poses) {
        if (pose.body < now.size()) {
            const ch::ChFramed f = toCh(pose.placement);
            now[pose.body] = {f.GetPos(), f.GetRot()};
        }
    }
    d->setPoses(now);  // grounded (fixed) bodies are left
    return Status::Ok;
}

Result ChronoSession::endDrag()
{
    d->dragBodies.clear();
    // A static solve at the dragged state.
    return d->run(d->current(), {});
}

std::vector<BodyPose> ChronoSession::poses() const
{
    if (!d->sys) {
        return {};
    }
    return d->posesNow();
}

Diagnostics ChronoSession::analyse()
{
    Diagnostics diagnostics;
    if (!d->loaded) {
        return diagnostics;
    }
    if (!d->refused.empty()) {
        diagnostics.unsupported = d->unsupported;
        diagnostics.known = Diagnostics::KnownUnsupported;
        return diagnostics;
    }
    d->prepare();
    ch::ChSparseMatrix Cq;
    d->sys->GetConstraintJacobianMatrix(Cq);
    const MatX J(Cq);
    const ch::ChVectorDynamic<> c = d->constraints();
    const Eigen::Index rows = J.rows();
    const Eigen::Index cols = J.cols();

    // Each row's owner and category, from its link's offset; which depend
    // on the rows before them in OndselSolver's order.
    const std::vector<RowInfo> info = d->rowInfo(rows);
    int kept = 0;
    const std::vector<char> independent = Impl::independentRows(J, info, kept);

    // The rank by column-pivoted QR, rows scaled to unit length.
    int rank = 0;
    if (rows > 0 && cols > 0) {
        MatX S = J;
        for (Eigen::Index r = 0; r < S.rows(); ++r) {
            const double n = S.row(r).norm();
            if (n > 0.0) {
                S.row(r) /= n;
            }
        }
        Eigen::ColPivHouseholderQR<MatX> qr(S);
        qr.setThreshold(rankTolerance);
        rank = static_cast<int>(qr.rank());
    }
    if (kept != rank) {
        Base::Console().log(
            "Assembly: the Chrono backend's rank by QR (%d) and by Gram-Schmidt (%d) differ\n",
            rank,
            kept);
    }
    int pinnedCount = 0;
    for (char p : d->pinned) {
        pinnedCount += p ? 1 : 0;
    }
    // OndselSession's convention (sec 6.6, deviation 3), as the Reference.
    diagnostics.dof = static_cast<int>(6 * (1 + d->bodies.size() - pinnedCount)) - rank;

    const std::size_t joints = d->model.joints.size();
    std::vector<int> total(joints, 0);
    std::vector<int> dependent(joints, 0);
    std::vector<char> conflict(joints, 0);
    for (Eigen::Index i = 0; i < rows; ++i) {
        const RowInfo& row = info[i];
        if (row.owner != Owner::Joint || row.id >= joints) {
            continue;
        }
        ++total[row.id];
        if (!independent[i]) {
            ++dependent[row.id];
            if (!(std::abs(c(i)) <= conflictTolerance)) {
                conflict[row.id] = 1;
            }
        }
    }
    // A joint on one rigid thing has equations without a gradient: all of
    // them dependent, conflicting when the joint is not satisfied.
    if (!d->unlinked.empty()) {
        std::vector<Base::Placement> placements;
        for (const auto& pose : d->posesNow()) {
            placements.push_back(pose.placement);
        }
        const std::vector<double> residuals = Reference::jointResiduals(d->model, placements);
        for (Id j : d->unlinked) {
            total[j] = std::max(total[j], 1);
            dependent[j] = total[j];
            if (!(residuals[j] <= conflictTolerance)) {
                conflict[j] = 1;
            }
        }
    }
    for (std::size_t j = 0; j < joints; ++j) {
        if (dependent[j] == 0) {
            continue;
        }
        diagnostics.redundant.push_back(static_cast<Id>(j));
        if (dependent[j] < total[j]) {
            diagnostics.partiallyRedundant.push_back(static_cast<Id>(j));
        }
        if (conflict[j]) {
            diagnostics.conflicting.push_back(static_cast<Id>(j));
        }
    }
    diagnostics.known = Diagnostics::KnownDof | Diagnostics::KnownRedundant
        | Diagnostics::KnownConflicting | Diagnostics::KnownPartiallyRedundant
        | Diagnostics::KnownUnsupported;
    return diagnostics;
}

Status ChronoSession::simulate(const SimulationSpec& spec, TrajectorySink& sink)
{
    if (!d->loaded) {
        return Status::Failed;
    }
    if (!d->refused.empty()) {
        return Status::Unsupported;
    }
    if (spec.analysis != Analysis::Kinematic) {
        return simulateDynamics(spec, sink);
    }
    for (const auto& driver : d->model.drivers) {
        for (const Formula* formula : {&driver.rotation, &driver.translation}) {
            if (!formula->text.empty() && !formula->tree && !formula->function
                && !FormulaTree::parse(formula->text)) {
                return Status::Failed;
            }
        }
    }
    if (!(spec.tOutput > 0.0) || spec.tEnd < spec.tStart) {
        return Status::Failed;
    }
    const auto steps =
        static_cast<std::size_t>(std::floor((spec.tEnd - spec.tStart) / spec.tOutput + 1e-9));

    // With the reactions asked for, the frames are kept for the inverse
    // dynamics pass, which sends them on with their values
    const bool inverse = spec.reactions && d->model.dynamics;
    std::vector<std::pair<double, std::vector<BodyPose>>> kept;
    auto emit = [&](double time, std::vector<BodyPose> poses) {
        if (!inverse) {
            return sink.frame(time, poses);
        }
        kept.emplace_back(time, std::move(poses));
        return true;
    };

    Status status = Status::Ok;
    try {
        d->addMotors();
        double now = spec.tStart;
        d->sys->SetChTime(now);
        d->prepare();
        for (std::size_t k = 0; k <= steps && status == Status::Ok; ++k) {
            const double time = spec.tStart + static_cast<double>(k) * spec.tOutput;
            // Outside a formula's domain the run stops (sec 4.6.1).
            const int n = k == 0 ? 1 : d->substeps(now, time);
            if (n == 0 || !d->driversFinite(time)) {
                status = Status::Failed;
                break;
            }
            for (int s = 1; s <= n; ++s) {
                const double t = k == 0 ? time : now + (time - now) * s / n;
                const std::vector<Pose> previous = d->current();
                if (t != d->sys->GetChTime()) {
                    // Predicted along the velocities at the last solve, so a
                    // driven angle and the gears it turns keep their turn.
                    ch::ChStateDelta v;
                    const bool predicted = d->velocity(v);
                    Impl::State state = d->gather();
                    if (predicted) {
                        state = d->stepped(state, v, t - d->sys->GetChTime());
                    }
                    state.t = t;
                    d->sys->SetChTime(t);
                    d->scatter(state);
                }
                Result result = d->run(previous, {});
                if (result.status != Status::Ok) {
                    status = result.status;
                    break;
                }
            }
            now = time;
            if (status == Status::Ok && !emit(time, d->posesNow())) {
                status = Status::Cancelled;
            }
        }
    }
    catch (const std::exception& e) {
        Base::Console().error("Assembly: the Chrono backend's kinematic run failed: %s\n",
                              e.what());
        status = Status::Failed;
    }
    d->removeMotors();
    d->sys->SetChTime(0.0);
    if (inverse && status == Status::Ok) {
        status = simulateDynamics(spec, sink, &kept);
    }
    return status;
}

Status ChronoSession::simulateDynamics(
    const SimulationSpec& spec,
    TrajectorySink& sink,
    const std::vector<std::pair<double, std::vector<BodyPose>>>* prescribed)
{
    const Model& model = d->model;
    const char* kind = prescribed           ? "kinematic"
        : spec.analysis == Analysis::Static ? "static"
                                            : "dynamic";
    auto refuse = [kind](const std::string& why) {
        Base::Console().error("Assembly: the %s analysis cannot run: %s\n", kind, why.c_str());
        return Status::Unsupported;
    };
    if (!model.dynamics) {
        return refuse("the model has no masses");
    }
    if (spec.analysis == Analysis::Static) {
        for (const auto& driver : model.drivers) {
            if (driver.rotationControl == MotorControl::Speed
                || driver.translationControl == MotorControl::Speed) {
                return refuse("the motion of " + model.sourceName(driver.source)
                              + " is a Speed, which holds nothing still");
            }
        }
    }
    if (!(spec.tOutput > 0.0) || spec.tEnd < spec.tStart) {
        return Status::Failed;
    }

    // In SI, each body's frame at its centre of mass: a ChBody's inertia is
    // about its own frame
    Model si = scaled(model, SI::length);
    const std::size_t n = si.bodies.size();
    std::vector<Base::Placement> centre(n);
    std::vector<const Inertia*> inertiaOf(n, nullptr);
    for (const auto& inertia : si.dynamics->inertias) {
        if (inertia.body < n) {
            inertiaOf[inertia.body] = &inertia;
            centre[inertia.body] = Base::Placement(inertia.com, Base::Rotation());
        }
    }
    for (std::size_t b = 0; b < n; ++b) {
        si.bodies[b].placement = si.bodies[b].placement * centre[b];
    }
    for (auto& frame : si.frames) {
        if (frame.body < n) {
            frame.local = centre[frame.body].inverse() * frame.local;
        }
    }

    ChronoSession run;
    if (run.load(si) != Status::Ok) {
        return Status::Failed;
    }
    if (!run.d->refused.empty()) {
        return refuse(run.d->refused);
    }
    // Redundant joints (sec 6.1): the rows that repeat others taken out, the
    // reactions they shared reported as not unique
    const ChronoSession::Impl::Redundancy redundancy = run.d->removeRedundant(SI::length);
    if (!redundancy.conflicting.empty()) {
        std::string names;
        for (Id j : redundancy.conflicting) {
            names += (names.empty() ? "" : ", ") + model.sourceName(model.joints[j].source);
        }
        return refuse("the joints " + names
                      + " conflict with others: they cannot all hold where the run starts");
    }
    ch::ChSystemNSC& sys = *run.d->sys;
    for (std::size_t b = 0; b < n; ++b) {
        auto& body = run.d->bodies[b];
        if (body->IsFixed()) {
            continue;
        }
        const Inertia* inertia = inertiaOf[b];
        if (!inertia || inertia->mass <= 0.0) {
            return refuse("the part " + model.sourceName(model.bodies[b].source)
                          + " has no mass, and is free to move");
        }
        body->SetMass(inertia->mass * SI::mass);
        // Scaled with the model: in kg m^2 already
        const auto& i = inertia->inertia;
        ch::ChMatrix33d tensor;
        tensor << i[0], i[3], i[4], i[3], i[1], i[5], i[4], i[5], i[2];
        body->SetInertia(tensor);
    }
    // What each frame reports besides the poses (sec 8.4)
    std::vector<std::pair<Id, std::shared_ptr<ch::ChLinkTSDA>>> springs;
    std::vector<std::pair<Id, std::shared_ptr<ch::ChLinkRSDA>>> torsions;
    std::vector<std::pair<Id, std::shared_ptr<ch::ChLinkMotorRotation>>> rotationMotors;
    std::vector<std::pair<Id, std::shared_ptr<ch::ChLinkMotorLinear>>> linearMotors;
    std::vector<std::pair<Id, std::shared_ptr<Bushing>>> bushings;
    // A Speed motor integrates its rate from an offset, and holds the joint
    // to that integral to keep it from drifting: from Chrono's offset of 0 a
    // joint not at its zero -- a slider 2 mm open -- was pulled there at the
    // start. The offset is the joint's value where the run starts.
    auto startHere = [&](const std::shared_ptr<ch::ChLinkMotor>& motor) {
        motor->Update(sys.GetChTime(), ch::UpdateFlags::UPDATE_ALL_NO_VISUAL);
        if (auto rotation = std::dynamic_pointer_cast<ch::ChLinkMotorRotationSpeed>(motor)) {
            rotation->SetAngleOffset(rotation->GetMotorAngle());
        }
        else if (auto linear = std::dynamic_pointer_cast<ch::ChLinkMotorLinearSpeed>(motor)) {
            linear->SetMotionOffset(linear->GetMotorPos());
        }
    };
    // The force elements, on frames of the model: in SI and at the centres
    // of mass with it (a stiffness and a damping need no factor, sec 8.1)
    auto loads = std::make_shared<ch::ChLoadContainer>();
    sys.Add(loads);
    for (const auto& force : si.dynamics->forces) {
        if (!run.d->validFrame(force.frameI) || !run.d->validFrame(force.frameJ)) {
            continue;
        }
        if (force.kind == ForceKind::AppliedForce || force.kind == ForceKind::AppliedTorque) {
            // A formula that does not parse the builder reported
            FormulaFunction magnitude = force.magnitude.evaluator();
            if (!magnitude || run.d->isRigid(force.frameI, force.frameJ)) {
                continue;
            }
            loads->Add(std::make_shared<AppliedLoad>(run.d->bodyOf(force.frameJ),
                                                     run.d->bodyOf(force.frameI),
                                                     run.d->globalOf(force.frameI),
                                                     std::move(magnitude),
                                                     toCh(force.direction),
                                                     force.followsFrame,
                                                     force.kind == ForceKind::AppliedTorque));
            continue;
        }
        if (force.kind == ForceKind::Bushing) {
            if (run.d->isRigid(force.frameI, force.frameJ)) {
                continue;
            }
            // In SI with the model: the rotational values scaled already
            ch::ChMatrix66d k;
            ch::ChMatrix66d c;
            k.setZero();
            c.setZero();
            for (int i = 0; i < 6; ++i) {
                k(i, i) = force.k6[i];
                c(i, i) = force.c6[i];
            }
            auto bushing = std::make_shared<Bushing>(run.d->bodyOf(force.frameJ),
                                                     run.d->bodyOf(force.frameI),
                                                     run.d->globalOf(force.frameI),
                                                     k,
                                                     c);
            bushing->SetNeutralForce(
                -ch::ChVector3d(force.preload6[0], force.preload6[1], force.preload6[2]));
            bushing->SetNeutralTorque(
                -ch::ChVector3d(force.preload6[3], force.preload6[4], force.preload6[5]));
            loads->Add(bushing);
            bushings.emplace_back(force.source, bushing);
            continue;
        }
        if (force.kind == ForceKind::TorsionSpring) {
            if (run.d->isRigid(force.frameI, force.frameJ)) {
                continue;
            }
            // Frame I's body first: Chrono's angle is then the joint's,
            // of xJ about zI, J as the joint's motors and limits see it.
            // In SI with the model: k, c and the preload scaled already.
            auto torsion = std::make_shared<ch::ChLinkRSDA>();
            torsion->Initialize(run.d->bodyOf(force.frameI),
                                run.d->bodyOf(force.frameJ),
                                true,
                                run.d->localOf(force.frameI),
                                run.d->driverLocalOf(force.frameJ));
            torsion->SetRestAngle(torsion->GetAngle() + force.rest);
            torsion->SetSpringCoefficient(force.k);
            torsion->SetDampingCoefficient(force.c);
            torsion->SetActuatorTorque(force.preload);
            sys.AddLink(torsion);
            torsions.emplace_back(force.source, torsion);
            continue;
        }
        if (force.kind != ForceKind::SpringDamper) {
            continue;
        }
        auto spring = std::make_shared<ch::ChLinkTSDA>();
        spring->Initialize(run.d->bodyOf(force.frameI),
                           run.d->bodyOf(force.frameJ),
                           true,
                           toCh(si.frames[force.frameI].local.getPosition()),
                           toCh(si.frames[force.frameJ].local.getPosition()));
        // Else the length at the start, as Initialize takes it
        if (force.rest > 0.0) {
            spring->SetRestLength(force.rest);
        }
        spring->SetSpringCoefficient(force.k * SI::linearStiffness);
        spring->SetDampingCoefficient(force.c * SI::linearDamping);
        spring->SetActuatorForce(force.preload);
        sys.AddLink(spring);
        springs.emplace_back(force.source, spring);
    }
    // Contact (sec 7, phase D2): each shape on its frame's body, collision
    // detection by Bullet making no contact of Chrono's own (its SMC contact
    // force is explicit, fixed over a step), and a ContactLoad per pair of
    // bodies a set lets touch, given the points found at each step's start
    std::vector<std::shared_ptr<ContactLoad>> contactLoads;
    ContactCollisionSystem* contactCollision = nullptr;
    struct Found
    {
        ContactLoad* load = nullptr;
        bool swapped = false;  // the info's A is the load's B
        ch::ChVector3d pA, pB, normal;
        const ch::ChCollisionShape* shapeA = nullptr;
        const ch::ChCollisionShape* shapeB = nullptr;
    };
    struct ContactFinder: ch::ChCollisionSystem::NarrowphaseCallback
    {
        std::map<std::pair<const ch::ChContactable*, const ch::ChContactable*>, ContactLoad*> loads;
        std::vector<Found> found;
        bool OnNarrowphase(ch::ChCollisionInfo& info) override
        {
            const ch::ChContactable* a = info.modelA ? info.modelA->GetContactable() : nullptr;
            const ch::ChContactable* b = info.modelB ? info.modelB->GetContactable() : nullptr;
            auto it = loads.find({a, b});
            // Bullet's EPA can fail to a zero normal, which Chrono normalises
            // to NaN: a point without a direction is no contact
            const bool usable = std::isfinite(info.vN.x()) && std::isfinite(info.vN.y())
                && std::isfinite(info.vN.z()) && std::abs(info.vN.Length() - 1.0) < 1e-6
                && std::isfinite(info.vpA.Length()) && std::isfinite(info.vpB.Length());
            if (it != loads.end() && usable) {
                const bool swapped =
                    static_cast<const ch::ChContactable*>(it->second->GetBodyA().get()) != a;
                found.push_back(
                    {it->second, swapped, info.vpA, info.vpB, info.vN, info.shapeA, info.shapeB});
            }
            // Never one of Chrono's contacts
            return false;
        }
    };
    struct ContactHandover: ch::ChSystem::CustomCollisionCallback
    {
        std::shared_ptr<ContactFinder> finder;
        const ContactCollisionSystem* collision = nullptr;
        std::vector<std::shared_ptr<ContactLoad>>* loads = nullptr;
        const std::map<const ch::ChCollisionShape*, ShapeOn>* shapes = nullptr;
        ShapesOn shapesOn;
        std::shared_ptr<std::map<const ch::ChCollisionShape*, ShapeOn>> keep;
        double reach = 0.0;
        void OnCustomCollision(ch::ChSystem*) override
        {
            // Collision detection's results kept from the check after the
            // last substep, at this state: its points are the loads' still
            if (collision->reused) {
                finder->found.clear();
                return;
            }
            for (auto& load : *loads) {
                load->points.clear();
            }
            // A point in the info's terms to its load's: the load's A first,
            // the normal from its A to its B
            auto handOver = [](const Found& f,
                               const ch::ChVector3d& pA,
                               const ch::ChVector3d& pB,
                               const ch::ChVector3d& normal) {
                ContactLoad& load = *f.load;
                load.points.push_back(
                    {load.GetBodyA()->TransformPointParentToLocal(f.swapped ? pB : pA),
                     load.GetBodyB()->TransformPointParentToLocal(f.swapped ? pA : pB),
                     f.swapped ? -normal : normal,
                     f.swapped ? f.shapeB : f.shapeA,
                     f.swapped ? f.shapeA : f.shapeB});
            };
            auto on = [&](const ch::ChCollisionShape* shape) -> const ShapeOn* {
                auto it = shapes->find(shape);
                return it == shapes->end() ? nullptr : &it->second;
            };
            // A cylinder's pair with a face: its points once, in place of
            // all GJK's
            std::set<std::pair<const ch::ChCollisionShape*, const ch::ChCollisionShape*>> done;
            std::vector<CylinderPoint> points;
            ch::ChVector3d normal;
            for (const Found& f : finder->found) {
                const ShapeOn* a = on(f.shapeA);
                const ShapeOn* b = on(f.shapeB);
                const bool cylinderA = a && a->radius > 0.0 && b && b->features;
                const bool cylinderB = b && b->radius > 0.0 && a && a->features;
                if (!cylinderA && !cylinderB) {
                    handOver(f, f.pA, f.pB, f.normal);
                    continue;
                }
                if (done.count({f.shapeA, f.shapeB})) {
                    continue;
                }
                const bool clipped = cylinderA
                    ? cylinderPoints(*a, *b, f.normal, reach, points, normal)
                    : cylinderPoints(*b, *a, -f.normal, reach, points, normal);
                if (!clipped) {
                    handOver(f, f.pA, f.pB, f.normal);
                    continue;
                }
                done.insert({f.shapeA, f.shapeB});
                for (const CylinderPoint& point : points) {
                    // normal is from the face, info's A to B as f.normal
                    if (cylinderA) {
                        handOver(f, point.onCylinder, point.onOther, -normal);
                    }
                    else {
                        handOver(f, point.onOther, point.onCylinder, normal);
                    }
                }
            }
            finder->found.clear();
            for (auto& load : *loads) {
                patchesOf(*load);
                patchStiffness(*load, shapesOn);
            }
        }
    };
    const bool contact = !si.dynamics->contacts.empty();
    if (contact) {
        // Bullet grows each shape by the envelope and rounds it by the
        // margin, and a contact's points are where those put them: Chrono's
        // defaults (3 cm, 1 cm) are a part's size here, and an envelope of
        // 2 mm put a 10 mm box's corners 2 mm out. A small envelope, then; a
        // contact it does not see coming is found by the step that goes too
        // deep (advance()). The margin stays above the penetrations: deeper,
        // Bullet takes a convex pair by EPA, whose normals are noisy -- a
        // hexagonal prism rocking on a plate until it flew off. Chrono caps
        // it at a tenth of the shape's least size.
        constexpr double envelope = 2e-5;
        constexpr double margin = 1e-4;
        ch::ChCollisionModel::SetDefaultSuggestedEnvelope(envelope);
        ch::ChCollisionModel::SetDefaultSuggestedMargin(margin);
        auto collision = std::make_shared<ContactCollisionSystem>();
        sys.SetCollisionSystem(collision);
        contactCollision = collision.get();
        auto material = std::make_shared<ch::ChContactMaterialNSC>();
        std::vector<ch::ChBody*> shapeBody(si.dynamics->contactShapes.size(), nullptr);
        auto shapesOn = std::make_shared<std::map<const ch::ChCollisionShape*, ShapeOn>>();
        for (std::size_t k = 0; k < si.dynamics->contactShapes.size(); ++k) {
            const ContactShape& shape = si.dynamics->contactShapes[k];
            if (!run.d->validFrame(shape.frame)) {
                continue;
            }
            std::shared_ptr<ch::ChCollisionShape> made;
            std::shared_ptr<ch::ChTriangleMeshConnected> meshMade;
            switch (shape.kind) {
                case ContactShape::Kind::Sphere:
                    made = std::make_shared<ch::ChCollisionShapeSphere>(material, shape.size.x);
                    break;
                case ContactShape::Kind::Box:
                    made = std::make_shared<ch::ChCollisionShapeBox>(material,
                                                                     shape.size.x,
                                                                     shape.size.y,
                                                                     shape.size.z);
                    break;
                case ContactShape::Kind::Cylinder:
                    made = std::make_shared<ch::ChCollisionShapeCylinder>(material,
                                                                          shape.size.x,
                                                                          shape.size.z);
                    break;
                case ContactShape::Kind::ConvexHull: {
                    std::vector<ch::ChVector3d> points;
                    points.reserve(shape.points.size());
                    for (const auto& point : shape.points) {
                        points.push_back(toCh(point));
                    }
                    made = std::make_shared<ch::ChCollisionShapeConvexHull>(material, points);
                    break;
                }
                case ContactShape::Kind::Mesh: {
                    // A triangle per Bullet shape, each pointing into the
                    // mesh's vertices and placed where they are: the
                    // vertices in the body's frame, the shape at its origin.
                    // Not static (the body may be ground or not), and not
                    // convex: Chrono keeps the triangles and their
                    // neighbours.
                    auto mesh = std::make_shared<ch::ChTriangleMeshConnected>();
                    const ch::ChFramed on = run.d->localOf(shape.frame);
                    for (const auto& point : shape.points) {
                        mesh->GetCoordsVertices().push_back(
                            on.TransformPointLocalToParent(toCh(point)));
                    }
                    for (const auto& t : shape.triangles) {
                        mesh->GetIndicesVertexes().emplace_back(t[0], t[1], t[2]);
                    }
                    made = std::make_shared<ch::ChCollisionShapeTriangleMesh>(material,
                                                                              mesh,
                                                                              false,
                                                                              false,
                                                                              0.0);
                    meshMade = mesh;
                    break;
                }
            }
            auto body = run.d->bodyOf(shape.frame);
            body->AddCollisionShape(made,
                                    shape.kind == ContactShape::Kind::Mesh
                                        ? ch::ChFramed()
                                        : run.d->localOf(shape.frame));
            body->GetCollisionModel()->SetEnvelope(envelope);
            body->GetCollisionModel()->SetSafeMargin(margin);
            body->EnableCollision(true);
            shapeBody[k] = body.get();
            ShapeOn& where = (*shapesOn)[made.get()];
            where.body = body.get();
            where.frame = run.d->localOf(shape.frame);
            where.envelope = envelope;
            if (shape.kind == ContactShape::Kind::Cylinder) {
                where.radius = shape.size.x;
                where.half = 0.5 * shape.size.z;
            }
            if (shape.kind == ContactShape::Kind::Sphere) {
                where.ball = shape.size.x;
            }
            if (shape.kind == ContactShape::Kind::ConvexHull) {
                for (const auto& point : shape.points) {
                    where.hull.push_back(toCh(point));
                }
                std::vector<ch::ChVector3i> triangles;
                for (const auto& t : shape.triangles) {
                    triangles.emplace_back(t[0], t[1], t[2]);
                }
                where.bends = meshBends(where.hull, triangles);
            }
            where.modulus = shape.modulus;
            where.size = std::cbrt(3.0 * std::max(shape.volume, 0.0) / (4.0 * pi));
            if (meshMade) {
                const auto& vertices = meshMade->GetCoordsVertices();
                where.vertices = vertices.data();
                where.count = vertices.size();
                where.bends = meshBends(vertices, meshMade->GetIndicesVertexes());
            }
        }
        auto finder = std::make_shared<ContactFinder>();
        // A load per pair of bodies a set lets touch, the first set's law
        auto pairUp = [&](ch::ChBody* x, ch::ChBody* y, std::size_t set) {
            if (!x || !y || x == y || (x->IsFixed() && y->IsFixed())
                || finder->loads.count({x, y})) {
                return;
            }
            std::shared_ptr<ch::ChBody> first;
            std::shared_ptr<ch::ChBody> second;
            auto own = [&](ch::ChBody* raw) {
                if (raw == run.d->world.get()) {
                    return run.d->world;
                }
                for (const auto& body : run.d->bodies) {
                    if (body.get() == raw) {
                        return body;
                    }
                }
                return std::shared_ptr<ch::ChBody>();
            };
            first = own(x);
            second = own(y);
            if (!first || !second) {
                return;
            }
            auto load =
                std::make_shared<ContactLoad>(first, second, si.dynamics->contacts[set].law, set);
            loads->Add(load);
            contactLoads.push_back(load);
            finder->loads[{x, y}] = load.get();
            finder->loads[{y, x}] = load.get();
        };
        for (std::size_t set = 0; set < si.dynamics->contacts.size(); ++set) {
            const ContactPair& pair = si.dynamics->contacts[set];
            auto bodyOf = [&](Id shape) {
                return shape < shapeBody.size() ? shapeBody[shape] : nullptr;
            };
            if (pair.groupB.empty()) {
                for (std::size_t i = 0; i < pair.groupA.size(); ++i) {
                    for (std::size_t j = i + 1; j < pair.groupA.size(); ++j) {
                        pairUp(bodyOf(pair.groupA[i]), bodyOf(pair.groupA[j]), set);
                    }
                }
            }
            else {
                for (Id x : pair.groupA) {
                    for (Id y : pair.groupB) {
                        pairUp(bodyOf(x), bodyOf(y), set);
                    }
                }
            }
        }
        collision->RegisterNarrowphaseCallback(finder);
        auto handover = std::make_shared<ContactHandover>();
        handover->finder = finder;
        handover->collision = contactCollision;
        handover->loads = &contactLoads;
        handover->shapes = shapesOn.get();
        handover->shapesOn.byShape = shapesOn.get();
        for (const auto& [shape, on] : *shapesOn) {
            if (on.vertices) {
                handover->shapesOn.meshes.push_back(&on);
            }
        }
        handover->reach = margin;
        handover->keep = shapesOn;
        sys.RegisterCustomCollisionCallback(handover);
        collision->Initialize();
        std::map<const ch::ChCollisionShape*, const cbtConvexPolyhedron*> features;
        std::set<ch::ChBody*> withShapes(shapeBody.begin(), shapeBody.end());
        for (ch::ChBody* body : withShapes) {
            if (body) {
                collision->addPolyhedralFeatures(*body->GetCollisionModel(), features);
            }
        }
        for (const auto& [shape, polyhedron] : features) {
            auto it = shapesOn->find(shape);
            if (it != shapesOn->end()) {
                it->second.features = polyhedron;
            }
        }
    }
    // The motions, as motors: an angle or a distance, a speed, or a torque or
    // a force (sec 5.2), the formulas in SI with the model
    for (const auto& driver : si.drivers) {
        if (!run.d->validFrame(driver.frameI) || !run.d->validFrame(driver.frameJ)
            || run.d->isRigid(driver.frameI, driver.frameJ)) {
            continue;
        }
        const ch::ChFramed f1 = run.d->driverLocalOf(driver.frameJ);
        const ch::ChFramed f2 = run.d->localOf(driver.frameI);
        auto functionOf = [](const Formula& formula) -> std::shared_ptr<ch::ChFunction> {
            if (formula.text.empty() && !formula.tree && !formula.function) {
                return nullptr;
            }
            FormulaFunction function = formula.evaluator();
            return function ? std::make_shared<FormulaChFunction>(std::move(function)) : nullptr;
        };
        if (driver.kind != DriverKind::TranslationZ) {
            if (auto function = functionOf(driver.rotation)) {
                std::shared_ptr<ch::ChLinkMotorRotation> motor;
                switch (driver.rotationControl) {
                    case MotorControl::Position:
                        motor = std::make_shared<ch::ChLinkMotorRotationAngle>();
                        break;
                    case MotorControl::Speed:
                        motor = std::make_shared<ch::ChLinkMotorRotationSpeed>();
                        break;
                    case MotorControl::Effort:
                        motor = std::make_shared<ch::ChLinkMotorRotationTorque>();
                        break;
                }
                motor->SetSpindleConstraint(ch::ChLinkMotorRotation::SpindleConstraint::FREE);
                motor->Initialize(run.d->bodyOf(driver.frameJ),
                                  run.d->bodyOf(driver.frameI),
                                  true,
                                  f1,
                                  f2);
                motor->SetMotorFunction(function);
                sys.AddLink(motor);
                startHere(motor);
                rotationMotors.emplace_back(driver.source, motor);
            }
        }
        if (driver.kind != DriverKind::RotationZ) {
            if (auto function = functionOf(driver.translation)) {
                std::shared_ptr<ch::ChLinkMotorLinear> motor;
                switch (driver.translationControl) {
                    case MotorControl::Position:
                        motor = std::make_shared<ch::ChLinkMotorLinearPosition>();
                        break;
                    case MotorControl::Speed:
                        motor = std::make_shared<ch::ChLinkMotorLinearSpeed>();
                        break;
                    case MotorControl::Effort:
                        motor = std::make_shared<ch::ChLinkMotorLinearForce>();
                        break;
                }
                motor->SetGuideConstraint(ch::ChLinkMotorLinear::GuideConstraint::FREE);
                motor->Initialize(run.d->bodyOf(driver.frameJ),
                                  run.d->bodyOf(driver.frameI),
                                  true,
                                  f1,
                                  f2);
                motor->SetMotorFunction(function);
                sys.AddLink(motor);
                startHere(motor);
                linearMotors.emplace_back(driver.source, motor);
            }
        }
    }
    sys.SetGravitationalAcceleration(toCh(si.dynamics->gravity));
    // HHT, which needs a direct solver: the load set LUThenQR
    sys.SetTimestepperType(ch::ChTimestepper::Type::HHT);
    if (auto hht = std::dynamic_pointer_cast<ch::ChTimestepperHHT>(sys.GetTimestepper())) {
        hht->SetAlpha(contact ? -1.0 / 3.0 : -0.2);
        hht->SetMaxIters(20);
        hht->SetStepControl(false);
    }

    const double hMax = spec.maxStep > 0.0 ? spec.maxStep : 1e-3;
    const int sub = std::max(1, static_cast<int>(std::ceil(spec.tOutput / hMax - 1e-9)));
    const double h = spec.tOutput / sub;
    const auto steps =
        static_cast<std::size_t>(std::floor((spec.tEnd - spec.tStart) / spec.tOutput + 1e-9));
    auto posesNow = [&]() {
        std::vector<BodyPose> poses;
        poses.reserve(n);
        for (std::size_t b = 0; b < n; ++b) {
            const auto& body = run.d->bodies[b];
            Base::Placement placement =
                toPlacement(body->GetPos(), body->GetRot()) * centre[b].inverse();
            placement.setPosition(placement.getPosition() / SI::length);
            poses.push_back({static_cast<Id>(b), placement});
        }
        return poses;
    };

    // The values each frame reports (sec 8.4). A joint's reaction is per
    // document joint: the links of its model joints summed, the torque about
    // the first one's frame I.
    struct Reaction
    {
        std::string source;
        std::vector<std::pair<const Joint*, std::shared_ptr<ch::ChLinkBase>>> links;
        std::vector<std::size_t> limits;  // its stops, by index in si.limits
        bool unique = true;               // false: a redundant joint's (sec 6.1)
    };
    std::vector<Reaction> reactions;
    for (const auto& record : run.d->links) {
        if (record.owner != Owner::Joint || record.id >= si.joints.size()) {
            continue;
        }
        const Joint& joint = si.joints[record.id];
        const std::string& name = si.sourceName(joint.source);
        auto at = std::find_if(reactions.begin(), reactions.end(), [&](const Reaction& r) {
            return r.source == name;
        });
        if (at == reactions.end()) {
            reactions.push_back({name, {}});
            at = std::prev(reactions.end());
        }
        at->links.emplace_back(&joint, record.link);
        if (redundancy.notUnique[record.id]) {
            at->unique = false;
        }
    }
    // A stop is its joint's: the limit's source is the joint's
    for (std::size_t k = 0; k < si.limits.size(); ++k) {
        const std::string& name = si.sourceName(si.limits[k].source);
        auto at = std::find_if(reactions.begin(), reactions.end(), [&](const Reaction& r) {
            return r.source == name;
        });
        if (at == reactions.end()) {
            reactions.push_back({name, {}, {}});
            at = std::prev(reactions.end());
        }
        at->limits.push_back(k);
    }
    std::vector<Channel> channels;
    std::string notUnique;
    for (const auto& reaction : reactions) {
        channels.push_back({reaction.source, "Force", 3, reaction.unique});
        channels.push_back({reaction.source, "Torque", 3, reaction.unique});
        if (!reaction.unique) {
            notUnique += (notUnique.empty() ? "" : ", ") + reaction.source;
        }
    }
    if (redundancy.removed > 0) {
        Base::Console().warning(
            "Assembly: the %s analysis takes out %d equations of redundant joints, which repeat "
            "what other joints hold; the reactions at %s are not unique\n",
            kind,
            redundancy.removed,
            notUnique.c_str());
    }
    for (const auto& motor : rotationMotors) {
        channels.push_back({si.sourceName(motor.first), "MotorTorque", 1});
    }
    for (const auto& motor : linearMotors) {
        channels.push_back({si.sourceName(motor.first), "MotorForce", 1});
    }
    for (const auto& spring : springs) {
        channels.push_back({si.sourceName(spring.first), "SpringForce", 1});
        channels.push_back({si.sourceName(spring.first), "SpringLength", 1});
    }
    for (const auto& torsion : torsions) {
        channels.push_back({si.sourceName(torsion.first), "SpringTorque", 1});
        channels.push_back({si.sourceName(torsion.first), "SpringAngle", 1});
    }
    for (const auto& bushing : bushings) {
        channels.push_back({si.sourceName(bushing.first), "BushingForce", 3});
        channels.push_back({si.sourceName(bushing.first), "BushingTorque", 3});
    }
    for (const auto& pair : si.dynamics->contacts) {
        channels.push_back({si.sourceName(pair.source), "ContactForce", 3});
    }
    // A link's wrench on the body of a frame (a joint's or a limit's frame
    // I), absolute, the torque about 'about': Chrono's links report it on
    // either body in their own frame on it, a NeutralLink from its rows
    auto wrenchOnI = [&](Id frameI,
                         const std::shared_ptr<ch::ChLinkBase>& base,
                         const ch::ChVector3d& about) -> ch::ChWrenchd {
        const ch::ChBody* bodyI = run.d->bodyOf(frameI).get();
        // A redundant joint's link the run took out holds nothing
        if (!base->GetSystem()) {
            return {ch::VNULL, ch::VNULL};
        }
        if (auto neutral = std::dynamic_pointer_cast<NeutralLink>(base)) {
            return neutral->wrenchOn(bodyI, about);
        }
        auto link = std::dynamic_pointer_cast<ch::ChLink>(base);
        if (!link) {
            return {ch::VNULL, ch::VNULL};
        }
        ch::ChWrenchd w;
        if (link->GetBody2() == bodyI) {
            w = link->GetFrame2Abs().TransformWrenchLocalToParent(link->GetReaction2());
        }
        else if (link->GetBody1() == bodyI) {
            w = link->GetFrame1Abs().TransformWrenchLocalToParent(link->GetReaction1());
        }
        else {
            return {ch::VNULL, ch::VNULL};
        }
        // About the absolute origin, as the frame transform gives it
        return {w.force, w.torque - ch::Vcross(about, w.force)};
    };

    // The limits as rigid stops (sec 4, D1's leftovers). A step that takes
    // a limit's value past a bound gets a link holding it there, and the
    // positions and velocities are projected onto it: the plastic impact,
    // the impulse that leaves the least kinetic energy. A restitution e
    // takes that impulse 1 + e times and lets the stop go at once; a held
    // stop is let go once it pulls rather than pushes. A static run settles
    // on its stops, never bouncing. HHT with the direct solver takes no
    // unilateral constraint, so the stops are events between steps.
    struct Stop
    {
        std::shared_ptr<ch::ChLinkBase> link;
        bool atMin = false;
        double last = 0.0;  // the value the step before
    };
    std::vector<double> reach(n, 1e-3);  // a body's frames' farthest from its centre, m
    for (const auto& frame : si.frames) {
        if (frame.body < n) {
            reach[frame.body] = std::max(reach[frame.body], frame.local.getPosition().Length());
        }
    }
    std::vector<Stop> stops(si.limits.size());
    std::vector<double> limitAngles = run.d->limitAngle;
    auto stopping = [&](const Limit& limit) {
        return run.d->validFrame(limit.frameI) && run.d->validFrame(limit.frameJ)
            && !run.d->isRigid(limit.frameI, limit.frameJ) && (limit.min || limit.max);
    };
    for (std::size_t k = 0; k < si.limits.size(); ++k) {
        if (stopping(si.limits[k])) {
            stops[k].last = run.d->limitValue(si.limits[k], limitAngles);
        }
    }
    const double restitution = std::clamp(spec.restitution, 0.0, 1.0);
    const double gravity = si.dynamics->gravity.Length();
    auto handleStops = [&](double step, bool bounce) {
        // Let go of the stops that pull
        for (std::size_t k = 0; k < stops.size(); ++k) {
            Stop& stop = stops[k];
            if (!stop.link) {
                continue;
            }
            const Limit& limit = si.limits[k];
            const ch::ChFramed fi = run.d->globalOf(limit.frameI);
            const ch::ChWrenchd w = wrenchOnI(limit.frameI, stop.link, fi.GetPos());
            // On frame I's body: at the least value the stop pushes it back
            // along -zI (J away along +zI), at the most along +zI
            const double push = ch::Vdot(
                limit.kind == LimitKind::TranslationZ ? w.force : w.torque,
                fi.GetRotMat().GetAxisZ());
            if (stop.atMin ? push > 0.0 : push < 0.0) {
                sys.RemoveLink(stop.link);
                stop.link.reset();
            }
        }
        // Hold the bounds crossed
        std::vector<std::size_t> hit;
        double fastest = 0.0;  // m/s, a turn's at its body's farthest frame
        for (std::size_t k = 0; k < stops.size(); ++k) {
            const Limit& limit = si.limits[k];
            if (!stopping(limit)) {
                continue;
            }
            Stop& stop = stops[k];
            const double value = run.d->limitValue(limit, limitAngles);
            double speed = std::abs(value - stop.last) / step;
            stop.last = value;
            if (stop.link) {
                continue;
            }
            if (limit.min && value < *limit.min - limit.tolerance) {
                stop.atMin = true;
                stop.link = run.d->boundLink(limit, *limit.min);
            }
            else if (limit.max && value > *limit.max + limit.tolerance) {
                stop.atMin = false;
                stop.link = run.d->boundLink(limit, *limit.max);
            }
            else {
                continue;
            }
            if (limit.kind == LimitKind::RotationZ) {
                const Id body = si.frames[limit.frameJ].body;
                speed *= body < n ? reach[body] : 1e-3;
            }
            fastest = std::max(fastest, speed);
            hit.push_back(k);
        }
        if (hit.empty()) {
            return;
        }
        std::vector<ch::ChVector3d> velocity(n);
        std::vector<ch::ChVector3d> spin(n);
        for (std::size_t b = 0; b < n; ++b) {
            velocity[b] = run.d->bodies[b]->GetPosDt();
            spin[b] = run.d->bodies[b]->GetAngVelLocal();
        }
        // Tight: the velocity projection adds the position residual over
        // its own step of 1e-6 s
        sys.DoAssembly(static_cast<int>(ch::AssemblyAnalysis::Level::POSITION)
                           | static_cast<int>(ch::AssemblyAnalysis::Level::VELOCITY),
                       30,
                       1e-14,
                       1e-12,
                       1e-15);
        run.d->commitAngles();
        // Bounce, unless it is too slow to leave the stop for a step: what
        // gravity gives in two steps, a resting part's bump
        if (bounce && restitution > 0.0 && fastest > 2.0 * gravity * step) {
            for (std::size_t b = 0; b < n; ++b) {
                const auto& body = run.d->bodies[b];
                if (body->IsFixed()) {
                    continue;
                }
                const ch::ChVector3d v = body->GetPosDt();
                const ch::ChVector3d w = body->GetAngVelLocal();
                body->SetPosDt(v + restitution * (v - velocity[b]));
                body->SetAngVelLocal(w + restitution * (w - spin[b]));
            }
            for (std::size_t k : hit) {
                sys.RemoveLink(stops[k].link);
                stops[k].link.reset();
            }
        }
        for (std::size_t k : hit) {
            stops[k].last = run.d->limitValue(si.limits[k], limitAngles);
        }
    };

    auto valuesNow = [&]() {
        std::vector<double> out;
        out.reserve(6 * reactions.size() + rotationMotors.size() + linearMotors.size()
                    + 2 * springs.size() + 2 * torsions.size() + 6 * bushings.size());
        for (const auto& reaction : reactions) {
            const Id frameI = reaction.links.empty() ? si.limits[reaction.limits.front()].frameI
                                                     : reaction.links.front().first->frameI;
            const ch::ChVector3d about = run.d->globalOf(frameI).GetPos();
            ch::ChVector3d force;
            ch::ChVector3d torque;
            for (const auto& [joint, link] : reaction.links) {
                const ch::ChWrenchd w = wrenchOnI(joint->frameI, link, about);
                force += w.force;
                torque += w.torque;
            }
            for (std::size_t k : reaction.limits) {
                if (stops[k].link) {
                    const ch::ChWrenchd w = wrenchOnI(si.limits[k].frameI, stops[k].link, about);
                    force += w.force;
                    torque += w.torque;
                }
            }
            for (int k = 0; k < 3; ++k) {
                out.push_back(force[k] / SI::force);
            }
            for (int k = 0; k < 3; ++k) {
                out.push_back(torque[k] / SI::torque);
            }
        }
        for (const auto& motor : rotationMotors) {
            out.push_back(motor.second->GetMotorTorque() / SI::torque);
        }
        for (const auto& motor : linearMotors) {
            out.push_back(motor.second->GetMotorForce() / SI::force);
        }
        for (const auto& spring : springs) {
            out.push_back(spring.second->GetForce() / SI::force);
            out.push_back(spring.second->GetLength() / SI::length);
        }
        for (const auto& torsion : torsions) {
            out.push_back(torsion.second->GetTorque() / SI::torque);
            out.push_back(torsion.second->GetAngle());
        }
        for (const auto& bushing : bushings) {
            const ch::ChWrenchd w = bushing.second->wrenchNow();
            for (int k = 0; k < 3; ++k) {
                out.push_back(w.force[k] / SI::force);
            }
            for (int k = 0; k < 3; ++k) {
                out.push_back(w.torque[k] / SI::torque);
            }
        }
        // A set's: the force its group A's bodies take, each load's A
        // being the A side (a set within A: its first body)
        std::vector<ch::ChVector3d> onSet(si.dynamics->contacts.size());
        for (const auto& load : contactLoads) {
            load->ComputeQ(nullptr, nullptr);
            onSet[load->set] -= load->onB;
        }
        for (const auto& force : onSet) {
            for (int k = 0; k < 3; ++k) {
                out.push_back(force[k] / SI::force);
            }
        }
        return out;
    };
    // Inverse dynamics (a Kinematic run's reactions, sec 6.3): the poses
    // are the kinematic run's. At each, the velocities and accelerations the
    // motions give -- central differences of the poses Chrono's assembly
    // finds a short step either side, the motors at those times (forward
    // ones where a formula has no value before) -- and the multipliers that
    // make them: Cq' L = M a - f, solved by least squares, which is exact
    // and the only answer when the motions drive every freedom. With a
    // freedom left undriven there is none: the frames go out without values.
    if (prescribed) {
        auto plain = [&]() {
            for (const auto& [time, poses] : *prescribed) {
                if (!sink.frame(time, poses)) {
                    return Status::Cancelled;
                }
            }
            return Status::Ok;
        };
        try {
            run.d->prepare();
            ch::ChSparseMatrix Cq;
            sys.GetConstraintJacobianMatrix(Cq);
            MatX J(Cq);
            for (Eigen::Index r = 0; r < J.rows(); ++r) {
                const double norm = J.row(r).norm();
                if (norm > 0.0) {
                    J.row(r) /= norm;
                }
            }
            Eigen::Index rank = 0;
            if (J.rows() > 0 && J.cols() > 0) {
                Eigen::ColPivHouseholderQR<MatX> qr(J);
                qr.setThreshold(rankTolerance);
                rank = qr.rank();
            }
            if (rank < J.cols()) {
                Base::Console().warning(
                    "Assembly: the kinematic run's reactions need the motions to drive every "
                    "freedom, and %d are left free: the run reports none\n",
                    static_cast<int>(J.cols() - rank));
                return plain();
            }
        }
        catch (const std::exception& e) {
            Base::Console().error("Assembly: the Chrono backend's reactions failed: %s\n",
                                  e.what());
            return plain();
        }
        if (!channels.empty()) {
            sink.channels(channels);
        }
        // The poses in SI, each body's frame at its centre of mass
        auto place = [&](const std::vector<BodyPose>& poses) {
            for (const auto& pose : poses) {
                if (pose.body >= n || run.d->bodies[pose.body]->IsFixed()) {
                    continue;
                }
                Base::Placement p = pose.placement;
                p.setPosition(p.getPosition() * SI::length);
                const ch::ChFramed f = toCh(p * centre[pose.body]);
                auto& body = run.d->bodies[pose.body];
                body->SetPos(f.GetPos());
                body->SetRot(f.GetRot());
                body->SetPosDt(ch::VNULL);
                body->SetAngVelLocal(ch::VNULL);
            }
        };
        // Short against any motion's time scale, long against the
        // assembly's tolerance: a rounding of 1e-14 m reads as 1e-6 m/s^2
        const double delta = 1e-4;
        struct Stencil
        {
            std::vector<int> at;
            std::vector<double> v;
            std::vector<double> a;
        };
        const Stencil central {{-1, 0, 1}, {-0.5, 0.0, 0.5}, {1.0, -2.0, 1.0}};
        const Stencil forward {{0, 1, 2, 3},
                               {-11.0 / 6.0, 3.0, -1.5, 1.0 / 3.0},
                               {2.0, -5.0, 4.0, -1.0}};
        bool warned = false;
        try {
            for (const auto& [time, poses] : *prescribed) {
                const double t = time;
                const Stencil& stencil = run.d->driversFinite(t - delta) ? central : forward;
                std::vector<std::vector<Pose>> at;
                std::vector<Pose> here;
                for (int k : stencil.at) {
                    place(poses);
                    sys.SetChTime(t + k * delta);
                    sys.DoAssembly(static_cast<int>(ch::AssemblyAnalysis::Level::POSITION),
                                   30,
                                   1e-14,
                                   1e-12,
                                   1e-15);
                    at.push_back(run.d->current());
                    if (k == 0) {
                        here = at.back();
                    }
                }
                run.d->setPoses(here);
                run.d->commitAngles();
                sys.SetChTime(t);
                for (std::size_t b = 0; b < n; ++b) {
                    auto& body = run.d->bodies[b];
                    if (body->IsFixed()) {
                        continue;
                    }
                    ch::ChVector3d v;
                    ch::ChVector3d a;
                    ch::ChVector3d w;
                    ch::ChVector3d alpha;
                    for (std::size_t k = 0; k < stencil.at.size(); ++k) {
                        const Pose& p = at[k][b];
                        const ch::ChVector3d turn = rotationFrom(here[b].q, p.q);
                        v += stencil.v[k] * p.p;
                        a += stencil.a[k] * p.p;
                        w += stencil.v[k] * turn;
                        alpha += stencil.a[k] * turn;
                    }
                    body->SetPosDt(v / delta);
                    body->SetPosDt2(a / (delta * delta));
                    body->SetAngVelLocal(w / delta);
                    body->SetAngAccLocal(alpha / (delta * delta));
                }
                // The contacts of the pose: what a step would have found
                if (contact) {
                    sys.ComputeCollisions();
                }
                run.d->prepare();
                const unsigned int nv = sys.GetNumCoordsVelLevel();
                ch::ChState X(sys.GetNumCoordsPosLevel(), &sys);
                ch::ChStateDelta V(nv, &sys);
                ch::ChStateDelta A(nv, &sys);
                double now = 0.0;
                sys.StateGather(X, V, now);
                sys.StateGatherAcceleration(A);
                ch::ChVectorDynamic<> b(nv);
                b.setZero();
                sys.LoadResidual_Mv(b, A, 1.0);
                sys.LoadResidual_F(b, -1.0);
                ch::ChSparseMatrix Cq;
                sys.GetConstraintJacobianMatrix(Cq);
                const MatX Jt = MatX(Cq).transpose();
                const VecX L = Jt.colPivHouseholderQr().solve(VecX(b));
                const double off = (Jt * L - VecX(b)).norm();
                if (!warned && !(off <= 1e-6 * b.norm() + 1e-9)) {
                    warned = true;
                    Base::Console().warning(
                        "Assembly: the kinematic run's reactions at %g s leave %g of the "
                        "motion's forces unbalanced\n",
                        t,
                        off / std::max(b.norm(), 1e-300));
                }
                sys.StateScatterReactions(L);
                sink.values(valuesNow());
                if (!sink.frame(t, poses)) {
                    return Status::Cancelled;
                }
            }
        }
        catch (const std::exception& e) {
            Base::Console().error("Assembly: the Chrono backend's reactions failed: %s\n",
                                  e.what());
            return Status::Failed;
        }
        return Status::Ok;
    }

    if (!channels.empty()) {
        sink.channels(channels);
    }

    // Static (sec 4, step 4): the stable equilibrium at a time, by dynamic
    // relaxation with kinetic damping -- the dynamics stepped from rest with
    // the time held, every velocity zeroed at each peak of the kinetic
    // energy, settled once a whole cycle moves no frame by more than the
    // tolerance. Chrono's static Newton (DoStaticNonlinear) is no use here:
    // only its mates carry a tangent stiffness, so a pendulum has none in
    // its free turn, and a Newton step goes to the nearest equilibrium,
    // stable or not. This descends, and its reactions are the acceleration
    // analysis's at rest.
    const bool statics = spec.analysis == Analysis::Static;
    // The accelerations and the reactions at the current state, from
    // velocities made consistent first: a single FULL pass projects them
    // and differentiates the jump over its own step of 1e-6 s, so a motion
    // starting to move (or a Position motion's rate after a static rest)
    // read as an impulse -- the cam's start, ten thousand times its force
    // `earlier`: the velocity projections made since the velocities were
    // last right, besides the two here (the initial velocities' one)
    auto assembleFull = [&](int earlier = 0) {
        sys.DoAssembly(static_cast<int>(ch::AssemblyAnalysis::Level::POSITION)
                       | static_cast<int>(ch::AssemblyAnalysis::Level::VELOCITY));
        sys.DoAssembly(static_cast<int>(ch::AssemblyAnalysis::Level::FULL));
        // Each velocity projection is a step of 1e-6 s under the forces: two
        // left the velocities 2e-6 s of acceleration off, the rack 1.4e-3 mm
        // off after 0.1 s. Taken back by the accelerations the second kept.
        constexpr double projectionStep = 1e-6;
        const double back = (2.0 + earlier) * projectionStep;
        for (const auto& body : run.d->bodies) {
            if (body->IsFixed()) {
                continue;
            }
            body->SetPosDt(body->GetPosDt() - back * body->GetPosDt2());
            body->SetAngVelLocal(body->GetAngVelLocal() - back * body->GetAngAccLocal());
        }
    };
    // The initial velocities on the start assembly (sec 6.1): a body's set
    // as given, a joint's rate held by a Speed motor of the joint's kind
    // while one projection makes the velocities consistent with every
    // joint -- the least mass-weighted change, what a joint does not allow
    // taken out. In SI with the model. False when there are none.
    auto setInitialVelocities = [&]() {
        bool any = false;
        std::vector<std::shared_ptr<ch::ChLinkBase>> held;
        for (const auto& initial : si.dynamics->initial) {
            if (!run.d->validFrame(initial.frameI)) {
                continue;
            }
            if (initial.kind == InitialVelocity::Kind::Body) {
                const auto& body = run.d->bodyOf(initial.frameI);
                if (!body || body->IsFixed()) {
                    continue;
                }
                // v is the frame's origin's; the body's own is its centre
                // of mass's
                const ch::ChVector3d w = toCh(initial.w);
                const ch::ChVector3d at = run.d->globalOf(initial.frameI).GetPos();
                body->SetPosDt(toCh(initial.v) + ch::Vcross(w, body->GetPos() - at));
                body->SetAngVelParent(w);
                any = true;
                continue;
            }
            if (!run.d->validFrame(initial.frameJ)
                || run.d->isRigid(initial.frameI, initial.frameJ)) {
                continue;
            }
            const ch::ChFramed f1 = run.d->driverLocalOf(initial.frameJ);
            const ch::ChFramed f2 = run.d->localOf(initial.frameI);
            auto rate = std::make_shared<ch::ChFunctionConst>(initial.rate);
            if (initial.kind == InitialVelocity::Kind::RotationZ) {
                auto motor = std::make_shared<ch::ChLinkMotorRotationSpeed>();
                motor->SetSpindleConstraint(ch::ChLinkMotorRotation::SpindleConstraint::FREE);
                motor->Initialize(run.d->bodyOf(initial.frameJ),
                                  run.d->bodyOf(initial.frameI),
                                  true,
                                  f1,
                                  f2);
                motor->SetMotorFunction(rate);
                sys.AddLink(motor);
                startHere(motor);
                held.push_back(motor);
            }
            else {
                auto motor = std::make_shared<ch::ChLinkMotorLinearSpeed>();
                motor->SetGuideConstraint(ch::ChLinkMotorLinear::GuideConstraint::FREE);
                motor->Initialize(run.d->bodyOf(initial.frameJ),
                                  run.d->bodyOf(initial.frameI),
                                  true,
                                  f1,
                                  f2);
                motor->SetMotorFunction(rate);
                sys.AddLink(motor);
                startHere(motor);
                held.push_back(motor);
            }
            any = true;
        }
        if (!any) {
            return false;
        }
        sys.DoAssembly(static_cast<int>(ch::AssemblyAnalysis::Level::VELOCITY));
        for (const auto& motor : held) {
            sys.RemoveLink(motor);
        }
        return true;
    };
    // A step of dt, in substeps that resolve the impacts coming (sec 8.2,
    // events): an impact on the default law is over in some tens of
    // microseconds, and HHT taking it in one step of a millisecond -- a
    // trapezoidal-family method on a stiff nonlinear spring -- gained energy,
    // a box landing flat bouncing higher each time. From the contacts within
    // the envelope: how deep an impact at the speed of approach goes (all of
    // its kinetic energy into the law's spring, or the penetration now), the
    // law's stiffness there, and the period of the lighter body on it; a
    // substep is a twentieth of the shortest. The law's damping there too:
    // a step longer than 2 m / c turns its decay into an oscillation, which
    // the contact, pushing only, rectified into a kick -- a box at rest
    // jumping 0.2 mm. A part resting on another is limited by neither: its
    // period is milliseconds, its penetration a fraction of the damping's.
    auto contactStep = [&]() {
        double shortest = std::numeric_limits<double>::infinity();
        double damped = std::numeric_limits<double>::infinity();
        double arrival = std::numeric_limits<double>::infinity();
        for (const auto& load : contactLoads) {
            if (load->points.empty()) {
                continue;
            }
            const auto a = load->GetBodyA();
            const auto b = load->GetBodyB();
            double mass = std::numeric_limits<double>::infinity();
            for (const auto& body : {a, b}) {
                if (!body->IsFixed() && body->GetMass() > 0.0) {
                    mass = std::min(mass, body->GetMass());
                }
            }
            if (!std::isfinite(mass)) {
                continue;
            }
            // Its patches are springs side by side on that mass, each
            // pushing as one point: each holds its share, the ones pressing
            // when any does. Each point's share, and a torus resting on a
            // ring of points took substeps of 1.4 us; each one within the
            // envelope's, a fraction of a microsecond.
            std::vector<bool> pressed(load->patches, false);
            for (const auto& point : load->points) {
                if (-ch::Vdot(point.normal,
                              b->TransformPointLocalToParent(point.onB)
                                  - a->TransformPointLocalToParent(point.onA))
                    > 0.0) {
                    pressed[point.patch] = true;
                }
            }
            const auto pressing = std::count(pressed.begin(), pressed.end(), true);
            mass /= static_cast<double>(pressing ? pressing : load->patches);
            const ContactLaw& law = load->law;
            for (const auto& point : load->points) {
                const ch::ChVector3d pA = a->TransformPointLocalToParent(point.onA);
                const ch::ChVector3d pB = b->TransformPointLocalToParent(point.onB);
                const double depth = -ch::Vdot(point.normal, pB - pA);
                const double rate = -ch::Vdot(point.normal,
                                              b->PointSpeedLocalToParent(point.onB)
                                                  - a->PointSpeedLocalToParent(point.onA));
                const double approach = std::max(rate, 0.0);
                const double k = load->stiffness[point.patch];
                const double reach =
                    std::pow((law.exponent + 1.0) * mass * approach * approach / (2.0 * k),
                             1.0 / (law.exponent + 1.0));
                const double deepest = std::max({depth, reach, 1e-12});
                const double stiffness = law.exponent * k * std::pow(deepest, law.exponent - 1.0);
                if (!(stiffness > 0.0)) {
                    continue;
                }
                const double period = 2.0 * pi * std::sqrt(mass / stiffness);
                // Not touching yet: steps up to half the time it takes to,
                // coming in at its speed and gravity's acceleration, the
                // impact's once that is within its period. A step to the
                // contact itself went in by what gravity added, and the
                // Newton iterations of a stiff law that deep failed.
                if (depth <= 0.0) {
                    const double gap = -depth;
                    const double root = std::sqrt(approach * approach + 2.0 * gravity * gap);
                    if (!(approach + root > 0.0)) {
                        continue;
                    }
                    const double toContact = 2.0 * gap / (approach + root);
                    if (toContact > period) {
                        arrival = std::min(arrival, 0.5 * toContact);
                        continue;
                    }
                }
                shortest = std::min(shortest, period);
                const double damping = step(deepest, 0.0, 0.0, law.penetration, law.maxDamping);
                if (damping > 0.0) {
                    damped = std::min(damped, 2.0 * mass / damping);
                }
            }
        }
        return std::min({shortest / 50.0, damped, arrival});
    };
    // The deepest a load's points press now, -inf for none
    auto pressing = [](const ContactLoad& load) {
        double deepest = -std::numeric_limits<double>::infinity();
        const auto a = load.GetBodyA();
        const auto b = load.GetBodyB();
        for (const auto& point : load.points) {
            deepest = std::max(deepest,
                               -ch::Vdot(point.normal,
                                         b->TransformPointLocalToParent(point.onB)
                                             - a->TransformPointLocalToParent(point.onA)));
        }
        return deepest;
    };
    // Whether a contact the last step found new is in too deep: deeper than a
    // tenth of how deep its impact goes at the speed it comes in at. A
    // contact found only once in -- the envelope is small -- had no force
    // over the step that took it there, and its law's spring hands back
    // what that depth stores: at a hundredth of a millimetre on the default
    // law, fifteen times a falling box's energy.
    auto late = [&](const ContactLoad& load) {
        const auto a = load.GetBodyA();
        const auto b = load.GetBodyB();
        double mass = std::numeric_limits<double>::infinity();
        for (const auto& body : {a, b}) {
            if (!body->IsFixed() && body->GetMass() > 0.0) {
                mass = std::min(mass, body->GetMass());
            }
        }
        const ContactLaw& law = load.law;
        for (const auto& point : load.points) {
            const ch::ChVector3d pA = a->TransformPointLocalToParent(point.onA);
            const ch::ChVector3d pB = b->TransformPointLocalToParent(point.onB);
            const double depth = -ch::Vdot(point.normal, pB - pA);
            if (!(depth > 1e-12)) {
                continue;
            }
            const double rate = std::max(-ch::Vdot(point.normal,
                                                   b->PointSpeedLocalToParent(point.onB)
                                                       - a->PointSpeedLocalToParent(point.onA)),
                                         0.0);
            const double reach = std::isfinite(mass)
                ? std::pow((law.exponent + 1.0) * mass * rate * rate
                               / (2.0 * load.stiffness[point.patch]),
                           1.0 / (law.exponent + 1.0))
                : 0.0;
            if (depth > 0.1 * reach) {
                return true;
            }
        }
        return false;
    };
    // A step of dt, in substeps; a substep that found a contact late is taken
    // again, an eighth as long, from the state before it, and the steps
    // grow back after.
    auto advance = [&](double dt) {
        if (!contact) {
            sys.DoStepDynamics(dt);
            return;
        }
        const unsigned int nx = sys.GetNumCoordsPosLevel();
        const unsigned int nv = sys.GetNumCoordsVelLevel();
        ch::ChState x(nx, &sys);
        ch::ChStateDelta v(nv, &sys);
        ch::ChStateDelta a(nv, &sys);
        double done = 0.0;
        double cap = dt;
        std::vector<double> before(contactLoads.size());
        bool fresh = false;  // collision detection last ran at the state now
        while (dt - done > 1e-12 * dt) {
            double h = std::min({dt - done, cap, contactStep()});
            if (dt - done - h < 1e-9 * dt) {
                h = dt - done;
            }
            for (std::size_t i = 0; i < contactLoads.size(); ++i) {
                before[i] = pressing(*contactLoads[i]);
            }
            double t = 0.0;
            sys.StateGather(x, v, t);
            sys.StateGatherAcceleration(a);
            contactCollision->reuse = fresh;
            auto implicit =
                std::dynamic_pointer_cast<ch::ChTimestepperImplicit>(sys.GetTimestepper());
            const unsigned int terminated = implicit ? implicit->GetNumTerminated() : 0;
            sys.DoStepDynamics(h);
            fresh = false;
            // A step whose Newton iterations ran off -- a state not finite
            // -- is taken again too, before collision detection sees it
            bool finite = true;
            for (const auto& body : run.d->bodies) {
                finite = finite && std::isfinite(body->GetPos().Length2())
                    && std::isfinite(body->GetRot().Length2())
                    && std::isfinite(body->GetPosDt().Length2())
                    && std::isfinite(body->GetAngVelLocal().Length2());
            }
            if (!finite && !(h > 1e-9)) {
                sys.StateScatter(x, v, t, ch::UpdateFlags::UPDATE_ALL);
                throw std::runtime_error("the contact's integration diverged");
            }
            // And one whose iterations stopped unconverged at their most:
            // HHT takes the state they stopped at, and a cube landing flat
            // on its mesh at k = 1.41e5 (1.2e5 and 2e5 landed) was thrown
            // half a kilometre, finite. Taken as it is below a nanosecond.
            const bool unconverged =
                implicit && implicit->GetNumTerminated() > terminated && h > 1e-9;
            bool tooDeep = !finite || unconverged;
            if (finite && !unconverged) {
                sys.ComputeCollisions();
                fresh = true;
                for (std::size_t i = 0; i < contactLoads.size() && !tooDeep; ++i) {
                    tooDeep = before[i] <= 0.0 && late(*contactLoads[i]);
                }
            }
            if (tooDeep && h > 1e-9) {
                sys.StateScatter(x, v, t, ch::UpdateFlags::UPDATE_ALL);
                sys.StateScatterAcceleration(a);
                sys.ComputeCollisions();
                fresh = true;
                cap = h / 8.0;
                continue;
            }
            done += h;
            cap = std::min(dt, cap * 2.0);
        }
    };
    const double tolerance = std::max(spec.errorTolerance, 1e-9) * SI::length;
    const int maxRelaxSteps = 50000;
    auto kinetic = [&]() {
        double energy = 0.0;
        for (const auto& body : run.d->bodies) {
            if (body->IsFixed()) {
                continue;
            }
            const ch::ChVector3d w = body->GetAngVelLocal();
            energy += body->GetMass() * body->GetPosDt().Length2() + w.Dot(body->GetInertia() * w);
        }
        return 0.5 * energy;
    };
    auto toRest = [&]() {
        for (const auto& body : run.d->bodies) {
            body->ForceToRest();
        }
    };
    auto settle = [&](double time) {
        sys.SetChTime(time);
        // At the time's motions first, at rest
        sys.DoAssembly(static_cast<int>(ch::AssemblyAnalysis::Level::POSITION));
        toRest();
        run.d->commitAngles();
        std::vector<ch::ChVector3d> at(n);
        std::vector<ch::ChQuaterniond> turned(n);
        auto mark = [&]() {
            for (std::size_t b = 0; b < n; ++b) {
                at[b] = run.d->bodies[b]->GetPos();
                turned[b] = run.d->bodies[b]->GetRot();
            }
        };
        auto moved = [&]() {
            double most = 0.0;
            for (std::size_t b = 0; b < n; ++b) {
                const auto& body = run.d->bodies[b];
                const ch::ChQuaterniond q = turned[b].GetConjugate() * body->GetRot();
                const double angle = 2.0 * std::atan2(q.GetVector().Length(), std::abs(q.e0()));
                most = std::max(most, (body->GetPos() - at[b]).Length() + angle * reach[b]);
            }
            return most;
        };
        mark();
        double last = 0.0;
        for (int i = 0; i < maxRelaxSteps; ++i) {
            // Ending at the time held: a Position motion holds its value then
            sys.SetChTime(time - h);
            advance(h);
            run.d->commitAngles();
            handleStops(h, false);
            const double energy = kinetic();
            if (energy > last) {
                last = energy;
                continue;
            }
            // Past a peak, or at rest
            toRest();
            last = 0.0;
            const bool still = moved() <= tolerance;
            mark();
            if (still) {
                sys.SetChTime(time);
                // The accelerations at rest, near none, and the reactions
                // (a Position motion's rate counts there, its inertia: a
                // slow sweep's is small)
                assembleFull();
                toRest();
                return true;
            }
        }
        return false;
    };

    Status status = Status::Ok;
    try {
        sys.SetChTime(spec.tStart);
        const bool moving = !statics && setInitialVelocities();
        // The accelerations at the start, gravity's: HHT takes those it is
        // given, and from rest that was none, the run lagging a part of a
        // step behind -- the pendulum's period long by 0.2% at 1 ms, the
        // error first order in the step
        assembleFull(moving ? 1 : 0);
        auto stepOnce = [&](double dt) {
            advance(dt);
            // The neutral links count their turns from the angles last
            // committed: a pinion past half a turn read as the other way
            // round, the rack jumping a pitch
            run.d->commitAngles();
            handleStops(dt, true);
        };
        // A dynamic run starts in steps growing from a thousandth of one:
        // 64 of h / 1024, then h / 16, h / 8, h / 4, h / 2, together the
        // first step. HHT starts from the accelerations it is given, the
        // assembly analysis's, which have none that motion gives (its
        // velocity projection holds the positions); a whole step from them
        // left the velocities off, a cam's force 15% off at the first frame
        // and 1.5% at the second. The start's reactions (a joint's, a
        // motor's effort) are those after 32 and after 64 of the smallest
        // taken back to the start, a line through the two: the transient
        // gone by then, as measured on the cam, and the 60 us the reading is
        // late (0.2% of its force) taken out. What the state alone gives (a
        // spring's force, length, angle) is read at the start itself: a line
        // through a quadratic path misses it by a t1 t2 / 2.
        const int tinySteps = 64;
        const double tiny = h / 1024.0;
        for (std::size_t k = 0; k <= steps; ++k) {
            const double time = spec.tStart + static_cast<double>(k) * spec.tOutput;
            if (!statics && k == 0) {
                const std::vector<BodyPose> poses = posesNow();
                const std::vector<double> start = channels.empty() ? std::vector<double>()
                                                                   : valuesNow();
                std::vector<double> half;
                for (int i = 0; i < tinySteps; ++i) {
                    stepOnce(tiny);
                    if (i + 1 == tinySteps / 2 && !channels.empty()) {
                        half = valuesNow();
                    }
                }
                if (!channels.empty()) {
                    std::vector<double> values = valuesNow();
                    std::size_t v = 0;
                    for (const auto& channel : channels) {
                        const bool reaction = channel.quantity == "Force"
                            || channel.quantity == "Torque" || channel.quantity == "MotorTorque"
                            || channel.quantity == "MotorForce";
                        for (int c = 0; c < channel.width && v < values.size(); ++c, ++v) {
                            values[v] = reaction ? 2.0 * half[v] - values[v] : start[v];
                        }
                    }
                    sink.values(values);
                }
                if (!sink.frame(time, poses)) {
                    status = Status::Cancelled;
                    break;
                }
                continue;
            }
            if (statics) {
                if (!settle(time)) {
                    Base::Console().error(
                        "Assembly: the static analysis found no equilibrium at %g s in %d "
                        "steps: a part keeps moving (one nothing holds, or a load nothing "
                        "balances)\n",
                        time,
                        maxRelaxSteps);
                    status = Status::Failed;
                    break;
                }
            }
            else {
                int s = 0;
                if (k == 1) {
                    // The rest of the first step
                    for (double part = tinySteps * tiny; part < h; part *= 2.0) {
                        stepOnce(part);
                    }
                    s = 1;
                }
                for (; s < sub; ++s) {
                    stepOnce(h);
                }
            }
            if (!channels.empty()) {
                sink.values(valuesNow());
            }
            if (!sink.frame(time, posesNow())) {
                status = Status::Cancelled;
                break;
            }
        }
    }
    catch (const std::exception& e) {
        Base::Console().error("Assembly: the Chrono backend's dynamic run failed: %s\n", e.what());
        status = Status::Failed;
    }
    return status;
}

Status ChronoSession::exportNative(const std::string& path)
{
    if (!d->loaded) {
        return Status::Failed;
    }
    try {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            return Status::Failed;
        }
        ch::ChArchiveOutJSON archive(out);
        ch::ChSystemNSC& system = *d->sys;
        archive << CHNVP(system);
        return out ? Status::Ok : Status::Failed;
    }
    catch (const std::exception& e) {
        Base::Console().error("Assembly: the Chrono backend could not write %s: %s\n",
                              path.c_str(),
                              e.what());
        return Status::Failed;
    }
}

std::string ChronoSession::refusalMessage() const
{
    return d->refused;
}

int ChronoSession::linkCount() const
{
    return static_cast<int>(d->links.size());
}

int ChronoSession::neutralLinkCount() const
{
    return static_cast<int>(d->neutral.size());
}

std::vector<double> ChronoSession::constraintsAt(const std::vector<Base::Placement>& poses)
{
    std::vector<double> out;
    if (!d->loaded) {
        return out;
    }
    const auto saved = d->gather();
    std::vector<Pose> at = d->current();
    for (std::size_t b = 0; b < at.size() && b < poses.size(); ++b) {
        const ch::ChFramed f = toCh(poses[b]);
        at[b] = {f.GetPos(), f.GetRot()};
    }
    d->setPoses(at);
    const ch::ChVectorDynamic<> c = d->constraints();
    out.assign(c.data(), c.data() + c.size());
    d->scatter(saved);
    return out;
}

std::vector<std::vector<double>>
ChronoSession::jacobianAt(const std::vector<Base::Placement>& poses)
{
    std::vector<std::vector<double>> out;
    if (!d->loaded) {
        return out;
    }
    const auto saved = d->gather();
    std::vector<Pose> at = d->current();
    for (std::size_t b = 0; b < at.size() && b < poses.size(); ++b) {
        const ch::ChFramed f = toCh(poses[b]);
        at[b] = {f.GetPos(), f.GetRot()};
    }
    d->setPoses(at);
    d->sys->DescriptorPrepareInject();
    const MatX J = d->jacobian();
    for (Eigen::Index r = 0; r < J.rows(); ++r) {
        std::vector<double> row(6 * d->bodies.size(), 0.0);
        for (std::size_t b = 0; b < d->bodies.size(); ++b) {
            if (d->bodies[b]->IsFixed()) {
                continue;
            }
            const unsigned int off = d->bodies[b]->GetOffset_w();
            for (int k = 0; k < 6; ++k) {
                row[6 * b + k] = J(r, off + k);
            }
        }
        out.push_back(std::move(row));
    }
    d->scatter(saved);
    return out;
}

int ChronoSession::rank() const
{
    if (!d->loaded || !d->refused.empty()) {
        return 0;
    }
    d->prepare();
    ch::ChSparseMatrix Cq;
    d->sys->GetConstraintJacobianMatrix(Cq);
    MatX S(Cq);
    if (S.rows() == 0 || S.cols() == 0) {
        return 0;
    }
    for (Eigen::Index r = 0; r < S.rows(); ++r) {
        const double n = S.row(r).norm();
        if (n > 0.0) {
            S.row(r) /= n;
        }
    }
    Eigen::ColPivHouseholderQR<MatX> qr(S);
    qr.setThreshold(rankTolerance);
    return static_cast<int>(qr.rank());
}
