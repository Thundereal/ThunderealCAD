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
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
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
#include <chrono/collision/bullet/BulletCollision/CollisionDispatch/cbtCompoundCollisionAlgorithm.h>
#include <chrono/collision/bullet/BulletCollision/CollisionDispatch/cbtCompoundCompoundCollisionAlgorithm.h>
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
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
# include <chrono/fea/ChNodeFEAxyz.h>
# include <chrono/solver/ChKRMBlock.h>

# include "ChronoFlexible.h"
#endif
#include "JointEquations.h"
#include "ProfileGeometry.h"
#include "ReferenceBackend.h"
#include "Units.h"

using namespace Assembly::Solver;
using namespace Assembly::Solver::Profile;
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
    // On B now, absolute: the force at frame I's origin, or the torque
    ch::ChVector3d valueNow()
    {
        ComputeQ(nullptr, nullptr);
        return frame_Bw.TransformDirectionLocalToParent(torque ? locB_torque : locB_force);
    }
    bool isTorque() const
    {
        return torque;
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

// A penalty stop (Stop::Mode::Penalty): a limit's bound pushed back by a
// spring and a damper on how far past it the joint's value is, k d + c
// dd/dt, none short of the bound and never pulling -- along zI for a slide,
// about it for a turn. A is frame J's body, B frame I's, each loaded at its
// frame: on B along -zI at the least value (J pushed back along +zI), along
// +zI at the most, as a rigid stop's link pushes. A turn is counted from the
// one last committed, as the limits' are (limitValue()). Implicit through
// Chrono's numerical jacobian, as a contact is.
class StopLoad: public ch::ChLoadBodyBody
{
public:
    StopLoad(std::shared_ptr<ch::ChBody> a,
             std::shared_ptr<ch::ChBody> b,
             const ch::ChFramed& frameJ,
             const ch::ChFramed& frameI,
             const Limit& limit,
             double k,
             double c,
             double angle)
        : ch::ChLoadBodyBody(a, b, frameI)
        , turn(limit.kind == LimitKind::RotationZ)
        , atMin(limit.min.has_value())
        , bound(limit.min ? *limit.min : limit.max.value_or(0.0))
        , k(k)
        , c(c)
        , angle(angle)
    {
        SetApplicationFrameA(a->ch::ChFramed::TransformParentToLocal(frameJ));
        SetApplicationFrameB(b->ch::ChFramed::TransformParentToLocal(frameI));
    }

    StopLoad* Clone() const override
    {
        return new StopLoad(*this);
    }
    bool IsStiff() override
    {
        return true;
    }
    void ComputeJacobian(ch::ChState* state_x, ch::ChStateDelta* state_w) override
    {
        ch::ChLoadBodyBody::ComputeJacobian(state_x, state_w);
        ComputeQ(state_x, state_w);
    }
    void ComputeBodyBodyForceTorque(const ch::ChFrameMoving<>& /*rel_AB*/,
                                    ch::ChVector3d& force,
                                    ch::ChVector3d& moment) override
    {
        force = ch::VNULL;
        moment = ch::VNULL;
        const auto [value, rate] = valueAndRate();
        // How far past the bound, and how fast going further
        const double depth = atMin ? bound - value : value - bound;
        const double deeper = atMin ? -rate : rate;
        if (!(depth > 0.0)) {
            return;
        }
        const double push = std::max(k * depth + c * deeper, 0.0);
        // In frame I's axes, on frame I's body
        const ch::ChVector3d along(0.0, 0.0, atMin ? -push : push);
        (turn ? moment : force) = along;
    }
    // The force on B and the torque about its frame now, absolute
    ch::ChWrenchd wrenchNow()
    {
        ComputeQ(nullptr, nullptr);
        return {frame_Bw.TransformDirectionLocalToParent(locB_force),
                frame_Bw.TransformDirectionLocalToParent(locB_torque)};
    }
    ch::ChVector3d pointNow() const
    {
        return frame_Bw.GetPos();
    }
    // The turn now counted from here on
    void commit()
    {
        ComputeQ(nullptr, nullptr);
        angle = valueAndRate().first;
    }

    // Past its bound now, by how much
    double depthNow()
    {
        ComputeQ(nullptr, nullptr);
        const double value = valueAndRate().first;
        return atMin ? bound - value : value - bound;
    }
    // The limit's value and its rate, at frame_Aw and frame_Bw
    std::pair<double, double> valueAndRate() const
    {
        const ch::ChVector3d z = frame_Bw.GetRotMat().GetAxisZ();
        const ch::ChVector3d wI = frame_Bw.GetAngVelParent();
        if (!turn) {
            const ch::ChVector3d d = frame_Aw.GetPos() - frame_Bw.GetPos();
            return {ch::Vdot(d, z),
                    ch::Vdot(frame_Aw.GetPosDt() - frame_Bw.GetPosDt(), z)
                        + ch::Vdot(d, ch::Vcross(wI, z))};
        }
        const ch::ChVector3d xJ = frame_Aw.GetRotMat().GetAxisX();
        const double s = ch::Vdot(frame_Bw.GetRotMat().GetAxisY(), xJ);
        const double co = ch::Vdot(frame_Bw.GetRotMat().GetAxisX(), xJ);
        return {angle + wrapAngle(std::atan2(s, co) - angle),
                ch::Vdot(frame_Aw.GetAngVelParent() - wI, z)};
    }

    const bool turn;
    const bool atMin;
    const double bound;
    const double k;
    const double c;

private:
    double angle = 0.0;  // the turn last committed
};

// A torsion spring (ForceKind::TorsionSpring): about its joint's axis zI,
// preload - k (turn - rest) - c rate, the turn of xJ about zI and its rate
// as a stop's (StopLoad), on frame J's body (A) about +zI and on frame I's
// (B) about -zI -- Chrono's ChLinkRSDA's. That counted the turn at each of
// its updates from the last angle it saw: the states of a substep whose
// Newton iterations ran off moved it on, and the substep taken again from
// before them started a whole turn out (a return spring of 1000 N mm/rad
// at 6000 N mm, the lever it held thrown off its cam). Here the turn is
// counted from the angle committed after each step, as a penalty stop's;
// implicit through Chrono's numerical jacobian.
class TorsionLoad: public ch::ChLoadBodyBody
{
public:
    TorsionLoad(std::shared_ptr<ch::ChBody> a,
                std::shared_ptr<ch::ChBody> b,
                const ch::ChFramed& frameJ,
                const ch::ChFramed& frameI,
                double k,
                double c,
                double preload,
                double rest)
        : ch::ChLoadBodyBody(a, b, frameI)
        , k(k)
        , c(c)
        , preload(preload)
    {
        SetApplicationFrameA(a->ch::ChFramed::TransformParentToLocal(frameJ));
        SetApplicationFrameB(b->ch::ChFramed::TransformParentToLocal(frameI));
        ComputeQ(nullptr, nullptr);
        angle = rawAngle();
        this->rest = angle + rest;
    }

    TorsionLoad* Clone() const override
    {
        return new TorsionLoad(*this);
    }
    bool IsStiff() override
    {
        return true;
    }
    void ComputeJacobian(ch::ChState* state_x, ch::ChStateDelta* state_w) override
    {
        ch::ChLoadBodyBody::ComputeJacobian(state_x, state_w);
        ComputeQ(state_x, state_w);
    }
    void ComputeBodyBodyForceTorque(const ch::ChFrameMoving<>& /*rel_AB*/,
                                    ch::ChVector3d& force,
                                    ch::ChVector3d& moment) override
    {
        force = ch::VNULL;
        // In frame I's axes, on frame I's body
        moment = ch::ChVector3d(0.0, 0.0, -torque());
    }
    // The turn now counted from here on
    void commit()
    {
        ComputeQ(nullptr, nullptr);
        angle = turnAndRate().first;
    }
    // The torque, positive turning J up about zI, and the turn, now
    double torqueNow()
    {
        ComputeQ(nullptr, nullptr);
        return torque();
    }
    double angleNow()
    {
        ComputeQ(nullptr, nullptr);
        return turnAndRate().first;
    }
    double committed() const
    {
        return angle;
    }
    void setCommitted(double value)
    {
        angle = value;
    }

private:
    double rawAngle() const
    {
        const ch::ChVector3d xJ = frame_Aw.GetRotMat().GetAxisX();
        return std::atan2(ch::Vdot(frame_Bw.GetRotMat().GetAxisY(), xJ),
                          ch::Vdot(frame_Bw.GetRotMat().GetAxisX(), xJ));
    }
    std::pair<double, double> turnAndRate() const
    {
        const ch::ChVector3d z = frame_Bw.GetRotMat().GetAxisZ();
        return {angle + wrapAngle(rawAngle() - angle),
                ch::Vdot(frame_Aw.GetAngVelParent() - frame_Bw.GetAngVelParent(), z)};
    }
    double torque() const
    {
        const auto [turn, rate] = turnAndRate();
        return preload - k * (turn - rest) - c * rate;
    }

    double k = 0.0;
    double c = 0.0;
    double preload = 0.0;
    double rest = 0.0;
    double angle = 0.0;  // the turn last committed
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

// A joint's friction (Friction, JointFriction): Coulomb's against the
// joint's slip -- J's relative to I, along zI (a slide) and about it (a
// turn, at the radius) -- its coefficient by the slip's speed as a
// contact's: none at rest, the static at the stiction velocity, the dynamic
// from the friction velocity. Its load 'normal', the joint's reaction
// across its axis, is set after each step (the last step's: the reaction
// is the constraints', which the step solves with it) and held over the
// next. A is frame J's body, B frame I's, each loaded at its frame: B
// dragged along J's slip, A held back. Implicit through Chrono's numerical
// jacobian, as a stop and a contact are: at the stiction velocity of 0.1
// mm/s it is a stiff damper.
class FrictionLoad: public ch::ChLoadBodyBody
{
public:
    FrictionLoad(std::shared_ptr<ch::ChBody> a,
                 std::shared_ptr<ch::ChBody> b,
                 const ch::ChFramed& frameJ,
                 const ch::ChFramed& frameI,
                 const Friction& friction)
        : ch::ChLoadBodyBody(a, b, frameI)
        , friction(friction)
    {
        SetApplicationFrameA(a->ch::ChFramed::TransformParentToLocal(frameJ));
        SetApplicationFrameB(b->ch::ChFramed::TransformParentToLocal(frameI));
    }

    FrictionLoad* Clone() const override
    {
        return new FrictionLoad(*this);
    }
    bool IsStiff() override
    {
        return true;
    }
    void ComputeJacobian(ch::ChState* state_x, ch::ChStateDelta* state_w) override
    {
        ch::ChLoadBodyBody::ComputeJacobian(state_x, state_w);
        ComputeQ(state_x, state_w);
    }
    void ComputeBodyBodyForceTorque(const ch::ChFrameMoving<>& /*rel_AB*/,
                                    ch::ChVector3d& force,
                                    ch::ChVector3d& moment) override
    {
        force = ch::VNULL;
        moment = ch::VNULL;
        const auto [slide, turn] = slip();
        const double s = std::hypot(slide, turn);
        if (!(s > 0.0)) {
            return;
        }
        // Adams' FRICTION's, through no slip from the other way's: steepest
        // at rest. The contact's from no slip, flat there, left Newton a
        // block stopping at 1 ms steps to overshoot -- it crept on at 2
        // mm/s, the friction at its static -- where this is all but linear.
        const Friction& f = friction;
        const double mu = s < f.vStiction
            ? step(s, -f.vStiction, -f.muStatic, f.vStiction, f.muStatic)
            : step(s, f.vStiction, f.muStatic, f.vFriction, f.muDynamic);
        // In frame I's axes, on frame I's body, each in its share of the slip
        force.z() = mu * (normal + friction.preloadForce) * slide / s;
        moment.z() = mu * (friction.radius * normal + friction.preloadTorque) * turn / s;
    }
    // J's slip from I now: along zI, and about it at the radius, as speeds
    std::pair<double, double> slip() const
    {
        const ch::ChVector3d z = frame_Bw.GetRotMat().GetAxisZ();
        const ch::ChVector3d wI = frame_Bw.GetAngVelParent();
        double slide = 0.0;
        double turn = 0.0;
        if (friction.slides) {
            const ch::ChVector3d d = frame_Aw.GetPos() - frame_Bw.GetPos();
            slide = ch::Vdot(frame_Aw.GetPosDt() - frame_Bw.GetPosDt(), z)
                + ch::Vdot(d, ch::Vcross(wI, z));
        }
        if (friction.turns) {
            turn = ch::Vdot(frame_Aw.GetAngVelParent() - wI, z) * friction.radius;
        }
        return {slide, turn};
    }
    // The force on B and the torque about its frame now, absolute
    ch::ChWrenchd wrenchNow()
    {
        ComputeQ(nullptr, nullptr);
        return {frame_Bw.TransformDirectionLocalToParent(locB_force),
                frame_Bw.TransformDirectionLocalToParent(locB_torque)};
    }
    // The slip now, as slip() at the bodies' state
    std::pair<double, double> slipNow()
    {
        ComputeQ(nullptr, nullptr);
        return slip();
    }
    ch::ChVector3d pointNow() const
    {
        return frame_Bw.GetPos();
    }
    ch::ChVector3d axisNow() const
    {
        return frame_Bw.GetRotMat().GetAxisZ();
    }

    const Friction friction;
    double normal = 0.0;  // the joint's reaction across its axis, N
};

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
//
// Hunt and Crossley's damping for a coefficient of restitution r, the
// force k d^e (1 + a / v0 dd/dt), v0 the speed the impact came in at: its
// a exactly (Carvalho and Martins, 2019). With u = a / v0 dd/dt the impact
// integrates in closed form, m v0^2 / (a^2 k) (u - ln(1 + u)) =
// -d^(e+1) / (e+1) from where it comes in, so from no depth to no depth
// u - ln(1 + u) is the same coming in, u = a, and going out, u = -a r:
// whatever the exponent, the stiffness and the mass. Flores et al.'s
// 8 (1 - r) / (5 r), the first try, approximates it and rebounds short,
// 0.26 for 0.3 and 0.79 for 0.8 -- what a ball dropped on the plate did;
// Gonthier et al.'s (1 - r^2) / r is closer below 0.5 and farther above.
// By bisection: the difference grows with a from none to infinity at 1 / r.
double huntCrossley(double restitution)
{
    const double r = std::clamp(restitution, 0.01, 1.0);
    if (!(r < 1.0)) {
        return 0.0;
    }
    double low = 0.0;
    double high = 1.0 / r;
    for (int i = 0; i < 200 && high - low > 1e-15 * high; ++i) {
        const double a = 0.5 * (low + high);
        if (a - std::log1p(a) > -a * r - std::log1p(-a * r)) {
            low = a;
        }
        else {
            high = a;
        }
    }
    return 0.5 * (low + high);
}

// The places a contact set's "PartContactPlaces" reports on a part, the
// most pressed first, the rest past them as one
constexpr std::size_t contactPlaces = 4;

// Whether a contact set is a profile contact's, its shapes profiles
bool isProfileSet(const Dynamics& dynamics, const ContactPair& pair)
{
    return !pair.groupA.empty() && pair.groupA.front() < dynamics.contactShapes.size()
        && dynamics.contactShapes[pair.groupA.front()].kind == ContactShape::Kind::Profile;
}

struct ShapeOn;
class FlexibleContact;

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
        // A profile's point: the pieces it is on instead, one or a corner's
        // two, each side's (ProfileContact)
        std::array<const void*, 2> piecesA {};
        std::array<const void*, 2> piecesB {};
        ProfileFound found;
        // The model's frames of the shapes it is on, A's and B's: the parts
        // a contact event names, where a body stands for a rigid group
        std::array<Id, 2> frames {NoId, NoId};
        // A sphere or a cylinder it is on, A's and B's, whose point moves
        // over it as the body turns (rollAlong()); and the body whose face
        // the normal is, 1 A, 2 B, 0 neither, the normal in its frame
        const ShapeOn* roundA = nullptr;
        const ShapeOn* roundB = nullptr;
        int face = 0;
        ch::ChVector3d normalOn;
        // A ball's on the rim of a bore too narrow for it (seatAt()): the
        // bore's round, and the point's angle round the rim from the rim's
        // point nearest the ball's centre
        const ShapeOn* seat = nullptr;
        double seatTurn = 0.0;
    };

    ContactLoad(std::shared_ptr<ch::ChBody> a,
                std::shared_ptr<ch::ChBody> b,
                const ContactLaw& law,
                std::size_t set)
        : ch::ChLoadBodyBody(a, b, ch::ChFramed(b->GetPos(), b->GetRot()))
        , law(law)
        , set(set)
        , hysteresis(huntCrossley(law.restitution))
    {}

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
        normalOnB = ch::VNULL;
        deepest = 0.0;
        count = 0;
        pressed.clear();
        forcesOnB.clear();
        // A cam's point, or a profile contact's, found again at this state
        // (follow): the points are collision detection's, at the substep's
        // start, fixed on each body -- on a cam the follower slides along the
        // curve, and a material point of it a substep later is off the curve
        // under the follower by (slip h)^2 / 2 rho: a roller sinking 0.01 mm
        // ended a substep of 0.9 ms, taken at rest (advance()), 4% deeper,
        // and its force rang, up to 6% off, each time it rested
        if (follow && !points.empty()) {
            moving = points;
            follow(a, b, moving);
        }
        const std::vector<Point>& at = follow && !points.empty() ? moving : points;
        // Each patch's springs and their squares: a point's share is the
        // patch's spring, sum(s^2) / sum(s), over the sum. The deepest
        // point's spring, the first try, has a kink where the deepest
        // changes, and the Newton iterations of a cube landing flat on a
        // mesh ran off on it, the cube thrown off at 3 km/s.
        depths.resize(at.size());
        squares.assign(patches, 0.0);
        sum.assign(patches, 0.0);
        for (std::size_t i = 0; i < at.size(); ++i) {
            const Point& point = at[i];
            const ch::ChVector3d pA = a.TransformPointLocalToParent(point.onA);
            const ch::ChVector3d pB =
                meshed(i) ? onMesh[i].at : b.TransformPointLocalToParent(point.onB);
            depths[i] = -ch::Vdot(point.normal, pB - pA);
            deepest = std::max(deepest, depths[i]);
            if (depths[i] > 0.0) {
                const double spring = std::pow(depths[i], exponents[point.patch]);
                squares[point.patch] += spring * spring;
                sum[point.patch] += spring;
            }
        }
        for (std::size_t i = 0; i < at.size(); ++i) {
            const Point& point = at[i];
            const double depth = depths[i];
            if (!(depth > 0.0)) {
                continue;
            }
            const bool mesh = meshed(i);
            const ch::ChVector3d pA = a.TransformPointLocalToParent(point.onA);
            const ch::ChVector3d pB = mesh ? onMesh[i].at : b.TransformPointLocalToParent(point.onB);
            const ch::ChVector3d relative =
                (mesh ? onMesh[i].speed : b.PointSpeedLocalToParent(point.onB))
                - a.PointSpeedLocalToParent(point.onA);
            const double rate = -ch::Vdot(point.normal, relative);
            const double total = sum[point.patch];
            const double share = total > 0.0 ? squares[point.patch] / total / total : 1.0;
            const double spring =
                stiffness[point.patch] * std::pow(depth, exponents[point.patch]);
            const bool restitution = law.damping == ContactLaw::Damping::Restitution;
            const double factor = restitution
                ? hunt(std::pow(squares[point.patch] / total, 1.0 / exponents[point.patch]),
                       point.patch)
                : 0.0;
            const double viscous =
                restitution ? 0.0 : step(depth, 0.0, 0.0, law.penetration, law.maxDamping);
            double normal =
                share * (restitution ? spring * (1.0 + factor * rate) : spring + viscous * rate);
            if (!(normal > 0.0)) {
                continue;
            }
            ch::ChVector3d f = point.normal * normal;
            normalOnB += f;
            const ch::ChVector3d sliding =
                relative - point.normal * ch::Vdot(point.normal, relative);
            const double speed = sliding.Length();
            Eigen::Matrix3d drag = Eigen::Matrix3d::Zero();
            if (speed > 0.0 && law.muStatic > 0.0) {
                const double mu = speed < law.vStiction
                    ? step(speed, 0.0, 0.0, law.vStiction, law.muStatic)
                    : step(speed, law.vStiction, law.muStatic, law.vFriction, law.muDynamic);
                f -= sliding * (mu * normal / speed);
                if (mesh) {
                    // The friction's rate with the sliding velocity: mu N / v
                    // across it, and along it mu' N where mu rises with the
                    // speed -- not where it falls from the static to the
                    // dynamic, a damper that drives
                    const Eigen::Vector3d n = point.normal.eigen();
                    const Eigen::Vector3d along = sliding.eigen() / speed;
                    const double u = speed / law.vStiction;
                    const double rising =
                        u < 1.0 ? law.muStatic * 6.0 * u * (1.0 - u) / law.vStiction : 0.0;
                    drag = normal
                        * (mu / speed
                               * (Eigen::Matrix3d::Identity() - n * n.transpose()
                                  - along * along.transpose())
                           + rising * along * along.transpose());
                }
            }
            // f on B at pB, -f on A at pA; the torques in each body's axes. A
            // flexible part's point's on its reduced coordinates instead
            // (FlexibleContact), with its rates along the normal: the
            // spring's with the depth, the share and the damping's factor
            // held, and the damping's -- and its friction's with them and
            // with the sliding
            if (mesh) {
                const double rates = restitution ? 1.0 + factor * rate : 1.0;
                forcesOnB.push_back(
                    {i,
                     f,
                     point.normal,
                     point.onA,
                     std::max(share * exponents[point.patch] * spring / depth * rates, 0.0),
                     std::max(share * (restitution ? spring * factor : viscous), 0.0),
                     f / normal,
                     drag});
            }
            else {
                load_Q.segment(6, 3) += f.eigen();
                load_Q.segment(9, 3) +=
                    b.GetRot().RotateBack(ch::Vcross(pB - b.GetPos(), f)).eigen();
            }
            load_Q.segment(0, 3) -= f.eigen();
            load_Q.segment(3, 3) -= a.GetRot().RotateBack(ch::Vcross(pA - a.GetPos(), f)).eigen();
            onB += f;
            ++count;
            if (keep) {
                pressed.push_back({f, pA, pB, point.frames, point.patch});
            }
        }
    }
    // Chrono's numerical jacobian, by differences of 1e-8 m: a metal's line
    // contact sinks 3e-10 m, and a difference that lifts it off or presses
    // it thirty times as deep gave Newton a jacobian it did not converge on
    // past a third of the contact's period. The differences in position here
    // a hundredth of the shallowest point pressing, at most Chrono's; and
    // the load left at the state, not at its last difference.
    void ComputeJacobian(ch::ChState* state_x, ch::ChStateDelta* state_w) override
    {
        const int rows = LoadGetNumCoordsVelLevel();
        ComputeQ(state_x, state_w);
        const ch::ChVectorDynamic<> q0 = load_Q;
        double delta = 1e-8;
        for (const double depth : depths) {
            if (depth > 0.0) {
                delta = std::min(delta, 0.01 * depth);
            }
        }
        delta = std::max(delta, 1e-14);
        ch::ChState moved(LoadGetNumCoordsPosLevel(), nullptr);
        ch::ChStateDelta step(rows, nullptr);
        step.setZero(rows, nullptr);
        for (int i = 0; i < rows; ++i) {
            step(i) = delta;
            LoadStateIncrement(*state_x, step, moved);
            step(i) = 0.0;
            ComputeQ(&moved, state_w);
            m_jacobians->K.block(0, i, rows, 1) = (load_Q - q0) * (-1.0 / delta);
        }
        for (int i = 0; i < rows; ++i) {
            (*state_w)(i) += 1e-8;
            ComputeQ(state_x, state_w);
            (*state_w)(i) -= 1e-8;
            m_jacobians->R.block(0, i, rows, 1) = (load_Q - q0) * (-1.0 / 1e-8);
        }
        ComputeQ(state_x, state_w);
    }

    ContactLaw law;
    std::size_t set = 0;  // the ContactPair's index
    // The mass each side moves where it is not its body's, A's and B's (0:
    // its body's): a flexible part's contact interface carries the part's
    // (D4b), its own a millionth of it
    std::array<double, 2> carried {0.0, 0.0};
    // The lighter of the two sides' masses that move, as a law and a step
    // size take it; infinity when neither moves
    double lighterMass() const
    {
        double mass = std::numeric_limits<double>::infinity();
        // Chrono's GetMass() is not const
        const std::array<ch::ChBody*, 2> sides {GetBodyA().get(), GetBodyB().get()};
        for (int k = 0; k < 2; ++k) {
            const double m = carried[k] > 0.0 ? carried[k] : sides[k]->GetMass();
            if (!sides[k]->IsFixed() && m > 0.0) {
                mass = std::min(mass, m);
            }
        }
        return mass;
    }
    std::vector<Point> points;
    // A cam's or a profile contact's (ContactHandover's CamFollow and
    // ProfileFollow), or one on a sphere or a cylinder's side (rollAlong()):
    // its points put where they are found with the bodies at the frames given
    std::function<void(const ch::ChFrame<>& a, const ch::ChFrame<>& b, std::vector<Point>& points)>
        follow;
    std::vector<Point> moving;  // ComputeQ()'s, followed
    // Its points with the bodies where they are now: a cam's, a profile
    // contact's or a round shape's found again there (follow), the others as
    // collision detection left them
    std::vector<Point> pointsNow() const
    {
        std::vector<Point> out = points;
        if (follow && !out.empty()) {
            follow(*GetBodyA(), *GetBodyB(), out);
        }
        return out;
    }
    std::size_t patches = 0;
    // Each patch's k and e
    std::vector<double> stiffness;
    std::vector<double> exponents;
    // The approach speed the contact started at, m/s: while it does not
    // press, the fastest its points come in at (patchStiffness()), kept
    // from then on
    double impact = 0.0;
    // Whether it rests (contactStep(), advance()): the range its deepest
    // point's depth has kept, for how long, and the shortest period of its
    // points on their springs
    double low = 0.0;
    double high = 0.0;
    double calm = 0.0;
    double period = 0.0;
    bool settled = false;
    // Whether it pressed at the end of the last substep kept, for the
    // contact events (advance())
    bool touching = false;

    // The law's restitution as Hunt and Crossley's a (huntCrossley()), the
    // damping's factor a speed v0 makes
    double hysteresis = 0.0;

    /** By restitution: Hunt and Crossley's damping, the force's factor a
     * speed in s/m, a / v0 (huntCrossley()) -- the energy an impact at v0
     * loses for r, exactly for any exponent, a line contact's as a ball's.
     * It scales with the stiffness, where a damping given by value drowned
     * a soft part and left a line contact's sink of 1e-7 mm all but
     * undamped. v0 at least the speed of an impact that goes as deep as
     * the patch presses now, sqrt(2 k d^(e+1) / ((e+1) m)), m the lighter
     * body moving: an impact goes no deeper than its own, and is damped as
     * it was; a contact pressed deeper, by its load or by a cam that
     * started on it, came in at none -- at 1 mm/s, all that was left, a
     * follower resting on its cam had a damping ratio in the hundreds and
     * took substeps of 0.2 us. At rest under its weight that ratio is
     * about a / 2.
     */
    double hunt(double depth, std::size_t patch) const
    {
        double v0 = impact;
        const double mass = lighterMass();
        if (depth > 0.0 && std::isfinite(mass)) {
            const double e = exponents[patch];
            v0 = std::max(v0,
                          std::sqrt(2.0 * stiffness[patch] * std::pow(depth, e + 1.0)
                                    / ((e + 1.0) * mass)));
        }
        return hysteresis / std::max(v0, 1e-3);
    }
    // At the last state ComputeQ was given: the force on B, absolute, its
    // part along the points' normals (the rest is friction), the deepest
    // point's depth, m, and the points pressing
    ch::ChVector3d onB;
    ch::ChVector3d normalOnB;
    double deepest = 0.0;
    int count = 0;
    // And, where 'keep' (the run's values, not each of the jacobian's
    // differences), each point pressing: its force on B, absolute, where it
    // is on A and on B, the model's frames of the shapes it is on, and the
    // patch it is in
    struct Pressed
    {
        ch::ChVector3d force;
        ch::ChVector3d onA;
        ch::ChVector3d onB;
        std::array<Id, 2> frames;
        std::size_t patch = 0;
    };
    bool keep = false;
    std::vector<Pressed> pressed;

    // A flexible part's side B, a contact anywhere on it (D4b,
    // FlexibleContact): each point's place on its mesh's surface -- a
    // triangle's corners and the point's weights on them, and the point's
    // offset from that place in the carrier's (B's) axes -- and where the
    // part's modes have it now, absolute, its velocity and its map onto the
    // part's reduced coordinates. The forces of those points on B go to those
    // coordinates (forcesOnB), not to B
    FlexibleContact* flexible = nullptr;
    // Its bodies the other way round from its set's groups: A from the set's
    // second, B from its first -- a flexible part touched anywhere is B. The
    // set's forces are on its first group's side
    bool swapped = false;
    struct OnMesh
    {
        bool placed = false;
        std::array<Id, 3> nodes {};
        std::array<double, 3> weights {};
        ch::ChVector3d offset;
        ch::ChVector3d at;
        ch::ChVector3d speed;
        ch::ChMatrixDynamic<> T;
    };
    std::vector<OnMesh> onMesh;
    bool meshed(std::size_t i) const
    {
        return flexible && i < onMesh.size() && onMesh[i].placed;
    }
    // A point's speed on B, absolute: B's there, or the part's surface's
    // where its modes have it. What sizes a substep and judges a contact
    // found late reads this one: the carrier's own is not how fast the
    // surface comes in, a strip of 2 mm 0.2 mm off its carrier's shape and
    // still bending as it came back onto a bar
    ch::ChVector3d speedOnB(std::size_t i, const Point& point) const
    {
        return meshed(i) ? onMesh[i].speed : GetBodyB()->PointSpeedLocalToParent(point.onB);
    }
    // At the last state ComputeQ was given, each such point pressing: its
    // force on B, absolute, its normal, where it is on A (A's frame), and the
    // force's rates along the normal with the depth and with its rate (N/m,
    // N s/m); the force for one newton along the normal, the normal less the
    // friction's mu along the sliding, and the friction's rate with the
    // sliding velocity (N s/m)
    struct ForceOnB
    {
        std::size_t point = 0;
        ch::ChVector3d force;
        ch::ChVector3d normal;
        ch::ChVector3d onA;
        double k = 0.0;
        double c = 0.0;
        ch::ChVector3d line;
        Eigen::Matrix3d drag = Eigen::Matrix3d::Zero();
    };
    std::vector<ForceOnB> forcesOnB;

private:
    std::vector<double> depths, squares, sum;
};

#ifdef ASSEMBLY_WITH_CHRONO_MODAL
// A contact anywhere on a flexible part (D4b): the part's contact shapes ride
// one of its interfaces' boundary bodies, the carrier, on which collision
// detection finds their points, and each point found is put on the mesh's
// surface where it is with the part at rest (place()). As the run goes each
// is where the part's modes have it (refresh(), at each update: the modal
// assembly is in the system before this item, and this before the loads),
// its force on the part taken to the part's reduced coordinates as T^T f --
// T the point's map onto them, our Chrono's GetInternalNodesMap() -- and its
// stiffness and damping too, T^T k n n' T, coupled with the other side's
// body. Without them a force on the mesh is in the residual only (a node's
// load is), and Newton's iterations on a metal's contact, far stiffer than
// the part's modes, run away
class FlexibleContact: public ch::ChPhysicsItem
{
public:
    FlexibleContact* Clone() const override
    {
        return new FlexibleContact(*this);
    }

    ch::modal::ChModalAssembly* assembly = nullptr;
    ch::ChBody* carrier = nullptr;
    // The carrier's frame in the part's, at rest
    ch::ChFramed carrierAt;
    std::shared_ptr<const ChronoFlexible::Surface> surface;
    // The mesh's nodes, by the record's numbers
    std::vector<const ch::fea::ChNodeFEAxyz*> nodes;
    // The loads it is B of
    std::vector<ContactLoad*> loads;
    // Its surface as the collision shape on the carrier (makeMesh()): the
    // triangles of ChronoFlexible::Surface, their vertices in the carrier's
    // frame where the modes have the nodes -- moved there as each collision
    // detection starts (moveMesh()). A shape riding the carrier rigid is off
    // the surface by what the part has bent since: a strip 200 x 20 x 2
    // hinged at an end, dropped 10 mm onto a bar 120 mm out, left it bent
    // and came back with its surface 0.18 mm in when its box came within
    // the run's reach of 0.1 mm -- 62 kN on 21 g. None when it could not be
    // made: the part's own shapes ride the carrier then
    std::shared_ptr<ch::ChTriangleMeshConnected> mesh;
    // Each vertex's node, by the record's numbers
    std::vector<Id> meshNodes;

    // Once the assembly is reduced, and before the carrier has its shapes
    bool makeMesh()
    {
        mesh.reset();
        meshNodes.clear();
        if (!surface || !assembly) {
            return false;
        }
        const auto& points = surface->nodes();
        auto made = std::make_shared<ch::ChTriangleMeshConnected>();
        std::vector<int> vertexOf(points.size(), -1);
        std::vector<const ch::fea::ChNodeFEAxyz*> moving;
        for (const auto& triangle : surface->triangles()) {
            int corners[3] = {0, 0, 0};
            for (int k = 0; k < 3; ++k) {
                const Id n = triangle[k];
                if (n >= points.size() || n >= nodes.size() || !nodes[n]) {
                    meshNodes.clear();
                    return false;
                }
                if (vertexOf[n] < 0) {
                    vertexOf[n] = static_cast<int>(meshNodes.size());
                    made->GetCoordsVertices().push_back(
                        carrierAt.TransformPointParentToLocal(points[n]));
                    meshNodes.push_back(n);
                    moving.push_back(nodes[n]);
                }
                corners[k] = vertexOf[n];
            }
            made->GetIndicesVertexes().emplace_back(corners[0], corners[1], corners[2]);
        }
        if (meshNodes.empty() || !assembly->GetInternalNodesShapes(moving, shapes)) {
            meshNodes.clear();
            return false;
        }
        mesh = made;
        return true;
    }

    // The mesh's vertices where the modes have their nodes now
    void moveMesh()
    {
        if (!mesh || !assembly->GetInternalNodesPos(shapes, moved)
            || moved.size() != meshNodes.size()) {
            return;
        }
        auto& vertices = mesh->GetCoordsVertices();
        for (std::size_t i = 0; i < moved.size(); ++i) {
            vertices[i] = carrier->TransformPointParentToLocal(moved[i]);
        }
    }

