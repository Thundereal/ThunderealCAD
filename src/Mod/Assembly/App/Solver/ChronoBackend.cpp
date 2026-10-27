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
#include <numbers>
#include <sstream>

#include <Eigen/Dense>

#include <chrono/core/ChFrame.h>
#include <chrono/functions/ChFunctionBase.h>
#include <chrono/functions/ChFunctionConst.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChLinkDistance.h>
#include <chrono/physics/ChLinkLockGear.h>
#include <chrono/physics/ChLinkMate.h>
#include <chrono/physics/ChLinkMotorLinearPosition.h>
#include <chrono/physics/ChLinkMotorRotationAngle.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/physics/ChLinkBase.h>
#include <chrono/serialization/ChArchiveJSON.h>
#include <chrono/solver/ChConstraintNgeneric.h>
#include <chrono/solver/ChDirectSolverLS.h>
#include <chrono/solver/ChSystemDescriptor.h>

#include <Base/Console.h>

#include "ChronoBackend.h"
#include "JointEquations.h"
#include "ReferenceBackend.h"

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
    // Kinematics only: no reactions are reported.
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
        const std::size_t n = std::min(rows.size(), constraints.size());
        for (std::size_t r = 0; r < n; ++r) {
            C(static_cast<Eigen::Index>(r)) = rows[r].s.v;
            ch::ChConstraintNgeneric& c = constraints[r];
            for (std::size_t b = 0; b < bodies.size(); ++b) {
                c.Get_Cq_N(b).setZero();
            }
            const eq::Grad& g = rows[r].s.g;
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
            if (limit.kind == LimitKind::TranslationZ) {
                auto link = std::make_shared<ch::ChLinkMateDistanceZ>();
                link->Initialize(bodyOf(limit.frameJ),
                                 bodyOf(limit.frameI),
                                 true,
                                 localOf(limit.frameJ),
                                 localOf(limit.frameI));
                link->SetDistance(*bound);
                sys->AddLink(link);
                boundLinks.push_back(link);
            }
            else {
                // Chrono's motor holds the angle modulo a turn; the bound is
                // the turn the angle is counted in.
                auto link = std::make_shared<ch::ChLinkMotorRotationAngle>();
                link->SetSpindleConstraint(ch::ChLinkMotorRotation::SpindleConstraint::FREE);
                link->Initialize(bodyOf(limit.frameJ),
                                 bodyOf(limit.frameI),
                                 true,
                                 driverLocalOf(limit.frameJ),
                                 localOf(limit.frameI));
                link->SetMotorFunction(std::make_shared<ch::ChFunctionConst>(*bound));
                sys->AddLink(link);
                boundLinks.push_back(link);
            }
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

    // Each row's owner and category, from its link's offset.
    struct RowInfo
    {
        Owner owner = Owner::Limit;
        Id id = NoId;
        Category category = Category::Essential;
    };
    std::vector<RowInfo> info(static_cast<std::size_t>(rows));
    for (const auto& record : d->links) {
        const unsigned int offset = record.link->GetOffset_L();
        for (std::size_t k = 0; k < record.rows.size(); ++k) {
            const std::size_t r = offset + k;
            if (r < info.size()) {
                info[r] = {record.owner, record.id, record.rows[k]};
            }
        }
    }

    // OndselSolver's order: its categories, then model order.
    std::vector<int> order(static_cast<std::size_t>(rows));
    for (Eigen::Index i = 0; i < rows; ++i) {
        order[i] = static_cast<int>(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return static_cast<int>(info[a].category) < static_cast<int>(info[b].category);
    });

    // Gram-Schmidt with reorthogonalisation (the Reference's).
    std::vector<char> independent(static_cast<std::size_t>(rows), 0);
    int kept = 0;
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
            if (status == Status::Ok && !sink.frame(time, d->posesNow())) {
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