    // Each point of a load just found to its place on the surface, then
    // where it is now: on the mesh's triangle it was found on, as the
    // triangle is; one of no triangle of the mesh's (the part's own shapes
    // on the carrier), on the surface nearest as the carrier has it at rest
    void place(ContactLoad& load)
    {
        load.onMesh.assign(load.points.size(), {});
        const ch::ChVector3d* first = mesh ? mesh->GetCoordsVertices().data() : nullptr;
        const std::size_t count = mesh ? mesh->GetCoordsVertices().size() : 0;
        for (std::size_t i = 0; i < load.points.size(); ++i) {
            auto& on = load.onMesh[i];
            const ch::ChVector3d& onB = load.points[i].onB;
            const ch::ChCollisionShape* shape = load.points[i].shapeB;
            if (first && shape && shape->GetType() == ch::ChCollisionShape::Type::MESHTRIANGLE) {
                const auto* triangle = static_cast<const ch::ChCollisionShapeMeshTriangle*>(shape);
                const ch::ChVector3d* corners[3] = {triangle->V1, triangle->V2, triangle->V3};
                if (std::all_of(std::begin(corners), std::end(corners), [&](const auto* corner) {
                        return corner >= first && corner < first + count;
                    })) {
                    on.weights = ChronoFlexible::Surface::weights(onB,
                                                                  *corners[0],
                                                                  *corners[1],
                                                                  *corners[2]);
                    ch::ChVector3d at = ch::VNULL;
                    for (int k = 0; k < 3; ++k) {
                        on.nodes[k] = meshNodes[static_cast<std::size_t>(corners[k] - first)];
                        at += *corners[k] * on.weights[k];
                    }
                    on.offset = onB - at;
                    on.placed = true;
                    continue;
                }
            }
            const ch::ChVector3d inPart = carrierAt.TransformPointLocalToParent(onB);
            ChronoFlexible::Surface::Place found;
            if (!surface || !surface->nearest(inPart, found)) {
                continue;
            }
            on.placed = true;
            on.nodes = found.nodes;
            on.weights = found.weights;
            on.offset = onB - carrierAt.TransformPointParentToLocal(found.at);
        }
        refresh(load);
    }

    // A load's points where the modes have them, and their maps; each one's
    // onB on the carrier there, for what reads a point by its bodies
    void refresh(ContactLoad& load)
    {
        std::vector<const ch::fea::ChNodeFEAxyz*> asked;
        for (const auto& on : load.onMesh) {
            if (on.placed) {
                for (Id n : on.nodes) {
                    asked.push_back(n < nodes.size() ? nodes[n] : nullptr);
                }
            }
        }
        if (asked.empty()) {
            return;
        }
        std::vector<ch::ChVector3d> pos;
        std::vector<ch::ChVector3d> vel;
        std::vector<ch::ChMatrixDynamic<>> maps;
        if (!assembly->GetInternalNodesMap(asked, pos, vel, maps)) {
            for (auto& on : load.onMesh) {
                on.placed = false;
            }
            return;
        }
        std::size_t k = 0;
        for (std::size_t i = 0; i < load.onMesh.size(); ++i) {
            auto& on = load.onMesh[i];
            if (!on.placed) {
                continue;
            }
            on.at = carrier->GetRot().Rotate(on.offset);
            on.speed = ch::VNULL;
            on.T.setZero(3, maps[k].cols());
            for (int j = 0; j < 3; ++j, ++k) {
                on.at += pos[k] * on.weights[j];
                on.speed += vel[k] * on.weights[j];
                on.T += maps[k] * on.weights[j];
            }
            if (i < load.points.size()) {
                load.points[i].onB = carrier->TransformPointParentToLocal(on.at);
            }
        }
    }

    // Its block of the system matrix: the reduced variables, then each other
    // side's body's that moves. Once the loads are its
    void setVariables()
    {
        std::vector<ch::ChVariables*> variables = assembly->GetReducedVariables();
        reduced = 0;
        for (const auto* v : variables) {
            reduced += static_cast<int>(v->GetDOF());
        }
        others.clear();
        for (ContactLoad* load : loads) {
            ch::ChBody* a = load->GetBodyA().get();
            if (a->IsFixed() || std::find(others.begin(), others.end(), a) != others.end()) {
                continue;
            }
            others.push_back(a);
            variables.push_back(&a->Variables());
        }
        block.SetVariables(variables);
    }

    void Update(double time, ch::UpdateFlags flags) override
    {
        ch::ChPhysicsItem::Update(time, flags);
        for (ContactLoad* load : loads) {
            refresh(*load);
        }
    }

    // The points' forces on the reduced coordinates, T^T f
    void IntLoadResidual_F(const unsigned int, ch::ChVectorDynamic<>& R, const double c) override
    {
        const unsigned int at = assembly->GetOffset_w();
        for (ContactLoad* load : loads) {
            for (const auto& f : load->forcesOnB) {
                if (f.point >= load->onMesh.size()) {
                    continue;
                }
                const auto& on = load->onMesh[f.point];
                if (on.T.cols() != reduced || at + reduced > R.size()) {
                    continue;
                }
                R.segment(at, reduced) += c * (on.T.transpose() * f.force.eigen());
            }
        }
    }

    void InjectKRMMatrices(ch::ChSystemDescriptor& descriptor) override
    {
        if (block.GetNumVariables()) {
            descriptor.InsertKRMBlock(&block);
        }
    }

    // Each point's k l n' and c l n' -- l the normal less the friction's mu
    // along the sliding, the force that comes with the normal's -- and its
    // friction's rate with the sliding, between where it is on the part, T,
    // and on A, J = [I, -R_A [r]x] (A's velocity, its angular in its axes):
    // A's own block is the load's (ContactLoad::ComputeJacobian()). Without
    // the friction's the Newton iterations stopped unconverged on any substep
    // past m / (mu' N): a strip of 21 g pressed 18 N onto a bar, mu 0.3
    // reached at 0.1 mm/s, took none longer than 0.3 us, each fourth try
    // taken back, and the tenth of a second it was bent in 1640 s
    void LoadKRMMatrices(double Kfactor, double Rfactor, double) override
    {
        auto& H = block.GetMatrix();
        H.setZero();
        for (ContactLoad* load : loads) {
            ch::ChBody* a = load->GetBodyA().get();
            const auto it = std::find(others.begin(), others.end(), a);
            const int other = it == others.end() ? -1 : static_cast<int>(it - others.begin());
            for (const auto& f : load->forcesOnB) {
                if (f.point >= load->onMesh.size()) {
                    continue;
                }
                const auto& on = load->onMesh[f.point];
                if (on.T.cols() != reduced) {
                    continue;
                }
                const Eigen::Matrix3d h = (Kfactor * f.k + Rfactor * f.c)
                    * ch::TensorProduct(f.line, f.normal)
                    + Rfactor * f.drag;
                const Eigen::MatrixXd hT = h * on.T;
                H.topLeftCorner(reduced, reduced) += on.T.transpose() * hT;
                if (other < 0) {
                    continue;
                }
                Eigen::Matrix<double, 3, 6> J;
                J.leftCols<3>().setIdentity();
                J.rightCols<3>() = -(a->GetRotMat() * ch::ChStarMatrix33<>(f.onA));
                const int col = reduced + 6 * other;
                H.block(0, col, reduced, 6) -= on.T.transpose() * (h * J);
                H.block(col, 0, 6, reduced) -= J.transpose() * hT;
            }
        }
    }

private:
    ch::ChKRMBlock block;
    int reduced = 0;
    std::vector<ch::ChBody*> others;
    // The mesh's nodes' shapes on the reduced coordinates, and where they
    // are, the last asked
    ch::modal::ChModalAssembly::NodesShapes shapes;
    std::vector<ch::ChVector3d> moved;
};
#endif

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
    // A profile's point by its pieces: a flat lying on a flat touches at
    // both its corners, each on the flat's piece, in one patch
    auto keysOfPoint = [](const std::array<const void*, 2>& pieces,
                          const ch::ChCollisionShape* shape,
                          const void* keys[3]) {
        if (!pieces[0]) {
            return keysOf(shape, keys);
        }
        keys[0] = pieces[0];
        keys[1] = pieces[1];
        return pieces[1] ? 2 : 1;
    };
    for (std::size_t i = 0; i < points.size(); ++i) {
        const void* keysA[3];
        const void* keysB[3];
        const int na = keysOfPoint(points[i].piecesA, points[i].shapeA, keysA);
        const int nb = keysOfPoint(points[i].piecesB, points[i].shapeB, keysB);
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

// Profile contact (sec 7.4's 2D path, ProfileContact): two planar profiles
// (ProfileGeometry.h) in one plane's coordinates -- A's frame's xy, B's
// brought into it.

// A piece of a profile in a plane's coordinates (profileIn()): through
// 'into', turned round when 'mirrored', with its radius and start; not its
// neighbours or its bounds
Profile2::Piece pieceIn(const ProfilePiece& piece, const ch::ChFramed& into, bool mirrored)
{
    auto at = [&](const Base::Vector3d& p) {
        const ch::ChVector3d q = into.TransformPointLocalToParent(toCh(p));
        return V2 {q.x(), q.y()};
    };
    Profile2::Piece p;
    p.from = at(mirrored ? piece.to : piece.from);
    p.to = at(mirrored ? piece.from : piece.to);
    p.centre = at(piece.centre);
    // A mirror reverses the turn, and turning the piece round reverses it
    // back
    p.sweep = piece.sweep;
    p.key = &piece;
    if (p.sweep != 0.0) {
        const V2 r = p.from - p.centre;
        p.radius = norm(r);
        p.start = std::atan2(r.y, r.x);
    }
    return p;
}

// A profile in a plane's coordinates: the shape's pieces through 'into'
// (its frame into the plane's), each loop turned round when 'mirrored' (B
// facing A), so that the face stays to their left
Profile2 profileIn(const ContactShape& shape, const ch::ChFramed& into, bool mirrored)
{
    Profile2 out;
    const std::size_t n = shape.pieces.size();
    out.pieces.reserve(n);
    for (std::size_t l = 0; l < shape.loops.size(); ++l) {
        const std::size_t first = static_cast<std::size_t>(shape.loops[l]);
        const std::size_t last =
            l + 1 < shape.loops.size() ? static_cast<std::size_t>(shape.loops[l + 1]) : n;
        const std::size_t base = out.pieces.size();
        for (std::size_t k = first; k < last; ++k) {
            // Turned round: the loop backwards, each piece from its end
            out.pieces.push_back(
                pieceIn(shape.pieces[mirrored ? last - 1 - (k - first) : k], into, mirrored));
        }
        const std::size_t count = out.pieces.size() - base;
        for (std::size_t k = 0; k < count; ++k) {
            Profile2::Piece& p = out.pieces[base + k];
            p.prev = base + (k + count - 1) % count;
            p.next = base + (k + 1) % count;
        }
    }
    finishProfile(out);
    return out;
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
    // A triangle of a mesh whose vertices move (a flexible part's surface,
    // ContactCollisionSystem::addMovingMesh()): its features are made again
    // from it when the mesh has moved since, 'moves' the mesh's count of
    // that and 'at' the count they were made at; 'layout' counts the times
    // its edges came out other ones, a fold between it and a neighbour
    // turned over
    const cbtCEtriangleShape* moving = nullptr;
    const unsigned* moves = nullptr;
    unsigned at = 0;
    unsigned layout = 0;
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
                        Polyhedra* polyhedra)
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
        if (a != polyhedra->end()) {
            freshen(a->second);
        }
        if (b != polyhedra->end()) {
            freshen(b->second);
        }
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
        const cbtScalar threshold = manifold->getContactBreakingThreshold()
            + resultOut->m_closestPointDistanceThreshold;
        bool kept = false;
        // An axis names its shapes' features by their numbers: one of a
        // moving mesh's triangle whose edges are others now is searched for
        // again. And one of a triangle that moved is kept only while it
        // holds the pair apart, the pair's frames having stayed or not
        if (a->second.layout != layoutA || b->second.layout != layoutB) {
            axis = SatAxis();
            layoutA = a->second.layout;
            layoutB = b->second.layout;
        }
        const bool reshaped = a->second.at != seenA || b->second.at != seenB;
        seenA = a->second.at;
        seenB = b->second.at;
        if (axis.kind != SatAxis::None) {
            satRemeasure(a->second, b->second, bInA, axis);
            const cbtScalar moved = (bInA.getOrigin() - searchedAt.getOrigin()).length();
            const cbtQuaternion turn = searchedAt.getRotation().inverse() * bInA.getRotation();
            const cbtScalar angle = 2 * std::acos(std::min<cbtScalar>(1, std::abs(turn.getW())));
            kept = axis.separation > threshold
                || (!reshaped && moved < 1e-5 && angle < 1e-3 && axis.kind != SatAxis::Apart
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
        Polyhedra* polyhedra = nullptr;
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
    // A moving mesh's triangle's features, as the mesh is now
    // (SatPolyhedron::moving): made when a pair asks for them, a part's
    // thousands of triangles a few of them near anything
    static void freshen(SatPolyhedron& sat)
    {
        if (!sat.moving || !sat.moves || sat.at == *sat.moves) {
            return;
        }
        SatPolyhedron now = satOfTriangle(*sat.moving, sat.grown);
        now.moving = sat.moving;
        now.moves = sat.moves;
        now.at = *sat.moves;
        bool same = now.edges.size() == sat.edges.size();
        for (std::size_t i = 0; same && i < now.edges.size(); ++i) {
            same = now.edges[i].tail == sat.edges[i].tail;
        }
        now.layout = same ? sat.layout : sat.layout + 1;
        sat = std::move(now);
    }

    std::vector<ClipPoint> clipped;
    Polyhedra* polyhedra = nullptr;
    SatAxis axis;
    cbtTransform searchedAt;
    unsigned seenA = 0;
    unsigned seenB = 0;
    unsigned layoutA = 0;
    unsigned layoutB = 0;
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
        gCompoundChildShapePairCallback = &collides;
        gCompoundCompoundChildShapePairCallback = &collides;
    }
    void addPolyhedralFeatures(
        ch::ChCollisionModel& model,
        std::map<const ch::ChCollisionShape*, const cbtConvexPolyhedron*>& features);
    // A mesh of a model (after addPolyhedralFeatures()) whose vertices
    // 'move' moves, in its body's frame, as each run starts: a flexible
    // part's surface, where its modes have it (FlexibleContact::moveMesh()).
    // Bullet's triangles read the vertices as they are; what it keeps of
    // them is brought along here -- each triangle's box in its body's tree,
    // 'fat' wider when it has left the one it had, the body's own box, and
    // the triangles' features (SatPolyhedron::moving)
    void addMovingMesh(ch::ChCollisionModel& model,
                       const ch::ChCollisionShape* shape,
                       double fat,
                       std::function<void()> move);
    // The Bullet shapes each shape of a model made (addPolyhedralFeatures()):
    // a mesh's its triangles, any other's one
    std::map<const ch::ChCollisionShape*, std::vector<const cbtCollisionShape*>> made;
    // Pairs of Bullet shapes it leaves out: one in 'inner' against a
    // triangle in 'hollow' under the same number, a round inside a hollow
    // one (ContactHandover::RoundPair), whose points are the rounds' own. A
    // chamfered roller's hull clipped against a bore's triangles was four
    // fifths of its run's time, for points then dropped.
    std::map<const cbtCollisionShape*, std::vector<std::size_t>> inner;
    std::map<const cbtCollisionShape*, std::vector<std::size_t>> hollow;
    // The numbers it leaves out at this state (nonzero), from 'refresh',
    // which it calls as each run starts: a pin across a bore (inLine())
    // has no points of the rounds', and Bullet's are its
    std::vector<char> live;
    std::function<void()> refresh;

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
            if (refresh) {
                refresh();
            }
            moveMeshes();
            const ContactCollisionSystem* outer = running;
            running = this;
            ch::ChCollisionSystemBullet::Run();
            running = outer;
        }
    }

private:
    // Bullet's test of a compound's child against another shape, before it
    // runs their algorithm: false leaves them out ('inner', 'hollow'), when
    // its run is this system's on this thread
    static thread_local const ContactCollisionSystem* running;
    static bool collides(const cbtCollisionShape* a, const cbtCollisionShape* b)
    {
        return !running || !(running->apart(a, b) || running->apart(b, a));
    }
    bool apart(const cbtCollisionShape* a, const cbtCollisionShape* b) const
    {
        if (inner.empty()) {
            return false;
        }
        auto i = inner.find(a);
        auto h = hollow.find(b);
        if (i == inner.end() || h == hollow.end()) {
            return false;
        }
        for (std::size_t k : i->second) {
            if (k < live.size() && live[k]
                && std::find(h->second.begin(), h->second.end(), k) != h->second.end()) {
                return true;
            }
        }
        return false;
    }

    // A model's Bullet shape: its object's, the one whose user pointer is
    // the model's Bullet half
    cbtCollisionShape* topOf(ch::ChCollisionModel& model) const
    {
        if (!model.HasImplementation()) {
            return nullptr;
        }
        const auto& objects = bt_collision_world->getCollisionObjectArray();
        for (int i = 0; i < objects.size(); ++i) {
            if (objects[i]->getUserPointer() == model.GetImplementation()) {
                return objects[i]->getCollisionShape();
            }
        }
        return nullptr;
    }

    struct MovingMesh
    {
        std::function<void()> move;
        cbtCompoundShape* compound = nullptr;
        std::vector<int> children;  // its triangles, of the compound's
        cbtScalar fat = 0;
    };
    std::vector<MovingMesh> moving;
    unsigned moves = 0;
    void moveMeshes()
    {
        if (moving.empty()) {
            return;
        }
        for (MovingMesh& mesh : moving) {
            mesh.move();
            if (!mesh.compound) {
                continue;
            }
            cbtDbvt* tree = mesh.compound->getDynamicAabbTree();
            cbtCompoundShapeChild* children = mesh.compound->getChildList();
            for (int i : mesh.children) {
                if (!tree || !children[i].m_node) {
                    continue;
                }
                cbtVector3 low;
                cbtVector3 high;
                children[i].m_childShape->getAabb(children[i].m_transform, low, high);
                cbtDbvtVolume volume = cbtDbvtVolume::FromMM(low, high);
                tree->update(children[i].m_node, volume, mesh.fat);
            }
            mesh.compound->recalculateLocalAabb();
        }
        ++moves;
    }

    PolyhedralAlgorithm::Polyhedra polyhedra;
    PolyhedralAlgorithm::CreateFunc polyhedral;
};

thread_local const ContactCollisionSystem* ContactCollisionSystem::running = nullptr;

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
    cbtCollisionShape* top = topOf(model);
    if (!top) {
        return;
    }
    const cbtScalar grow = static_cast<cbtScalar>(model.GetEnvelope());
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
                made[instances[i].shape.get()].push_back(triangle);
                polyhedra[triangle] = satOfTriangle(*triangle, grow);
            }
            continue;
        }
        if (next >= children.size()) {
            return;
        }
        cbtCollisionShape* shape = children[next++];
        made[instances[i].shape.get()].push_back(shape);
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

void ContactCollisionSystem::addMovingMesh(ch::ChCollisionModel& model,
                                           const ch::ChCollisionShape* shape,
                                           double fat,
                                           std::function<void()> move)
{
    const auto triangles = made.find(shape);
    cbtCollisionShape* top = topOf(model);
    if (triangles == made.end() || !top || !move) {
        return;
    }
    MovingMesh& mesh = moving.emplace_back();
    mesh.move = std::move(move);
    mesh.fat = static_cast<cbtScalar>(fat);
    const std::set<const cbtCollisionShape*> its(triangles->second.begin(),
                                                 triangles->second.end());
    for (const cbtCollisionShape* triangle : triangles->second) {
        SatPolyhedron& sat = polyhedra[triangle];
        sat.moving = static_cast<const cbtCEtriangleShape*>(triangle);
        sat.moves = &moves;
        sat.at = moves;
    }
    if (top->getShapeType() == COMPOUND_SHAPE_PROXYTYPE) {
        mesh.compound = static_cast<cbtCompoundShape*>(top);
        for (int i = 0; i < mesh.compound->getNumChildShapes(); ++i) {
            if (its.count(mesh.compound->getChildShape(i))) {
                mesh.children.push_back(i);
            }
        }
    }
}

// A contact shape where it is: on its body at its frame, a cylinder's
// radius and half length, a box's or a hull's polyhedral features
struct ShapeOn
{
    ch::ChBody* body = nullptr;
    ch::ChFramed frame;
    Id source = NoId;     // the model's frame of its ContactShape
    double radius = 0.0;  // a cylinder's, 0 for any other
    double half = 0.0;
    const cbtConvexPolyhedron* features = nullptr;
    double envelope = 0.0;
    // For Hertz's law (patchStiffness()): a sphere's radius, the material's
    // E / (1 - nu^2), the radius of the ball of the solid's volume, and the
    // surface's curvature tensor at each vertex of its mesh, a mesh's or a
    // hull's, 1/m -- a mesh's in its body's frame, a hull's at its points,
    // in its frame. Not of a hull's features: Bullet's merges faces all but
    // in one plane, and the vertices it kept about an ellipsoid's pole
    // curved it as 1/8 mm where it is 1/12.
    double ball = 0.0;
    double modulus = 0.0;
    double size = 0.0;
    std::vector<ch::ChMatrix33d> curvatures;
    std::vector<ch::ChVector3d> hull;
    // The solid at each of a hull's or a mesh's points (ContactShape::
    // surfaces), in the hull's frame or the mesh's body's: each face's
    // outward normal and curvature tensor there
    struct Surface
    {
        ch::ChVector3d normal;
        ch::ChMatrix33d tensor;
        int round = -1;  // its face's in rounds, or none
    };
    std::vector<std::vector<Surface>> surfaces;
    // Its faces that are a convex cylinder's or sphere's (ContactShape::
    // rounds), each a sphere or a cylinder on the body as a primitive is,
    // a cylinder's frame at the middle of its reach; and a round's angles,
    // from..to about its z from x, all round when to - from is 2 pi
    std::vector<ShapeOn> rounds;
    double angleFrom = 0.0;
    double angleTo = 2.0 * pi;
    // A round that is hollow: a bore, a socket, its surface facing its
    // axis or its centre
    bool concave = false;
    // A mesh's vertices, in its body's frame: its triangles point into them
    const ch::ChVector3d* vertices = nullptr;
    std::size_t count = 0;
};

// Where a sphere or a cylinder's side, on its body at 'body', is farthest
// out along 'outward', absolute: a cylinder's at the point's own place
// along its axis ('at', absolute), within its caps. False on a cylinder's
// cap, 'outward' within 60 deg of its axis as cylinderPoints() takes it: a
// face, its points fixed on it
bool roundSupport(const ShapeOn& on,
                  const ch::ChFrame<>& body,
                  const ch::ChVector3d& outward,
                  const ch::ChVector3d& at,
                  ch::ChVector3d& out)
{
    const ch::ChFramed frame = on.frame >> body;
    // A hollow one's surface facing 'outward' is across its axis or its
    // centre from it
    const double facing = on.concave ? -1.0 : 1.0;
    if (on.ball > 0.0) {
        out = frame.GetPos() + outward * (facing * on.ball);
        return true;
    }
    if (!(on.radius > 0.0)) {
        return false;
    }
    const ch::ChVector3d axis = frame.GetRotMat().GetAxisZ();
    const double along = ch::Vdot(axis, outward);
    const ch::ChVector3d side = (outward - axis * along) * facing;
    const double length = side.Length();
    if (std::abs(along) >= 0.5 || !(length > 1e-9)) {
        return false;
    }
    // A part of a round only: a D shaft's, beside its flat
    if (on.angleTo - on.angleFrom < 2.0 * pi - 1e-9) {
        const double angle = std::atan2(ch::Vdot(side, frame.GetRotMat().GetAxisY()),
                                        ch::Vdot(side, frame.GetRotMat().GetAxisX()));
        const double from = on.angleFrom + std::fmod(angle - on.angleFrom + 4.0 * pi, 2.0 * pi);
        if (from > on.angleTo) {
            return false;
        }
    }
    const double t = std::clamp(ch::Vdot(axis, at - frame.GetPos()), -on.half, on.half);
    out = frame.GetPos() + axis * t + side * (on.radius / length);
    return true;
}

// The round a point of a hull or a mesh is on ('at' in its body's frame,
// the surface facing 'outward', absolute): its face's, at the shape's point
// nearest -- a hull's, or the triangle's corner -- the face there whose
// normal is nearest, when it is a round's (ShapeOn::rounds) and within 0.3
// rad of the facet's: none on the solid's other faces
const ShapeOn* roundOn(const ShapeOn& on,
                       const ch::ChCollisionShape* shape,
                       const ch::ChVector3d& at,
                       const ch::ChVector3d& outward)
{
    if (on.rounds.empty()) {
        return nullptr;
    }
    std::size_t nearest = on.surfaces.size();
    ch::ChVector3d facing;
    if (on.vertices && shape->GetType() == ch::ChCollisionShape::Type::MESHTRIANGLE) {
        const auto* triangle = static_cast<const ch::ChCollisionShapeMeshTriangle*>(shape);
        double least = std::numeric_limits<double>::infinity();
        for (const ch::ChVector3d* corner : {triangle->V1, triangle->V2, triangle->V3}) {
            const double apart = (*corner - at).Length2();
            if (apart < least) {
                least = apart;
                nearest = static_cast<std::size_t>(corner - on.vertices);
            }
        }
        facing = on.body->TransformDirectionParentToLocal(outward);
    }
    else if (!on.hull.empty()) {
        const ch::ChVector3d point = on.frame.TransformPointParentToLocal(at);
        double least = std::numeric_limits<double>::infinity();
        for (std::size_t v = 0; v < on.hull.size(); ++v) {
            const double apart = (on.hull[v] - point).Length2();
            if (apart < least) {
                least = apart;
                nearest = v;
            }
        }
        facing = on.frame.TransformDirectionParentToLocal(
            on.body->TransformDirectionParentToLocal(outward));
    }
    if (nearest >= on.surfaces.size()) {
        return nullptr;
    }
    const ShapeOn::Surface* best = nullptr;
    for (const auto& face : on.surfaces[nearest]) {
        if (!best || ch::Vdot(face.normal, facing) > ch::Vdot(best->normal, facing)) {
            best = &face;
        }
    }
    if (!best || best->round < 0 || static_cast<std::size_t>(best->round) >= on.rounds.size()
        || ch::Vdot(best->normal, facing) < std::cos(0.3)) {
        return nullptr;
    }
    return &on.rounds[static_cast<std::size_t>(best->round)];
}

// Whether a box's or a hull's shape, or a mesh's triangle, on its body has
// a face whose outward normal is 'outward', absolute: the clipping's
// normal is its face's own, exactly (PolyhedralAlgorithm, cylinderPoints()).
// A triangle's either way.
bool hasFace(const ShapeOn& on, const ch::ChCollisionShape* shape, const ch::ChVector3d& outward)
{
    constexpr double parallel = 1.0 - 1e-8;
    if (on.vertices && shape->GetType() == ch::ChCollisionShape::Type::MESHTRIANGLE) {
        const auto* triangle = static_cast<const ch::ChCollisionShapeMeshTriangle*>(shape);
        const ch::ChVector3d n = on.body->TransformDirectionLocalToParent(
            ch::Vcross(*triangle->V2 - *triangle->V1, *triangle->V3 - *triangle->V1));
        const double length = n.Length();
        return length > 0.0 && std::abs(ch::Vdot(n, outward)) > parallel * length;
    }
    if (!on.features) {
        return false;
    }
    const ch::ChMatrix33d turn = (on.frame >> *on.body).GetRotMat();
    for (int f = 0; f < on.features->m_faces.size(); ++f) {
        const cbtScalar* plane = on.features->m_faces[f].m_plane;
        const ch::ChVector3d n = turn * ch::ChVector3d(plane[0], plane[1], plane[2]);
        if (ch::Vdot(n, outward) > parallel * n.Length()) {
            return true;
        }
    }
    return false;
}

bool inLine(const ch::ChVector3d& inner, const ch::ChVector3d& bore);
bool rimFarthest(const ch::ChVector3d& cap,
                 const ch::ChVector3d& axis,
                 double radius,
                 const ch::ChFramed& outer,
                 ch::ChVector3d& p,
                 ch::ChVector3d& out);

/** The way a round inside a hollow one touches it at 'centre' -- a point
 * of a cylinder's axis, a ball's centre -- from the inner surface to the
 * hollow one's, both at their bodies' frames (outer, inner): along the
 * hollow one's radius through it, a pin's end or its side on a bore's
 * wall; beyond a bore's end, to the end's rim that way, its edge on the
 * inner one's side, 'centre' moved along a cylinder's axis square with the
 * rim's point. The bore's radius there lifted a pin hanging out of a bore
 * off the mouth's edge by r t^2 at a tilt t, 4.4 um at 1.2 deg; the rim's
 * way is square to the pin. Zero where there is none: on the axis, at the
 * centre, or outside the hollow one's radius.
 */
ch::ChVector3d hollowWay(const ShapeOn& hollow,
                         const ch::ChFramed& outer,
                         const ShapeOn& inside,
                         const ch::ChFramed& inner,
                         ch::ChVector3d& centre)
{
    ch::ChVector3d out = centre - outer.GetPos();
    if (hollow.ball > 0.0) {
        const double length = out.Length();
        return length > 1e-12 && length < hollow.ball ? out / length : ch::VNULL;
    }
    const ch::ChVector3d axis = outer.GetRotMat().GetAxisZ();
    const double along = ch::Vdot(axis, out);
    out -= axis * along;
    const double apart = out.Length();
    if (!(apart > 1e-12) || apart >= hollow.radius) {
        return ch::VNULL;
    }
    out /= apart;
    if (std::abs(along) < hollow.half * (1.0 - 1e-9)) {
        return out;
    }
    const ch::ChVector3d edge =
        outer.GetPos() + axis * std::copysign(hollow.half, along) + out * hollow.radius;
    if (!(inside.ball > 0.0)) {
        const ch::ChVector3d ai = inner.GetRotMat().GetAxisZ();
        centre = inner.GetPos()
            + ai * std::clamp(ch::Vdot(ai, edge - inner.GetPos()), -inside.half, inside.half);
    }
    const ch::ChVector3d way = edge - centre;
    const double length = way.Length();
    return length > 1e-12 ? way / length : ch::VNULL;
}

// A point of a ball on a bore's rim: on the ball, on the rim, and the way
// from the ball's centre to the rim's point
struct SeatPoint
{
    ch::ChVector3d onBall;
    ch::ChVector3d onRim;
    ch::ChVector3d out;
};

/** A ball on the rim of a bore too narrow for it, a check valve's on its
 * seat, at the frames of the ball's sphere and the bore's (fb, fh): the
 * point of the circle of the bore's end on the ball's side 'turn' round the
 * rim from its point nearest the ball's centre, the ball's surface toward
 * it, and the way there. The rim's own circle: Bullet's points on the
 * facets lie on its polygon, inside it by the polygon's sagitta, and a ball
 * of 6 on a hole of 4 rested 4.4 um high on them. False where the ball's
 * centre is not past the bore's end.
 */
bool seatAt(const ShapeOn& ball,
            const ShapeOn& hollow,
            const ch::ChFramed& fb,
            const ch::ChFramed& fh,
            double turn,
            SeatPoint& point)
{
    const ch::ChVector3d centre = fb.GetPos();
    const ch::ChVector3d axis = fh.GetRotMat().GetAxisZ();
    const double along = ch::Vdot(axis, centre - fh.GetPos());
    if (!(std::abs(along) > hollow.half)) {
        return false;
    }
    const ch::ChVector3d end = fh.GetPos() + axis * std::copysign(hollow.half, along);
    ch::ChVector3d x = centre - end;
    x -= axis * ch::Vdot(axis, x);
    const double off = x.Length();
    x = off > 1e-12 ? x / off : ch::ChVector3d(fh.GetRotMat().GetAxisX());
    const ch::ChVector3d y = ch::Vcross(axis, x);
    point.onRim = end + (x * std::cos(turn) + y * std::sin(turn)) * hollow.radius;
    point.out = point.onRim - centre;
    const double length = point.out.Length();
    if (!(length > 1e-12)) {
        return false;
    }
    point.out /= length;
    point.onBall = centre + point.out * ball.ball;
    return true;
}

/** A point on a sphere or a cylinder's side put where that surface meets
 * the other at the bodies' frames a and b, as ContactLoad::follow: collision
 * detection's point is fixed on each body, and a ball rolling turns it
 * v h / 2 behind its centre by a substep's end, where the normal force
 * turns the ball on -- one on a level plate gained 5.7 mm/s^2 at 1 m/s. The
 * round side's point is its surface's farthest along the normal, the other
 * side's under it, as deep as the fixed points are: exact on a face. Two
 * spheres' normal is along their centres; any other turns with the body
 * whose face it is (Point::face), a round shape's the other's, a point of
 * two flat shapes either's whose face it is, an edge's against an edge
 * neither. Fixed in space it lags the face: a ball rolling down a plate
 * tipping at 1 rad/s, against the exact model with the friction's slip,
 * ended 0.5 s 6.3e-3 mm short at output steps of 1 ms and 3.2e-2 at 0.1
 * ms; turning, 2.4e-3 over and 6e-5. A cube on it, friction 0.5, sliding
 * off from 0.46 s: at 0.6 s 9.9e-3 short of the model's 4.857 mm and
 * 3.6e-3; turning, 5.4e-4 over and 4e-5. Frictionless, against its closed
 * form to 0.4 s: 3.8e-2 short and 2.8e-2; turning, 2.3e-3 over and 2e-4.
 */
void rollAlong(const ch::ChFrame<>& a, const ch::ChFrame<>& b, ContactLoad::Point& point)
{
    if (!point.roundA && !point.roundB && !point.face) {
        return;
    }
    // A ball on a bore's rim: its point the same turn round the rim from
    // the one nearest the ball's centre now
    if (point.seat) {
        const bool ballA = point.roundA != nullptr;
        const ShapeOn& ball = ballA ? *point.roundA : *point.roundB;
        SeatPoint at;
        if (seatAt(ball,
                   *point.seat,
                   ball.frame >> (ballA ? a : b),
                   point.seat->frame >> (ballA ? b : a),
                   point.seatTurn,
                   at)) {
            point.onA = a.TransformPointParentToLocal(ballA ? at.onBall : at.onRim);
            point.onB = b.TransformPointParentToLocal(ballA ? at.onRim : at.onBall);
            point.normal = ballA ? at.out : -at.out;
        }
        return;
    }
    const ch::ChVector3d atA = a.TransformPointLocalToParent(point.onA);
    const ch::ChVector3d atB = b.TransformPointLocalToParent(point.onB);
    ch::ChVector3d normal = point.normal;
    if (point.face == 2) {
        normal = b.TransformDirectionLocalToParent(point.normalOn);
    }
    else if (point.face == 1) {
        normal = a.TransformDirectionLocalToParent(point.normalOn);
    }
    else if (point.roundA && point.roundB && point.roundA->concave != point.roundB->concave) {
        // A pin in a bore, a ball in a socket: hollowWay() through the
        // other's axis where the point is along it, or its centre --
        // Bullet's against a bore's long facets leaned along it, and pushed
        // a pin out of its hole at 300 mm/s. Through a pin's middle, a pin
        // hanging out of a bore had its inner end's point, pressed on the
        // bore's top, turned onto its underside, and went through the wall
        const bool hollowB = point.roundB->concave;
        const ShapeOn& hollow = hollowB ? *point.roundB : *point.roundA;
        const ShapeOn& inside = hollowB ? *point.roundA : *point.roundB;
        const ch::ChFramed outer = hollow.frame >> (hollowB ? b : a);
        const ch::ChFramed inner = inside.frame >> (hollowB ? a : b);
        ch::ChVector3d centre = inner.GetPos();
        if (!(inside.ball > 0.0)) {
            const ch::ChVector3d axis = inner.GetRotMat().GetAxisZ();
            const ch::ChVector3d& on = hollowB ? atA : atB;
            centre += axis * std::clamp(ch::Vdot(axis, on - centre), -inside.half, inside.half);
        }
        // Across a bore, a cylinder's rim farthest out (rimFarthest()), the
        // cap's the point is nearer
        if (!(inside.ball > 0.0) && !(hollow.ball > 0.0)
            && !inLine(inner.GetRotMat().GetAxisZ(), outer.GetRotMat().GetAxisZ())) {
            const ch::ChVector3d axis = inner.GetRotMat().GetAxisZ();
            const double t = ch::Vdot(axis, (hollowB ? atA : atB) - inner.GetPos());
            const ch::ChVector3d cap = inner.GetPos() + axis * std::copysign(inside.half, t);
            ch::ChVector3d p;
            ch::ChVector3d out;
            if (rimFarthest(cap, axis, inside.radius, outer, p, out)) {
                const ch::ChVector3d h = outer.GetRotMat().GetAxisZ();
                const ch::ChVector3d wall =
                    outer.GetPos() + h * ch::Vdot(h, p - outer.GetPos()) + out * hollow.radius;
                point.onA = a.TransformPointParentToLocal(hollowB ? p : wall);
                point.onB = b.TransformPointParentToLocal(hollowB ? wall : p);
                point.normal = out * (hollowB ? 1.0 : -1.0);
                return;
            }
        }
        const ch::ChVector3d out = hollowWay(hollow, outer, inside, inner, centre);
        // From A to B: out of the hollow one's axis, or into it
        if (out.Length2() > 0.0) {
            normal = out * (hollowB ? 1.0 : -1.0);
        }
    }
    else if (point.roundA && point.roundB && point.roundA->ball > 0.0 && point.roundB->ball > 0.0) {
        const ch::ChVector3d centres =
            (point.roundB->frame >> b).GetPos() - (point.roundA->frame >> a).GetPos();
        const double length = centres.Length();
        if (length > 0.0) {
            normal = centres / length;
        }
    }
    ch::ChVector3d pA;
    ch::ChVector3d pB;
    const bool movesA = point.roundA && roundSupport(*point.roundA, a, normal, atA, pA);
    const bool movesB = point.roundB && roundSupport(*point.roundB, b, -normal, atB, pB);
    if (!movesA && !movesB) {
        point.normal = normal;
        return;
    }
    if (!movesB) {
        pB = pA + normal * ch::Vdot(normal, atB - pA);
    }
    if (!movesA) {
        pA = pB + normal * ch::Vdot(normal, atA - pB);
    }
    point.onA = a.TransformPointParentToLocal(pA);
    point.onB = b.TransformPointParentToLocal(pB);
    point.normal = normal;
}

void rollAll(const ch::ChFrame<>& a,
             const ch::ChFrame<>& b,
             std::vector<ContactLoad::Point>& points)
{
    for (auto& point : points) {
        rollAlong(a, b, point);
    }
}

// A round's curvature tensor where it touches, facing 'outward', absolute:
// convex positive, a hollow one's negative
ch::ChMatrix33d
roundCurvature(const ShapeOn& on, const ch::ChBody& body, const ch::ChVector3d& outward)
{
    const double sign = on.concave ? -1.0 : 1.0;
    if (on.ball > 0.0) {
        return ch::ChMatrix33d((ch::ChMatrix33d::Identity() - ch::TensorProduct(outward, outward))
                               * (sign / on.ball));
    }
    // A cylinder's cap is flat, as ShapesOn::curvatureAt() takes it: the
    // normal within 60 deg of its axis -- curved there, a cylinder on its
    // cap took a ball's law and ran 118 s for 2
    const ch::ChVector3d axis = (on.frame >> body).GetRotMat().GetAxisZ();
    ch::ChVector3d round = ch::Vcross(axis, outward);
    const double length = round.Length();
    if (!(on.radius > 0.0) || std::abs(ch::Vdot(axis, outward)) > 0.5 || !(length > 1e-9)) {
        return ch::ChMatrix33d::Zero();
    }
    round /= length;
    return ch::ChMatrix33d(ch::TensorProduct(round, round) * (sign / on.radius));
}

// The largest sagitta of a bore's rims as its mesh has them: the widest
// turn between the mesh's vertices on either end's circle, which the
// polygon's sides cut across inside it. A third of a turn where an end has
// fewer than three
double rimSagitta(const ShapeOn& hollow, const ShapeOn& mesh)
{
    std::array<std::vector<double>, 2> angles;
    const double close = 1e-6 * hollow.radius;
    for (std::size_t v = 0; v < mesh.count; ++v) {
        const ch::ChVector3d p = hollow.frame.TransformPointParentToLocal(mesh.vertices[v]);
        if (std::abs(std::hypot(p.x(), p.y()) - hollow.radius) > close
            || std::abs(std::abs(p.z()) - hollow.half) > close) {
            continue;
        }
        angles[p.z() > 0.0 ? 1 : 0].push_back(std::atan2(p.y(), p.x()));
    }
    double widest = 0.0;
    for (auto& end : angles) {
        if (end.size() < 3) {
            widest = std::max(widest, 2.0 * pi / 3.0);
            continue;
        }
        std::sort(end.begin(), end.end());
        widest = std::max(widest, end.front() + 2.0 * pi - end.back());
        for (std::size_t k = 1; k < end.size(); ++k) {
            widest = std::max(widest, end[k] - end[k - 1]);
        }
    }
    return hollow.radius * (1.0 - std::cos(0.5 * widest));
}

// Whether a round goes inside a hollow one: a ball or a cylinder narrower
// than a bore, a ball smaller than a socket. Their pair's points are the
// rounds' own (hollowPoints()); a ball too big for a bore sits on its rim
// (seatAt()) -- one of 6 on a hole of 4 fell through the block when it was
// a pair, its points against the bore dropped and none of its own
bool fitsIn(const ShapeOn& inner, const ShapeOn& hollow)
{
    if (hollow.ball > 0.0) {
        return inner.ball > 0.0 && inner.ball < hollow.ball;
    }
    const double small = inner.ball > 0.0 ? inner.ball : inner.radius;
    return small > 0.0 && small < hollow.radius;
}

// Whether a cylinder's axis is in line with a bore's: within 25 deg. In
// line it touches the bore along its side and at a stretch's ends
// (hollowPoints()); across it, with its rims (rimFarthest())
bool inLine(const ch::ChVector3d& inner, const ch::ChVector3d& bore)
{
    return std::abs(ch::Vdot(inner, bore)) >= std::cos(25.0 * pi / 180.0);
}

// Whether a round inside a hollow one has the rounds' own points now, at
// their bodies' frames, and not Bullet's: a ball; a cylinder in line with a
// bore, or across it with both its caps within the bore's length. Across a
// bore, or with a cap out of it, a cylinder's are Bullet's
bool held(const ShapeOn& inner, const ShapeOn& hollow)
{
    if (inner.ball > 0.0) {
        return true;
    }
    if (hollow.ball > 0.0) {
        return false;
    }
    const ch::ChFramed fi = inner.frame >> *inner.body;
    const ch::ChFramed fh = hollow.frame >> *hollow.body;
    const ch::ChVector3d ai = fi.GetRotMat().GetAxisZ();
    const ch::ChVector3d ah = fh.GetRotMat().GetAxisZ();
    if (inLine(ai, ah)) {
        return true;
    }
    for (const double end : {-1.0, 1.0}) {
        if (std::abs(ch::Vdot(ah, fi.GetPos() + ai * (end * inner.half) - fh.GetPos()))
            > hollow.half) {
            return false;
        }
    }
    return true;
}

/** The point of a cylinder's rim -- its cap's centre at 'cap', its axis
 * 'axis' -- farthest from a bore's axis (the bore's frame 'outer'), and the
 * bore's radius through it, 'out': across a bore, where the cylinder
 * presses on the wall; along any line on the cylinder the distance from the
 * bore's axis is convex, so its deepest points are on its rims. Each step
 * takes the rim's point that way and the radius through it; where they
 * agree the point is farthest. A pin lying across a bore on its rims, its
 * points Bullet's on the facets, lost them all while it pressed -- Bullet
 * read the rims 24 um off the facets where the true wall had them 0.1 um
 * in -- and was thrown off; along the radius through the cap's centre it
 * sank 27 um. False where there is no one way: the cap's centre on the
 * bore's axis, or the axis along the radius.
 */
bool rimFarthest(const ch::ChVector3d& cap,
                 const ch::ChVector3d& axis,
                 double radius,
                 const ch::ChFramed& outer,
                 ch::ChVector3d& p,
                 ch::ChVector3d& out)
{
    const ch::ChVector3d h = outer.GetRotMat().GetAxisZ();
    auto radial = [&](const ch::ChVector3d& at, ch::ChVector3d& way) {
        way = at - outer.GetPos();
        way -= h * ch::Vdot(h, way);
        const double length = way.Length();
        if (!(length > 1e-12)) {
            return false;
        }
        way /= length;
        return true;
    };
    if (!radial(cap, out)) {
        return false;
    }
    for (int k = 0; k < 16; ++k) {
        const ch::ChVector3d side = out - axis * ch::Vdot(axis, out);
        const double length = side.Length();
        if (!(length > 1e-9)) {
            return false;
        }
        p = cap + side * (radius / length);
        ch::ChVector3d next;
        if (!radial(p, next)) {
            return false;
        }
        const bool same = (next - out).Length2() < 1e-24;
        out = next;
        if (same) {
            break;
        }
    }
    return true;
}

// A point of a round inside a hollow one: on the inner surface, on the
// hollow one's, and the way from the hollow one's axis or centre through it
struct HollowPoint
{
    ch::ChVector3d onInner;
    ch::ChVector3d onHollow;
    ch::ChVector3d out;
};

/** The points of a round inside a hollow one, a pin in a bore or a ball in a
 * bore or a socket, from the two surfaces themselves: a cylinder's at both
 * ends of its stretch within the bore's reach along its axis, a ball's at
 * its centre, each the way hollowWay() takes: along the bore's radius
 * through it (the socket's, through its centre), or from the bore's rim
 * where it stands out past the bore's end, a ball's centre too. None but
 * within 'reach' of touching, nor across a bore (a pin more than 25 deg
 * off its axis),
 * nor past a part round's angles (roundSupport()); a cylinder in a socket
 * none. Bullet's own against a bore's facets lost a pin riding the bore's
 * true surface, which lies up to the mesh's deflection outside them: 10 um
 * into the facets, GJK gave no point, and the pin fell through the wall.
 */
void hollowPoints(const ShapeOn& inner,
                  const ShapeOn& hollow,
                  double reach,
                  std::vector<HollowPoint>& points)
{
    points.clear();
    const ch::ChFramed fi = inner.frame >> *inner.body;
    const ch::ChFramed fh = hollow.frame >> *hollow.body;
    const ch::ChVector3d ah = fh.GetRotMat().GetAxisZ();
    const bool tube = !(hollow.ball > 0.0);
    const double big = tube ? hollow.radius : hollow.ball;
    const double small = inner.ball > 0.0 ? inner.ball : inner.radius;
    if (!(small > 0.0) || !(small < big)) {
        return;
    }
    std::vector<ch::ChVector3d> at;
    if (inner.ball > 0.0) {
        at.push_back(fi.GetPos());
    }
    else {
        if (!tube) {
            return;
        }
        const ch::ChVector3d ai = fi.GetRotMat().GetAxisZ();
        const double along = ch::Vdot(ai, ah);
        if (!inLine(ai, ah)) {
            // Across it, its rims farthest out, both caps within the bore
            if (!held(inner, hollow)) {
                return;
            }
            for (const double end : {-1.0, 1.0}) {
                HollowPoint point;
                if (!rimFarthest(fi.GetPos() + ai * (end * inner.half),
                                 ai,
                                 inner.radius,
                                 fh,
                                 point.onInner,
                                 point.out)
                    || !roundSupport(hollow,
                                     *hollow.body,
                                     -point.out,
                                     point.onInner,
                                     point.onHollow)
                    || ch::Vdot(point.onHollow - point.onInner, point.out) > reach) {
                    continue;
                }
                points.push_back(point);
            }
            return;
        }
        // Where each end is along the bore's axis, kept within its reach
        const double s0 = ch::Vdot(ah, fi.GetPos() - fh.GetPos());
        double t1 = (-hollow.half - s0) / along;
        double t2 = (hollow.half - s0) / along;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        const double low = std::max(-inner.half, t1);
        const double high = std::min(inner.half, t2);
        if (low > high) {
            return;
        }
        at.push_back(fi.GetPos() + ai * low);
        at.push_back(fi.GetPos() + ai * high);
    }
    for (ch::ChVector3d centre : at) {
        const ch::ChVector3d out = hollowWay(hollow, fh, inner, fi, centre);
        if (!(out.Length2() > 0.0)) {
            continue;
        }
        HollowPoint point;
        point.out = out;
        if (!roundSupport(hollow, *hollow.body, -out, centre, point.onHollow)
            || !roundSupport(inner, *inner.body, out, centre, point.onInner)
            || ch::Vdot(point.onHollow - point.onInner, out) > reach) {
            continue;
        }
        points.push_back(point);
    }
}

// A surface's curvature tensor at each vertex of its faces, convex
// positive, in the tangent plane: that of the height quadric
// h = A x^2/2 + B x y + C y^2/2 + D x + E y fitted by
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
std::vector<ch::ChMatrix33d> vertexCurvatures(const std::vector<ch::ChVector3d>& vertices,
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
    std::vector<ch::ChMatrix33d> curvatures(vertices.size(), ch::ChMatrix33d::Zero());
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
        // [[a, b], [b, c]] in the tangent plane's axes, in the vertex's frame
        auto tensor = [&](double a, double b, double c) {
            const ch::ChMatrix33d t11 = ch::TensorProduct(t1, t1);
            const ch::ChMatrix33d t12 = ch::TensorProduct(t1, t2);
            const ch::ChMatrix33d t22 = ch::TensorProduct(t2, t2);
            return ch::ChMatrix33d(a * t11 + b * (t12 + t12.transpose()) + c * t22);
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
                curvatures[v] = tensor(-fit[0], -fit[1], -fit[2]);
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
            curvatures[v] = tensor(most, 0.0, most);
            continue;
        }
        const Eigen::Vector3d abc = lu.solve(r);
        curvatures[v] = tensor(abc[0], abc[1], abc[2]);
    }
    return curvatures;
}

// A mesh's curvature tensor at each vertex
std::vector<ch::ChMatrix33d> meshCurvatures(const std::vector<ch::ChVector3d>& vertices,
                                            const std::vector<ch::ChVector3i>& triangles)
{
    std::vector<std::vector<int>> faces;
    faces.reserve(triangles.size());
    for (const auto& t : triangles) {
        faces.push_back({t.x(), t.y(), t.z()});
    }
    return vertexCurvatures(vertices, faces);
}

// The solid at each vertex of a mesh that has no model of it, a flexible
// part's surface of its elements' faces (ShapeOn::surfaces): its faces there
// that an edge of the solid parts -- a turn of 40 deg or more, as
// vertexCurvatures() tells one -- each by its normal and the curvature of
// the mesh on its own side of the edge. Fitted across it, a vertex on a
// strip's edge is a ridge, round 1/1 mm at elements of 3, and a bar lying
// across the strip there would touch it as a ball does, not along a line.
std::vector<std::vector<ShapeOn::Surface>>
meshSurfaces(const std::vector<ch::ChVector3d>& vertices,
             const std::vector<ch::ChVector3i>& triangles)
{
    const double sharp = std::cos(40.0 * pi / 180.0);
    // A vertex's faces: the normal of the first triangle of each, the sum
    // of its triangles', by their areas, and its copy of the vertex
    struct Side
    {
        ch::ChVector3d seed;
        ch::ChVector3d sum;
        int copy;
    };
    std::vector<std::vector<Side>> sides(vertices.size());
    std::vector<ch::ChVector3d> copies;
    std::vector<ch::ChVector3i> faces;
    for (const auto& t : triangles) {
        const int corners[3] = {t.x(), t.y(), t.z()};
        if (std::any_of(std::begin(corners), std::end(corners), [&](int v) {
                return v < 0 || static_cast<std::size_t>(v) >= vertices.size();
            })) {
            continue;
        }
        const ch::ChVector3d area = ch::Vcross(vertices[corners[1]] - vertices[corners[0]],
                                               vertices[corners[2]] - vertices[corners[0]]);
        const double length = area.Length();
        if (!(length > 0.0)) {
            continue;
        }
        const ch::ChVector3d n = area / length;
        int face[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            auto& at = sides[static_cast<std::size_t>(corners[k])];
            auto side = std::find_if(at.begin(), at.end(), [&](const Side& one) {
                return ch::Vdot(one.seed, n) > sharp;
            });
            if (side == at.end()) {
                at.push_back({n, ch::VNULL, static_cast<int>(copies.size())});
                copies.push_back(vertices[corners[k]]);
                side = at.end() - 1;
            }
            side->sum += area;
            face[k] = side->copy;
        }
        faces.emplace_back(face[0], face[1], face[2]);
    }
    const std::vector<ch::ChMatrix33d> tensors = meshCurvatures(copies, faces);
    std::vector<std::vector<ShapeOn::Surface>> surfaces(vertices.size());
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        for (const Side& side : sides[v]) {
            const double length = side.sum.Length();
            surfaces[v].push_back({length > 0.0 ? side.sum / length : side.seed,
                                   tensors[static_cast<std::size_t>(side.copy)],
                                   -1});
        }
    }
    return surfaces;
}

// Whether a mesh's triangle, its corners among the mesh's vertices, has
// one on the mesh's round face 'round' (ContactShape::Surface::round)
bool onRound(const ShapeOn& mesh, std::initializer_list<const ch::ChVector3d*> corners, int round)
{
    for (const ch::ChVector3d* corner : corners) {
        const auto v = static_cast<std::size_t>(corner - mesh.vertices);
        if (v < mesh.surfaces.size()) {
            for (const auto& face : mesh.surfaces[v]) {
                if (face.round == round) {
                    return true;
                }
            }
        }
    }
    return false;
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

    // The curvature tensor of the shape where a point of its body touches
    // ('at', in the body's frame), the surface there facing 'outward', both
    // absolute: a mesh's at the triangle's corner nearest, a hull's at its
    // point farthest out that way, where a convex shape touches; a sphere's
    // all round, a cylinder's side round its axis, its cap and a box flat
    ch::ChMatrix33d curvatureAt(const ch::ChCollisionShape* shape,
                                const ch::ChBody& body,
                                const ch::ChVector3d& at,
                                const ch::ChVector3d& outward) const
    {
        const ShapeOn* on = of(shape);
        if (!on) {
            return ch::ChMatrix33d::Zero();
        }
        const ch::ChMatrix33d turn = body.GetRotMat();
        if (on->vertices) {
            const auto* triangle = static_cast<const ch::ChCollisionShapeMeshTriangle*>(shape);
            double nearest = std::numeric_limits<double>::infinity();
            std::size_t best = on->curvatures.size();
            for (const ch::ChVector3d* corner : {triangle->V1, triangle->V2, triangle->V3}) {
                const auto index = static_cast<std::size_t>(corner - on->vertices);
                const double apart = (*corner - at).Length2();
                if (index < on->curvatures.size() && apart < nearest) {
                    nearest = apart;
                    best = index;
                }
            }
            if (const ch::ChMatrix33d* exact = surfaceAt(*on, best, turn.transpose() * outward)) {
                return ch::ChMatrix33d(turn * *exact * turn.transpose());
            }
            if (best >= on->curvatures.size()) {
                return ch::ChMatrix33d::Zero();
            }
            return ch::ChMatrix33d(turn * on->curvatures[best] * turn.transpose());
        }
        if (on->ball > 0.0) {
            const ch::ChMatrix33d across =
                ch::ChMatrix33d::Identity() - ch::TensorProduct(outward, outward);
            return ch::ChMatrix33d(across / on->ball);
        }
        const ch::ChMatrix33d frame = on->frame.GetRotMat();
        if (on->radius > 0.0) {
            // Its cap is flat, its side round its axis
            const ch::ChVector3d axis = turn * (frame * ch::VECT_Z);
            if (std::abs(ch::Vdot(axis, outward)) > 0.5) {
                return ch::ChMatrix33d::Zero();
            }
            ch::ChVector3d round = ch::Vcross(axis, outward);
            round.Normalize();
            return ch::ChMatrix33d(ch::TensorProduct(round, round) / on->radius);
        }
        const ch::ChVector3d local = frame.transpose() * (turn.transpose() * outward);
        const ch::ChMatrix33d whole = turn * frame;
        // The solid's own, at the hull's point nearest: the point farthest
        // out ties on a facet face down, a roller's corners at both its
        // ends, and the fits there differed -- a 5 mm roller read as 1/7.7
        // mm round one way, 1/3.9 and 1/12 another, its law a line's and
        // then a ball's, the force at one depth 150 times as much
        if (!on->surfaces.empty()) {
            const ch::ChVector3d point = on->frame.TransformPointParentToLocal(at);
            std::size_t nearest = on->hull.size();
            double least = std::numeric_limits<double>::infinity();
            for (std::size_t v = 0; v < on->hull.size(); ++v) {
                const double apart = (on->hull[v] - point).Length2();
                if (apart < least) {
                    least = apart;
                    nearest = v;
                }
            }
            if (const ch::ChMatrix33d* exact = surfaceAt(*on, nearest, local)) {
                return ch::ChMatrix33d(whole * *exact * whole.transpose());
            }
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
        if (best >= on->curvatures.size()) {
            return ch::ChMatrix33d::Zero();
        }
        return ch::ChMatrix33d(whole * on->curvatures[best] * whole.transpose());
    }

    // The curvature of the solid's face at a hull's or a mesh's point v
    // whose normal is nearest 'outward' (the same frame), none without them
    static const ch::ChMatrix33d*
    surfaceAt(const ShapeOn& on, std::size_t v, const ch::ChVector3d& outward)
    {
        if (v >= on.surfaces.size()) {
            return nullptr;
        }
        const ch::ChMatrix33d* best = nullptr;
        double most = -std::numeric_limits<double>::infinity();
        for (const auto& face : on.surfaces[v]) {
            const double along = ch::Vdot(face.normal, outward);
            if (along > most) {
                most = along;
                best = &face.tensor;
            }
        }
        return best;
    }
};

// The length of a line contact's points: the deepest of each cross-section,
// across the line the curved way, chained by their minimum spanning tree --
// a torus lying flat touches along its ring, 2 pi 10 mm, its patch's
// extent 10.7, and a cylinder on its side along its length. 0 for one. A
// ring's tree is the ring less a segment: a path whose ends are no farther
// apart than its longest segment and a half is closed -- the torus's ring of
// 13 points came 8% short.
double lineLength(const std::vector<ch::ChVector3d>& at,
                  const std::vector<double>& gaps,
                  const std::vector<ch::ChVector3d>& across)
{
    std::vector<std::size_t> line;
    for (std::size_t i = 0; i < at.size(); ++i) {
        bool deepest = true;
        for (std::size_t j = 0; j < at.size() && deepest; ++j) {
            const ch::ChVector3d d = at[j] - at[i];
            const double over = std::abs(ch::Vdot(d, across[i]));
            const double along = std::sqrt(std::max(d.Length2() - over * over, 0.0));
            deepest = !(j != i && over > 2.0 * along && gaps[j] < gaps[i] - 1e-8);
        }
        if (deepest) {
            line.push_back(i);
        }
    }
    // Prim's
    double length = 0.0;
    double longest = 0.0;
    std::vector<double> reach(line.size(), std::numeric_limits<double>::infinity());
    std::vector<std::size_t> from(line.size(), line.size());
    std::vector<int> degree(line.size(), 0);
    std::vector<bool> in(line.size(), false);
    if (!line.empty()) {
        reach[0] = 0.0;
    }
    for (std::size_t n = 0; n < line.size(); ++n) {
        std::size_t next = line.size();
        for (std::size_t k = 0; k < line.size(); ++k) {
            if (!in[k] && (next == line.size() || reach[k] < reach[next])) {
                next = k;
            }
        }
        in[next] = true;
        length += reach[next];
        if (from[next] < line.size()) {
            longest = std::max(longest, reach[next]);
            ++degree[next];
            ++degree[from[next]];
        }
        for (std::size_t k = 0; k < line.size(); ++k) {
            const double apart = (at[line[k]] - at[line[next]]).Length();
            if (!in[k] && apart < reach[k]) {
                reach[k] = apart;
                from[k] = next;
            }
        }
    }
    // A path, its ends close: a ring
    std::vector<std::size_t> ends;
    bool path = line.size() > 2;
    for (std::size_t k = 0; k < line.size() && path; ++k) {
        if (degree[k] == 1) {
            ends.push_back(k);
        }
        path = degree[k] <= 2;
    }
    // -- not a straight line's: a roller's facet touches at its four
    // corners, a path end to end whose ends are as far apart, and the line
    // read twice its length
    if (path && ends.size() == 2) {
        const double gap = (at[line[ends[0]]] - at[line[ends[1]]]).Length();
        if (gap <= 1.5 * longest && gap < 0.5 * length) {
            length += gap;
        }
    }
    return length;
}

// Johnson's line contact, a cylinder of radius R pressed into a half-space
// (Johnson, Contact Mechanics, 1985; Puttock and Thwaite's for one radius):
// under P a unit length its axis sinks d = P / (pi E*) (2 ln(4R/a) - 1),
// a = sqrt(4 P R / (pi E*)) the contact's half width. Linear, pi/4 E* L d,
// but for the logarithm, which makes it five times as soft at a metal's
// sinks of 1e-7 mm -- the comparisons of Pereira et al. and Skrinjar et al.
// put it first among the line contact laws. P at a sink d by fixed point on
// the logarithm (its slope 1 / (2 ln(4R/a) - 1), a tenth), then the power
// law tangent to it there, k d^e, e its slope d ln P / d ln d =
// (2 ln - 1) / (2 ln - 2), 1.05 to 1.1; Pereira et al.'s explicit fit to
// it takes 1.094. The logarithm at least 2: a contact half as wide as the
// cylinder is past the law, and e stays at most 1.5, Hertz's.
void johnsonLine(double modulus,
                 double radius,
                 double length,
                 double depth,
                 double& stiffness,
                 double& exponent)
{
    double load = pi / 4.0 * modulus * depth;
    double log = 2.0;
    for (int i = 0; i < 8; ++i) {
        const double half = std::sqrt(4.0 * load * radius / (pi * modulus));
        log = std::max(std::log(4.0 * radius / half), 2.0);
        load = pi * modulus * depth / (2.0 * log - 1.0);
    }
    exponent = (2.0 * log - 1.0) / (2.0 * log - 2.0);
    stiffness = length * load / std::pow(depth, exponent);
}

// Each patch's stiffness and exponent (ContactLaw): the law's, or Hertz's
// from the shapes' materials, 1/E* the sum of the two shapes'
// 1 / (E / (1 - nu^2)). By the parts' sizes, a ball's: k = 4/3 E* R^0.5,
// e = 1.5, 1/R = 1/RA + 1/RB, each the radius of the ball of the solid's
// volume. Where the patch touches, by its relative curvature: at each point
// the two sides' curvature tensors summed in the contact plane, and its
// principal curvatures k1 >= k2, the patch's their means --
// - k1 none, two flat faces: a ball's, R the patch's extent, the farthest of
//   its points from their centre (a face resting flat on a face its size),
//   and a single point on two flat faces the parts' sizes';
// - k2 at least a hundredth of k1, round or oval: a ball's, R 1/sqrt(k1 k2)
//   (an ellipse's is within a few percent of it below an aspect of ten);
// - else a line, a cylinder on its side or a torus lying flat on its ring:
//   Johnson's line contact (johnsonLine()), R 1/k1, L the line's length
//   (lineLength()), as the power law tangent to it at the patch's depth
//   now, or at the depth an impact at its speed goes to before it presses;
//   one point's line, a ball's, R 1/k1.
// The means, not the tensors': a torus's ring curves across the tube at
// each point, a different way round the ring, and the tensors averaged over
// it read it as a round contact of 4 mm.
void patchStiffness(ContactLoad& load, const ShapesOn& shapes)
{
    const ContactLaw& law = load.law;
    load.stiffness.assign(load.patches, law.stiffness);
    load.exponents.assign(load.patches, law.exponent);
    // The speed the contact comes in at, while it does not press yet
    {
        const auto a = load.GetBodyA();
        const auto b = load.GetBodyB();
        bool pressed = false;
        double fastest = 0.0;
        for (std::size_t i = 0; i < load.points.size(); ++i) {
            const auto& point = load.points[i];
            const ch::ChVector3d pA = a->TransformPointLocalToParent(point.onA);
            const ch::ChVector3d pB = b->TransformPointLocalToParent(point.onB);
            pressed = pressed || -ch::Vdot(point.normal, pB - pA) > 0.0;
            fastest = std::max(fastest,
                               -ch::Vdot(point.normal,
                                         load.speedOnB(i, point)
                                             - a->PointSpeedLocalToParent(point.onA)));
        }
        if (!pressed) {
            load.impact = fastest;
        }
    }
    // By the allowance (Drake's penetration allowance): k = m g / d^e, m the
    // lighter body moving, so that it sinks d under its weight whatever it
    // is made of -- standard gravity's where the run has none
    if (law.from == ContactLaw::Stiffness::Penetration) {
        const double mass = load.lighterMass();
        const auto* system = load.GetBodyA()->GetSystem();
        double gravity = system ? system->GetGravitationalAcceleration().Length() : 0.0;
        if (!(gravity > 0.0)) {
            gravity = 9.80665;
        }
        if (std::isfinite(mass) && law.allowance > 0.0) {
            load.stiffness.assign(load.patches,
                                  mass * gravity / std::pow(law.allowance, law.exponent));
        }
        return;
    }
    if (law.from != ContactLaw::Stiffness::Materials || load.points.empty()) {
        return;
    }
    const auto& points = load.points;
    const auto bodyA = load.GetBodyA();
    const auto bodyB = load.GetBodyB();
    struct Patch
    {
        std::size_t count = 0;
        double k1 = 0.0;
        double k2 = 0.0;
        ch::ChVector3d centre;
        double extent = 0.0;
        const ShapeOn* onA = nullptr;
        const ShapeOn* onB = nullptr;
        // Its points, absolute, how far apart the bodies are there, and the
        // way the contact curves most there
        std::vector<ch::ChVector3d> at;
        std::vector<double> gaps;
        std::vector<ch::ChVector3d> across;
    };
    std::vector<Patch> patches(load.patches);
    for (const auto& point : points) {
        Patch& patch = patches[point.patch];
        const ch::ChVector3d& n = point.normal;
        const ch::ChVector3d pA = bodyA->TransformPointLocalToParent(point.onA);
        const ch::ChVector3d pB = bodyB->TransformPointLocalToParent(point.onB);
        // A round's its own (a hollow one's has no shape of Bullet's)
        const ch::ChMatrix33d both =
            (point.roundA ? roundCurvature(*point.roundA, *bodyA, n)
                          : shapes.curvatureAt(point.shapeA, *bodyA, point.onA, n))
            + (point.roundB ? roundCurvature(*point.roundB, *bodyB, -n)
                            : shapes.curvatureAt(point.shapeB, *bodyB, point.onB, -n));
        // In the contact plane, its principal curvatures and the first's way
        ch::ChVector3d u = ch::Vcross(n, std::abs(n.x()) < 0.9 ? ch::VECT_X : ch::VECT_Y);
        u.Normalize();
        const ch::ChVector3d v = ch::Vcross(n, u);
        const double a = ch::Vdot(u, both * u);
        const double b = ch::Vdot(u, both * v);
        const double c = ch::Vdot(v, both * v);
        const double mean = 0.5 * (a + c);
        const double half = std::sqrt(0.25 * (a - c) * (a - c) + b * b);
        const double k1 = std::max(mean + half, 0.0);
        const double k2 = std::clamp(mean - half, 0.0, k1);
        const double angle = 0.5 * std::atan2(2.0 * b, a - c);
        ++patch.count;
        patch.k1 += k1;
        patch.k2 += k2;
        patch.centre += point.onA;
        patch.at.push_back(pA);
        patch.gaps.push_back(ch::Vdot(n, pB - pA));
        patch.across.push_back(u * std::cos(angle) + v * std::sin(angle));
        if (!patch.onA) {
            patch.onA = point.roundA ? point.roundA : shapes.of(point.shapeA);
            patch.onB = point.roundB ? point.roundB : shapes.of(point.shapeB);
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
            const double k1 = patch.k1 / static_cast<double>(patch.count);
            const double k2 = patch.k2 / static_cast<double>(patch.count);
            // Flat below a curvature of a kilometre's
            if (!(k1 > 1e-3)) {
                radius = patch.extent > 0.0 ? patch.extent : part;
            }
            else if (k2 >= 0.01 * k1) {
                radius = 1.0 / std::sqrt(k1 * k2);
            }
            else {
                const double length = lineLength(patch.at, patch.gaps, patch.across);
                radius = 1.0 / k1;
                if (length > 0.0) {
                    double depth = 0.0;
                    for (const double gap : patch.gaps) {
                        depth = std::max(depth, -gap);
                    }
                    if (!(depth > 0.0)) {
                        const double mass = load.lighterMass();
                        if (std::isfinite(mass)) {
                            depth = load.impact * std::sqrt(mass / (pi / 4.0 * modulus * length));
                        }
                    }
                    johnsonLine(modulus,
                                radius,
                                length,
                                std::max(depth, 1e-12 * radius),
                                load.stiffness[p],
                                load.exponents[p]);
                    continue;
                }
            }
        }
        if (radius > 0.0) {
            load.stiffness[p] = 4.0 / 3.0 * modulus * std::sqrt(radius);
            load.exponents[p] = 1.5;
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

// The points of a cylinder's contact with a mesh's triangle, as
// cylinderPoints() has a face's: its cap's rim as a polygon, or its side's
// segment, clipped to the triangle, each within reach of its plane; the
// triangle's normal, outward as a mesh's triangles are wound, in normal.
// False when none is over the triangle, or the cylinder's middle is behind
// it: a bar lying across a plate's edge has the plate's side face through
// it, and its cap, 5 mm past that face, was a point 5 mm deep as soon as the
// bar's rim was over the side's triangle. Bullet's GJK and EPA on a
// triangle, which has no inside, gave a cylinder lying on a mesh's flat
// face one point, at one end -- a pin dropped on its side rocked on it and
// was thrown off -- and a pin's rim on a bore's facet the pin's own side's
// normal, 20 deg off.
bool cylinderTrianglePoints(const ShapeOn& cylinder,
                            const ShapeOn& mesh,
                            const ch::ChCollisionShapeMeshTriangle& triangle,
                            double reach,
                            std::vector<CylinderPoint>& points,
                            ch::ChVector3d& normal)
{
    points.clear();
    const ch::ChVector3d v[3] = {mesh.body->TransformPointLocalToParent(*triangle.V1),
                                 mesh.body->TransformPointLocalToParent(*triangle.V2),
                                 mesh.body->TransformPointLocalToParent(*triangle.V3)};
    ch::ChVector3d n = ch::Vcross(v[1] - v[0], v[2] - v[0]);
    const double area = n.Length();
    if (!(area > 0.0)) {
        return false;
    }
    n /= area;
    const ch::ChFramed c = cylinder.frame >> *cylinder.body;
    if (ch::Vdot(n, c.GetPos() - v[0]) < 0.0) {
        return false;
    }
    normal = n;
    // Its feature toward the triangle, as cylinderPoints() takes it
    const ch::ChVector3d axis = c.GetRotMat().GetAxisZ();
    const double along = -ch::Vdot(axis, n);
    const ch::ChVector3d side = -n - axis * along;
    const double sideLength = side.Length();
    std::vector<ch::ChVector3d> polygon;
    if (std::abs(along) >= 0.5) {
        const ch::ChVector3d center =
            c.GetPos() + axis * (along > 0.0 ? cylinder.half : -cylinder.half);
        const ch::ChVector3d u = sideLength > 1e-9 ? side / sideLength : c.GetRotMat().GetAxisX();
        const ch::ChVector3d w = ch::Vcross(axis, u);
        constexpr int rim = 16;
        for (int k = 0; k < rim; ++k) {
            const double angle = 2.0 * pi * k / rim;
            polygon.push_back(center
                              + (u * std::cos(angle) + w * std::sin(angle)) * cylinder.radius);
        }
    }
    else {
        if (!(sideLength > 1e-9)) {
            return false;
        }
        const ch::ChVector3d out = side / sideLength * cylinder.radius;
        polygon.push_back(c.GetPos() + axis * cylinder.half + out);
        polygon.push_back(c.GetPos() - axis * cylinder.half + out);
    }
    // Clipped to the triangle: inside each edge's plane square to it
    // (Sutherland and Hodgman's; a segment is a polygon of two, its points
    // come twice)
    std::vector<ch::ChVector3d> clipped;
    for (int e = 0; e < 3 && !polygon.empty(); ++e) {
        const ch::ChVector3d& from = v[e];
        ch::ChVector3d inward = ch::Vcross(n, v[(e + 1) % 3] - from);
        if (ch::Vdot(inward, v[(e + 2) % 3] - from) < 0.0) {
            inward = -inward;
        }
        clipped.clear();
        for (std::size_t k = 0; k < polygon.size(); ++k) {
            const ch::ChVector3d& p = polygon[k];
            const ch::ChVector3d& q = polygon[(k + 1) % polygon.size()];
            const double dp = ch::Vdot(inward, p - from);
            const double dq = ch::Vdot(inward, q - from);
            if (dp >= 0.0) {
                clipped.push_back(p);
            }
            if ((dp >= 0.0) != (dq >= 0.0)) {
                clipped.push_back(p + (q - p) * (dp / (dp - dq)));
            }
        }
        polygon.swap(clipped);
    }
    for (const ch::ChVector3d& p : polygon) {
        const double gap = ch::Vdot(n, p - v[0]);
        bool again = gap > reach;
        for (const auto& known : points) {
            again = again || (known.onCylinder - p).Length2() < 1e-18;
        }
        if (!again) {
            points.push_back({p, p - n * gap});
        }
    }
    return !points.empty();
}

// The nearest points of two segments, p1..q1 and p2..q2 (Ericson's); of two
// in line, the middle of where they are beside each other, and that
// stretch's ends on the first, as its parameters, in beside
void nearestOfSegments(const ch::ChVector3d& p1,
                       const ch::ChVector3d& q1,
                       const ch::ChVector3d& p2,
                       const ch::ChVector3d& q2,
                       ch::ChVector3d& on1,
                       ch::ChVector3d& on2,
                       std::array<double, 2>& beside)
{
    const ch::ChVector3d d1 = q1 - p1;
    const ch::ChVector3d d2 = q2 - p2;
    const ch::ChVector3d r = p1 - p2;
    const double a = d1.Length2();
    const double e = d2.Length2();
    const double f = ch::Vdot(d2, r);
    constexpr double tiny = 1e-24;
    double s = 0.0;
    double t = 0.0;
    beside = {0.0, 0.0};
    if (a > tiny && e > tiny) {
        const double b = ch::Vdot(d1, d2);
        const double c = ch::Vdot(d1, r);
        const double denom = a * e - b * b;
        if (denom > 1e-10 * a * e) {
            s = std::clamp((b * f - c * e) / denom, 0.0, 1.0);
            beside = {s, s};
        }
        else {
            const double sp = ch::Vdot(p2 - p1, d1) / a;
            const double sq = ch::Vdot(q2 - p1, d1) / a;
            const double low = std::max(0.0, std::min(sp, sq));
            const double high = std::min(1.0, std::max(sp, sq));
            s = std::clamp(0.5 * (low + high), 0.0, 1.0);
            beside = low < high ? std::array<double, 2> {low, high} : std::array<double, 2> {s, s};
        }
        t = (b * s + f) / e;
        if (t < 0.0) {
            t = 0.0;
            s = std::clamp(-c / a, 0.0, 1.0);
        }
        else if (t > 1.0) {
            t = 1.0;
            s = std::clamp((b - c) / a, 0.0, 1.0);
        }
    }
    else if (e > tiny) {
        t = std::clamp(f / e, 0.0, 1.0);
    }
    else if (a > tiny) {
        s = std::clamp(-ch::Vdot(d1, r) / a, 0.0, 1.0);
    }
    on1 = p1 + d1 * s;
    on2 = p2 + d2 * t;
}

// A cylinder's side on a mesh's triangle its segment is not over
// (cylinderTrianglePoints()): an edge of the triangle or a corner, the point
// of it nearest the cylinder's axis and the cylinder's point over it, the
// normal from there to the axis; an edge along the axis, the two ends of
// where they are beside each other. 1 with the points, 0 when none is within
// reach, -1 where the cap is what is nearest, or the axis goes through the
// triangle: Bullet's point then. Bullet's on a corner 0.8 mm from where a
// bar lay on a strip's mesh had a normal 8 deg off the corner's way to the
// axis: along it the corner, 34 um clear of the bar, measured 13 um in, and
// the strip took 4.7 kN where its blow is 340 N.
int cylinderTriangleEdge(const ShapeOn& cylinder,
                         const ShapeOn& mesh,
                         const ch::ChCollisionShapeMeshTriangle& triangle,
                         double reach,
                         std::vector<CylinderPoint>& points,
                         ch::ChVector3d& normal)
{
    points.clear();
    const ch::ChVector3d v[3] = {mesh.body->TransformPointLocalToParent(*triangle.V1),
                                 mesh.body->TransformPointLocalToParent(*triangle.V2),
                                 mesh.body->TransformPointLocalToParent(*triangle.V3)};
    ch::ChVector3d n = ch::Vcross(v[1] - v[0], v[2] - v[0]);
    const double area = n.Length();
    if (!(area > 0.0)) {
        return -1;
    }
    n /= area;
    const ch::ChFramed c = cylinder.frame >> *cylinder.body;
    const ch::ChVector3d axis = c.GetRotMat().GetAxisZ();
    const ch::ChVector3d ends[2] = {c.GetPos() - axis * cylinder.half,
                                    c.GetPos() + axis * cylinder.half};
    // Inside the triangle, a point of its plane
    auto inside = [&](const ch::ChVector3d& p) {
        for (int e = 0; e < 3; ++e) {
            ch::ChVector3d inward = ch::Vcross(n, v[(e + 1) % 3] - v[e]);
            if (ch::Vdot(inward, v[(e + 2) % 3] - v[e]) < 0.0) {
                inward = -inward;
            }
            if (ch::Vdot(inward, p - v[e]) < 0.0) {
                return false;
            }
        }
        return true;
    };
    const double heights[2] = {ch::Vdot(n, ends[0] - v[0]), ch::Vdot(n, ends[1] - v[0])};
    if ((heights[0] < 0.0) != (heights[1] < 0.0)) {
        const double at = heights[0] / (heights[0] - heights[1]);
        if (inside(ends[0] + (ends[1] - ends[0]) * at)) {
            return -1;
        }
    }
    double nearest = std::numeric_limits<double>::infinity();
    ch::ChVector3d onAxis;
    ch::ChVector3d onTriangle;
    std::array<double, 2> beside {};
    for (int e = 0; e < 3; ++e) {
        ch::ChVector3d a;
        ch::ChVector3d q;
        std::array<double, 2> along {};
        nearestOfSegments(ends[0], ends[1], v[e], v[(e + 1) % 3], a, q, along);
        const double distance = (q - a).Length2();
        if (distance < nearest) {
            nearest = distance;
            onAxis = a;
            onTriangle = q;
            beside = along;
        }
    }
    for (int k = 0; k < 2; ++k) {
        const ch::ChVector3d q = ends[k] - n * heights[k];
        if (heights[k] * heights[k] < nearest && inside(q)) {
            nearest = heights[k] * heights[k];
            onAxis = ends[k];
            onTriangle = q;
            beside = {0.0, 0.0};
        }
    }
    const double distance = std::sqrt(nearest);
    if (!(distance > 1e-12)) {
        return -1;
    }
    const ch::ChVector3d out = (onTriangle - onAxis) / distance;
    // Past a cap's plane: the cap's, or its rim's
    if (std::abs(ch::Vdot(axis, out)) > 1e-6) {
        return -1;
    }
    if (distance - cylinder.radius > reach) {
        return 0;
    }
    normal = -out;
    if (beside[1] > beside[0]) {
        for (const double s : beside) {
            const ch::ChVector3d a = ends[0] + (ends[1] - ends[0]) * s;
            points.push_back({a + out * cylinder.radius, a + out * distance});
        }
    }
    else {
        points.push_back({onAxis + out * cylinder.radius, onTriangle});
    }
    return 1;
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
    // The distinct moving bodies it joins
    const std::vector<ch::ChBody*>& joins() const
    {
        return bodies;
    }

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
    // A dynamic or static run's: the joints its cams that lift off hold
    // (ContactPair::cam) not made, their followers pushed by the contact
    bool liftCams = false;
    // A dynamic run's: its flexible bodies built so (docs/AssemblyDynamics.md
    // D4b), each a ChModalAssembly, the frames on it on its interfaces'
    // boundary bodies. Its own ChBody stays out of the system, a proxy that
    // carries the floating frame's motion for what reads a body's
    // (syncFlexible()). Else a flexible body is rigid, with its mass.
    bool flexibleRun = false;
    struct FlexibleRun
    {
        Id body = NoId;
        std::size_t record = 0;  // in model.dynamics->flexibles
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        ChronoFlexible::Built built;
#endif
        // The body's frame in the floating frame's, at the start
        ch::ChFramed inFloating;
        // The floating frame and the interfaces' boundary bodies at the
        // start, what the deformation is from
        ch::ChFramed floating0;
        std::vector<ch::ChFramed> interfaces0;
        std::vector<double> hz;
        // Its contact interface's, in built.interfaces, or -1 (step 5)
        int contact = -1;
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        // Or a contact anywhere on it: its shapes on its first interface's
        // body (FlexibleContact)
        std::shared_ptr<FlexibleContact> anywhere;
#endif
    };
    std::vector<FlexibleRun> flexibles;
    // Per frame, the boundary body of the interface it is on, a flexible
    // body's, and where the frame is on it (its origin but for a contact
    // shape's frame, which shares its part's contact interface); null, the
    // frame's body's
    std::vector<std::shared_ptr<ch::ChBody>> frameBody;
    std::vector<ch::ChFramed> frameLocal;
    // Per body, its FlexibleRun, or -1
    std::vector<int> flexibleOf;

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
        if (frame < frameBody.size() && frameBody[frame]) {
            return frameBody[frame];
        }
        const Id body = model.frames[frame].body;
        return body == NoId ? world : bodies[body];
    }

    bool isFlexible(std::size_t body) const
    {
        return body < flexibleOf.size() && flexibleOf[body] >= 0;
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
        if (frame < frameBody.size() && frameBody[frame]) {
            return frameLocal[frame];
        }
        return toCh(model.frames[frame].local);
    }

    // The flexible part whose interface a body is the boundary body of, or
    // null: what moves with it is the part (D4b), its proxy's mass, inertia
    // and pose -- its own mass, a millionth of the part's when built, reads
    // 0 in a run
    const FlexibleRun* carrierOf(const ch::ChBody* body) const
    {
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        for (const auto& run : flexibles) {
            for (const auto& face : run.built.interfaces) {
                if (face.get() == body) {
                    return &run;
                }
            }
        }
#else
        (void)body;
#endif
        return nullptr;
    }

    // The body that moves with a load on a body: a flexible part's interface
    // carries its part, the proxy (syncFlexible() brought to the floating
    // frame); any other body, itself
    ch::ChBody* carrier(ch::ChBody* body) const
    {
        const FlexibleRun* run = carrierOf(body);
        return run ? bodies[run->body].get() : body;
    }

    // The mass a flexible part's interface carries, its part's (D4b); 0 for
    // any other body
    double carriedMass(const ch::ChBody* body) const
    {
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        if (const FlexibleRun* run = carrierOf(body)) {
            return run->built.mass;
        }
#endif
        return 0.0;
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

    // A dynamic run's flexible bodies (flexibleRun, D4b): each a modal
    // assembly with a boundary body for every frame on it -- its interface's
    // nodes, or the nearest -- reduced before the joints go on those bodies.
    // Why one cannot be built, or empty.
    std::string buildFlexibles()
    {
        flexibleOf.assign(bodies.size(), -1);
        frameBody.assign(model.frames.size(), nullptr);
        frameLocal.assign(model.frames.size(), ch::ChFramed());
        flexibles.clear();
        if (!flexibleRun || !model.dynamics || model.dynamics->flexibles.empty()) {
            return {};
        }
#ifndef ASSEMBLY_WITH_CHRONO_MODAL
        return "it has flexible bodies, and the backend was built without them";
#else
        for (std::size_t r = 0; r < model.dynamics->flexibles.size(); ++r) {
            const Flexible& record = model.dynamics->flexibles[r];
            const std::string name = model.sourceName(record.source);
            if (record.body >= bodies.size() || isFlexible(record.body)) {
                return "the flexible body " + name + " is on no part, or on one with another";
            }
            if (pinned[record.body] || model.bodies[record.body].grounded) {
                return "the flexible body " + name + " is on a part fixed to the ground";
            }
            FlexibleRun run;
            run.body = record.body;
            run.record = r;
            // Its contact shapes' frames (step 5): not interfaces each, all
            // on the one of the faces it is touched on, where the first is
            std::vector<Id> touched;
            for (const auto& shape : model.dynamics->contactShapes) {
                if (shape.frame < model.frames.size()
                    && model.frames[shape.frame].body == record.body
                    && std::find(touched.begin(), touched.end(), shape.frame) == touched.end()) {
                    touched.push_back(shape.frame);
                }
            }
            // Its contact shapes without the faces it is touched on: a
            // contact anywhere on its mesh's surface (FlexibleContact)
            const bool anywhere = !touched.empty() && record.contactNodes.empty();
            std::vector<ChronoFlexible::Interface> faces;
            std::vector<Id> on;
            for (const auto& frame : model.frames) {
                if (frame.body != record.body
                    || std::find(touched.begin(), touched.end(), frame.id) != touched.end()) {
                    continue;
                }
                ChronoFlexible::Interface face;
                face.local = toCh(frame.local);
                for (const auto& given : record.interfaces) {
                    if (given.frame == frame.id) {
                        face.nodes = given.nodes;
                    }
                }
                faces.push_back(std::move(face));
                on.push_back(frame.id);
            }
            if (faces.empty()) {
                return "nothing holds the flexible body " + name;
            }
            // The contact interface's body at its nodes' centre, in the
            // body's axes, as a joint's is on its faces
            ch::ChFramed contactAt;
            if (!touched.empty() && !anywhere) {
                ch::ChVector3d centre;
                std::size_t counted = 0;
                for (Id n : record.contactNodes) {
                    if (n < record.nodes.size()) {
                        centre += toCh(record.nodes[n]);
                        ++counted;
                    }
                }
                if (counted) {
                    contactAt.SetPos(centre / static_cast<double>(counted));
                }
            }
            if (!touched.empty() && !anywhere) {
                run.contact = static_cast<int>(faces.size());
                faces.push_back({contactAt, record.contactNodes});
            }
            const auto& proxy = bodies[record.body];
            const ch::ChFramed pose(proxy->GetPos(), proxy->GetRot());
            try {
                run.built = ChronoFlexible::build(*sys, record, pose, faces);
                run.hz = ChronoFlexible::reduce(*sys, run.built, record);
            }
            catch (const std::exception& e) {
                return "the flexible body " + name + ": " + e.what();
            }
            const ch::ChFramed floating = run.built.assembly->GetFloatingFrameOfReference();
            run.inFloating = pose >> floating.GetInverse();
            run.floating0 = floating;
            for (const auto& body : run.built.interfaces) {
                run.interfaces0.emplace_back(body->GetPos(), body->GetRot());
            }
            for (std::size_t i = 0; i < on.size(); ++i) {
                frameBody[on[i]] = run.built.interfaces[i];
            }
            // Each contact shape's frame where it is on the contact interface,
            // or on the first interface's body, which carries it for a
            // contact anywhere
            for (Id frame : touched) {
                if (anywhere) {
                    frameBody[frame] = run.built.interfaces[0];
                    frameLocal[frame] =
                        toCh(model.frames[frame].local) >> faces[0].local.GetInverse();
                }
                else {
                    frameBody[frame] = run.built.interfaces[run.contact];
                    frameLocal[frame] = toCh(model.frames[frame].local) >> contactAt.GetInverse();
                }
            }
            // In the system after the modal assembly, and before the loads
            if (anywhere) {
                auto item = std::make_shared<FlexibleContact>();
                item->assembly = run.built.assembly.get();
                item->carrier = run.built.interfaces[0].get();
                item->carrierAt = faces[0].local;
                item->surface = std::make_shared<ChronoFlexible::Surface>(record);
                for (const auto& node : run.built.mesh->GetNodes()) {
                    item->nodes.push_back(dynamic_cast<const ch::fea::ChNodeFEAxyz*>(node.get()));
                }
                if (!item->makeMesh()) {
                    Base::Console().warning(
                        "Assembly: the flexible body %s has no mesh of its surface to be touched "
                        "on: its shapes are carried by its first interface, and do not follow its "
                        "bending\n",
                        name.c_str());
                }
                sys->Add(item);
                run.anywhere = item;
            }
            // Its mass the mesh's, for what reads a body's (the tolerance's
            // lightest body); it is not in the system
            proxy->SetMass(run.built.mass);
            flexibleOf[record.body] = static_cast<int>(flexibles.size());
            flexibles.push_back(std::move(run));
        }
        return {};
#endif
    }

    // A flexible body's modal coordinates, of the modes the reduction kept
    std::vector<double> modalCoordinates(const FlexibleRun& run) const
    {
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        const auto& q = run.built.assembly->GetModalCoordinatesPosLevel();
        return std::vector<double>(q.data(), q.data() + q.size());
#else
        (void)run;
        return {};
#endif
    }

    // A flexible body's inertia loads per unit mass, in SI: the coefficients
    // of its residual modes' fields (our Chrono's GetInertialResidualModes())
    // for its floating frame's motion and the gravity -- g - a, -alpha, then
    // w's products -- in the frame's axes; none without those modes. At the
    // start ('rest') the frame's accelerations are not its motion's yet, so
    // the gravity and w's products alone: those it held read 122 rad/s^2 for
    // a clamped rod at rest, and the line through the smallest steps' (as a
    // reaction's) 83 m/s^2 along it, the reduced model's first steps settling
    std::vector<double> inertialLoad(const FlexibleRun& run, bool rest = false) const
    {
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        if (run.built.assembly->GetInertialResidualModes().cols() != 12) {
            return {};
        }
        const ch::ChFrameMoving<> floating = run.built.assembly->GetFloatingFrameOfReference();
        // The translation's absolute, the turn's in F's axes
        ch::ChVectorDynamic<> acc = run.built.assembly->GetFloatingFrameAccelerations();
        if (rest) {
            acc.setZero();
        }
        const ch::ChVector3d load = floating.GetRot().RotateBack(
            sys->GetGravitationalAcceleration() - ch::ChVector3d(acc(0), acc(1), acc(2)));
        const ch::ChVector3d w = floating.GetAngVelLocal();
        return {load.x(),
                load.y(),
                load.z(),
                -acc(3),
                -acc(4),
                -acc(5),
                w.x() * w.x(),
                w.y() * w.y(),
                w.z() * w.z(),
                -w.x() * w.y(),
                -w.x() * w.z(),
                -w.y() * w.z()};
#else
        (void)run;
        return {};
#endif
    }

    // A flexible body's interfaces' deformation: each boundary body's pose in
    // the floating frame against its pose there at the start, as Chrono's
    // GetLocalDeformations() has it (the reduction matrix's coordinates): its
    // translation (in mm, 'length' the model's to SI) and its turn (rad)
    std::vector<double> interfaceDeformation(const FlexibleRun& run, double length) const
    {
        std::vector<double> out;
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        const ch::ChFrameMoving<> floating = run.built.assembly->GetFloatingFrameOfReference();
        for (std::size_t i = 0; i < run.built.interfaces.size(); ++i) {
            const auto& body = run.built.interfaces[i];
            const ch::ChFramed& start = run.interfaces0[i];
            const ch::ChVector3d moved =
                floating.GetRot().RotateBack(body->GetPos() - floating.GetPos())
                - run.floating0.GetRot().RotateBack(start.GetPos() - run.floating0.GetPos());
            const ch::ChQuaterniond turn = start.GetRot().GetConjugate() * run.floating0.GetRot()
                * floating.GetRot().GetConjugate() * body->GetRot();
            double angle = 0.0;
            ch::ChVector3d axis;
            turn.GetAngleAxis(angle, axis);
            for (int k = 0; k < 3; ++k) {
                out.push_back(moved[k] / length);
            }
            for (int k = 0; k < 3; ++k) {
                out.push_back(angle * axis[k]);
            }
        }
#else
        (void)run;
        (void)length;
#endif
        return out;
    }

    // A flexible body's mode shapes (ModeShapes): the reduction matrix's
    // rows of the mesh's nodes, then its inertia loads' residual modes', in
    // the floating frame's axes at the start, turned into the body's and in
    // mm: a column of a translation in mm per mm, of a turn in mm per rad, of
    // a mode in mm per modal coordinate, of a residual mode in mm per its
    // coefficient in SI (inertialLoad())
    ModeShapes modeShapesOf(const FlexibleRun& run, const std::string& source, double length) const
    {
        ModeShapes shapes;
        shapes.source = source;
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        const auto& psi = run.built.assembly->GetModalReductionMatrix();
        const auto& residual = run.built.assembly->GetInertialResidualModes();
        const auto boundary = static_cast<Eigen::Index>(6 * run.built.interfaces.size());
        const auto nodes = static_cast<Eigen::Index>(run.built.mesh->GetNumNodes());
        if (psi.rows() < boundary + 3 * nodes) {
            return shapes;
        }
        const Eigen::Index reduced = psi.cols();
        const Eigen::Index extra = residual.cols() == 12 && residual.rows() >= 3 * nodes ? 12 : 0;
        const Eigen::Index columns = reduced + extra;
        shapes.columns = static_cast<std::size_t>(columns);
        shapes.values.resize(static_cast<std::size_t>(3 * nodes * columns));
        // The floating frame's axes to the body's
        const ch::ChMatrix33<> back = run.inFloating.GetRotMat().transpose();
        for (Eigen::Index node = 0; node < nodes; ++node) {
            for (Eigen::Index c = 0; c < columns; ++c) {
                const bool translation = c < boundary && (c % 6) < 3;
                const double scale = translation ? 1.0 : 1.0 / length;
                const ch::ChVector3d inFloating =
                    c < reduced ? ch::ChVector3d(psi(boundary + 3 * node, c),
                                                 psi(boundary + 3 * node + 1, c),
                                                 psi(boundary + 3 * node + 2, c))
                                : ch::ChVector3d(residual(3 * node, c - reduced),
                                                 residual(3 * node + 1, c - reduced),
                                                 residual(3 * node + 2, c - reduced));
                const ch::ChVector3d inBody = back * inFloating;
                for (int k = 0; k < 3; ++k) {
                    shapes.values[static_cast<std::size_t>((3 * node + k) * columns + c)] =
                        inBody[k] * scale;
                }
            }
        }
#else
        (void)run;
        (void)length;
#endif
        return shapes;
    }

    // Each flexible body's proxy at its floating frame's motion, carried to
    // the body's frame: its pose, velocities and accelerations
    void syncFlexible()
    {
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        for (const auto& run : flexibles) {
            ch::ChFrameMoving<> floating = run.built.assembly->GetFloatingFrameOfReference();
            const ch::ChVectorDynamic<> a = run.built.assembly->GetFloatingFrameAccelerations();
            floating.SetPosDt2(ch::ChVector3d(a(0), a(1), a(2)));
            floating.SetAngAccLocal(ch::ChVector3d(a(3), a(4), a(5)));
            const ch::ChFrameMoving<> moving = ch::ChFrameMoving<>(run.inFloating) >> floating;
            static_cast<ch::ChFrameMoving<>&>(*bodies[run.body]) = moving;
        }
#endif
    }

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
        std::string why;
        auto link = neutralLink(joint, why);
        if (!link) {
            return why;
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

    // A joint's NeutralLink, not added; none, and why, where it cannot be
    std::shared_ptr<NeutralLink> neutralLink(const Joint& joint, std::string& why)
    {
        auto ref = [&](Id frame) {
            NeutralLink::FrameRef r;
            const Id body = model.frames[frame].body;
            r.body = body == NoId ? nullptr : bodyOf(frame).get();
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
                why = "it holds a point on a curve the model does not have";
                return nullptr;
            }
        }
        auto link =
            std::make_shared<NeutralLink>(joint, fi, fj, carrier ? &fk : nullptr, std::move(curve));
        if (link->rowCategories().empty()) {
            // The Reference fails the same.
            why = "its frames are coaxial and it has no carrier frame to measure the turns from";
            return nullptr;
        }
        return link;
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
    d->flexibles.clear();
    d->frameBody.clear();
    d->flexibleOf.clear();
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
        // A flexible body's ChBody is its proxy, not in the system
        std::set<Id> flexible;
        if (d->flexibleRun && d->model.dynamics) {
            for (const auto& record : d->model.dynamics->flexibles) {
                flexible.insert(record.body);
            }
        }
        for (const auto& body : d->model.bodies) {
            auto b = std::make_shared<ch::ChBody>();
            const ch::ChFramed f = toCh(body.placement);
            b->SetPos(f.GetPos());
            b->SetRot(f.GetRot());
            if (!flexible.count(body.id)) {
                sys.AddBody(b);
            }
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
        const std::string flexibleWhy = d->buildFlexibles();
        if (!flexibleWhy.empty()) {
            why << flexibleWhy;
        }
        std::set<Id> lifted;
        if (d->liftCams && d->model.dynamics) {
            for (const auto& pair : d->model.dynamics->contacts) {
                if (pair.cam != NoId) {
                    lifted.insert(pair.cam);
                }
            }
        }
        for (const auto& joint : d->model.joints) {
            if (lifted.count(joint.id)) {
                continue;
            }
            const std::string reason = d->addJoint(joint);
            if (!reason.empty()) {
                d->unsupported.push_back(joint.id);
                why << (why.tellp() > 0 ? "; " : "") << "the joint "
                    << d->model.sourceName(joint.source) << " (" << kindName(joint.kind)
                    << "): " << reason;
            }
        }
        if (!d->unsupported.empty() || !flexibleWhy.empty()) {
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
    // A flexible body's mesh with its frames; built so in a dynamic run only
    // (D4b), else the body is rigid with its mass
    for (auto& flexible : si.dynamics->flexibles) {
        if (flexible.body < n) {
            const Base::Placement toCentre = centre[flexible.body].inverse();
            for (auto& node : flexible.nodes) {
                toCentre.multVec(node, node);
            }
        }
    }

    ChronoSession run;
    run.d->liftCams = true;
    run.d->flexibleRun = !prescribed && spec.analysis == Analysis::Dynamic;
    if (run.load(si) != Status::Ok) {
        return Status::Failed;
    }
    if (!run.d->refused.empty()) {
        return refuse(run.d->refused);
    }
    // A part's own initial velocity would start one of a flexible body's
    // interfaces alone
    for (const auto& initial : si.dynamics->initial) {
        if (initial.frameJ == NoId && initial.frameI < si.frames.size()
            && run.d->isFlexible(si.frames[initial.frameI].body)) {
            return refuse("an initial velocity of the flexible part "
                          + model.sourceName(model.bodies[si.frames[initial.frameI].body].source)
                          + " is not supported yet");
        }
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
        // A flexible body's mass is its mesh's (buildFlexibles()), and its
        // proxy, out of the system, takes the part's inertia at that mass,
        // for what reads a body's -- a stop's and a friction's substeps about
        // a joint on it (carrier()), which Chrono's default of 1 kg m^2 made
        // a stop's period 1.99 s for the 27 g rod's 19 ms
        const bool flexible = run.d->isFlexible(b);
        if (flexible && (!inertia || inertia->mass <= 0.0)) {
            continue;
        }
        if (!inertia || inertia->mass <= 0.0) {
            return refuse("the part " + model.sourceName(model.bodies[b].source)
                          + " has no mass, and is free to move");
        }
        const double scale = flexible ? body->GetMass() / (inertia->mass * SI::mass) : 1.0;
        if (!flexible) {
            body->SetMass(inertia->mass * SI::mass);
        }
        // Scaled with the model: in kg m^2 already
        const auto& i = inertia->inertia;
        ch::ChMatrix33d tensor;
        tensor << i[0], i[3], i[4], i[3], i[1], i[5], i[4], i[5], i[2];
        body->SetInertia(tensor * scale);
    }
    // What each frame reports besides the poses (sec 8.4)
    std::vector<std::pair<Id, std::shared_ptr<ch::ChLinkTSDA>>> springs;
    std::vector<std::pair<Id, std::shared_ptr<TorsionLoad>>> torsions;
    std::vector<std::pair<Id, std::shared_ptr<AppliedLoad>>> appliedLoads;
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
            auto load = std::make_shared<AppliedLoad>(run.d->bodyOf(force.frameJ),
                                                      run.d->bodyOf(force.frameI),
                                                      run.d->globalOf(force.frameI),
                                                      std::move(magnitude),
                                                      toCh(force.direction),
                                                      force.followsFrame,
                                                      force.kind == ForceKind::AppliedTorque);
            loads->Add(load);
            appliedLoads.emplace_back(force.source, load);
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
            // The joint's angle, of xJ about zI, J as the joint's motors
            // and limits see it. In SI with the model: k, c and the
            // preload scaled already.
            auto torsion = std::make_shared<TorsionLoad>(
                run.d->bodyOf(force.frameJ),
                run.d->bodyOf(force.frameI),
                run.d->driverLocalOf(force.frameJ) >> *run.d->bodyOf(force.frameJ),
                run.d->globalOf(force.frameI),
                force.k,
                force.c,
                force.preload,
                force.rest);
            loads->Add(torsion);
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
                           run.d->localOf(force.frameI).GetPos(),
                           run.d->localOf(force.frameJ).GetPos());
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
    // The limits' stops (sec 5.2's JointStop): each limit's record, none for
    // a rigid stop at the study's restitution; a penalty stop's load
    using StopRecord = Assembly::Solver::Stop;
    std::vector<const StopRecord*> stopOf(si.limits.size(), nullptr);
    for (const auto& stop : si.dynamics->stops) {
        if (stop.limit < stopOf.size()) {
            stopOf[stop.limit] = &stop;
        }
    }
    std::vector<std::shared_ptr<StopLoad>> penalties(si.limits.size());
    for (std::size_t k = 0; k < si.limits.size(); ++k) {
        const Limit& limit = si.limits[k];
        const StopRecord* stop = stopOf[k];
        if (!stop || stop->mode != StopRecord::Mode::Penalty || !(limit.min || limit.max)
            || !run.d->validFrame(limit.frameI) || !run.d->validFrame(limit.frameJ)
            || run.d->isRigid(limit.frameI, limit.frameJ)) {
            continue;
        }
        // In SI with the model: an angle's k and c scaled already
        penalties[k] = std::make_shared<StopLoad>(run.d->bodyOf(limit.frameJ),
                                                  run.d->bodyOf(limit.frameI),
                                                  run.d->globalOf(limit.frameJ),
                                                  run.d->globalOf(limit.frameI),
                                                  limit,
                                                  stop->k,
                                                  stop->c,
                                                  run.d->limitAngle[limit.id]);
        loads->Add(penalties[k]);
    }
    // The joints' frictions: a load each between its joint's bodies, its
    // normal load from the joint's reaction after each step. None in a
    // static run, which slips at no speed.
    std::vector<std::shared_ptr<FrictionLoad>> frictions;
    if (spec.analysis != Analysis::Static) {
        for (const auto& friction : si.dynamics->frictions) {
            if (!run.d->validFrame(friction.frameI) || !run.d->validFrame(friction.frameJ)
                || run.d->isRigid(friction.frameI, friction.frameJ)) {
                continue;
            }
            auto load = std::make_shared<FrictionLoad>(run.d->bodyOf(friction.frameJ),
                                                       run.d->bodyOf(friction.frameI),
                                                       run.d->globalOf(friction.frameJ),
                                                       run.d->globalOf(friction.frameI),
                                                       friction);
            loads->Add(load);
            frictions.push_back(load);
        }
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
        // A cam that lifts off (ContactPair::cam): its load, A the cam's
        // body and B the follower's, frames I and J on them, the curve in
        // frame I's coordinates, the joint's offset (a roller's radius,
        // signed) and the follower's side; the curve's parameter at the
        // closest point last found, and its unit tangent there, in frame I's
        // coordinates
        struct CamFollow
        {
            ContactLoad* load = nullptr;
            const Joint* joint = nullptr;
            std::shared_ptr<const CurveGeometry> curve;
            ch::ChFramed frameI;
            ch::ChFramed frameJ;
            double offset = 0.0;
            double side = 1.0;
            double u = 0.0;
            Base::Vector3d tangent;
            bool started = false;

            // The curve's normal in its plane at a point, N2 = T x N1, N1 the
            // reference square to the tangent T (the joint's)
            Base::Vector3d normal(const CurveProjection& at) const
            {
                Base::Vector3d tangent = at.at.d1;
                tangent.Normalize();
                const Base::Vector3d& a = curve->reference();
                Base::Vector3d n1 = a - tangent * (a * tangent);
                n1.Normalize();
                return tangent % n1;
            }
            // Frame J's origin in frame I's coordinates, the bodies at a and
            // b (now by default)
            Base::Vector3d follower(const ch::ChFrame<>& a, const ch::ChFrame<>& b) const
            {
                const ch::ChFramed i = frameI >> a;
                const ch::ChFramed j = frameJ >> b;
                const ch::ChVector3d local = i.TransformPointParentToLocal(j.GetPos());
                return Base::Vector3d(local.x(), local.y(), local.z());
            }
            Base::Vector3d follower() const
            {
                return follower(*load->GetBodyA(), *load->GetBodyB());
            }
            // The cam's point with the bodies at a and b: the curve's closest
            // to the follower, from u, on the cam, and the follower's nearest
            // it -- a roller's centre less its radius along the curve's
            // normal in its plane, N2 -- the normal N2 to the follower's
            // side; the curve's projection, for its parameter and tangent
            CurveProjection place(const ch::ChFrame<>& a,
                                  const ch::ChFrame<>& b,
                                  double from,
                                  bool start,
                                  ContactLoad::Point& point) const
            {
                const ch::ChFramed i = frameI >> a;
                const Base::Vector3d x = follower(a, b);
                const CurveProjection at =
                    start ? projectOnCurveGlobally(*curve, x) : projectOnCurve(*curve, x, from);
                const Base::Vector3d n2 = normal(at);
                point.onA =
                    a.TransformPointParentToLocal(i.TransformPointLocalToParent(toCh(at.at.p)));
                point.onB = b.TransformPointParentToLocal(
                    i.TransformPointLocalToParent(toCh(x - n2 * offset)));
                point.normal = i.TransformDirectionLocalToParent(toCh(n2 * side));
                return at;
            }
            // How far the follower is off the cam now, to its side
            double gap() const
            {
                const Base::Vector3d x = follower();
                const CurveProjection at = projectOnCurveGlobally(*curve, x);
                return side * ((x - at.at.p) * normal(at) - offset);
            }
        };
        std::vector<CamFollow> cams;
        // A profile contact's pair of faces (ContactPair's profiles): its
        // load, A the body of profile a, B of b; the profiles' frames on
        // them; A's profile in its frame's plane, made once; whether B's
        // plane faces A's, taken where the run starts
        struct ProfileFollow
        {
            ContactLoad* load = nullptr;
            const ContactShape* a = nullptr;
            const ContactShape* b = nullptr;
            ch::ChFramed frameA;
            ch::ChFramed frameB;
            Profile2 profileA;
            bool mirrored = false;

            // The heights a pair's points are taken at, through where the two
            // solids overlap through the plane, B's frame in A's 'into': the
            // two-point Gauss rule's, level -1 and 1, so that the force acts
            // where a contact spread evenly along that overlap does and
            // holds a part's tilt as it would -- one point at the middle
            // held none, and a part on profile contacts alone tipped out of
            // their plane from round-off, tenfold each 0.1 s. Level 0, the
            // middle; half way between the solids when they do not overlap,
            // its one level.
            std::vector<double> levels(const ch::ChFramed& into) const
            {
                double low = 0.0;
                double high = 0.0;
                if (overlap(into, low, high) && high > low) {
                    return {-1.0, 1.0};
                }
                return {0.0};
            }
            double height(const ch::ChFramed& into, double level) const
            {
                double low = 0.0;
                double high = 0.0;
                if (!overlap(into, low, high)) {
                    return 0.5 * (low + high);
                }
                return 0.5 * (low + high) + level * (high - low) / (2.0 * std::sqrt(3.0));
            }
            // Where the solids overlap through the plane, low to high; false
            // when they do not, low and high then the middles of A's and B's
            bool overlap(const ch::ChFramed& into, double& low, double& high) const
            {
                const double up = into.GetRotMat().GetAxisZ().z() < 0.0 ? -1.0 : 1.0;
                double lowB = into.GetPos().z() + up * b->size.x;
                double highB = into.GetPos().z() + up * b->size.y;
                if (lowB > highB) {
                    std::swap(lowB, highB);
                }
                low = std::max(a->size.x, lowB);
                high = std::min(a->size.y, highB);
                if (low <= high) {
                    return true;
                }
                low = 0.5 * (a->size.x + a->size.y);
                high = 0.5 * (lowB + highB);
                return false;
            }
            // B's frame in A's 'into' slid along B's axis to the height z:
            // B's profile there is its solid's section through A's plane at
            // z, to within the square of its tilt -- the profile of a solid
            // extruded along that axis is the same at any height along it.
            // A tilted part's section moves across the plane with the
            // height, which is what holds the tilt.
            static ch::ChFramed sectionAt(const ch::ChFramed& into, double z)
            {
                const ch::ChVector3d axis = into.GetRotMat().GetAxisZ();
                if (!(std::abs(axis.z()) > 1e-9)) {
                    return into;
                }
                ch::ChFramed out = into;
                out.SetPos(into.GetPos() + axis * ((z - into.GetPos().z()) / axis.z()));
                return out;
            }
            // A point in the plane's terms to the load's, the bodies at a and
            // b, frame A's plane there fa
            static void put(const ch::ChFrame<>& a,
                            const ch::ChFrame<>& b,
                            const ch::ChFramed& fa,
                            double height,
                            V2 onA,
                            V2 onB,
                            V2 normal,
                            ContactLoad::Point& point)
            {
                point.onA = a.TransformPointParentToLocal(
                    fa.TransformPointLocalToParent({onA.x, onA.y, height}));
                point.onB = b.TransformPointParentToLocal(
                    fa.TransformPointLocalToParent({onB.x, onB.y, height}));
                point.normal = fa.TransformDirectionLocalToParent({normal.x, normal.y, 0.0});
            }
            // A point found again with the bodies at a and b, the way it was
            // found (ProfileFound): the arc's point farthest across the line
            // (farthestOnOrEnd()), the nearest on the line (nearestOnLine()),
            // the normal the line's outward one; or two arcs' on the line of
            // their centres, each on its side, the normal along it. Not
            // looked for again among
            // the other pieces within a substep: one leaving its pieces is
            // held at their ends, smoothly -- left behind as collision
            // detection found it, the force jumped within Newton's
            // iterations, and a Geneva drive's second turn ran off in steps of
            // nanoseconds.
            void place(const ch::ChFrame<>& bodyA,
                       const ch::ChFrame<>& bodyB,
                       ContactLoad::Point& point) const
            {
                using Kind = ProfileFound::Kind;
                const ProfileFound& found = point.found;
                if (found.kind == Kind::None) {
                    return;
                }
                const ch::ChFramed fa = frameA >> bodyA;
                const ch::ChFramed into = fa.TransformParentToLocal(frameB >> bodyB);
                const double h = height(into, found.level);
                const ch::ChFramed section = sectionAt(into, h);
                bool known = true;
                auto pieceA = [&](const void* key) {
                    for (const auto& p : profileA.pieces) {
                        if (p.key == key) {
                            return p;
                        }
                    }
                    known = false;
                    return Profile2::Piece {};
                };
                auto pieceB = [&](const void* key) {
                    return pieceIn(*static_cast<const ProfilePiece*>(key), section, mirrored);
                };
                if (found.kind == Kind::ArcArc) {
                    // Each on the line of the centres, on the side it was
                    // found
                    const Profile2::Piece p = pieceA(found.at);
                    const Profile2::Piece q = pieceB(found.across);
                    const V2 u = unit(q.centre - p.centre);
                    if (!known || norm(u) == 0.0) {
                        return;
                    }
                    put(bodyA,
                        bodyB,
                        fa,
                        h,
                        farthestOnOrEnd(p, u * found.sides[0]),
                        farthestOnOrEnd(q, u * found.sides[1]),
                        u * found.sides[2],
                        point);
                    return;
                }
                if (found.kind == Kind::CornerA || found.kind == Kind::CornerB) {
                    // The corner where it is; the nearest on the other's
                    // piece, a line's (nearestOnLine()), or an arc's on the
                    // line from its centre through the corner, each held at
                    // its ends, the normal out of its material there
                    const bool cornerA = found.kind == Kind::CornerA;
                    const V2 x = cornerA ? pieceA(found.at).from : pieceB(found.at).from;
                    const Profile2::Piece other =
                        cornerA ? pieceB(found.across) : pieceA(found.across);
                    if (!known) {
                        return;
                    }
                    V2 y;
                    V2 out;
                    if (other.sweep == 0.0) {
                        nearestOnLine(other, x, y, out);
                    }
                    else {
                        const V2 u = unit(x - other.centre);
                        if (norm(u) == 0.0) {
                            return;
                        }
                        y = farthestOnOrEnd(other, u);
                        out = other.sweep > 0.0 ? u : u * -1.0;
                    }
                    if (cornerA) {
                        put(bodyA, bodyB, fa, h, x, y, out * -1.0, point);
                    }
                    else {
                        put(bodyA, bodyB, fa, h, y, x, out, point);
                    }
                    return;
                }
                // The arc's point farthest across the line, into its side;
                // the nearest on the line, and its outward normal there
                const bool onA = found.kind == Kind::ArcA;
                const Profile2::Piece arc = onA ? pieceA(found.at) : pieceB(found.at);
                const Profile2::Piece line = onA ? pieceB(found.across) : pieceA(found.across);
                const V2 x = farthestOnOrEnd(arc, rightOf(unit(line.to - line.from)) * -1.0);
                V2 y;
                V2 out;
                nearestOnLine(line, x, y, out);
                if (!known) {
                    return;
                }
                if (onA) {
                    put(bodyA, bodyB, fa, h, x, y, out * -1.0, point);
                }
                else {
                    put(bodyA, bodyB, fa, h, y, x, out, point);
                }
            }
        };
        std::vector<ProfileFollow> profiles;
        // A round inside a hollow one, a pin in a bore, a ball in a socket,
        // on a load's two bodies, the inner one A's or B's: their points
        // are the rounds' own (hollowPoints()), Bullet's dropped
        struct RoundPair
        {
            ContactLoad* load = nullptr;
            const ShapeOn* inner = nullptr;
            const ShapeOn* hollow = nullptr;
            bool innerA = true;
            // The shapes they are of: the inner one's (itself, a
            // primitive), the hollow one's mesh, and its face there
            const ShapeOn* innerOf = nullptr;
            const ShapeOn* hollowOf = nullptr;
            int face = -1;
        };
        std::vector<RoundPair> roundPairs;
        // A ball on a load's body, a primitive's or a round face's, and a
        // bore too narrow for it on the other's, all round: its points on
        // the bore's rim are the rim's own (seatAt()), and Bullet's within
        // twice the rim's polygon's sagitta of it dropped -- on the bore's
        // facets and the face around the hole, they are on the polygon
        struct SeatPair
        {
            ContactLoad* load = nullptr;
            const ShapeOn* ball = nullptr;
            const ShapeOn* hollow = nullptr;
            bool ballA = true;
            const ShapeOn* ballOf = nullptr;
            const ShapeOn* hollowOf = nullptr;
            double sagitta = 0.0;
        };
        std::vector<SeatPair> seats;
        std::vector<HollowPoint> hollowAt;
        double envelope = 0.0;
        std::vector<ProfileTouch> touches;
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
            // Bullet's loads follow their points on a sphere or a cylinder,
            // and turn their normals with their faces (rollAlong()), when
            // they have one
            for (auto& [bodies, load] : finder->loads) {
                load->follow = nullptr;
            }
            // A point in the info's terms to its load's: the load's A first,
            // the normal from its A to its B
            auto handOver = [this](const Found& f,
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
                auto frameOf = [this](const ch::ChCollisionShape* shape) {
                    const ShapeOn* on = shapesOn.of(shape);
                    return on ? on->source : NoId;
                };
                load.points.back().frames = {frameOf(load.points.back().shapeA),
                                             frameOf(load.points.back().shapeB)};
                auto roundOf = [this](const ch::ChCollisionShape* shape) -> const ShapeOn* {
                    const ShapeOn* on = shapesOn.of(shape);
                    return on && (on->ball > 0.0 || on->radius > 0.0) ? on : nullptr;
                };
                ContactLoad::Point& point = load.points.back();
                point.roundA = roundOf(point.shapeA);
                point.roundB = roundOf(point.shapeB);
                // A hull's or a mesh's round face (roundOn())
                if (!point.roundA) {
                    if (const ShapeOn* on = shapesOn.of(point.shapeA)) {
                        point.roundA = roundOn(*on, point.shapeA, point.onA, point.normal);
                    }
                }
                if (!point.roundB) {
                    if (const ShapeOn* on = shapesOn.of(point.shapeB)) {
                        point.roundB = roundOn(*on, point.shapeB, point.onB, -point.normal);
                    }
                }
                if (point.roundA || point.roundB) {
                    point.face = point.roundB ? (point.roundA ? 0 : 1) : 2;
                }
                else {
                    const ShapeOn* a = shapesOn.of(point.shapeA);
                    const ShapeOn* b = shapesOn.of(point.shapeB);
                    if (b && hasFace(*b, point.shapeB, -point.normal)) {
                        point.face = 2;
                    }
                    else if (a && hasFace(*a, point.shapeA, point.normal)) {
                        point.face = 1;
                    }
                }
                if (point.face) {
                    const auto& body = point.face == 1 ? load.GetBodyA() : load.GetBodyB();
                    point.normalOn = body->TransformDirectionParentToLocal(point.normal);
                }
                if (point.roundA || point.roundB || point.face) {
                    load.follow = rollAll;
                }
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
            // A sphere's pair its one point, the other shape's nearest its
            // centre: Bullet's manifold keeps a rolling ball's older points
            // -- the ball's under one rolls off it without sliding, and
            // nothing breaks it -- each with the normal it had when found. A
            // ball on a plate tipping at 1 rad/s took more from 0.217 s on,
            // up to 0.9 mm from its point and their normals up to 5e-3 rad
            // old, and rode up to 4 um off the plate on them.
            std::vector<bool> drop(finder->found.size(), false);
            std::map<std::pair<const ch::ChCollisionShape*, const ch::ChCollisionShape*>,
                     std::pair<std::size_t, double>>
                nearest;
            for (std::size_t i = 0; i < finder->found.size(); ++i) {
                const Found& f = finder->found[i];
                const ShapeOn* a = on(f.shapeA);
                const ShapeOn* b = on(f.shapeB);
                const ShapeOn* ball = a && a->ball > 0.0 ? a : b && b->ball > 0.0 ? b : nullptr;
                if (!ball) {
                    continue;
                }
                const ch::ChVector3d centre = (ball->frame >> *ball->body).GetPos();
                const double distance = ((ball == a ? f.pB : f.pA) - centre).Length();
                auto [it, added] = nearest.try_emplace({f.shapeA, f.shapeB}, i, distance);
                if (added) {
                    continue;
                }
                if (distance < it->second.second) {
                    drop[it->second.first] = true;
                    it->second = {i, distance};
                }
                else {
                    drop[i] = true;
                }
            }
            for (std::size_t i = 0; i < finder->found.size(); ++i) {
                if (drop[i]) {
                    continue;
                }
                const Found& f = finder->found[i];
                const ShapeOn* a = on(f.shapeA);
                const ShapeOn* b = on(f.shapeB);
                // A mesh's triangle: the mesh it is of
                auto meshOf = [this](const ch::ChCollisionShape* shape) -> const ShapeOn* {
                    return shape && shape->GetType() == ch::ChCollisionShape::Type::MESHTRIANGLE
                        ? shapesOn.of(shape)
                        : nullptr;
                };
                const ShapeOn* meshA = meshOf(f.shapeA);
                const ShapeOn* meshB = meshOf(f.shapeB);
                const bool cylinderA = a && a->radius > 0.0 && ((b && b->features) || meshB);
                const bool cylinderB = b && b->radius > 0.0 && ((a && a->features) || meshA);
                if (!cylinderA && !cylinderB) {
                    handOver(f, f.pA, f.pB, f.normal);
                    continue;
                }
                if (done.count({f.shapeA, f.shapeB})) {
                    continue;
                }
                // Against a face of a box or a hull, or a mesh's triangle
                auto clip = [&](const ShapeOn& cylinder,
                                const ShapeOn* face,
                                const ShapeOn* mesh,
                                const ch::ChCollisionShape* shape,
                                const ch::ChVector3d& toOther) {
                    return mesh ? cylinderTrianglePoints(
                                      cylinder,
                                      *mesh,
                                      *static_cast<const ch::ChCollisionShapeMeshTriangle*>(shape),
                                      reach,
                                      points,
                                      normal)
                                : cylinderPoints(cylinder, *face, toOther, reach, points, normal);
                };
                const bool clipped = cylinderA ? clip(*a, b, meshB, f.shapeB, f.normal)
                                               : clip(*b, a, meshA, f.shapeA, -f.normal);
                if (!clipped) {
                    // A mesh's triangle the cylinder's segment is not over:
                    // its edge or corner nearest the axis, not Bullet's point
                    const ShapeOn* mesh = cylinderA ? meshB : meshA;
                    const int edge = mesh
                        ? cylinderTriangleEdge(
                              cylinderA ? *a : *b,
                              *mesh,
                              *static_cast<const ch::ChCollisionShapeMeshTriangle*>(
                                  cylinderA ? f.shapeB : f.shapeA),
                              reach,
                              points,
                              normal)
                        : -1;
                    if (edge < 0) {
                        handOver(f, f.pA, f.pB, f.normal);
                        continue;
                    }
                    if (edge == 0) {
                        done.insert({f.shapeA, f.shapeB});
                        continue;
                    }
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
            for (const RoundPair& pair : roundPairs) {
                if (!held(*pair.inner, *pair.hollow)) {
                    continue;
                }
                ContactLoad& load = *pair.load;
                const ShapeOn* onA = pair.innerA ? pair.inner : pair.hollow;
                const ShapeOn* onB = pair.innerA ? pair.hollow : pair.inner;
                // Bullet's between the two, should it find any (it leaves
                // them out, ContactCollisionSystem::inner): the inner shape's
                // against the hollow face's triangles, however their normal
                // leans -- one 18 deg off the bore's took no round, 75 um
                // deep on its facet
                auto ofHollow = [&](const ch::ChCollisionShape* shape) {
                    if (!shape || shapesOn.of(shape) != pair.hollowOf
                        || shape->GetType() != ch::ChCollisionShape::Type::MESHTRIANGLE) {
                        return false;
                    }
                    const auto* triangle =
                        static_cast<const ch::ChCollisionShapeMeshTriangle*>(shape);
                    return onRound(*pair.hollowOf,
                                   {triangle->V1, triangle->V2, triangle->V3},
                                   pair.face);
                };
                load.points.erase(
                    std::remove_if(
                        load.points.begin(),
                        load.points.end(),
                        [&](const ContactLoad::Point& point) {
                            const auto* inner = pair.innerA ? point.shapeA : point.shapeB;
                            const auto* hollow = pair.innerA ? point.shapeB : point.shapeA;
                            return (point.roundA == onA && point.roundB == onB)
                                || (shapesOn.of(inner) == pair.innerOf && ofHollow(hollow));
                        }),
                    load.points.end());
                hollowPoints(*pair.inner, *pair.hollow, reach, hollowAt);
                for (const HollowPoint& at : hollowAt) {
                    ContactLoad::Point point;
                    point.onA = load.GetBodyA()->TransformPointParentToLocal(
                        pair.innerA ? at.onInner : at.onHollow);
                    point.onB = load.GetBodyB()->TransformPointParentToLocal(
                        pair.innerA ? at.onHollow : at.onInner);
                    point.normal = pair.innerA ? at.out : -at.out;
                    point.roundA = onA;
                    point.roundB = onB;
                    point.frames = {onA->source, onB->source};
                    load.points.push_back(point);
                    load.follow = rollAll;
                }
            }
            for (const SeatPair& seat : seats) {
                ContactLoad& load = *seat.load;
                const ch::ChFramed fb = seat.ball->frame >> *seat.ball->body;
                const ch::ChFramed fh = seat.hollow->frame >> *seat.hollow->body;
                const ch::ChVector3d axis = fh.GetRotMat().GetAxisZ();
                const double along = ch::Vdot(axis, fb.GetPos() - fh.GetPos());
                if (!(std::abs(along) > seat.hollow->half)) {
                    continue;
                }
                const ch::ChVector3d end =
                    fh.GetPos() + axis * std::copysign(seat.hollow->half, along);
                const auto& rimBody = seat.ballA ? load.GetBodyB() : load.GetBodyA();
                load.points.erase(
                    std::remove_if(load.points.begin(),
                                   load.points.end(),
                                   [&](const ContactLoad::Point& point) {
                                       const auto* ball = seat.ballA ? point.shapeA : point.shapeB;
                                       const auto* rim = seat.ballA ? point.shapeB : point.shapeA;
                                       if (point.seat || !ball || !rim
                                           || shapesOn.of(ball) != seat.ballOf
                                           || shapesOn.of(rim) != seat.hollowOf) {
                                           return false;
                                       }
                                       ch::ChVector3d off = rimBody->TransformPointLocalToParent(
                                                                seat.ballA ? point.onB : point.onA)
                                           - end;
                                       const double height = ch::Vdot(axis, off);
                                       off -= axis * height;
                                       return std::hypot(off.Length() - seat.hollow->radius, height)
                                           <= 2.0 * seat.sagitta + 1e-12;
                                   }),
                    load.points.end());
                // A ring of 16 round the rim from its point nearest the
                // ball's centre: the ball centred on it touches it all round
                constexpr int ring = 16;
                for (int k = 0; k < ring; ++k) {
                    const double turn = 2.0 * pi * k / ring;
                    SeatPoint at;
                    if (!seatAt(*seat.ball, *seat.hollow, fb, fh, turn, at)
                        || ch::Vdot(at.onRim - at.onBall, at.out) > reach) {
                        continue;
                    }
                    ContactLoad::Point point;
                    point.onA = load.GetBodyA()->TransformPointParentToLocal(seat.ballA ? at.onBall
                                                                                        : at.onRim);
                    point.onB = load.GetBodyB()->TransformPointParentToLocal(
                        seat.ballA ? at.onRim : at.onBall);
                    point.normal = seat.ballA ? at.out : -at.out;
                    (seat.ballA ? point.roundA : point.roundB) = seat.ball;
                    point.seat = seat.hollow;
                    point.seatTurn = turn;
                    point.frames = seat.ballA
                        ? std::array<Id, 2> {seat.ball->source, seat.hollow->source}
                        : std::array<Id, 2> {seat.hollow->source, seat.ball->source};
                    load.points.push_back(point);
                    load.follow = rollAll;
                }
            }
            // Their points on round shapes put where they are now, and the
            // normals of the faces' (rollAlong()): the laws read them
            // (patchStiffness()), a hull's round face's exact where the
            // facet's are a deflection off
            std::set<ContactLoad*> rolled;
            for (auto& [bodies, load] : finder->loads) {
                if (load->follow && rolled.insert(load).second) {
                    rollAll(*load->GetBodyA(), *load->GetBodyB(), load->points);
                }
            }
            // A cam's one point, where it is or not (CamFollow::place()),
            // followed from the last: the curve's closest point to the
            // follower, as the joint's equations take it. Exact on the
            // curve, where Bullet would have a faceted cam's noise in the
            // follower's accelerations; and the load finds it again at each
            // state it is given within the substep (ContactLoad::follow)
            for (CamFollow& cam : cams) {
                ContactLoad& load = *cam.load;
                ContactLoad::Point point;
                const CurveProjection at =
                    cam.place(*load.GetBodyA(), *load.GetBodyB(), cam.u, !cam.started, point);
                cam.u = at.u;
                cam.started = true;
                cam.tangent = at.at.d1;
                cam.tangent.Normalize();
                point.frames = {cam.joint->frameI, cam.joint->frameJ};
                load.points.push_back(point);
            }
            // A profile contact's points: B's profile brought into A's
            // plane, where they touch (profileTouches()), at each of the
            // heights through where the solids overlap (levels()), B's
            // section there; how each was found kept, for the load to find
            // it again at each state it is given within the substep
            // (ProfileFollow::place())
            for (std::size_t k = 0; k < profiles.size(); ++k) {
                const ProfileFollow& follow = profiles[k];
                ContactLoad& load = *follow.load;
                const ch::ChFramed fa = follow.frameA >> *load.GetBodyA();
                const ch::ChFramed fb = follow.frameB >> *load.GetBodyB();
                const ch::ChFramed into = fa.TransformParentToLocal(fb);
                for (const double level : follow.levels(into)) {
                    const double height = follow.height(into, level);
                    const Profile2 profileB = profileIn(*follow.b,
                                                        ProfileFollow::sectionAt(into, height),
                                                        follow.mirrored);
                    profileTouches(follow.profileA, profileB, envelope, touches);
                    for (const ProfileTouch& touch : touches) {
                        ContactLoad::Point point;
                        ProfileFollow::put(*load.GetBodyA(),
                                           *load.GetBodyB(),
                                           fa,
                                           height,
                                           touch.onA,
                                           touch.onB,
                                           touch.normal,
                                           point);
                        point.piecesA = touch.piecesA;
                        point.piecesB = touch.piecesB;
                        point.found = touch.found;
                        point.found.pair = k;
                        point.found.level = level;
                        point.frames = {follow.a->frame, follow.b->frame};
                        load.points.push_back(point);
                    }
                }
            }
            for (auto& load : *loads) {
                patchesOf(*load);
                // A flexible part's points onto its surface first: the speed
                // a contact comes in at, and whether it presses, are the
                // surface's (patchStiffness())
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
                if (load->flexible) {
                    load->flexible->place(*load);
                }
#endif
                patchStiffness(*load, shapesOn);
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
                // And its forces at the points just found: the ones it has
                // are of the points before, by their numbers (forcesOnB), and
                // a step taken back from a contact found late has them of
                // the late points at the state before -- 7.6 kN on a strip of
                // 21 g found 0.1 mm in, read by the next step as its start's
                // (HHT's share of the old forces) with no point left
                if (load->flexible) {
                    load->ComputeQ(nullptr, nullptr);
                }
#endif
            }
        }
    };
    std::shared_ptr<ContactHandover> contactHandover;
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
        // A flexible part's carrier, touched anywhere on the part: its item
        auto anywhereOn = [&](const ch::ChBody* body) -> FlexibleContact* {
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
            for (const auto& flexible : run.d->flexibles) {
                if (flexible.anywhere && flexible.anywhere->carrier == body) {
                    return flexible.anywhere.get();
                }
            }
#else
            (void)body;
#endif
            return nullptr;
        };
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        // Each such part's surface, the one shape of its carrier
        std::map<FlexibleContact*, const ch::ChCollisionShape*> surfaceOf;
#endif
        for (std::size_t k = 0; k < si.dynamics->contactShapes.size(); ++k) {
            const ContactShape& shape = si.dynamics->contactShapes[k];
            // A profile is not Bullet's (ContactHandover's profiles)
            if (!run.d->validFrame(shape.frame) || shape.kind == ContactShape::Kind::Profile) {
                continue;
            }
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
            // A flexible part touched anywhere has its mesh's surface for
            // all its shapes, moving with its modes (FlexibleContact::mesh):
            // the shapes ride its carrier rigid, off the part once it has
            // bent. Its faces are the elements', flat each, a round one
            // facets as fine: for a round face's own law, name the faces
            // the part is touched on (ContactFaces)
            if (FlexibleContact* flexible = anywhereOn(run.d->bodyOf(shape.frame).get());
                flexible && flexible->mesh) {
                auto carrier = run.d->bodyOf(shape.frame);
                shapeBody[k] = carrier.get();
                const ch::ChCollisionShape*& surface = surfaceOf[flexible];
                if (!surface) {
                    auto whole = std::make_shared<ch::ChCollisionShapeTriangleMesh>(material,
                                                                                    flexible->mesh,
                                                                                    false,
                                                                                    false,
                                                                                    0.0);
                    carrier->AddCollisionShape(whole, ch::ChFramed());
                    carrier->GetCollisionModel()->SetEnvelope(envelope);
                    carrier->GetCollisionModel()->SetSafeMargin(margin);
                    carrier->EnableCollision(true);
                    surface = whole.get();
                    ShapeOn& where = (*shapesOn)[surface];
                    where.body = carrier.get();
                    where.frame = run.d->localOf(shape.frame);
                    where.source = shape.frame;
                    where.envelope = envelope;
                    where.modulus = shape.modulus;
                    const auto& vertices = flexible->mesh->GetCoordsVertices();
                    const auto& triangles = flexible->mesh->GetIndicesVertexes();
                    where.vertices = vertices.data();
                    where.count = vertices.size();
                    where.curvatures = meshCurvatures(vertices, triangles);
                    where.surfaces = meshSurfaces(vertices, triangles);
                }
                // The largest of its solids'
                ShapeOn& where = (*shapesOn)[surface];
                where.size = std::max(where.size,
                                      std::cbrt(3.0 * std::max(shape.volume, 0.0) / (4.0 * pi)));
                continue;
            }
#endif
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
            where.source = shape.frame;
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
                where.curvatures = meshCurvatures(where.hull, triangles);
            }
            where.modulus = shape.modulus;
            where.size = std::cbrt(3.0 * std::max(shape.volume, 0.0) / (4.0 * pi));
            if (meshMade) {
                const auto& vertices = meshMade->GetCoordsVertices();
                where.vertices = vertices.data();
                where.count = vertices.size();
                where.curvatures = meshCurvatures(vertices, meshMade->GetIndicesVertexes());
            }
            // A mesh's into its body's frame, as its vertices
            const ch::ChFramed into = meshMade ? run.d->localOf(shape.frame) : ch::ChFramed();
            for (const auto& faces : shape.surfaces) {
                auto& at = where.surfaces.emplace_back();
                for (const auto& face : faces) {
                    const ch::ChVector3d way1 =
                        into.TransformDirectionLocalToParent(toCh(face.way1));
                    const ch::ChVector3d way2 =
                        into.TransformDirectionLocalToParent(toCh(face.way2));
                    at.push_back({into.TransformDirectionLocalToParent(toCh(face.normal)),
                                  ch::TensorProduct(way1, way1) * face.k1
                                      + ch::TensorProduct(way2, way2) * face.k2,
                                  face.round});
                }
            }
            // Its rounds, on the body: in the shape's frame (where.frame),
            // a hull's points' and a mesh's before they were put on it
            for (const auto& round : shape.rounds) {
                ShapeOn& made = where.rounds.emplace_back();
                made.body = where.body;
                made.source = where.source;
                made.envelope = where.envelope;
                made.modulus = where.modulus;
                made.size = where.size;
                const ch::ChVector3d z = toCh(round.axis);
                const ch::ChVector3d x = toCh(round.way);
                ch::ChMatrix33d turn;
                turn.SetFromDirectionAxes(x, ch::Vcross(z, x), z);
                const double middle = round.sphere ? 0.0 : 0.5 * (round.from + round.to);
                made.frame = ch::ChFramed(toCh(round.centre) + z * middle, turn) >> where.frame;
                made.concave = round.concave;
                if (round.sphere) {
                    made.ball = round.radius;
                }
                else {
                    made.radius = round.radius;
                    made.half = 0.5 * (round.to - round.from);
                    made.angleFrom = round.angleFrom;
                    made.angleTo = round.angleTo;
                }
            }
        }
        auto finder = std::make_shared<ContactFinder>();
        bool anywhereClash = false;
        // A load per pair of bodies a set lets touch, the first set's law --
        // its friction, from the materials, its first two shapes' families'
        // pair's where the model has it (ContactLaw::FrictionFrom)
        auto lawOf = [&](std::size_t set, Id x, Id y) {
            ContactLaw law = si.dynamics->contacts[set].law;
            if (law.friction != ContactLaw::FrictionFrom::Materials
                || x >= si.dynamics->contactShapes.size()
                || y >= si.dynamics->contactShapes.size()) {
                return law;
            }
            const Id fx = si.dynamics->contactShapes[x].family;
            const Id fy = si.dynamics->contactShapes[y].family;
            if (fx == NoId || fy == NoId) {
                return law;
            }
            for (const FrictionPair& pair : si.dynamics->frictionPairs) {
                if ((pair.a == fx && pair.b == fy) || (pair.a == fy && pair.b == fx)) {
                    law.muStatic = pair.muStatic;
                    law.muDynamic = pair.muDynamic;
                    break;
                }
            }
            return law;
        };
        auto pairUp = [&](ch::ChBody* x, ch::ChBody* y, std::size_t set, Id sx, Id sy) {
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
                // A flexible part's interface (D4b)
                for (const auto& body : run.d->frameBody) {
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
            // A flexible part touched anywhere is B (FlexibleContact); two
            // such touching each other are not supported
            const bool swapped = anywhereOn(first.get()) != nullptr;
            if (swapped) {
                if (anywhereOn(second.get())) {
                    anywhereClash = true;
                    return;
                }
                std::swap(first, second);
            }
            auto load = std::make_shared<ContactLoad>(first, second, lawOf(set, sx, sy), set);
            load->swapped = swapped;
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
                        pairUp(bodyOf(pair.groupA[i]),
                               bodyOf(pair.groupA[j]),
                               set,
                               pair.groupA[i],
                               pair.groupA[j]);
                    }
                }
            }
            else {
                for (Id x : pair.groupA) {
                    for (Id y : pair.groupB) {
                        pairUp(bodyOf(x), bodyOf(y), set, x, y);
                    }
                }
            }
        }
        collision->RegisterNarrowphaseCallback(finder);
        auto handover = std::make_shared<ContactHandover>();
        contactHandover = handover;
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
        // The rounds inside hollow ones (RoundPair): on each load's bodies,
        // a convex round, a primitive's or a round face's, on one, a hollow
        // round face on the other
        {
            std::set<ContactLoad*> seen;
            // Each round with the shape it is of and its face's number
            struct Round
            {
                const ShapeOn* round;
                const ShapeOn* of;
                int face;
            };
            auto roundsOn = [&](const ch::ChBody* body, bool hollow) {
                std::vector<Round> out;
                for (const auto& [shape, on] : *shapesOn) {
                    if (on.body != body) {
                        continue;
                    }
                    if (!hollow && (on.ball > 0.0 || on.radius > 0.0)) {
                        out.push_back({&on, &on, -1});
                    }
                    for (std::size_t k = 0; k < on.rounds.size(); ++k) {
                        if (on.rounds[k].concave == hollow && (!hollow || on.vertices)) {
                            out.push_back({&on.rounds[k], &on, static_cast<int>(k)});
                        }
                    }
                }
                return out;
            };
            for (const auto& [bodies, load] : finder->loads) {
                if (!seen.insert(load).second) {
                    continue;
                }
                const ch::ChBody* a = load->GetBodyA().get();
                const ch::ChBody* b = load->GetBodyB().get();
                for (const bool innerA : {true, false}) {
                    for (const Round& inner : roundsOn(innerA ? a : b, false)) {
                        for (const Round& hollow : roundsOn(innerA ? b : a, true)) {
                            if (!fitsIn(*inner.round, *hollow.round)) {
                                if (inner.round->ball > 0.0 && hollow.round->radius > 0.0
                                    && hollow.round->angleTo - hollow.round->angleFrom
                                        >= 2.0 * pi - 1e-9) {
                                    handover->seats.push_back(
                                        {load,
                                         inner.round,
                                         hollow.round,
                                         innerA,
                                         inner.of,
                                         hollow.of,
                                         rimSagitta(*hollow.round, *hollow.of)});
                                }
                                continue;
                            }
                            handover->roundPairs.push_back({load,
                                                            inner.round,
                                                            hollow.round,
                                                            innerA,
                                                            inner.of,
                                                            hollow.of,
                                                            hollow.face});
                        }
                    }
                }
            }
        }
        // The cams that lift off: a load each between the cam's body and
        // the follower's, its one point from the curve at each collision
        // detection (ContactHandover), not Bullet's
        for (std::size_t set = 0; set < si.dynamics->contacts.size(); ++set) {
            const ContactPair& pair = si.dynamics->contacts[set];
            if (pair.cam == NoId) {
                continue;
            }
            const Joint* joint = nullptr;
            for (const auto& record : si.joints) {
                if (record.id == pair.cam) {
                    joint = &record;
                    break;
                }
            }
            if (!joint || joint->curve >= si.curves.size() || !si.curves[joint->curve].geometry
                || !run.d->validFrame(joint->frameI) || !run.d->validFrame(joint->frameJ)
                || run.d->isRigid(joint->frameI, joint->frameJ)) {
                continue;
            }
            auto cam = run.d->bodyOf(joint->frameI);
            auto follower = run.d->bodyOf(joint->frameJ);
            if (!cam || !follower || cam == follower || (cam->IsFixed() && follower->IsFixed())) {
                continue;
            }
            auto load = std::make_shared<ContactLoad>(cam, follower, pair.law, set);
            loads->Add(load);
            contactLoads.push_back(load);
            ContactHandover::CamFollow follow;
            follow.load = load.get();
            follow.joint = joint;
            follow.curve = si.curves[joint->curve].geometry;
            follow.frameI = run.d->localOf(joint->frameI);
            follow.frameJ = run.d->localOf(joint->frameJ);
            follow.offset = joint->value;
            follow.side = pair.side;
            handover->cams.push_back(std::move(follow));
            load->follow = [cams = &handover->cams, k = handover->cams.size() - 1](
                               const ch::ChFrame<>& a,
                               const ch::ChFrame<>& b,
                               std::vector<ContactLoad::Point>& points) {
                const auto& cam = (*cams)[k];
                cam.place(a, b, cam.u, false, points.front());
            };
        }
        // The profile contacts: a load per pair of bodies of each, and each
        // pair of faces on it, its points from the profiles at each
        // collision detection (ContactHandover), not Bullet's
        handover->envelope = envelope;
        auto isProfile = [&](Id shape) {
            return shape < si.dynamics->contactShapes.size()
                && si.dynamics->contactShapes[shape].kind == ContactShape::Kind::Profile;
        };
        for (std::size_t set = 0; set < si.dynamics->contacts.size(); ++set) {
            const ContactPair& pair = si.dynamics->contacts[set];
            if (pair.groupA.empty() || !isProfile(pair.groupA.front())) {
                continue;
            }
            std::map<std::pair<ch::ChBody*, ch::ChBody*>, ContactLoad*> made;
            for (Id x : pair.groupA) {
                for (Id y : pair.groupB) {
                    if (!isProfile(x) || !isProfile(y)) {
                        continue;
                    }
                    const ContactShape& a = si.dynamics->contactShapes[x];
                    const ContactShape& b = si.dynamics->contactShapes[y];
                    if (!run.d->validFrame(a.frame) || !run.d->validFrame(b.frame)
                        || run.d->isRigid(a.frame, b.frame)) {
                        continue;
                    }
                    auto bodyA = run.d->bodyOf(a.frame);
                    auto bodyB = run.d->bodyOf(b.frame);
                    if (!bodyA || !bodyB || bodyA == bodyB
                        || (bodyA->IsFixed() && bodyB->IsFixed())) {
                        continue;
                    }
                    ContactLoad*& load = made[{bodyA.get(), bodyB.get()}];
                    if (!load) {
                        auto shared = std::make_shared<ContactLoad>(bodyA, bodyB, pair.law, set);
                        loads->Add(shared);
                        contactLoads.push_back(shared);
                        load = shared.get();
                        load->follow = [profiles = &handover->profiles](
                                           const ch::ChFrame<>& a,
                                           const ch::ChFrame<>& b,
                                           std::vector<ContactLoad::Point>& points) {
                            for (auto& point : points) {
                                (*profiles)[point.found.pair].place(a, b, point);
                            }
                        };
                    }
                    ContactHandover::ProfileFollow follow;
                    follow.load = load;
                    follow.a = &a;
                    follow.b = &b;
                    follow.frameA = run.d->localOf(a.frame);
                    follow.frameB = run.d->localOf(b.frame);
                    follow.profileA = profileIn(a, ch::ChFramed(), false);
                    const ch::ChFramed fa = follow.frameA >> *bodyA;
                    const ch::ChFramed fb = follow.frameB >> *bodyB;
                    follow.mirrored = ch::Vdot(fa.GetRotMat().GetAxisZ(), fb.GetRotMat().GetAxisZ())
                        < 0.0;
                    handover->profiles.push_back(std::move(follow));
                }
            }
        }
        sys.RegisterCustomCollisionCallback(handover);
        // A side on a flexible part's contact interface moves the part's
        // mass, not the interface body's millionth of it (D4b step 5)
        for (const auto& load : contactLoads) {
            load->carried = {run.d->carriedMass(load->GetBodyA().get()),
                             run.d->carriedMass(load->GetBodyB().get())};
        }
        if (anywhereClash) {
            return refuse("two flexible parts touch each other with no faces either is touched "
                          "on: name one's, its ContactFaces");
        }
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        // A contact anywhere on a flexible part: its loads, and its block
        for (const auto& load : contactLoads) {
            if (FlexibleContact* flexible = anywhereOn(load->GetBodyB().get())) {
                load->flexible = flexible;
                flexible->loads.push_back(load.get());
            }
        }
        for (const auto& flexible : run.d->flexibles) {
            if (flexible.anywhere) {
                flexible.anywhere->setVariables();
            }
        }
#endif
        collision->Initialize();
        std::map<const ch::ChCollisionShape*, const cbtConvexPolyhedron*> features;
        std::set<ch::ChBody*> withShapes(shapeBody.begin(), shapeBody.end());
        for (ch::ChBody* body : withShapes) {
            if (body) {
                collision->addPolyhedralFeatures(*body->GetCollisionModel(), features);
            }
        }
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        // A flexible part's surface goes with its modes. A triangle's box in
        // its carrier's tree is five envelopes wider when it has left the
        // one it had: a part swinging on its modes leaves it once a swing,
        // not each step
        for (const auto& [flexible, surface] : surfaceOf) {
            collision->addMovingMesh(*flexible->carrier->GetCollisionModel(),
                                     surface,
                                     5.0 * envelope,
                                     [item = flexible]() {
                                         item->moveMesh();
                                     });
        }
#endif
        for (const auto& [shape, polyhedron] : features) {
            auto it = shapesOn->find(shape);
            if (it != shapesOn->end()) {
                it->second.features = polyhedron;
            }
        }
        // Bullet leaves out each round pair's inner shape against its
        // hollow face's triangles
        auto madeOf = [&](const ShapeOn* on) -> const std::vector<const cbtCollisionShape*>* {
            for (const auto& [shape, of] : *shapesOn) {
                if (&of == on) {
                    auto it = collision->made.find(shape);
                    return it == collision->made.end() ? nullptr : &it->second;
                }
            }
            return nullptr;
        };
        for (std::size_t k = 0; k < handover->roundPairs.size(); ++k) {
            const ContactHandover::RoundPair& pair = handover->roundPairs[k];
            const auto* inner = madeOf(pair.innerOf);
            const auto* hollow = madeOf(pair.hollowOf);
            if (!inner || !hollow) {
                continue;
            }
            for (const cbtCollisionShape* shape : *inner) {
                collision->inner[shape].push_back(k);
            }
            for (const cbtCollisionShape* shape : *hollow) {
                if (shape->getShapeType() != CH_TRIANGLE_SHAPE_PROXYTYPE) {
                    continue;
                }
                const auto* triangle = static_cast<const cbtCEtriangleShape*>(shape);
                if (onRound(*pair.hollowOf,
                            {triangle->get_p1(), triangle->get_p2(), triangle->get_p3()},
                            pair.face)) {
                    collision->hollow[shape].push_back(k);
                }
            }
        }
        collision->live.assign(handover->roundPairs.size(), 1);
        collision->refresh = [system = collision.get(), pairs = &handover->roundPairs]() {
            for (std::size_t k = 0; k < pairs->size(); ++k) {
                system->live[k] = held(*(*pairs)[k].inner, *(*pairs)[k].hollow) ? 1 : 0;
            }
        };
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
        run.d->syncFlexible();
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
        std::vector<std::size_t> frictions;  // its friction, by index in frictions
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
    // A friction is its joint's, the joint on its frames; and the joint's
    // links are what press it
    std::vector<std::vector<std::shared_ptr<ch::ChLinkBase>>> frictionLinks(frictions.size());
    for (std::size_t k = 0; k < frictions.size(); ++k) {
        const Friction& friction = frictions[k]->friction;
        for (auto& reaction : reactions) {
            bool mine = false;
            for (const auto& [joint, link] : reaction.links) {
                if (joint->frameI == friction.frameI && joint->frameJ == friction.frameJ) {
                    frictionLinks[k].push_back(link);
                    mine = true;
                }
            }
            if (mine) {
                reaction.frictions.push_back(k);
                break;
            }
        }
    }
    auto bodyName = [&](const ch::ChBody* body) {
        for (std::size_t b = 0; b < run.d->bodies.size() && b < si.bodies.size(); ++b) {
            if (run.d->bodies[b].get() == body) {
                return std::string(si.sourceName(si.bodies[b].source));
            }
        }
        return std::string();
    };
    // The component a frame is on (Frame::part): where a body stands for a
    // rigid group, the part of it that touched, not the group's first -- and
    // a part placed on the ground itself, not the ground; a frame not known,
    // the body's name
    auto partName = [&](Id frame, const ch::ChBody* body) {
        if (frame < si.frames.size()) {
            const Frame& f = si.frames[frame];
            if (f.part != NoId) {
                return std::string(si.sourceName(f.part));
            }
            if (f.body == NoId) {
                return std::string();
            }
            if (f.body < si.bodies.size()) {
                return std::string(si.sourceName(si.bodies[f.body].source));
            }
        }
        return bodyName(body);
    };
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
    for (const auto& load : appliedLoads) {
        const char* quantity = load.second->isTorque() ? "AppliedTorque" : "AppliedForce";
        channels.push_back({si.sourceName(load.first), quantity, 3});
    }
    // A set's parts, by the names the events give them: those its shapes
    // are on, the ground's none
    std::vector<std::vector<std::string>> setParts(si.dynamics->contacts.size());
    for (std::size_t s = 0; s < setParts.size(); ++s) {
        const ContactPair& pair = si.dynamics->contacts[s];
        for (const auto* group : {&pair.groupA, &pair.groupB}) {
            for (Id shape : *group) {
                if (shape >= si.dynamics->contactShapes.size()) {
                    continue;
                }
                const std::string part =
                    partName(si.dynamics->contactShapes[shape].frame, nullptr);
                auto& parts = setParts[s];
                if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end()) {
                    parts.push_back(part);
                }
            }
        }
    }
    for (std::size_t s = 0; s < setParts.size(); ++s) {
        const ContactPair& pair = si.dynamics->contacts[s];
        const std::string& name = si.sourceName(pair.source);
        channels.push_back({name, "ContactForce", 3});
        channels.push_back({name, "ContactNormalForce", 3});
        channels.push_back({name, "ContactFrictionForce", 3});
        channels.push_back({name, "ContactPenetration", 1});
        // A cam's: its pressure angle, its slip, and the force's parts along
        // the follower's motion and across it (sec 8.5); a profile
        // contact's the same, its B the follower
        if (pair.cam != NoId || isProfileSet(*si.dynamics, pair)) {
            channels.push_back({name, "CamPressureAngle", 1});
            channels.push_back({name, "CamSlip", 1});
            channels.push_back({name, "CamDriveForce", 3});
            channels.push_back({name, "CamSideForce", 3});
        }
        // On each of its parts: where it presses, for its loads (D4a)
        for (const std::string& part : setParts[s]) {
            channels.push_back({name, "PartContactForce", 3, true, part});
            channels.push_back({name, "PartContactPoint", 3, true, part});
            channels.push_back({name, "PartContactTorque", 3, true, part});
            channels.push_back(
                {name, "PartContactPlaces", static_cast<int>(10 * contactPlaces), true, part});
        }
    }
    for (const auto& load : frictions) {
        const std::string& name = si.sourceName(load->friction.source);
        if (load->friction.slides) {
            channels.push_back({name, "FrictionForce", 1});
        }
        if (load->friction.turns) {
            channels.push_back({name, "FrictionTorque", 1});
        }
    }
    // Each moving body's motion, its frame at its centre of mass: what its
    // inertia loads are made of (D4a, loads to FEM)
    std::vector<std::size_t> moving;
    // By another name where a run's own 'moving' hides it
    const std::vector<std::size_t>& movingBodies = moving;
    for (std::size_t b = 0; b < n; ++b) {
        if (!run.d->bodies[b]->IsFixed()) {
            const std::string& name = si.sourceName(si.bodies[b].source);
            channels.push_back({name, "Acceleration", 3});
            channels.push_back({name, "AngularVelocity", 3});
            channels.push_back({name, "AngularAcceleration", 3});
            moving.push_back(b);
        }
    }
    // Each flexible body's modal coordinates (D4b): what its modes add to
    // the deformation its interfaces make about its floating frame
    // and the deformation its interfaces make, six each (mm, rad), and its
    // inertia loads, the residual modes' coefficients (SI): together its
    // ModeShapes' coordinates
    for (const auto& flexible : run.d->flexibles) {
        const std::string& name = si.sourceName(si.dynamics->flexibles[flexible.record].source);
        channels.push_back({name,
                            "InterfaceDeformation",
                            static_cast<int>(6 * flexible.interfaces0.size())});
        channels.push_back({name,
                            "ModalCoordinates",
                            static_cast<int>(run.d->modalCoordinates(flexible).size())});
        const auto loads = run.d->inertialLoad(flexible);
        if (!loads.empty()) {
            channels.push_back({name, "InertialLoad", static_cast<int>(loads.size())});
        }
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
    auto stopping = [&](std::size_t k) {
        const Limit& limit = si.limits[k];
        return (!stopOf[k] || stopOf[k]->mode == StopRecord::Mode::Rigid)
            && run.d->validFrame(limit.frameI) && run.d->validFrame(limit.frameJ)
            && !run.d->isRigid(limit.frameI, limit.frameJ) && (limit.min || limit.max);
    };
    for (std::size_t k = 0; k < si.limits.size(); ++k) {
        if (stopping(k)) {
            stops[k].last = run.d->limitValue(si.limits[k], limitAngles);
        }
    }
    // A stop's own restitution, or the study's
    std::vector<double> restitutions(si.limits.size(), std::clamp(spec.restitution, 0.0, 1.0));
    for (std::size_t k = 0; k < si.limits.size(); ++k) {
        if (stopOf[k] && stopOf[k]->restitution >= 0.0) {
            restitutions[k] = std::clamp(stopOf[k]->restitution, 0.0, 1.0);
        }
    }
    const double gravity = si.dynamics->gravity.Length();
    // Each friction's load: its joint's reaction across its axis now, the
    // force on frame I's body less its part along the axis
    auto pressFrictions = [&]() {
        for (std::size_t k = 0; k < frictions.size(); ++k) {
            FrictionLoad& load = *frictions[k];
            const Id frameI = load.friction.frameI;
            const ch::ChFramed at = run.d->globalOf(frameI);
            ch::ChVector3d force;
            for (const auto& link : frictionLinks[k]) {
                force += wrenchOnI(frameI, link, at.GetPos()).force;
            }
            const ch::ChVector3d z = at.GetRotMat().GetAxisZ();
            load.normal = (force - z * ch::Vdot(force, z)).Length();
        }
    };
    auto handleStops = [&](double step, bool bounce) {
        // The frictions take the reactions the step ended with
        pressFrictions();
        // The penalty stops and the torsion springs count their turns from
        // here
        for (const auto& penalty : penalties) {
            if (penalty) {
                penalty->commit();
            }
        }
        for (const auto& torsion : torsions) {
            torsion.second->commit();
        }
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
            if (!stopping(k)) {
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
        // gravity gives in two steps, a resting part's bump. The projection's
        // impulse is one for the stops it hit: at the least restitution
        // among them, so that none gains energy
        double restitution = 1.0;
        for (std::size_t k : hit) {
            restitution = std::min(restitution, restitutions[k]);
        }
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

    // A cam's pressure angle and slip (sec 8.5), at the point its collision
    // detection last found, touching or not: the angle, rad, between the
    // contact's normal and the way the follower's point of contact moves --
    // its velocity, or where it stands still (a rise's top) its
    // acceleration, as an acute angle; the slip, m/s, the follower's
    // point's velocity from the cam's along the curve's tangent; and the
    // way, the follower's line of motion as a unit vector away from the cam
    // (at rest, the normal), along which the contact force is split
    struct CamReading
    {
        double pressure = 0.0;
        double slip = 0.0;
        ch::ChVector3d way;
    };
    // The angle and the way from the normal and the follower's point's
    // velocity v, or at a standstill its acceleration
    auto leaning = [](const ch::ChVector3d& normal,
                      const ch::ChVector3d& v,
                      const ch::ChVector3d& acceleration) {
        CamReading out;
        out.way = normal;
        ch::ChVector3d way = v.Length() < 1e-9 ? acceleration : v;
        if (way.Length() > 1e-12) {
            way.Normalize();
            const double c = ch::Vdot(normal, way);
            out.pressure = std::acos(std::min(1.0, std::abs(c)));
            out.way = c < 0.0 ? -way : way;
        }
        return out;
    };
    auto camReading = [&](std::size_t set) -> CamReading {
        if (!contactHandover) {
            return {};
        }
        for (const auto& cam : contactHandover->cams) {
            const ContactLoad& load = *cam.load;
            if (load.set != set || load.points.empty()) {
                continue;
            }
            const auto& point = load.points.front();
            const auto a = load.GetBodyA();
            const auto b = load.GetBodyB();
            const ch::ChVector3d v = b->PointSpeedLocalToParent(point.onB);
            CamReading out = leaning(point.normal.GetNormalized(),
                                     v,
                                     b->PointAccelerationLocalToParent(point.onB));
            const ch::ChFramed i = cam.frameI >> *a;
            const ch::ChVector3d tangent = i.TransformDirectionLocalToParent(toCh(cam.tangent));
            out.slip = ch::Vdot(v - a->PointSpeedLocalToParent(point.onA), tangent);
            return out;
        }
        return {};
    };
    // A profile contact's the same way, its B the follower, from the points
    // where they are now (pointsNow()): the normal the set's normal force's
    // on B, 'pushed', the follower's point's velocity and acceleration, and
    // A's, the pressing points' by their depths -- a touch is two points,
    // taken at two heights, and a flat on a flat two corners; touching none,
    // the nearest point's, its normal. The slip along A's outline, the way
    // it runs with A's face to its left.
    auto profileReading = [&](std::size_t set, const ch::ChVector3d& pushed) -> CamReading {
        if (!contactHandover) {
            return {};
        }
        double weights = 0.0;
        double nearest = -std::numeric_limits<double>::infinity();
        ch::ChVector3d v, acceleration, vA, normal, plane;
        ch::ChVector3d vNearest, accelerationNearest, vANearest, normalNearest;
        for (const auto& follow : contactHandover->profiles) {
            const ContactLoad& load = *follow.load;
            if (load.set != set) {
                continue;
            }
            const auto a = load.GetBodyA();
            const auto b = load.GetBodyB();
            if (plane == ch::VNULL) {
                plane = (follow.frameA >> *a).GetRotMat().GetAxisZ();
            }
            for (const auto& point : load.pointsNow()) {
                const double depth = -ch::Vdot(point.normal,
                                               b->TransformPointLocalToParent(point.onB)
                                                   - a->TransformPointLocalToParent(point.onA));
                const ch::ChVector3d speed = b->PointSpeedLocalToParent(point.onB);
                const ch::ChVector3d accel = b->PointAccelerationLocalToParent(point.onB);
                const ch::ChVector3d speedA = a->PointSpeedLocalToParent(point.onA);
                if (depth > 0.0) {
                    weights += depth;
                    v += speed * depth;
                    acceleration += accel * depth;
                    vA += speedA * depth;
                }
                if (depth > nearest) {
                    nearest = depth;
                    vNearest = speed;
                    accelerationNearest = accel;
                    vANearest = speedA;
                    normalNearest = point.normal;
                }
            }
        }
        if (!std::isfinite(nearest)) {
            return {};
        }
        if (weights > 0.0 && pushed.Length() > 0.0) {
            v /= weights;
            acceleration /= weights;
            vA /= weights;
            normal = pushed.GetNormalized();
        }
        else {
            v = vNearest;
            acceleration = accelerationNearest;
            vA = vANearest;
            normal = normalNearest.GetNormalized();
        }
        CamReading out = leaning(normal, v, acceleration);
        out.slip = ch::Vdot(v - vA, ch::Vcross(plane, normal).GetNormalized());
        return out;
    };

    // A moving body's accelerations from the forces on it, R's rows of its
    // (the loads' and the constraints' Cq' L), its turn 'turn': its centre's
    // (model units) and its angular one
    auto motionFrom = [&](const ch::ChVectorDynamic<>& R,
                          std::size_t b,
                          const ch::ChMatrix33<>& turn) {
        const auto& body = run.d->bodies[b];
        // A flexible body's: its floating frame's, carried to its frame
        if (run.d->isFlexible(b)) {
            run.d->syncFlexible();
            return std::pair<ch::ChVector3d, ch::ChVector3d>(body->GetPosDt2() / SI::acceleration,
                                                             body->GetAngAccParent());
        }
        const unsigned int at = body->GetOffset_w();
        const ch::ChVector3d force(R(at), R(at + 1), R(at + 2));
        const ch::ChVector3d torque(R(at + 3), R(at + 4), R(at + 5));
        const double mass = body->GetMass();
        const ch::ChVector3d a = mass > 0.0 ? force / mass / SI::acceleration : ch::VNULL;
        const ch::ChVector3d alpha = turn * (body->GetInvInertia() * torque);
        return std::pair<ch::ChVector3d, ch::ChVector3d>(a, alpha);
    };
    // The constraints' reactions L at the state as it stands, from its own
    // forces: [M Cq'; Cq 0] [a; -L] = [F; -g], g the constraints'
    // acceleration with none -- a motion's own, the velocities' turning --
    // C's second difference along the velocities over 'd'. And R = F + Cq'
    // L there, motionFrom's: Cq is the state's own. The system's
    // accelerations and reactions are left as they were: HHT starts from
    // them, and a run's frames are as without
    auto solveReactions = [&](double d, ch::ChVectorDynamic<>& L, ch::ChVectorDynamic<>& R) {
        const auto nx = sys.GetNumCoordsPosLevel();
        const auto nv = sys.GetNumCoordsVelLevel();
        const auto nc = sys.GetNumConstraints();
        ch::ChState x(nx, &sys);
        ch::ChStateDelta v(nv, &sys);
        double t = 0.0;
        sys.StateGather(x, v, t);
        ch::ChStateDelta accelerationWas(nv, &sys);
        sys.StateGatherAcceleration(accelerationWas);
        ch::ChVectorDynamic<> reactionsWas(nc);
        reactionsWas.setZero();
        sys.StateGatherReactions(reactionsWas);
        ch::ChVectorDynamic<> g(nc);
        g.setZero();
        sys.LoadConstraint_C(g, -2.0 / (d * d));
        ch::ChState moved(nx, &sys);
        for (const double s : {d, -d}) {
            const ch::ChStateDelta dx(v * s, &sys);
            sys.StateIncrementX(moved, x, dx);
            sys.StateScatter(moved, v, t + s, ch::UpdateFlags::UPDATE_ALL_NO_VISUAL);
            sys.LoadConstraint_C(g, 1.0 / (d * d));
        }
        sys.StateScatter(x, v, t, ch::UpdateFlags::UPDATE_ALL_NO_VISUAL);
        R.setZero(nv);
        sys.LoadResidual_F(R, 1.0);
        ch::ChStateDelta a(nv, &sys);
        a.setZero(nv, &sys);
        L.setZero(nc);
        const bool ok = sys.StateSolveCorrection(a,
                                                 L,
                                                 R,
                                                 g,
                                                 1.0,
                                                 0.0,
                                                 0.0,
                                                 x,
                                                 v,
                                                 t,
                                                 false,
                                                 ch::UpdateFlags::UPDATE_ALL_NO_VISUAL,
                                                 true,
                                                 true);
        sys.StateScatterAcceleration(accelerationWas);
        sys.StateScatterReactions(reactionsWas);
        sys.LoadResidual_CqL(R, L, 1.0);
        return ok && L.allFinite();
    };
    // The start's values: a flexible body's inertia loads without its
    // floating frame's accelerations (inertialLoad())
    bool atStart = false;
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
                if (penalties[k]) {
                    const ch::ChWrenchd w = penalties[k]->wrenchNow();
                    force += w.force;
                    torque += w.torque + ch::Vcross(penalties[k]->pointNow() - about, w.force);
                }
            }
            // Its friction is part of what holds it
            for (std::size_t k : reaction.frictions) {
                const ch::ChWrenchd w = frictions[k]->wrenchNow();
                force += w.force;
                torque += w.torque + ch::Vcross(frictions[k]->pointNow() - about, w.force);
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
            out.push_back(torsion.second->torqueNow() / SI::torque);
            out.push_back(torsion.second->angleNow());
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
        for (const auto& load : appliedLoads) {
            const ch::ChVector3d v = load.second->valueNow();
            const double unit = load.second->isTorque() ? SI::torque : SI::force;
            for (int k = 0; k < 3; ++k) {
                out.push_back(v[k] / unit);
            }
        }
        // A set's: the force its group A's bodies take, each load's A
        // being the A side (a set within A: its first body), that force's
        // normal and friction parts, and the deepest point's depth
        const std::size_t sets = si.dynamics->contacts.size();
        std::vector<ch::ChVector3d> onSet(sets);
        std::vector<ch::ChVector3d> normalOnSet(sets);
        std::vector<double> deepest(sets, 0.0);
        // And on each of its parts, in all and at each place it presses (a
        // patch of a load, its pair of bodies): the force, its points
        // weighted by the sizes of their forces and the sum of those, the
        // moment about the origin, and the points
        struct Place
        {
            const ContactLoad* load = nullptr;
            std::size_t patch = 0;
            ch::ChVector3d force;
            ch::ChVector3d weighted;
            double weight = 0.0;
            ch::ChVector3d moment;
            std::vector<ch::ChVector3d> points;
            void add(const ch::ChVector3d& f, const ch::ChVector3d& at)
            {
                force += f;
                weighted += at * f.Length();
                weight += f.Length();
                moment += ch::Vcross(at, f);
                points.push_back(at);
            }
            void add(const Place& other)
            {
                force += other.force;
                weighted += other.weighted;
                weight += other.weight;
                moment += other.moment;
                points.insert(points.end(), other.points.begin(), other.points.end());
            }
            ch::ChVector3d centre() const
            {
                return weight > 0.0 ? weighted / weight : ch::VNULL;
            }
        };
        struct OnPart
        {
            Place total;
            std::vector<Place> places;
        };
        std::vector<std::vector<OnPart>> onParts(sets);
        for (std::size_t s = 0; s < sets; ++s) {
            onParts[s].resize(setParts[s].size());
        }
        auto addOn = [&](std::size_t s,
                         const std::string& part,
                         const ch::ChVector3d& f,
                         const ch::ChVector3d& at,
                         const ContactLoad* load,
                         std::size_t patch) {
            const auto& parts = setParts[s];
            const auto found = std::find(parts.begin(), parts.end(), part);
            if (found == parts.end()) {
                return;
            }
            OnPart& on = onParts[s][found - parts.begin()];
            on.total.add(f, at);
            auto place = std::find_if(on.places.begin(), on.places.end(), [&](const Place& p) {
                return p.load == load && p.patch == patch;
            });
            if (place == on.places.end()) {
                on.places.push_back({load, patch});
                place = on.places.end() - 1;
            }
            place->add(f, at);
        };
        for (const auto& load : contactLoads) {
            load->keep = true;
            load->ComputeQ(nullptr, nullptr);
            load->keep = false;
            const double side = load->swapped ? -1.0 : 1.0;
            onSet[load->set] -= side * load->onB;
            normalOnSet[load->set] -= side * load->normalOnB;
            deepest[load->set] = std::max(deepest[load->set], load->deepest);
            // f on B at its point there, -f on A at A's
            for (const auto& point : load->pressed) {
                addOn(load->set,
                      partName(point.frames[1], load->GetBodyB().get()),
                      point.force,
                      point.onB,
                      load.get(),
                      point.patch);
                addOn(load->set,
                      partName(point.frames[0], load->GetBodyA().get()),
                      -point.force,
                      point.onA,
                      load.get(),
                      point.patch);
            }
        }
        for (std::size_t s = 0; s < sets; ++s) {
            const ch::ChVector3d friction = onSet[s] - normalOnSet[s];
            for (const ch::ChVector3d& force : {onSet[s], normalOnSet[s], friction}) {
                for (int k = 0; k < 3; ++k) {
                    out.push_back(force[k] / SI::force);
                }
            }
            out.push_back(deepest[s] / SI::length);
            const ContactPair& pair = si.dynamics->contacts[s];
            const bool profile = isProfileSet(*si.dynamics, pair);
            if (pair.cam != NoId || profile) {
                const CamReading cam = profile ? profileReading(s, -normalOnSet[s]) : camReading(s);
                out.push_back(cam.pressure);
                out.push_back(cam.slip / SI::velocity);
                // The set's force split along the follower's line of motion
                // and across it, the side load its guide takes
                const ch::ChVector3d drive = cam.way * ch::Vdot(onSet[s], cam.way);
                for (const ch::ChVector3d& force : {drive, onSet[s] - drive}) {
                    for (int k = 0; k < 3; ++k) {
                        out.push_back(force[k] / SI::force);
                    }
                }
            }
            // The resultant on each part at the centre of its points
            // weighted by their forces, and the torque about it
            // And the places it presses each part at, the most pressed first:
            // each's force at its centre, the torque about it and how far
            // its points reach from it; past the slots, the rest as one
            for (OnPart& on : onParts[s]) {
                const Place& total = on.total;
                const ch::ChVector3d centre = total.centre();
                const ch::ChVector3d torque = total.moment - ch::Vcross(centre, total.force);
                for (int k = 0; k < 3; ++k) {
                    out.push_back(total.force[k] / SI::force);
                }
                for (int k = 0; k < 3; ++k) {
                    out.push_back(centre[k] / SI::length);
                }
                for (int k = 0; k < 3; ++k) {
                    out.push_back(torque[k] / SI::torque);
                }
                std::ranges::stable_sort(on.places, std::greater<>(), [](const Place& p) {
                    return p.force.Length();
                });
                std::vector<Place> slots(contactPlaces);
                for (std::size_t i = 0; i < on.places.size(); ++i) {
                    slots[std::min(i, contactPlaces - 1)].add(on.places[i]);
                }
                for (const Place& slot : slots) {
                    const ch::ChVector3d c = slot.centre();
                    const ch::ChVector3d t = slot.moment - ch::Vcross(c, slot.force);
                    double reach = 0.0;
                    for (const ch::ChVector3d& p : slot.points) {
                        reach = std::max(reach, (p - c).Length());
                    }
                    for (int k = 0; k < 3; ++k) {
                        out.push_back(slot.force[k] / SI::force);
                    }
                    for (int k = 0; k < 3; ++k) {
                        out.push_back(c[k] / SI::length);
                    }
                    for (int k = 0; k < 3; ++k) {
                        out.push_back(t[k] / SI::torque);
                    }
                    out.push_back(reach / SI::length);
                }
            }
        }
        // A friction's, on frame I's body: along its joint's axis, about it
        for (const auto& load : frictions) {
            const ch::ChWrenchd w = load->wrenchNow();
            const ch::ChVector3d z = load->axisNow();
            if (load->friction.slides) {
                out.push_back(ch::Vdot(w.force, z) / SI::force);
            }
            if (load->friction.turns) {
                out.push_back(ch::Vdot(w.torque, z) / SI::torque);
            }
        }
        // A body's accelerations from the forces on it, M^-1 (f + Cq' L):
        // those the reactions reported balance. The integrator's own are
        // HHT's, the forces blended across the step, its centre's off by up
        // to 0.8% of g on a falling pendulum whose reactions were within
        // 1e-4 (sec 4's D4a)
        if (!moving.empty()) {
            const unsigned int nv = sys.GetNumCoordsVelLevel();
            ch::ChVectorDynamic<> L(sys.GetNumConstraints());
            sys.StateGatherReactions(L);
            ch::ChVectorDynamic<> R(nv);
            R.setZero();
            sys.LoadResidual_F(R, 1.0);
            sys.LoadResidual_CqL(R, L, 1.0);
            for (std::size_t b : moving) {
                const auto [a, alpha] = motionFrom(R, b, run.d->bodies[b]->GetRotMat());
                for (const ch::ChVector3d& v : {a, run.d->bodies[b]->GetAngVelParent(), alpha}) {
                    for (int k = 0; k < 3; ++k) {
                        out.push_back(v[k]);
                    }
                }
            }
        }
        for (const auto& flexible : run.d->flexibles) {
            for (double e : run.d->interfaceDeformation(flexible, SI::length)) {
                out.push_back(e);
            }
            for (double q : run.d->modalCoordinates(flexible)) {
                out.push_back(q);
            }
            for (double c : run.d->inertialLoad(flexible, atStart)) {
                out.push_back(c);
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
                for (const auto& torsion : torsions) {
                    torsion.second->commit();
                }
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
    for (const auto& flexible : run.d->flexibles) {
        ModeShapes shapes = run.d->modeShapesOf(
            flexible,
            si.sourceName(si.dynamics->flexibles[flexible.record].source),
            SI::length);
        // And its reduction, for the document to keep
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
        shapes.reduction = ChronoFlexible::reductionNumbers(flexible.built, flexible.hz);
#endif
        sink.modeShapes(shapes);
    }

    // The events found (sec 8.4), held until their step is kept -- a
    // sensor's tries take theirs back -- and sent in time order before the
    // frame that follows them. A contact let go has its speed read again
    // after the next substep (noteTouch()), 'reading' its load until then,
    // and holds back what comes after it to the next frame, or the run's
    // last.
    struct Queued
    {
        Event event;
        ContactLoad* reading = nullptr;
        bool seen = false;  // by the sensors on events (eventTrips())
    };
    std::vector<Queued> queued;
    auto sendEvents = [&](bool all) {
        std::ranges::stable_sort(queued, {}, [](const Queued& q) {
            return q.event.time;
        });
        std::size_t n = 0;
        for (; n < queued.size() && (all || !queued[n].reading); ++n) {
            sink.event(queued[n].event);
        }
        queued.erase(queued.begin(), queued.begin() + static_cast<std::ptrdiff_t>(n));
    };

    // The sensors (sec 5.2, P15): each on its channel's components, in
    // valuesNow()'s values, and what it read last. It trips where its value
    // passed its threshold since the last reading; it acts on its count'th
    // pass, 'passes' those kept so far.
    struct Watch
    {
        const Sensor* sensor = nullptr;
        std::size_t offset = 0;
        int width = 1;
        double last = 0.0;
        int passes = 0;
    };
    std::vector<Watch> watches;
    // And those on a contact set's events, a contact made or let go: the
    // names of its pair's bodies, as the events name them, and the events it
    // matched in the steps kept
    struct EventWatch
    {
        const Sensor* sensor = nullptr;
        std::vector<std::string> pair;
        int passes = 0;
    };
    std::vector<EventWatch> eventWatches;
    {
        std::vector<std::size_t> offsets;
        std::size_t offset = 0;
        for (const auto& channel : channels) {
            offsets.push_back(offset);
            offset += static_cast<std::size_t>(channel.width);
        }
        for (const auto& sensor : si.dynamics->sensors) {
            const std::string watched = si.sourceName(sensor.watched);
            if (sensor.quantity == "ContactBegin" || sensor.quantity == "ContactEnd") {
                const bool set = std::ranges::any_of(si.dynamics->contacts, [&](const auto& c) {
                    return si.sourceName(c.source) == watched;
                });
                if (!set || spec.analysis == Analysis::Static) {
                    Base::Console().warning(
                        "Assembly: the sensor %s watches the %s events of %s, which the run "
                        "does not report%s; it does nothing\n",
                        si.sourceName(sensor.source).c_str(),
                        sensor.quantity.c_str(),
                        watched.c_str(),
                        set ? " (a Static run has none)" : "");
                    continue;
                }
                // Its parts by the names the events give them (partName())
                EventWatch watch {&sensor};
                for (const Id frame : sensor.frames) {
                    std::string name;
                    if (frame < si.frames.size()) {
                        const Frame& f = si.frames[frame];
                        if (f.part != NoId) {
                            name = si.sourceName(f.part);
                        }
                        else if (f.body != NoId && f.body < si.bodies.size()) {
                            name = si.sourceName(si.bodies[f.body].source);
                        }
                    }
                    watch.pair.push_back(name);
                }
                eventWatches.push_back(std::move(watch));
                continue;
            }
            std::size_t c = 0;
            while (c < channels.size()
                   && (channels[c].source != watched || channels[c].quantity != sensor.quantity)) {
                ++c;
            }
            if (c == channels.size()) {
                Base::Console().warning(
                    "Assembly: the sensor %s watches the %s of %s, which the run does not "
                    "report; it does nothing\n",
                    si.sourceName(sensor.source).c_str(),
                    sensor.quantity.c_str(),
                    watched.c_str());
                continue;
            }
            watches.push_back({&sensor, offsets[c], channels[c].width});
        }
    }
    auto reading = [](const Watch& watch, const std::vector<double>& values) {
        const double* v = &values[watch.offset];
        if (watch.width == 1) {
            return v[0];
        }
        if (watch.sensor->component >= 0 && watch.sensor->component < watch.width) {
            return v[watch.sensor->component];
        }
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    };
    // Whether the value passed the sensor's threshold from 'last' to 'now':
    // rising above it, falling below it, or crossing it either way
    auto passed = [](const Sensor& sensor, double last, double now) {
        if (sensor.crosses) {
            return (now > sensor.value) != (last > sensor.value);
        }
        if (sensor.below) {
            return now < sensor.value && !(last < sensor.value);
        }
        return now > sensor.value && !(last > sensor.value);
    };
    // A pass it acts on: its count'th, and for a mark each after -- or, one
    // that repeats, every count'th
    auto acts = [](const Sensor& sensor, int passes) {
        if (sensor.stop) {
            return passes == sensor.count;
        }
        return sensor.repeat ? passes % sensor.count == 0 : passes >= sensor.count;
    };
    // The sensors that trip at these values, the readings kept: an event
    // each that acts, sent; whether one stops the run
    auto tripsAt = [&](double time, const std::vector<double>& values) {
        bool stop = false;
        for (auto& watch : watches) {
            const double now = reading(watch, values);
            if (passed(*watch.sensor, watch.last, now) && acts(*watch.sensor, ++watch.passes)) {
                stop = stop || watch.sensor->stop;
                queued.push_back(
                    {Event {time, si.sourceName(watch.sensor->source), now, watch.sensor->stop}});
            }
            watch.last = now;
        }
        return stop;
    };
    // Whether a sensor passes, on a pass it acts on: the step is then
    // bisected to the time
    auto anyTrips = [&](const std::vector<double>& values) {
        return std::ranges::any_of(watches, [&](const Watch& watch) {
            return passed(*watch.sensor, watch.last, reading(watch, values))
                && acts(*watch.sensor, watch.passes + 1);
        });
    };
    // The readings to watch from, as the run starts
    auto watchFrom = [&](const std::vector<double>& values) {
        for (auto& watch : watches) {
            watch.last = reading(watch, values);
        }
    };
    // A step kept where no sensor acts: its passes counted
    auto passBy = [&](const std::vector<double>& values) {
        for (auto& watch : watches) {
            const double now = reading(watch, values);
            if (passed(*watch.sensor, watch.last, now)) {
                ++watch.passes;
            }
            watch.last = now;
        }
    };
    // A sensor on a contact set's events trips at each contact made, or let
    // go, of its pair, coming in or leaving at a speed at or above its
    // threshold -- or, below, under it
    auto eventMatches = [&](const EventWatch& watch, const Event& event) {
        const Sensor& sensor = *watch.sensor;
        const Event::Kind kind =
            sensor.quantity == "ContactBegin" ? Event::Kind::ContactBegin : Event::Kind::ContactEnd;
        if (event.kind != kind || event.source != si.sourceName(sensor.watched)
            || (sensor.below ? !(event.value < sensor.value) : event.value < sensor.value)) {
            return false;
        }
        const auto& pair = watch.pair;
        const auto in = [&](const std::string& name) {
            return name == event.bodyA || name == event.bodyB;
        };
        if (pair.size() == 2) {
            return (pair[0] == event.bodyA && pair[1] == event.bodyB)
                || (pair[0] == event.bodyB && pair[1] == event.bodyA);
        }
        return pair.empty() || in(pair[0]);
    };
    // Whether the events not yet seen trip a sensor that stops, on the pass
    // it acts on: one let go whose speed is still to be read again
    // (readLeaving()) on its first reading, so the stop is located where it
    // let go
    auto eventStops = [&]() {
        return std::ranges::any_of(eventWatches, [&](const EventWatch& watch) {
            if (!watch.sensor->stop) {
                return false;
            }
            const auto matched = std::ranges::count_if(queued, [&](const Queued& q) {
                return !q.seen && q.event.kind != Event::Kind::Sensor
                    && eventMatches(watch, q.event);
            });
            return matched > 0 && watch.passes + matched >= watch.sensor->count;
        });
    };
    // The events not yet seen, their speeds read, in time order: an event
    // each sensor they trip on a pass it acts on, at the contact's time;
    // whether one stops the run
    auto eventTrips = [&]() {
        bool stop = false;
        std::vector<Event> trips;
        std::vector<Queued*> unseen;
        for (auto& q : queued) {
            if (!q.seen && !q.reading && q.event.kind != Event::Kind::Sensor) {
                unseen.push_back(&q);
            }
        }
        std::ranges::stable_sort(unseen, {}, [](const Queued* q) {
            return q->event.time;
        });
        for (Queued* q : unseen) {
            q->seen = true;
            for (auto& watch : eventWatches) {
                const Sensor& sensor = *watch.sensor;
                if (eventMatches(watch, q->event) && acts(sensor, ++watch.passes)) {
                    stop = stop || sensor.stop;
                    trips.push_back(
                        {q->event.time, si.sourceName(sensor.source), q->event.value, sensor.stop});
                }
            }
        }
        for (auto& event : trips) {
            queued.push_back({std::move(event)});
        }
        return stop;
    };

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
    // jumping 0.2 mm. A part resting on another is limited by neither on the
    // default law: its period is milliseconds, its penetration a fraction of
    // the damping's. On a metal's it is microseconds, and the period is for
    // the impacts: a load at rest (advance()), unless strict, keeps only the
    // damping's limit -- HHT, implicit and given the loads' jacobians, damps
    // the period it steps over, as the codes stepping by their error do
    // (RecurDyn, Adams: smaller steps only about an impact).
    auto contactStep = [&](bool strict) {
        double shortest = std::numeric_limits<double>::infinity();
        double damped = std::numeric_limits<double>::infinity();
        double arrival = std::numeric_limits<double>::infinity();
        for (const auto& load : contactLoads) {
            load->period = std::numeric_limits<double>::infinity();
            if (load->points.empty()) {
                continue;
            }
            const auto a = load->GetBodyA();
            const auto b = load->GetBodyB();
            double mass = load->lighterMass();
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
            for (std::size_t i = 0; i < load->points.size(); ++i) {
                const auto& point = load->points[i];
                const ch::ChVector3d pA = a->TransformPointLocalToParent(point.onA);
                const ch::ChVector3d pB = b->TransformPointLocalToParent(point.onB);
                const double depth = -ch::Vdot(point.normal, pB - pA);
                const double rate = -ch::Vdot(point.normal,
                                              load->speedOnB(i, point)
                                                  - a->PointSpeedLocalToParent(point.onA));
                const double approach = std::max(rate, 0.0);
                const double k = load->stiffness[point.patch];
                const double e = load->exponents[point.patch];
                const double reach = std::pow((e + 1.0) * mass * approach * approach / (2.0 * k),
                                              1.0 / (e + 1.0));
                const double deepest = std::max({depth, reach, 1e-12});
                const double stiffness = e * k * std::pow(deepest, e - 1.0);
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
                        if (strict || !load->settled) {
                            arrival = std::min(arrival, 0.5 * toContact);
                        }
                        continue;
                    }
                }
                load->period = std::min(load->period, period);
                if (strict || !load->settled) {
                    shortest = std::min(shortest, period);
                }
                const double damping = law.damping == ContactLaw::Damping::Restitution
                    ? k * std::pow(deepest, e) * load->hunt(std::max(depth, 0.0), point.patch)
                    : step(deepest, 0.0, 0.0, law.penetration, law.maxDamping);
                if (damping > 0.0 && (strict || !load->settled)) {
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
    // The same, with how fast that point comes in, m/s, and how fast that
    // grows, m/s^2
    struct Touch
    {
        double depth = -std::numeric_limits<double>::infinity();
        double rate = 0.0;
        double accel = 0.0;
        std::array<Id, 2> frames {NoId, NoId};  // its shapes' (Point::frames)
    };
    // A body's frame dt on along its motion now: its place to the second
    // order, its velocities to the first
    auto ahead = [](const ch::ChBody& body, double dt) {
        const ch::ChVector3d v = body.GetPosDt();
        const ch::ChVector3d acc = body.GetPosDt2();
        const ch::ChVector3d w = body.GetAngVelParent();
        const ch::ChVector3d alpha = body.GetAngAccParent();
        ch::ChQuaterniond turn;
        turn.SetFromRotVec(w * dt + alpha * (0.5 * dt * dt));
        ch::ChFrameMoving<> frame;
        frame.SetPos(body.GetPos() + v * dt + acc * (0.5 * dt * dt));
        frame.SetRot(turn * body.GetRot());
        frame.SetPosDt(v + acc * dt);
        frame.SetAngVelParent(w + alpha * dt);
        return frame;
    };
    // How fast a point comes in, m/s, the bodies at a and b
    auto rateOf = [](const ContactLoad::Point& point,
                     const ch::ChFrameMoving<>& a,
                     const ch::ChFrameMoving<>& b) {
        return -ch::Vdot(point.normal,
                         b.PointSpeedLocalToParent(point.onB)
                             - a.PointSpeedLocalToParent(point.onA));
    };
    // The deepest point's growth in speed, where the load follows its
    // points (a cam's, a profile contact's), is its speed's own: taken a
    // microsecond either side along the bodies' motion, the points found
    // again there. The material points' accelerations are not it: the point
    // slides along each body and the normal turns -- a roller on a disk cam
    // turning at 45 rad/s, the disk's point under it pulled round at 5e4
    // mm/s^2, read -39550 mm/s^2 where its speed grew by -1080, and a
    // landing at 328.04 mm/s, carried over a substep the wrong way, read
    // 326.7. The others' points are fixed on each body, their normal fixed:
    // the material points' is theirs.
    auto touchOf = [&](const ContactLoad& load) {
        Touch touch;
        const auto a = load.GetBodyA();
        const auto b = load.GetBodyB();
        const std::vector<ContactLoad::Point> now = load.pointsNow();
        std::size_t deepest = now.size();
        for (std::size_t i = 0; i < now.size(); ++i) {
            const ContactLoad::Point& point = now[i];
            const double depth = -ch::Vdot(point.normal,
                                           b->TransformPointLocalToParent(point.onB)
                                               - a->TransformPointLocalToParent(point.onA));
            if (depth > touch.depth) {
                touch.depth = depth;
                touch.frames = point.frames;
                touch.rate = load.meshed(i)
                    ? -ch::Vdot(point.normal,
                                load.speedOnB(i, point) - a->PointSpeedLocalToParent(point.onA))
                    : rateOf(point, *a, *b);
                touch.accel = -ch::Vdot(point.normal,
                                        b->PointAccelerationLocalToParent(point.onB)
                                            - a->PointAccelerationLocalToParent(point.onA));
                deepest = i;
            }
        }
        if (load.follow && deepest < now.size()) {
            const double dt = 1e-6;
            double rates[2] {};
            for (int side = 0; side < 2; ++side) {
                const double step = side == 0 ? -dt : dt;
                const ch::ChFrameMoving<> atA = ahead(*a, step);
                const ch::ChFrameMoving<> atB = ahead(*b, step);
                std::vector<ContactLoad::Point> there = load.points;
                load.follow(atA, atB, there);
                rates[side] = rateOf(there[deepest], atA, atB);
            }
            touch.accel = (rates[1] - rates[0]) / (2.0 * dt);
        }
        return touch;
    };
    // A dynamic run's contact events (sec 8.4): a load starting to press, or
    // letting go, at the end of a substep kept, by the points found there.
    // Located within the substep where its deepest point's depth crosses
    // none, on the line between the substep's ends read on the points found
    // at its start; a change those points do not see (a point found within
    // the substep, or gone with its points) at the end, one they already had
    // at the start there. Its speed there from the end where the bodies are
    // apart, and move free: a line between the ends took a stiff contact's
    // braking within the substep, a ball landing at 139.99 mm/s for 140.05
    // and leaving 0.13% slower than its flight took. Leaving, read again
    // after the next substep kept (readLeaving()), taken back to the time it
    // let go: HHT's accelerations at a substep's end mix in the force at its
    // start, -11.7 m/s^2 for g's -9.8 where the ball left, and its velocity
    // settles in the substep after -- read where it left, the ball left
    // 0.06% to 0.28% slower than its flight took; a substep on, to 1e-6 of
    // the speed it comes back in at, and the same two and three on. The
    // carry by the speed's own growth (touchOf()): by the material points'
    // accelerations, a cam's follower read coming in at -12.8 mm/s and
    // leaving at -32.6 (the manual's cam-pair video), and a sensor on every
    // contact made missed those. Never below none still: the depth crossed
    // none the way it went, and a speed the other way is the carry's.
    auto readLeaving = [&](double time) {
        for (auto& q : queued) {
            if (!q.reading) {
                continue;
            }
            const Touch touch = touchOf(*q.reading);
            if (std::isfinite(touch.depth)) {
                q.event.value =
                    std::max(-(touch.rate - (time - q.event.time) * touch.accel) / SI::velocity,
                             0.0);
            }
            q.reading = nullptr;
        }
    };
    auto noteTouch =
        [&](ContactLoad& load, double t0, double h, const Touch& from, const Touch& to) {
            const bool now = pressing(load) > 0.0;
            if (now == load.touching) {
                return;
            }
            load.touching = now;
            // Pressing again: what the last reading of its letting go gave
            for (auto& q : queued) {
                if (q.reading == &load) {
                    q.reading = nullptr;
                }
            }
            double f = 1.0;
            double rate = to.rate;
            if (std::isfinite(from.depth) && (from.depth > 0.0) == now) {
                f = 0.0;
                rate = from.rate;
            }
            else if (std::isfinite(from.depth) && std::isfinite(to.depth)
                     && (to.depth > 0.0) == now) {
                f = from.depth / (from.depth - to.depth);
                rate = now ? from.rate + f * h * from.accel : to.rate - (1.0 - f) * h * to.accel;
            }
            Event event;
            event.kind = now ? Event::Kind::ContactBegin : Event::Kind::ContactEnd;
            event.time = t0 + f * h;
            event.source = si.sourceName(si.dynamics->contacts[load.set].source);
            event.value = std::max((now ? rate : -rate) / SI::velocity, 0.0);
            // The parts of its deepest point where it presses: at the end
            // made, at the start let go
            const Touch& pressed =
                std::isfinite((now ? to : from).depth) ? (now ? to : from) : (now ? from : to);
            event.bodyA = partName(pressed.frames[0], load.GetBodyA().get());
            event.bodyB = partName(pressed.frames[1], load.GetBodyB().get());
            queued.push_back({std::move(event), now ? nullptr : &load});
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
        const double mass = load.lighterMass();
        for (std::size_t i = 0; i < load.points.size(); ++i) {
            const auto& point = load.points[i];
            const ch::ChVector3d pA = a->TransformPointLocalToParent(point.onA);
            const ch::ChVector3d pB = b->TransformPointLocalToParent(point.onB);
            const double depth = -ch::Vdot(point.normal, pB - pA);
            if (!(depth > 1e-12)) {
                continue;
            }
            const double rate = std::max(-ch::Vdot(point.normal,
                                                   load.speedOnB(i, point)
                                                       - a->PointSpeedLocalToParent(point.onA)),
                                         0.0);
            const double e = load.exponents[point.patch];
            const double reach = std::isfinite(mass)
                ? std::pow((e + 1.0) * mass * rate * rate / (2.0 * load.stiffness[point.patch]),
                           1.0 / (e + 1.0))
                : 0.0;
            if (depth > 0.1 * reach) {
                return true;
            }
        }
        return false;
    };
    const double tolerance = std::max(spec.errorTolerance, 1e-9) * SI::length;
    // Whether a load rests: the range its deepest point's depth has kept,
    // given the depth now too, within 2% of it or within the tolerance; and
    // that range kept, after a substep of h, settled once it has held five
    // of its periods -- an impact's sink and rebound move it all the way, and
    // a resting part's rocking dies down under the damping within some. The
    // tolerance for a metal's: a torus of aluminium on its ring, sunk 6e-8
    // mm, rocks on its facets by 5e-8. Its deepest point within the
    // tolerance of touching, not only pressing: a pin of 0.6 g lying on its
    // rims across a bore sinks 0.17 nm, and chattered from 0.4 nm off to
    // 0.1 nm in -- never resting, it took substeps of 0.3 us, 74 s for 0.3.
    auto resting = [&](const ContactLoad& load) {
        const double depth = pressing(load);
        const double high = std::max(load.high, depth);
        const double range = high - std::min(load.low, depth);
        return depth > -tolerance && (range <= 0.02 * high || range <= tolerance);
    };
    auto keepCalm = [&](ContactLoad& load, double h) {
        if (load.calm > 0.0 && resting(load)) {
            const double depth = pressing(load);
            load.low = std::min(load.low, depth);
            load.high = std::max(load.high, depth);
            load.calm += h;
            load.settled = load.calm >= 5.0 * load.period;
            return;
        }
        const double depth = pressing(load);
        load.low = load.high = depth;
        load.calm = depth > -tolerance ? h : 0.0;
        load.settled = false;
    };
    // Newton's test, HHT's, is on its iterations' increments, in
    // accelerations and multipliers, 1e-10 absolute by default: a
    // constraint's round-off, 1e-15 m, is eps / h^2 there, and below a
    // microsecond no step passed it -- each of the 64 smallest starting a
    // run ran its 20 iterations, two iterates apart, |R| at 1e-17, and a
    // contact's step taken again for that, an eighth as long, failed it the
    // more: a cam's follower stuck at a nanosecond. The increments a
    // thousandth of the run's tolerance in position instead, the
    // multipliers' as they would move the lightest body -- where there is
    // contact, whose substeps are taken again on it. A run without takes
    // each step as Newton ends it, and its 20 iterations get the start's
    // reactions closer: stopped at those tolerances, a hinge's was 0.3%
    // off and redundant hinges' sum 5 N off the one hinge's.
    double lightest = std::numeric_limits<double>::infinity();
    for (const auto& body : run.d->bodies) {
        if (!body->IsFixed() && body->GetMass() > 0.0) {
            lightest = std::min(lightest, body->GetMass());
        }
    }
    if (!std::isfinite(lightest)) {
        lightest = 1.0;
    }
    auto tolerate = [&](double h) {
        if (auto implicit =
                std::dynamic_pointer_cast<ch::ChTimestepperImplicit>(sys.GetTimestepper())) {
            const double s = std::max(1e-3 * tolerance / (h * h), 1e-10);
            implicit->SetAbsTolerances(s, std::max(s * lightest, 1e-10));
        }
    };
    // The substep the penalty stops allow, as a contact's (contactStep()): a
    // fiftieth of the period of the lighter body on a stop's spring -- its
    // mass, or for a turn its inertia about the joint's axis -- and 2 m / c
    // for its damper, once it is past its bound or will be within a period
    // at the speed it comes in at; half the time it takes to get there
    // before. HHT stepping over a stiff spring's period damps it away: the
    // stop takes the energy a bumper hands back. A side on a flexible part's
    // interface moves the part (carrier()): its boundary body's mass reads 0
    // in the run, and a stop on the flexible rod's hinge, the ground's other
    // side, set no substep -- let go level onto it undamped, the rod came
    // back 0.28 deg above where it fell from, the rigid one's 0.02.
    const bool anyPenalty =
        std::any_of(penalties.begin(), penalties.end(), [](const auto& p) { return bool(p); });
    auto penaltyStep = [&]() {
        if (!run.d->flexibles.empty()) {
            run.d->syncFlexible();
        }
        double h = std::numeric_limits<double>::infinity();
        for (const auto& load : penalties) {
            if (!load || !(load->k > 0.0)) {
                continue;
            }
            const double depth = load->depthNow();
            const auto [value, rate] = load->valueAndRate();
            const double deeper = load->atMin ? -rate : rate;
            const ch::ChVector3d z = load->GetAbsoluteFrameB().GetRotMat().GetAxisZ();
            const ch::ChVector3d at = load->GetAbsoluteFrameB().GetPos();
            double mass = std::numeric_limits<double>::infinity();
            for (const auto& side : {load->GetBodyA(), load->GetBodyB()}) {
                ch::ChBody* body = run.d->carrier(side.get());
                if (side->IsFixed() || !(body->GetMass() > 0.0)) {
                    continue;
                }
                if (!load->turn) {
                    mass = std::min(mass, body->GetMass());
                    continue;
                }
                const ch::ChVector3d local = body->TransformDirectionParentToLocal(z);
                const ch::ChVector3d arm = body->GetPos() - at;
                const ch::ChVector3d off = arm - z * ch::Vdot(arm, z);
                mass = std::min(mass,
                                ch::Vdot(local, body->GetInertia() * local)
                                    + body->GetMass() * off.Length2());
            }
            if (!std::isfinite(mass)) {
                continue;
            }
            const double period = 2.0 * pi * std::sqrt(mass / load->k);
            if (depth <= 0.0) {
                const double toContact =
                    deeper > 0.0 ? -depth / deeper : std::numeric_limits<double>::infinity();
                if (toContact > period) {
                    h = std::min(h, 0.5 * toContact);
                    continue;
                }
            }
            h = std::min(h, period / 50.0);
            if (load->c > 0.0) {
                h = std::min(h, 2.0 * mass / load->c);
            }
        }
        return h;
    };
    // The substep the frictions allow: between the stiction and the friction
    // velocities the friction is flat, or falls as the slip grows, and
    // Newton's iterations there run off -- a block stopping on its rail at
    // 1 ms steps crept on at 2 mm/s, its friction at the static's, where at
    // 50 us it stopped dead. So while a joint slips through that band, half
    // the time its friction takes to change the slip by the stiction
    // velocity, the lighter body moving (for a turn its inertia about the
    // axis, over the radius squared); faster, half the time to come down to
    // the band. Stuck below half the stiction velocity, or sliding fast, the
    // steps are the run's. A flexible part's side is the part's, as a stop's.
    auto frictionStep = [&]() {
        if (!run.d->flexibles.empty()) {
            run.d->syncFlexible();
        }
        double h = std::numeric_limits<double>::infinity();
        for (const auto& load : frictions) {
            const Friction& f = load->friction;
            const auto [slide, turn] = load->slipNow();
            const double s = std::hypot(slide, turn);
            if (!(s > 0.5 * f.vStiction)) {
                continue;
            }
            const ch::ChVector3d z = load->axisNow();
            const ch::ChVector3d at = load->pointNow();
            double mass = std::numeric_limits<double>::infinity();
            for (const auto& side : {load->GetBodyA(), load->GetBodyB()}) {
                ch::ChBody* body = run.d->carrier(side.get());
                if (side->IsFixed() || !(body->GetMass() > 0.0)) {
                    continue;
                }
                if (f.slides) {
                    mass = std::min(mass, body->GetMass());
                }
                if (f.turns) {
                    const ch::ChVector3d local = body->TransformDirectionParentToLocal(z);
                    const ch::ChVector3d arm = body->GetPos() - at;
                    const ch::ChVector3d off = arm - z * ch::Vdot(arm, z);
                    mass = std::min(mass,
                                    (ch::Vdot(local, body->GetInertia() * local)
                                     + body->GetMass() * off.Length2())
                                        / (f.radius * f.radius));
                }
            }
            const double pressed = load->normal + f.preloadForce
                + (f.turns ? f.preloadTorque / f.radius : 0.0);
            const double fastest = std::max(f.muStatic, f.muDynamic) * pressed / mass;
            if (!std::isfinite(mass) || !(fastest > 0.0)) {
                continue;
            }
            if (s < f.vFriction) {
                h = std::min(h, 0.5 * f.vStiction / fastest);
            }
            else {
                h = std::min(h, std::max(0.5 * (s - f.vFriction) / fastest,
                                         0.5 * f.vStiction / fastest));
            }
        }
        return h;
    };
    const bool anyFriction = !frictions.empty();
    // What a step over a load at rest starts from. HHT takes the
    // accelerations it is given into the step, (1 - gamma) h of them into the
    // velocities, and a light part resting on a stiff contact keeps up a
    // rattle no wider than its sink that its substeps resolve: a pin of 0.6 g
    // on its side, sunk 0.055 nm, passes through touching, falling freely
    // there, every 15 us. A step of a millisecond from such an instant took
    // the fall into its velocity, 1.6 mm/s, and threw the pin up 23 nm; its
    // landing, found late, cut the steps short again, and the pin rattled
    // in and out of rest at substeps of 0.3 us. So a load at rest that the
    // step does not resolve starts it with its points' normal accelerations
    // those of its bodies' mean accelerations, over the last five of its
    // periods or over the last substep when that was longer -- the rattle's
    // average, none -- by the least change to the bodies' own, in the
    // mass's measure (meanAccelerations()). Only the normal's: the means
    // whole lag what moves the bodies, and a ball on the tipping plate
    // ended 0.10 mm off. Only bodies on no joint: one on a joint has its
    // accelerations held by it too, which the change does not keep, and a
    // cam's follower's contact force came out 0.014 N off. Each body's
    // velocities at the end of each substep kept, while they go on from
    // the state the last left: a stop's bounce, a sensor's step taken back
    // or a part put to rest starts them again.
    struct Moment
    {
        double t = 0.0;
        std::vector<ch::ChVector3d> v;  // its centre's, each body's
        std::vector<ch::ChVector3d> w;  // its angular velocity, in its frame
    };
    std::deque<Moment> past;
    auto moment = [&]() {
        Moment m;
        m.t = sys.GetChTime();
        for (const auto& body : run.d->bodies) {
            m.v.push_back(body->GetPosDt());
            m.w.push_back(body->GetAngVelLocal());
        }
        return m;
    };
    auto goOn = [&]() {
        const Moment now = moment();
        while (!past.empty() && past.back().t > now.t) {
            past.pop_back();
        }
        if (past.empty() || past.back().t != now.t || past.back().v != now.v
            || past.back().w != now.w) {
            past.assign(1, now);
        }
    };
    auto keepMoment = [&]() {
        past.push_back(moment());
        double window = 0.0;
        for (const auto& load : contactLoads) {
            if (std::isfinite(load->period)) {
                window = std::max(window, 5.0 * load->period);
            }
        }
        while (past.size() > 2 && past[1].t <= past.back().t - window) {
            past.pop_front();
        }
    };
    auto meanAccelerations = [&](double h) {
        if (past.size() < 2) {
            return;
        }
        // The loads the step does not resolve, and their bodies: each one's
        // six columns, its linear and angular accelerations in the absolute
        // axes, and its window
        std::set<const ch::ChBodyFrame*> joined;
        for (const auto& base : sys.GetLinks()) {
            if (const auto* link = dynamic_cast<const ch::ChLink*>(base.get())) {
                joined.insert(link->GetBody1());
                joined.insert(link->GetBody2());
            }
            else if (const auto* neutral = dynamic_cast<const NeutralLink*>(base.get())) {
                joined.insert(neutral->joins().begin(), neutral->joins().end());
            }
        }
        std::vector<const ContactLoad*> held;
        std::map<ch::ChBody*, std::pair<Eigen::Index, double>> on;
        Eigen::Index rows = 0;
        for (const auto& load : contactLoads) {
            if (!load->settled || load->points.empty() || !(h > load->period / 50.0)) {
                continue;
            }
            held.push_back(load.get());
            rows += static_cast<Eigen::Index>(load->points.size());
            for (const auto& body : {load->GetBodyA(), load->GetBodyB()}) {
                if (body->IsFixed() || joined.contains(body.get())) {
                    continue;
                }
                const auto column = static_cast<Eigen::Index>(6 * on.size());
                auto& [at, window] = on.try_emplace(body.get(), column, 0.0).first->second;
                window = std::max(window, 5.0 * load->period);
            }
        }
        if (on.empty()) {
            return;
        }
        // Each point's normal acceleration, B's against A's: n . (a + alpha x
        // r) on each side, the terms of the velocities alike in both
        const auto n = static_cast<Eigen::Index>(6 * on.size());
        MatX J = MatX::Zero(rows, n);
        Eigen::Index row = 0;
        for (const ContactLoad* load : held) {
            for (const auto& point : load->points) {
                const std::array<std::pair<ch::ChBody*, ch::ChVector3d>, 2> sides {
                    std::pair {load->GetBodyA().get(), point.onA},
                    std::pair {load->GetBodyB().get(), point.onB}};
                for (std::size_t s = 0; s < 2; ++s) {
                    const auto it = on.find(sides[s].first);
                    if (it == on.end()) {
                        continue;
                    }
                    const ch::ChBody& body = *sides[s].first;
                    const ch::ChVector3d r =
                        body.TransformPointLocalToParent(sides[s].second) - body.GetPos();
                    const ch::ChVector3d turn = ch::Vcross(r, point.normal);
                    const double sign = s == 0 ? -1.0 : 1.0;
                    for (int k = 0; k < 3; ++k) {
                        J(row, it->second.first + k) += sign * point.normal[k];
                        J(row, it->second.first + 3 + k) += sign * turn[k];
                    }
                }
                ++row;
            }
        }
        // The accelerations now, their means over each body's window, and
        // the inverse of the mass
        Eigen::VectorXd now(n);
        Eigen::VectorXd mean(n);
        MatX inverseMass = MatX::Zero(n, n);
        const Moment& end = past.back();
        for (std::size_t b = 0; b < run.d->bodies.size(); ++b) {
            const auto& body = run.d->bodies[b];
            const auto it = on.find(body.get());
            if (it == on.end()) {
                continue;
            }
            const auto [c, window] = it->second;
            std::size_t then = 0;
            while (then + 2 < past.size() && past[then + 1].t <= end.t - window) {
                ++then;
            }
            const double span = end.t - past[then].t;
            const ch::ChMatrix33<> turn = body->GetRotMat();
            const ch::ChVector3d linear = (end.v[b] - past[then].v[b]) / span;
            const ch::ChVector3d angular = turn * ((end.w[b] - past[then].w[b]) / span);
            const ch::ChVector3d alpha = turn * body->GetAngAccLocal();
            for (int k = 0; k < 3; ++k) {
                now(c + k) = body->GetPosDt2()[k];
                now(c + 3 + k) = alpha[k];
                mean(c + k) = linear[k];
                mean(c + 3 + k) = angular[k];
            }
            inverseMass.block(c, c, 3, 3) = Eigen::Matrix3d::Identity() / body->GetMass();
            const ch::ChMatrix33<> inertia = turn * body->GetInertia() * turn.transpose();
            inverseMass.block(c + 3, c + 3, 3, 3) = inertia.inverse();
        }
        // The least change, in the mass's measure, that gives the points the
        // normal accelerations of the means
        const MatX JW = J * inverseMass;
        const MatX S = JW * J.transpose();
        const Eigen::VectorXd lambda = S.completeOrthogonalDecomposition().solve(J * (mean - now));
        const Eigen::VectorXd change = JW.transpose() * lambda;
        for (const auto& body : run.d->bodies) {
            const auto it = on.find(body.get());
            if (it == on.end()) {
                continue;
            }
            const Eigen::Index c = it->second.first;
            const ch::ChMatrix33<> turn = body->GetRotMat();
            const ch::ChVector3d linear(change(c), change(c + 1), change(c + 2));
            const ch::ChVector3d angular(change(c + 3), change(c + 4), change(c + 5));
            body->SetPosDt2(body->GetPosDt2() + linear);
            body->SetAngAccLocal(body->GetAngAccLocal() + turn.transpose() * angular);
        }
    };
    // Frames through the impacts (SimulationSpec::impactFrames): an impact of
    // steel on steel lasts some tens of microseconds and falls between the
    // output frames, which saw a block dropped on a lever land at none of
    // its bounces but one; and a frame at each one's peak alone drew it as a
    // spike from the output frames each side. After each substep kept, the
    // contact loads' forces: a substep where one moved from the last kept is
    // kept, its values and poses, and so is the one before it where they set
    // off from a steady force -- the line from the last kept would run
    // through it. At each output frame those since the last are thinned to
    // the ones the forces' curves need (Douglas-Peucker): each load within
    // 5% of the most it is at between the two ends of a line through the
    // ones kept, or 0.1% of the largest force so far -- a peak is the
    // farthest from its line, and kept. A load resting at a steady force
    // keeps none.
    struct Sample
    {
        double time = 0.0;
        std::vector<double> forces;  // the contact loads', N
        std::vector<double> values;  // the channels'
        std::vector<BodyPose> poses;
    };
    bool sampling = false;
    Sample anchor;                 // the last output frame's
    std::vector<Sample> samples;   // since it, in time order
    Sample previous;               // the substep before
    bool previousKept = true;
    std::vector<double> scales;    // each load's largest force so far, N
    auto forcesNow = [&]() {
        std::vector<double> forces(contactLoads.size());
        for (std::size_t i = 0; i < contactLoads.size(); ++i) {
            contactLoads[i]->ComputeQ(nullptr, nullptr);
            forces[i] = contactLoads[i]->onB.Length();
        }
        return forces;
    };
    auto largestScale = [&]() {
        return scales.empty() ? 0.0 : *std::ranges::max_element(scales);
    };
    auto watchImpacts = [&]() {
        if (!sampling) {
            return;
        }
        std::vector<double> forces = forcesNow();
        for (std::size_t i = 0; i < forces.size(); ++i) {
            scales[i] = std::max(scales[i], forces[i]);
        }
        const double largest = largestScale();
        const std::vector<double>& last = samples.empty() ? anchor.forces : samples.back().forces;
        bool moved = false;
        for (std::size_t i = 0; i < forces.size() && !moved; ++i) {
            moved = std::abs(forces[i] - last[i]) > 1e-3 * largest;
        }
        const double time = sys.GetChTime();
        if (!moved) {
            // Its values read too: on the line between its neighbours', a
            // set-off's contact point and force were, but not their moment
            // -- r x F is not linear in them -- and a lever's free body was
            // out by 0.95% of its largest hinge torque at such a frame
            previous = {time,
                        std::move(forces),
                        channels.empty() ? std::vector<double>() : valuesNow(),
                        posesNow()};
            previousKept = false;
            return;
        }
        if (!previousKept) {
            samples.push_back(std::move(previous));
        }
        samples.push_back({time,
                           std::move(forces),
                           channels.empty() ? std::vector<double>() : valuesNow(),
                           posesNow()});
        previousKept = true;
    };
    // The samples before 'time', thinned, then 'time' the next to thin
    // from, its values 'values'; false when the sink cancels the run
    auto sendImpacts = [&](double time, const std::vector<double>& values) {
        if (!sampling) {
            return true;
        }
        std::erase_if(samples, [&](const Sample& s) {
            return !(s.time < time - 1e-12 * std::max(1.0, std::abs(time)));
        });
        Sample end {time, forcesNow(), values, {}};
        const std::size_t n = samples.size();
        // The points: the anchor, the samples, the end
        auto at = [&](std::size_t k) -> const Sample& {
            return k == 0 ? anchor : k == n + 1 ? end : samples[k - 1];
        };
        const double largest = largestScale();
        std::vector<double> tolerance(scales.size());
        std::vector<char> keep(n + 2, 0);
        if (largest > 0.0) {
            std::vector<std::pair<std::size_t, std::size_t>> spans {{0, n + 1}};
            while (!spans.empty()) {
                const auto [a, b] = spans.back();
                spans.pop_back();
                const Sample& from = at(a);
                const Sample& to = at(b);
                // Within 5% of the most each load is at in the span: a
                // bounce of a few newtons after a landing of hundreds keeps
                // its peak -- 5% of the load's largest so far thinned the
                // block's bounces on the lever away
                for (std::size_t i = 0; i < tolerance.size(); ++i) {
                    double most = 0.0;
                    for (std::size_t k = a; k <= b; ++k) {
                        most = std::max(most, at(k).forces[i]);
                    }
                    tolerance[i] = std::max(0.05 * most, 1e-3 * largest);
                }
                double worst = 1.0;
                std::size_t far = 0;
                for (std::size_t k = a + 1; k < b; ++k) {
                    const Sample& s = at(k);
                    const double u = (s.time - from.time) / (to.time - from.time);
                    for (std::size_t i = 0; i < tolerance.size(); ++i) {
                        const double line = from.forces[i] + u * (to.forces[i] - from.forces[i]);
                        const double off = std::abs(s.forces[i] - line) / tolerance[i];
                        if (off > worst) {
                            worst = off;
                            far = k;
                        }
                    }
                }
                if (far != 0) {
                    keep[far] = 1;
                    spans.emplace_back(a, far);
                    spans.emplace_back(far, b);
                }
            }
        }
        for (std::size_t k = 1; k <= n; ++k) {
            if (!keep[k]) {
                continue;
            }
            const Sample& s = samples[k - 1];
            if (!channels.empty()) {
                sink.values(s.values);
            }
            if (!sink.frame(s.time, s.poses)) {
                return false;
            }
        }
        samples.clear();
        anchor = std::move(end);
        previousKept = true;
        return true;
    };
    // A step of dt, in substeps; a substep that found a contact late is taken
    // again, an eighth as long, from the state before it, and the steps
    // grow back after. One longer than the strict rule's that moved a load
    // at rest is taken again by it.
    // A run asked to stop is seen here too, not only as a frame is given
    // (TrajectorySink::cancelled()): a frame's substeps can take minutes.
    // And one that will not get anywhere says so: 100000 substeps kept in a
    // row, each under 10 ns -- a contact's, a hundredth of a metal's line
    // contact's on a part of grams, or ones taken back down to a nanosecond
    // again and again -- are an hour for a millisecond. A strip of 2 mm
    // coming back onto a bar ran 0.4 ms in four minutes so, its panel
    // showing a run under way, Cancel doing nothing.
    struct Stopped
    {};
    int crawled = 0;
    auto advance = [&](double dt) {
        if (!contact) {
            // A flexible body's modal coordinates never pass the default
            // test: the modal assembly's Jacobian is approximate (its
            // inertial terms linear), Newton's convergence linear, and each
            // step of a flexible pendulum ran its 20 iterations
            if (!run.d->flexibles.empty()) {
                tolerate(dt);
            }
            if (!anyPenalty && !anyFriction) {
                sys.DoStepDynamics(dt);
                return;
            }
            double done = 0.0;
            while (dt - done > 1e-12 * dt) {
                double h = std::min({dt - done, penaltyStep(), frictionStep()});
                if (dt - done - h < 1e-9 * dt) {
                    h = dt - done;
                }
                sys.DoStepDynamics(h);
                done += h;
                for (const auto& load : penalties) {
                    if (load) {
                        load->commit();
                    }
                }
            }
            return;
        }
        const unsigned int nx = sys.GetNumCoordsPosLevel();
        const unsigned int nv = sys.GetNumCoordsVelLevel();
        ch::ChState x(nx, &sys);
        ch::ChStateDelta v(nv, &sys);
        ch::ChStateDelta a(nv, &sys);
        // And the reactions: HHT takes a step's start's into its residual
        // (alpha's share of the old Cq' l), gathered from the system, and a
        // substep whose Newton iterations ran off leaves them there not
        // finite -- every substep after, taken again from the state before
        // it, started with a residual not finite and ran off too, down to a
        // nanosecond (a lever on a cam at 2 rev/s, "the contact's
        // integration diverged"); one that stopped unconverged, finite,
        // left them wrong.
        ch::ChVectorDynamic<> l;
        // And each flexible part's floating frame (D4b), which is not in the
        // state: its update iterates from where it is, and one a step that
        // ran off left not finite stayed so, every substep after failing
        // down to a nanosecond
        std::vector<ch::ChFrameMoving<>> floating;
        auto gatherFloating = [&]() {
            floating.clear();
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
            for (const auto& flexible : run.d->flexibles) {
                floating.push_back(flexible.built.assembly->GetFloatingFrameOfReference());
            }
#endif
        };
        auto takeBack = [&](double time) {
#ifdef ASSEMBLY_WITH_CHRONO_MODAL
            for (std::size_t i = 0; i < floating.size() && i < run.d->flexibles.size(); ++i) {
                run.d->flexibles[i].built.assembly->SetFloatingFrameOfReference(floating[i]);
            }
#endif
            sys.StateScatter(x, v, time, ch::UpdateFlags::UPDATE_ALL);
            sys.StateScatterAcceleration(a);
            if (l.size() == static_cast<Eigen::Index>(sys.GetNumConstraints())) {
                sys.StateScatterReactions(l);
            }
        };
        double done = 0.0;
        double cap = dt;
        std::vector<double> before(contactLoads.size());
        // Each load's deepest point at the substep's start and at its end, on
        // the points found at its start (noteTouch())
        std::vector<Touch> from(contactLoads.size());
        std::vector<Touch> to(contactLoads.size());
        bool fresh = false;  // collision detection last ran at the state now
        goOn();
        while (dt - done > 1e-12 * dt) {
            if (sink.cancelled()) {
                throw Stopped {};
            }
            const double strict = contactStep(true);
            double h =
                std::min({dt - done, cap, contactStep(false), penaltyStep(), frictionStep()});
            if (dt - done - h < 1e-9 * dt) {
                h = dt - done;
            }
            for (std::size_t i = 0; i < contactLoads.size(); ++i) {
                from[i] = touchOf(*contactLoads[i]);
                before[i] = from[i].depth;
            }
            double t = 0.0;
            sys.StateGather(x, v, t);
            sys.StateGatherAcceleration(a);
            l.setZero(sys.GetNumConstraints());
            sys.StateGatherReactions(l);
            gatherFloating();
            contactCollision->reuse = fresh;
            auto implicit =
                std::dynamic_pointer_cast<ch::ChTimestepperImplicit>(sys.GetTimestepper());
            const unsigned int terminated = implicit ? implicit->GetNumTerminated() : 0;
            tolerate(h);
            if (h > strict) {
                meanAccelerations(h);
            }
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
            // A flexible part's state is its interfaces' and its modes',
            // which its proxy body only follows (D4b)
            if (finite && !run.d->flexibles.empty()) {
                ch::ChState xNow(nx, &sys);
                ch::ChStateDelta vNow(nv, &sys);
                double tNow = 0.0;
                sys.StateGather(xNow, vNow, tNow);
                finite = xNow.allFinite() && vNow.allFinite();
            }
            if (!finite && !(h > 1e-9)) {
                takeBack(t);
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
                for (std::size_t i = 0; i < contactLoads.size(); ++i) {
                    to[i] = touchOf(*contactLoads[i]);
                }
                sys.ComputeCollisions();
                fresh = true;
                for (std::size_t i = 0; i < contactLoads.size() && !tooDeep; ++i) {
                    tooDeep = before[i] <= 0.0 && late(*contactLoads[i]);
                }
            }
            bool moved = false;
            if (!tooDeep && h > strict) {
                for (const auto& load : contactLoads) {
                    if (load->settled && !resting(*load)) {
                        load->settled = false;
                        load->calm = 0.0;
                        moved = true;
                    }
                }
            }
            if ((tooDeep || moved) && h > 1e-9) {
                takeBack(t);
                sys.ComputeCollisions();
                fresh = true;
                if (tooDeep) {
                    cap = h / 8.0;
                }
                continue;
            }
            done += h;
            cap = std::min(dt, cap * 2.0);
            if (!(h < 1e-8)) {
                crawled = 0;
            }
            else if (++crawled > 100000) {
                throw std::runtime_error("the contact's integration stalled at "
                                         + std::to_string(sys.GetChTime())
                                         + " s: 100000 substeps in a row under 10 ns");
            }
            keepMoment();
            watchImpacts();
            // A load pressing as the substep ended that collision detection
            // then found nothing of: the next substeps short, growing back.
            // HHT takes the last substep's accelerations into the next, and
            // a pin whose points Bullet lost 0.1 um in was thrown at 1.6 m/s
            // by a substep of 0.9 ms after one of 0.8 us
            for (std::size_t i = 0; i < contactLoads.size(); ++i) {
                if (to[i].depth > 0.0 && !(pressing(*contactLoads[i]) > 0.0)) {
                    cap = std::min(cap, 2.0 * h);
                }
            }
            if (!statics) {
                readLeaving(t + h);
            }
            for (std::size_t i = 0; i < contactLoads.size(); ++i) {
                keepCalm(*contactLoads[i], h);
                if (!statics) {
                    noteTouch(*contactLoads[i], t, h, from[i], to[i]);
                }
            }
        }
    };
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
        // A follower on its cam at the start (within a micrometre) starts as
        // the cam's joint would have it: the joint held over the start's
        // assembly, as an initial velocity's motor is, and taken off. From
        // rest, the cam at its speed came at it as an impact -- at 60 mm/s
        // on the disk cam, the follower sunk three times its allowance
        // before it rode the lift.
        std::vector<std::shared_ptr<ch::ChLinkBase>> camsHeld;
        if (!statics && contactHandover) {
            for (const auto& cam : contactHandover->cams) {
                std::string why;
                if (cam.gap() <= 1e-6) {
                    if (auto link = run.d->neutralLink(*cam.joint, why)) {
                        sys.AddLink(link);
                        camsHeld.push_back(link);
                    }
                }
            }
        }
        const bool moving = !statics && setInitialVelocities();
        // The accelerations at the start, gravity's: HHT takes those it is
        // given, and from rest that was none, the run lagging a part of a
        // step behind -- the pendulum's period long by 0.2% at 1 ms, the
        // error first order in the step
        assembleFull(moving ? 1 : 0);
        // The frictions' loads from the reactions there
        pressFrictions();
        for (const auto& link : camsHeld) {
            sys.RemoveLink(link);
        }
        auto stepOnce = [&](double dt) {
            advance(dt);
            // The neutral links count their turns from the angles last
            // committed: a pinion past half a turn read as the other way
            // round, the rack jumping a pitch
            run.d->commitAngles();
            handleStops(dt, true);
        };
        // A step watched by the sensors. One that trips one is located by
        // bisection, each try a step from the state before the trip: that
        // taken back whole -- the system's state, the stops held (their
        // links), the angles counted, the contacts' bookkeeping and the cams'
        // parameters -- to a small fraction of the output step (Adams'
        // SENSOR to its TERROR). There it is an event; a sensor that stops
        // ends the step there (true), one that marks takes the rest of it.
        struct Snapshot
        {
            ch::ChState x;
            ch::ChStateDelta v;
            ch::ChStateDelta a;
            // The reactions, which HHT's next step takes from the system
            // (advance()'s takeBack())
            ch::ChVectorDynamic<> l;
            double t = 0.0;
            std::vector<Stop> stops;
            std::vector<double> limitAngles;
            struct LoadState
            {
                double impact, low, high, calm, period;
                bool settled, touching;
            };
            std::vector<LoadState> loads;
            std::vector<std::pair<double, bool>> cams;
            std::vector<double> frictions;
            std::vector<double> torsions;  // their turns committed
            std::vector<Queued> queued;
        };
        auto snapshot = [&]() {
            Snapshot s;
            s.x = ch::ChState(sys.GetNumCoordsPosLevel(), &sys);
            s.v = ch::ChStateDelta(sys.GetNumCoordsVelLevel(), &sys);
            s.a = ch::ChStateDelta(sys.GetNumCoordsVelLevel(), &sys);
            sys.StateGather(s.x, s.v, s.t);
            sys.StateGatherAcceleration(s.a);
            s.l.setZero(sys.GetNumConstraints());
            sys.StateGatherReactions(s.l);
            s.stops = stops;
            s.limitAngles = limitAngles;
            for (const auto& load : contactLoads) {
                s.loads.push_back({load->impact,
                                   load->low,
                                   load->high,
                                   load->calm,
                                   load->period,
                                   load->settled,
                                   load->touching});
            }
            s.queued = queued;
            if (contactHandover) {
                for (const auto& cam : contactHandover->cams) {
                    s.cams.emplace_back(cam.u, cam.started);
                }
            }
            for (const auto& load : frictions) {
                s.frictions.push_back(load->normal);
            }
            for (const auto& torsion : torsions) {
                s.torsions.push_back(torsion.second->committed());
            }
            return s;
        };
        auto restore = [&](Snapshot& s) {
            // The samples after it never were, nor the substep before
            std::erase_if(samples, [&](const Sample& kept) { return kept.time > s.t; });
            previousKept = true;
            for (std::size_t k = 0; k < stops.size(); ++k) {
                if (stops[k].link != s.stops[k].link) {
                    if (stops[k].link) {
                        sys.RemoveLink(stops[k].link);
                    }
                    if (s.stops[k].link) {
                        sys.AddLink(s.stops[k].link);
                    }
                }
            }
            stops = s.stops;
            limitAngles = s.limitAngles;
            sys.StateScatter(s.x, s.v, s.t, ch::UpdateFlags::UPDATE_ALL);
            sys.StateScatterAcceleration(s.a);
            // Its stops' links as they were, so as many reactions
            if (s.l.size() == static_cast<Eigen::Index>(sys.GetNumConstraints())) {
                sys.StateScatterReactions(s.l);
            }
            for (std::size_t i = 0; i < contactLoads.size(); ++i) {
                ContactLoad& load = *contactLoads[i];
                const auto& kept = s.loads[i];
                load.impact = kept.impact;
                load.low = kept.low;
                load.high = kept.high;
                load.calm = kept.calm;
                load.period = kept.period;
                load.settled = kept.settled;
                load.touching = kept.touching;
            }
            queued = s.queued;
            if (contactHandover) {
                for (std::size_t i = 0; i < contactHandover->cams.size(); ++i) {
                    contactHandover->cams[i].u = s.cams[i].first;
                    contactHandover->cams[i].started = s.cams[i].second;
                }
            }
            for (std::size_t i = 0; i < frictions.size(); ++i) {
                frictions[i]->normal = s.frictions[i];
            }
            for (std::size_t i = 0; i < torsions.size(); ++i) {
                torsions[i].second->setCommitted(s.torsions[i]);
            }
            if (contact) {
                sys.ComputeCollisions();
            }
            run.d->commitAngles();
        };
        // The run stopping: a contact let go has its speed read a step on,
        // the state then taken back
        auto settleReadings = [&]() {
            if (std::ranges::none_of(queued, [](const Queued& q) {
                    return q.reading != nullptr;
                })) {
                return;
            }
            Snapshot s = snapshot();
            stepOnce(contactStep(false));
            std::vector<double> read;
            for (std::size_t i = 0; i < s.queued.size(); ++i) {
                read.push_back(queued[i].event.value);
            }
            restore(s);
            for (std::size_t i = 0; i < read.size(); ++i) {
                if (queued[i].reading) {
                    queued[i].event.value = read[i];
                    queued[i].reading = nullptr;
                }
            }
        };
        const double locate = std::max(spec.hMin, 1e-6 * spec.tOutput);
        std::function<bool(double)> watchedPiece = [&](double dt) {
            Snapshot from = snapshot();
            const double t0 = sys.GetChTime();
            stepOnce(dt);
            // An impact begun and over within the piece: advance() resolved
            // it in substeps of its own, the contact found late, and the
            // values are read only at the piece's end -- a ball's third
            // landing went by its force's sensor unseen. The piece taken
            // again halfway to the contact, the rest in the contact's
            // substeps, which then see it coming. Not cut at the contact
            // itself: the next piece starting where it touched read the
            // impact's speed as none, and the bounce came out slower.
            if (contact && !watches.empty()) {
                double made = std::numeric_limits<double>::infinity();
                for (const auto& q : queued) {
                    if (!q.seen && q.event.kind == Event::Kind::ContactBegin && q.event.time > t0) {
                        made = std::min(made, q.event.time);
                    }
                }
                const bool over = std::ranges::any_of(queued, [&](const Queued& q) {
                    return !q.seen && q.event.kind == Event::Kind::ContactEnd
                        && q.event.time > made;
                });
                if (over && made - t0 > 2.0 * locate) {
                    restore(from);
                    const double half = 0.5 * (made - t0);
                    if (watchedPiece(half)) {
                        return true;
                    }
                    double done = half;
                    while (dt - done > 1e-12 * dt) {
                        double piece = std::min(dt - done, contactStep(false));
                        if (dt - done - piece < 1e-9 * dt) {
                            piece = dt - done;
                        }
                        if (watchedPiece(piece)) {
                            return true;
                        }
                        done += piece;
                    }
                    return false;
                }
            }
            std::vector<double> values = valuesNow();
            if (!anyTrips(values) && !eventStops()) {
                passBy(values);
                eventTrips();
                return false;
            }
            // Located aside, each try one step from the piece's start; a
            // mark then goes back to the piece's own step, so that the run
            // goes on as it would unwatched. Run through the tries, the
            // halving steps down to nanoseconds -- HHT holding the joints in
            // position, a step of h turns their round-off e into a speed
            // e / (beta h) -- the follower of a cam on a slider came out of
            // a mark moving at 4e-5 m/s square to the slider, its force
            // ringing 0.7% and its pressure angle 1.3 deg off (TestCamJoint).
            Snapshot end = snapshot();
            std::vector<Queued> marks;
            double lo = 0.0;
            for (;;) {
                double hi = dt;
                while (hi - lo > locate) {
                    const double mid = 0.5 * (lo + hi);
                    restore(from);
                    stepOnce(mid);
                    values = valuesNow();
                    if (anyTrips(values) || eventStops()) {
                        hi = mid;
                    }
                    else {
                        lo = mid;
                    }
                }
                restore(from);
                stepOnce(hi);
                const std::size_t was = queued.size();
                bool stop = tripsAt(t0 + hi, valuesNow());
                if (stop || eventStops()) {
                    settleReadings();
                    if (eventTrips() || stop) {
                        // The run ends here, after the piece's marks before
                        queued.insert(queued.begin() + was, marks.begin(), marks.end());
                        return true;
                    }
                }
                marks.insert(marks.end(), queued.begin() + was, queued.end());
                lo = hi;
                restore(end);
                values = valuesNow();
                if (dt - hi > 1e-12 * dt && (anyTrips(values) || eventStops())) {
                    continue;
                }
                passBy(values);
                queued.insert(queued.end(), marks.begin(), marks.end());
                return eventTrips();
            }
        };
        // With contact, in the contact's substeps (contactStep()): an
        // impact is over in tens of microseconds, and one watched at the
        // step's end went by unseen -- a ball landing between two
        // millisecond steps tripped its contact force's sensor at a later
        // bounce.
        auto stepWatched = [&](double dt) {
            if (watches.empty() && eventWatches.empty()) {
                stepOnce(dt);
                return false;
            }
            if (!contact) {
                return watchedPiece(dt);
            }
            double done = 0.0;
            while (dt - done > 1e-12 * dt) {
                double piece = std::min(dt - done, contactStep(false));
                if (dt - done - piece < 1e-9 * dt) {
                    piece = dt - done;
                }
                if (watchedPiece(piece)) {
                    return true;
                }
                done += piece;
            }
            return false;
        };
        // A dynamic run starts in steps growing from a thousandth of one:
        // 64 of h / 1024, then h / 16, h / 8, h / 4, h / 2, together the
        // first step. HHT starts from the accelerations it is given, the
        // assembly analysis's, which have none that motion gives (its
        // velocity projection holds the positions); a whole step from them
        // left the velocities off, a cam's force 15% off at the first frame
        // and 1.5% at the second. The start's reactions (a joint's, a
        // motor's effort) are solved at it (solveReactions()); where that
        // fails, those after 32 and after 64 of the smallest taken back to
        // the start, a line through the two -- which take in what a contact
        // begun in those steps did. What the state alone gives (a spring's
        // force, length, angle) is read at the start itself: a line through
        // a quadratic path misses it by a t1 t2 / 2.
        const int tinySteps = 64;
        const double tiny = h / 1024.0;
        for (std::size_t k = 0; k <= steps; ++k) {
            const double time = spec.tStart + static_cast<double>(k) * spec.tOutput;
            if (!statics && k == 0) {
                const std::vector<BodyPose> poses = posesNow();
                // The contacts pressing at the start: none of them begins.
                // Found before the start's values are read: a cam's pressure
                // angle read none there, and a part resting from the start
                // no contact force
                if (contact) {
                    sys.ComputeCollisions();
                    for (const auto& load : contactLoads) {
                        load->touching = pressing(*load) > 0.0;
                    }
                }
                atStart = true;
                const std::vector<double> start =
                    channels.empty() ? std::vector<double>() : valuesNow();
                atStart = false;
                // The start's own forces, its loads' (a contact's as it
                // stands, none where it only touches) and its bodies' turns:
                // the fallback's accelerations are those with the reactions
                // taken back to it. Taken back with them, they took in what a
                // contact begun in the smallest steps did -- a lever let go
                // onto two steel posts read 131 rad/s^2 for 122 free, its
                // free body 5% of its weight out of balance at the start
                ch::ChVectorDynamic<> startForces(sys.GetNumCoordsVelLevel());
                startForces.setZero();
                sys.LoadResidual_F(startForces, 1.0);
                std::vector<ch::ChMatrix33<>> startTurns;
                for (std::size_t b : movingBodies) {
                    startTurns.push_back(run.d->bodies[b]->GetRotMat());
                }
                // Its reactions solved at it, from those forces. Taken back
                // from the smallest steps, they took in what a contact begun
                // in them did: a lever let go across two steel posts turned
                // 3.6% under its free rate at the start. Those are the
                // fallback where the solve fails
                ch::ChVectorDynamic<> startL;
                ch::ChVectorDynamic<> startR;
                std::vector<double> solved;
                if (!channels.empty() && solveReactions(h / 64.0, startL, startR)) {
                    ch::ChVectorDynamic<> was(sys.GetNumConstraints());
                    was.setZero();
                    sys.StateGatherReactions(was);
                    sys.StateScatterReactions(startL);
                    solved = valuesNow();
                    sys.StateScatterReactions(was);
                }
                std::vector<double> half;
                ch::ChVectorDynamic<> halfL;
                for (int i = 0; i < tinySteps; ++i) {
                    stepOnce(tiny);
                    if (i + 1 == tinySteps / 2 && !channels.empty()) {
                        half = valuesNow();
                        halfL.setZero(sys.GetNumConstraints());
                        sys.StateGatherReactions(halfL);
                    }
                }
                if (!channels.empty()) {
                    std::vector<double> values = valuesNow();
                    std::size_t v = 0;
                    for (const auto& channel : channels) {
                        const bool reaction = channel.quantity == "Force"
                            || channel.quantity == "Torque" || channel.quantity == "MotorTorque"
                            || channel.quantity == "MotorForce"
                            || channel.quantity == "Acceleration"
                            || channel.quantity == "AngularAcceleration";
                        for (int c = 0; c < channel.width && v < values.size(); ++c, ++v) {
                            values[v] = !reaction        ? start[v]
                                : solved.size() > v ? solved[v]
                                                    : 2.0 * half[v] - values[v];
                        }
                    }
                    // The bodies' accelerations from the start's forces and
                    // its reactions: the solve's, Cq' L at the start; else
                    // those taken back (Cq' L is L's linear map), through Cq
                    // after the smallest steps -- a turn's at 3 rad/s read
                    // 0.25% under through it
                    ch::ChVectorDynamic<> endL(sys.GetNumConstraints());
                    sys.StateGatherReactions(endL);
                    if (!movingBodies.empty() && halfL.size() == endL.size()
                        && startForces.size() == sys.GetNumCoordsVelLevel()) {
                        ch::ChVectorDynamic<> R = startForces;
                        if (!solved.empty() && startR.size() == R.size()) {
                            R = startR;
                        }
                        else {
                            sys.LoadResidual_CqL(R, halfL, 2.0);
                            sys.LoadResidual_CqL(R, endL, -1.0);
                        }
                        std::size_t m = 0;
                        v = 0;
                        for (const auto& channel : channels) {
                            const bool linear = channel.quantity == "Acceleration";
                            const bool angular = channel.quantity == "AngularAcceleration";
                            if ((linear || angular) && m < movingBodies.size()) {
                                const auto [a, alpha] = motionFrom(R, movingBodies[m], startTurns[m]);
                                const ch::ChVector3d& value = linear ? a : alpha;
                                for (int c = 0; c < 3; ++c) {
                                    values[v + c] = value[c];
                                }
                                if (angular) {
                                    ++m;
                                }
                            }
                            v += static_cast<std::size_t>(channel.width);
                        }
                    }
                    sink.values(values);
                }
                if (!sink.frame(time, poses)) {
                    status = Status::Cancelled;
                    break;
                }
                // The frames through the impacts from the end of the smallest
                // steps, whose transient the start's values take back
                if (spec.impactFrames && contact && !contactLoads.empty()) {
                    sampling = true;
                    anchor = {sys.GetChTime(),
                              forcesNow(),
                              channels.empty() ? std::vector<double>() : valuesNow(),
                              {}};
                    scales = anchor.forces;
                }
                // The sensors from the end of the smallest steps
                if (!watches.empty()) {
                    watchFrom(valuesNow());
                }
                continue;
            }
            bool tripped = false;
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
                // A static run's sensors at its output times, from the first
                if (!watches.empty()) {
                    const std::vector<double> values = valuesNow();
                    if (k == 0) {
                        watchFrom(values);
                    }
                    else {
                        tripped = tripsAt(time, values);
                    }
                }
            }
            else {
                int s = 0;
                if (k == 1) {
                    // The rest of the first step
                    for (double part = tinySteps * tiny; part < h && !tripped; part *= 2.0) {
                        tripped = stepWatched(part);
                    }
                    s = 1;
                }
                for (; s < sub && !tripped; ++s) {
                    tripped = stepWatched(h);
                }
            }
            // A sensor that stops: the last frame where it tripped
            const double at = tripped && !statics ? sys.GetChTime() : time;
            // The run's end: a contact let go in its last step read too
            if (!statics && !tripped && k == steps) {
                settleReadings();
                eventTrips();
            }
            sendEvents(tripped || k == steps);
            const std::vector<double> values =
                channels.empty() ? std::vector<double>() : valuesNow();
            if (!sendImpacts(at, values)) {
                status = Status::Cancelled;
                break;
            }
            if (!channels.empty()) {
                sink.values(values);
            }
            if (!sink.frame(at, posesNow())) {
                status = Status::Cancelled;
                break;
            }
            if (tripped) {
                break;
            }
        }
    }
    catch (const Stopped&) {
        status = Status::Cancelled;
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

#ifndef ASSEMBLY_WITH_CHRONO_MODAL
// Without Chrono's modal module (ChronoFlexible.cpp has them with it)
bool Chrono::hasFlexibleBodies()
{
    return false;
}

std::vector<double> Chrono::flexibleFrequencies(const Model&, std::size_t, std::string* error)
{
    if (error) {
        *error = "the Chrono backend was built without its modal module (Spectra)";
    }
    return {};
}

std::size_t Chrono::flexibleModesKept()
{
    return 0;
}

void Chrono::forgetFlexibleReductions()
{}
#endif
