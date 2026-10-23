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

// The reference backend (docs/AssemblySolverInterface.md sec 6.13).
//
// Variables: per body a position and a rotation increment -- the spatial
// rotation vector applied on the left of the body's quaternion, which is
// renormalised after every step. No Euler angles, so no singularity.
//
// Equations: the neutral layer's (JointEquations.h, shared with the Chrono
// backend), one function per JointKind, each equation written from the
// frames' global origins and axes, in the form OndselSolver uses for it (the
// same direction cosines, the same squared xy distance, the same angle
// conventions for couplers), so that the rank of the Jacobian -- and with it
// the DOF and the redundancy -- answers the same question. The Jacobian is
// analytic: every quantity carries its derivative with respect to the
// variables of the (at most three) bodies it depends on.
//
// Solve: the pose closest to a target -- the start for a static solve, the
// drag targets for a drag step -- in the metric 1 per mm^2 and 1/4 per rad^2
// (OndselSolver's: its Euler parameters move by half the angle), weighted per
// body by Body::dragWeight during a drag, subject to the equations. Equations
// dependent at the start are set aside, as OndselSolver does at its first
// singular matrix; the rest are solved by weighted minimum-norm Gauss-Newton
// steps with Levenberg-Marquardt damping (restoration), alternated with steps
// along the constraint manifold towards the target (projection), until the
// projected step vanishes. Limits are inequalities by an active set: a bound
// a solution violates is held at its value and the solve repeated; a bound
// whose multiplier says the solution would leave it is released.
//
// Diagnostics: the rank of the Jacobian at the solution, by column-pivoted
// QR (DOF), and which equations are dependent on the ones before them,
// taken in OndselSolver's order (its three categories, then model order) by
// Gram-Schmidt with reorthogonalisation; a joint with a dependent equation is
// redundant, partially when not all of them are, and conflicting when a
// dependent equation is not satisfied (|residual| > 1e-6, OndselSolver's
// threshold).

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>

#include <Eigen/Dense>

#include <Base/Console.h>

#include "JointEquations.h"
#include "ReferenceBackend.h"

using namespace Assembly::Solver;

using Owner = Reference::EquationInfo::Owner;

namespace
{

// The equations, their gradients and the angle conventions are the neutral
// layer's (JointEquations.h), shared with the Chrono backend's NeutralLink.
using namespace Assembly::Solver::Equations;

using Quat = Eigen::Quaterniond;
using VecX = Eigen::VectorXd;
using MatX = Eigen::MatrixXd;

// A dependent equation still unsatisfied after the solve by more than this is
// a conflict (OndselSession's conflictResidualTolerance).
constexpr double conflictTolerance = 1.0e-6;
// An equation is dependent when what is left of its gradient after taking out
// the span of the equations before it is below this, relative to the
// gradient's norm (at least 1).
constexpr double rankTolerance = 1.0e-8;
// The metric of a rotation against a translation: Euler parameters move by
// half the angle.
constexpr double rotationWeight = 0.25;

struct Pose
{
    Vec3 x = Vec3::Zero();
    Quat q = Quat::Identity();
    Mat3 R = Mat3::Identity();

    void normalise()
    {
        if (!(q.norm() > 0.0) || !std::isfinite(q.norm())) {
            q = Quat::Identity();
        }
        q.normalize();
        R = q.toRotationMatrix();
    }
};

Pose toPose(const Base::Placement& plc)
{
    Pose pose;
    const Base::Vector3d& b = plc.getPosition();
    pose.x = Vec3(b.x, b.y, b.z);
    double x, y, z, w;
    plc.getRotation().getValue(x, y, z, w);
    pose.q = Quat(w, x, y, z);
    pose.normalise();
    return pose;
}

Base::Placement toPlacement(const Pose& pose)
{
    return Base::Placement(
        Base::Vector3d(pose.x.x(), pose.x.y(), pose.x.z()),
        Base::Rotation(pose.q.x(), pose.q.y(), pose.q.z(), pose.q.w())
    );
}

Quat expRotation(const Vec3& w)
{
    const double angle = w.norm();
    if (angle < 1e-300) {
        return Quat::Identity();
    }
    return Quat(Eigen::AngleAxisd(angle, w / angle));
}

// The rotation vector of q, angle in [0, pi].
Vec3 logRotation(Quat q)
{
    if (q.w() < 0.0) {
        q.coeffs() *= -1.0;
    }
    const Vec3 v = q.vec();
    const double s = v.norm();
    if (s < 1e-300) {
        return 2.0 * v;
    }
    return v / s * (2.0 * std::atan2(s, q.w()));
}

// ---------------------------------------------------------------- equations

// An equation (its raw residual, natural error and category) and its owner.
struct Row: Equation
{
    Owner owner = Owner::Joint;
    Id id = NoId;
    int bound = -1;  // a limit's bound: 2 * limit + (0 min, 1 max)
};

// The equations of a model at some body poses. Holds what a session keeps
// between evaluations: the frames' local placements, each coupler's constant
// and the unwrapped angles it measures (OndselSolver unwraps them across
// iterations the same way), and which limit bounds are active.
class System
{
public:
    // The angles a set of poses measures, unwrapped against the last committed.
    struct Angles
    {
        std::vector<std::array<double, 2>> joint;
        std::vector<double> limit;
        std::vector<double> driver;
    };

    // After pin().
    void init(const Model& m, const std::vector<Pose>& start)
    {
        model = &m;
        error.clear();
        coaxial.assign(m.joints.size(), 0);
        constants.assign(m.joints.size(), 0.0);
        angles.joint.assign(m.joints.size(), {0.0, 0.0});
        angles.limit.assign(m.limits.size(), 0.0);
        angles.driver.assign(m.drivers.size(), 0.0);
        activeBound.assign(2 * m.limits.size(), 0);

        // First readings, OndselSolver's branch, then the couplers' constants:
        // each holds the relation it has at the start.
        for (const auto& joint : m.joints) {
            if (!valid(joint.frameI) || !valid(joint.frameJ)) {
                continue;
            }
            if (joint.kind == JointKind::Gear || joint.kind == JointKind::CoaxialGear) {
                FrameState fi = frameState(joint.frameI, start);
                FrameState fj = frameState(joint.frameJ, start);
                coaxial[joint.id] = framesAreCoaxial(fi, fj);
                if (coaxial[joint.id] && !valid(joint.frameK)) {
                    error = "a coaxial gear needs a carrier frame";
                    continue;
                }
                if (joint.value2 == 0.0) {
                    error = "a gear radius is zero";
                    continue;
                }
            }
            if (joint.kind == JointKind::PointOnCurve && !curveOf(joint)) {
                error = "a point-on-curve joint has no curve";
                continue;
            }
            if (jointAngleSlots(joint.kind) > 0) {
                const FrameState fi = frameState(joint.frameI, start);
                const FrameState fj = frameState(joint.frameJ, start);
                FrameState fk;
                const bool carrier = valid(joint.frameK);
                if (carrier) {
                    fk = frameState(joint.frameK, start);
                }
                const JointState state = startState(
                    joint,
                    coaxial[joint.id],
                    fi,
                    fj,
                    carrier ? &fk : nullptr,
                    curveOf(joint)
                );
                angles.joint[joint.id] = state.angles;
                constants[joint.id] = state.constant;
            }
        }
        for (const auto& limit : m.limits) {
            if (limit.kind == LimitKind::RotationZ && valid(limit.frameI) && valid(limit.frameJ)) {
                auto [s, c] = rawAngleZ(limit.frameI, limit.frameJ, start);
                angles.limit[limit.id] = initialAngle(s, c);
            }
        }
        alignDrivers(start);
    }

    // The bodies the Ground records pin are not variables: each is put where
    // its first Ground holds it ('adjust'; a body already there within 1e-12
    // keeps its pose bit for bit) and stays there. Further Ground records on
    // the same body are equations without a gradient, dependent on the first
    // when they agree with it.
    void pin(const Model& m, std::vector<Pose>& poses, bool adjust)
    {
        model = &m;
        pinned.assign(m.bodies.size(), 0);
        locals.clear();
        for (const auto& frame : m.frames) {
            locals.push_back(toPose(frame.local));
        }
        for (const auto& ground : m.grounds) {
            if (!valid(ground.frameI) || !valid(ground.frameJ)) {
                continue;
            }
            const Id body = m.frames[ground.frameJ].body;
            if (body == NoId || body >= poses.size() || m.frames[ground.frameI].body != NoId
                || pinned[body]) {
                continue;
            }
            pinned[body] = 1;
            if (!adjust) {
                continue;
            }
            // body * localJ = frame I
            const Pose& fi = locals[ground.frameI];
            const Pose& lj = locals[ground.frameJ];
            Pose want;
            want.q = fi.q * lj.q.inverse();
            want.normalise();
            want.x = fi.x - want.R * lj.x;
            Pose& have = poses[body];
            if ((want.x - have.x).norm() > 1e-12
                || logRotation(want.q * have.q.inverse()).norm() > 1e-12) {
                have = want;
            }
        }
    }

    std::size_t pinnedCount() const
    {
        return static_cast<std::size_t>(std::count(pinned.begin(), pinned.end(), 1));
    }

    // A PointOnCurve's curve; null for another kind, or a curve not in the
    // model.
    const CurveGeometry* curveOf(const Joint& joint) const
    {
        if (joint.kind != JointKind::PointOnCurve || !model || joint.curve >= model->curves.size()) {
            return nullptr;
        }
        return model->curves[joint.curve].geometry.get();
    }

    bool valid(Id frame) const
    {
        if (!model || frame >= model->frames.size()) {
            return false;
        }
        const Id body = model->frames[frame].body;
        return body == NoId || body < model->bodies.size();
    }

    FrameState frameState(Id frame, const std::vector<Pose>& poses) const
    {
        FrameState f;
        const Pose& local = locals[frame];
        const Id body = model->frames[frame].body;
        if (body == NoId || body >= poses.size()) {
            f.o = local.x;
            f.A = local.R;
            return f;
        }
        const Pose& p = poses[body];
        // A pinned body is not a variable: its frames have no gradient.
        f.body = body < pinned.size() && pinned[body] ? -1 : static_cast<int>(body);
        f.base = p.x;
        f.o = p.x + p.R * local.x;
        f.A = p.R * local.R;
        return f;
    }

    // Every equation at the poses, grounds first, then joints, drivers and the
    // active limit bounds, each in model order. 'measured' receives the
    // unwrapped angles, for commit() once the poses are taken.
    void evaluate(const std::vector<Pose>& poses, std::vector<Row>& rows, Angles& measured) const
    {
        rows.clear();
        measured = angles;
        std::vector<Equation> scratch;
        for (const auto& ground : model->grounds) {
            if (!valid(ground.frameI) || !valid(ground.frameJ)) {
                continue;
            }
            scratch.clear();
            fixedRows(frameState(ground.frameI, poses), frameState(ground.frameJ, poses), scratch);
            append(rows, scratch, Owner::Ground, ground.id);
        }
        for (const auto& joint : model->joints) {
            jointRows(joint, poses, rows, measured, scratch);
        }
        for (const auto& driver : model->drivers) {
            driverRows(driver, poses, rows, measured);
        }
        for (const auto& limit : model->limits) {
            // Every limit's angle is read, active or not, so its count of
            // turns follows the motion.
            const S value = limitValue(limit, poses, measured);
            for (int side = 0; side < 2; ++side) {
                if (activeBound[2 * limit.id + side]) {
                    const std::optional<double>& bound = side == 0 ? limit.min : limit.max;
                    if (!bound) {
                        continue;
                    }
                    Row row;
                    row.s = value - constant(*bound);
                    row.natural = row.s.v;
                    row.category = limit.kind == LimitKind::RotationZ ? Category::Essential
                                                                      : Category::Displacement;
                    row.owner = Owner::Limit;
                    row.id = limit.id;
                    row.bound = 2 * static_cast<int>(limit.id) + side;
                    rows.push_back(row);
                }
            }
        }
    }

    void commit(const Angles& measured)
    {
        angles = measured;
    }

    // Each PointOnCurve's parameter set to the closest point over its whole
    // curve at these poses: for the checker, which measures a joint at poses
    // it did not follow the motion to.
    void seedCurves(const std::vector<Pose>& poses)
    {
        for (const auto& joint : model->joints) {
            const CurveGeometry* curve = curveOf(joint);
            if (!curve || !valid(joint.frameI) || !valid(joint.frameJ)) {
                continue;
            }
            const Base::Vector3d x =
                localPoint(frameState(joint.frameI, poses), frameState(joint.frameJ, poses));
            angles.joint[joint.id][0] = projectOnCurveGlobally(*curve, x).u;
        }
    }

    // A driver's value at 'time'; false when it does not drive (an empty
    // half) or has no evaluator.
    bool driverValue(const Formula& formula, double& value) const
    {
        if (formula.text.empty() && !formula.tree && !formula.function) {
            return false;
        }
        auto evaluate = formula.evaluator();
        if (!evaluate) {
            return false;
        }
        value = evaluate(time).value;
        return true;
    }

    // Each rotation driver's angle is counted from the turn nearest to the
    // formula's value now: the formula is an absolute angle, so a joint at
    // 200 degrees driven by "initialValue + ..." starts where it is, and from
    // there the count of turns follows the motion, as a continuous run would.
    void alignDrivers(const std::vector<Pose>& poses)
    {
        for (const auto& driver : model->drivers) {
            double value = 0.0;
            if (driver.kind == DriverKind::TranslationZ || !valid(driver.frameI)
                || !valid(driver.frameJ) || !driverValue(driver.rotation, value)
                || !std::isfinite(value)) {
                continue;
            }
            auto [s, c] = rawAngleZ(driver.frameI, driver.frameJ, poses);
            angles.driver[driver.id] = value + wrapAngle(std::atan2(s, c) - value);
        }
    }

    // The time the drivers are evaluated at.
    double time = 0.0;

    // What a limit bounds: the slide along zI, or the turn about it.
    S limitValue(const Limit& limit, const std::vector<Pose>& poses, Angles& measured) const
    {
        if (!valid(limit.frameI) || !valid(limit.frameJ)) {
            return {};
        }
        FrameState fi = frameState(limit.frameI, poses);
        FrameState fj = frameState(limit.frameJ, poses);
        if (limit.kind == LimitKind::TranslationZ) {
            return dot(origin(fj) - origin(fi), axis(fi, 2));
        }
        S a = angleZ(fi, fj);
        a.v = unwrap(a.v, angles.limit[limit.id]);
        measured.limit[limit.id] = a.v;
        return a;
    }

    const Model* model = nullptr;
    std::vector<Pose> locals;
    std::vector<char> pinned;
    std::vector<char> coaxial;
    std::vector<double> constants;
    std::vector<char> activeBound;
    Angles angles;
    std::string error;

private:
    std::pair<double, double> rawAngleZ(Id i, Id j, const std::vector<Pose>& poses) const
    {
        return Equations::rawAngleZ(frameState(i, poses), frameState(j, poses));
    }

    static void push(std::vector<Row>& rows, const S& s, Category category)
    {
        Row row;
        row.s = s;
        row.natural = s.v;
        row.category = category;
        rows.push_back(row);
    }

    // The equations, owned.
    static void
    append(std::vector<Row>& rows, const std::vector<Equation>& equations, Owner owner, Id id)
    {
        for (const auto& equation : equations) {
            Row row;
            static_cast<Equation&>(row) = equation;
            row.owner = owner;
            row.id = id;
            rows.push_back(row);
        }
    }

    // The joint's equations (JointEquations.h), with this system's constant
    // and angles for it.
    void jointRows(
        const Joint& joint,
        const std::vector<Pose>& poses,
        std::vector<Row>& rows,
        Angles& measured,
        std::vector<Equation>& scratch
    ) const
    {
        if (!valid(joint.frameI) || !valid(joint.frameJ)) {
            return;
        }
        const FrameState fi = frameState(joint.frameI, poses);
        const FrameState fj = frameState(joint.frameJ, poses);
        FrameState fk;
        const bool carrier = valid(joint.frameK);
        if (carrier) {
            fk = frameState(joint.frameK, poses);
        }
        JointState state;
        state.coaxial = coaxial[joint.id] != 0;
        state.constant = constants[joint.id];
        state.angles = angles.joint[joint.id];
        scratch.clear();
        Equations::jointRows(
            joint,
            fi,
            fj,
            carrier ? &fk : nullptr,
            state,
            false,
            scratch,
            measured.joint[joint.id],
            curveOf(joint)
        );
        append(rows, scratch, Owner::Joint, joint.id);
    }

    // A driver held at its formula's value at 'time' (a static solve: time 0).
    void driverRows(
        const Driver& driver,
        const std::vector<Pose>& poses,
        std::vector<Row>& rows,
        Angles& measured
    ) const
    {
        if (!valid(driver.frameI) || !valid(driver.frameJ)) {
            return;
        }
        const FrameState fi = frameState(driver.frameI, poses);
        const FrameState fj = frameState(driver.frameJ, poses);
        auto valueOf = [&](const Formula& formula, double& value) {
            return driverValue(formula, value) && std::isfinite(value);
        };
        const std::size_t first = rows.size();
        double value = 0.0;
        if (driver.kind != DriverKind::TranslationZ && valueOf(driver.rotation, value)) {
            // The turn of J about zI with its turns counted (alignDrivers):
            // the formula's angle itself -- not its sine, which OndselSolver's
            // ZRotation holds and which is zero half a turn away too, and not
            // the angle modulo a turn, which a gear downstream cannot tell
            // from the motion it follows.
            S thez = angleZ(fi, fj);
            thez.v = unwrap(thez.v, angles.driver[driver.id]);
            measured.driver[driver.id] = thez.v;
            push(rows, thez - constant(value), Category::Essential);
        }
        if (driver.kind != DriverKind::RotationZ && valueOf(driver.translation, value)) {
            const S z = dot(origin(fj) - origin(fi), axis(fi, 2));
            push(rows, z - constant(value), Category::Essential);
        }
        for (std::size_t i = first; i < rows.size(); ++i) {
            rows[i].owner = Owner::Driver;
            rows[i].id = driver.id;
        }
    }
};

// ---------------------------------------------------------------- linear algebra

MatX jacobian(const std::vector<Row>& rows, const std::vector<int>& select, std::size_t bodies)
{
    MatX J = MatX::Zero(
        static_cast<Eigen::Index>(select.size()),
        static_cast<Eigen::Index>(6 * bodies)
    );
    for (std::size_t r = 0; r < select.size(); ++r) {
        const Grad& g = rows[select[r]].s.g;
        for (int k = 0; k < g.n; ++k) {
            J.block<1, 6>(static_cast<Eigen::Index>(r), 6 * g.body[k]) = g.g[k];
        }
    }
    return J;
}

// The rows in the order the rank analysis takes them: OndselSolver's
// categories (essential, displacement, perpendicular), then model order.
std::vector<int> analysisOrder(const std::vector<Row>& rows)
{
    std::vector<int> order(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        order[i] = static_cast<int>(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return static_cast<int>(rows[a].category) < static_cast<int>(rows[b].category);
    });
    return order;
}

// Whether each row is independent of the rows taken before it (in 'order'),
// by Gram-Schmidt with reorthogonalisation.
std::vector<char>
independentRows(const std::vector<Row>& rows, const std::vector<int>& order, std::size_t bodies)
{
    const Eigen::Index n = static_cast<Eigen::Index>(6 * bodies);
    std::vector<char> independent(rows.size(), 0);
    MatX Q(n, std::min<Eigen::Index>(n, static_cast<Eigen::Index>(rows.size())) + 1);
    Eigen::Index rank = 0;
    VecX v(n);
    for (int i : order) {
        if (rank >= n) {
            break;
        }
        v.setZero();
        const Grad& g = rows[i].s.g;
        for (int k = 0; k < g.n; ++k) {
            v.segment<6>(6 * g.body[k]) = g.g[k].transpose();
        }
        const double norm0 = v.norm();
        if (!(norm0 > rankTolerance)) {
            continue;
        }
        for (int pass = 0; pass < 2 && rank > 0; ++pass) {
            v -= Q.leftCols(rank) * (Q.leftCols(rank).transpose() * v);
        }
        const double rest = v.norm();
        if (rest > rankTolerance * std::max(1.0, norm0)) {
            Q.col(rank++) = v / rest;
            independent[i] = 1;
        }
    }
    return independent;
}

// The rank by column-pivoted QR, rows scaled to unit length (which changes no
// rank, and makes the threshold mean the same for mm and for cosines).
int qrRank(const std::vector<Row>& rows, std::size_t bodies)
{
    if (rows.empty() || bodies == 0) {
        return 0;
    }
    std::vector<int> all(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        all[i] = static_cast<int>(i);
    }
    MatX J = jacobian(rows, all, bodies);
    for (Eigen::Index r = 0; r < J.rows(); ++r) {
        const double n = J.row(r).norm();
        if (n > 0.0) {
            J.row(r) /= n;
        }
    }
    Eigen::ColPivHouseholderQR<MatX> qr(J);
    qr.setThreshold(rankTolerance);
    return static_cast<int>(qr.rank());
}

std::vector<Pose> applyStep(const std::vector<Pose>& poses, const VecX& dq, double alpha)
{
    std::vector<Pose> out = poses;
    for (std::size_t b = 0; b < out.size(); ++b) {
        const auto i = static_cast<Eigen::Index>(6 * b);
        out[b].x += alpha * dq.segment<3>(i);
        out[b].q = expRotation(alpha * dq.segment<3>(i + 3)) * out[b].q;
        out[b].normalise();
    }
    return out;
}

// The step from 'from' to 'to': positions, and the spatial rotation vector.
VecX difference(const std::vector<Pose>& to, const std::vector<Pose>& from)
{
    VecX d(static_cast<Eigen::Index>(6 * from.size()));
    for (std::size_t b = 0; b < from.size(); ++b) {
        const auto i = static_cast<Eigen::Index>(6 * b);
        d.segment<3>(i) = to[b].x - from[b].x;
        d.segment<3>(i + 3) = logRotation(to[b].q * from[b].q.inverse());
    }
    return d;
}

double objective(const std::vector<Pose>& poses, const std::vector<Pose>& target, const VecX& W)
{
    const VecX d = difference(target, poses);
    return 0.5 * d.dot(W.cwiseProduct(d));
}

double maxAbs(const std::vector<Row>& rows, const std::vector<int>& select)
{
    double m = 0.0;
    for (int i : select) {
        const double v = std::abs(rows[i].s.v);
        m = std::max(m, std::isfinite(v) ? v : std::numeric_limits<double>::infinity());
    }
    return m;
}

double norm2(const std::vector<Row>& rows, const std::vector<int>& select)
{
    double s = 0.0;
    for (int i : select) {
        s += rows[i].s.v * rows[i].s.v;
    }
    return std::isfinite(s) ? std::sqrt(s) : std::numeric_limits<double>::infinity();
}

}  // namespace

// ---------------------------------------------------------------- the checker

namespace
{

std::vector<double> ownerResiduals(
    const Model& model,
    const std::vector<Base::Placement>& placements,
    Owner owner
)
{
    std::vector<Pose> start;
    start.reserve(model.bodies.size());
    for (const auto& body : model.bodies) {
        start.push_back(toPose(body.placement));
    }
    std::vector<Pose> poses = start;
    for (std::size_t i = 0; i < poses.size() && i < placements.size(); ++i) {
        poses[i] = toPose(placements[i]);
    }
    System system;
    system.pin(model, start, false);
    system.init(model, start);
    system.seedCurves(poses);
    std::vector<Row> rows;
    System::Angles measured;
    system.evaluate(poses, rows, measured);
    const std::size_t count = owner == Owner::Joint ? model.joints.size() : model.grounds.size();
    std::vector<double> out(count, std::numeric_limits<double>::quiet_NaN());
    std::vector<char> seen(count, 0);
    for (const auto& row : rows) {
        if (row.owner != owner || row.id >= count) {
            continue;
        }
        const double e = std::abs(row.natural);
        out[row.id] = seen[row.id] ? std::max(out[row.id], e) : e;
        seen[row.id] = 1;
    }
    return out;
}

}  // namespace

std::vector<double>
Reference::jointResiduals(const Model& model, const std::vector<Base::Placement>& poses)
{
    return ownerResiduals(model, poses, Owner::Joint);
}

std::vector<double>
Reference::groundResiduals(const Model& model, const std::vector<Base::Placement>& poses)
{
    return ownerResiduals(model, poses, Owner::Ground);
}

// ---------------------------------------------------------------- backend

std::string ReferenceBackend::name() const
{
    return "Reference";
}

std::string ReferenceBackend::label() const
{
    return "Reference (Eigen)";
}

Capabilities ReferenceBackend::capabilities() const
{
    Capabilities caps;
    caps.joints.set();
    caps.limits = true;
    caps.drivers = true;
    caps.drag = true;
    caps.kinematics = true;  // by position solves (sec 4.6)
    caps.redundancy = true;
    caps.conflicts = true;
    caps.nativeFormat = "neutral";
    return caps;
}

std::unique_ptr<Session> ReferenceBackend::createSession()
{
    return std::make_unique<ReferenceSession>();
}

// ---------------------------------------------------------------- session

struct ReferenceSession::Impl
{
    Model model;
    bool loaded = false;
    std::vector<Pose> poses;
    System system;
    std::vector<Id> dragBodies;
    int iterations = 0;

    // Residuals the solve drives the kept equations to; a solve that stalls
    // above 'tight' but below 'loose' still counts as converged.
    double tight = 1e-11;
    double loose = 1e-9;
    int maxRestore = 100;
    int maxRounds = 400;
    double stepTolerance = 1e-11;
    // Rounds of projected gradient before the reduced Newton step is tried.
    int newtonAfter = 6;

    std::vector<Row> rows;
    System::Angles measured;

    void evaluate()
    {
        system.evaluate(poses, rows, measured);
    }

    std::vector<int> allRows() const
    {
        std::vector<int> all(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            all[i] = static_cast<int>(i);
        }
        return all;
    }

    // The rows independent at the current poses (evaluate() first).
    std::vector<int> independentSelection() const
    {
        const auto order = analysisOrder(rows);
        const auto independent = independentRows(rows, order, poses.size());
        std::vector<int> select;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (independent[i]) {
                select.push_back(static_cast<int>(i));
            }
        }
        return select;
    }

    VecX weights(const std::vector<double>& bodyWeight) const
    {
        VecX W(static_cast<Eigen::Index>(6 * poses.size()));
        for (std::size_t b = 0; b < poses.size(); ++b) {
            const double w = b < bodyWeight.size() && bodyWeight[b] > 0.0 ? bodyWeight[b] : 1.0;
            for (int k = 0; k < 3; ++k) {
                W[static_cast<Eigen::Index>(6 * b + k)] = w;
                W[static_cast<Eigen::Index>(6 * b + 3 + k)] = w * rotationWeight;
            }
        }
        return W;
    }

    // The metric of the restoration: a turn weighed by the square of the
    // body's largest lever arm (at least 1 mm), so that a step that a
    // translation can make is not made by turning a body through radians,
    // where the linearisation is worthless. Where the restoration lands does
    // not decide the answer: project() then moves along the manifold to the
    // pose closest to the target in the solve's own metric.
    VecX restoreWeights() const
    {
        std::vector<double> arm(poses.size(), 1.0);
        for (std::size_t f = 0; f < model.frames.size(); ++f) {
            const Id body = model.frames[f].body;
            if (body < arm.size()) {
                arm[body] = std::max(arm[body], system.locals[f].x.norm());
            }
        }
        VecX W(static_cast<Eigen::Index>(6 * poses.size()));
        for (std::size_t b = 0; b < poses.size(); ++b) {
            for (int k = 0; k < 3; ++k) {
                W[static_cast<Eigen::Index>(6 * b + k)] = 1.0;
                W[static_cast<Eigen::Index>(6 * b + 3 + k)] = arm[b] * arm[b];
            }
        }
        return W;
    }

    // Gauss-Newton to the kept equations, each step the smallest in the
    // restoration's metric, no body turned more than a radian in one step,
    // damped (Levenberg-Marquardt) and backtracked when it does not reduce
    // the residual.
    bool restore(const std::vector<int>& select)
    {
        const VecX Winv = restoreWeights().cwiseInverse();
        double mu = 0.0;
        std::vector<Row> trialRows;
        System::Angles trialMeasured;
        for (int it = 0; it < maxRestore; ++it) {
            evaluate();
            const double worst = maxAbs(rows, select);
            if (worst <= tight || select.empty()) {
                system.commit(measured);
                return true;
            }
            ++iterations;
            const MatX J = jacobian(rows, select, poses.size());
            VecX c(static_cast<Eigen::Index>(select.size()));
            for (std::size_t r = 0; r < select.size(); ++r) {
                c[static_cast<Eigen::Index>(r)] = rows[select[r]].s.v;
            }
            const MatX JW = J * Winv.asDiagonal();
            MatX A = JW * J.transpose();
            const double scale = std::max(1.0, A.diagonal().maxCoeff());
            A.diagonal().array() += mu + 1e-14 * scale;
            const VecX y = A.ldlt().solve(-c);
            VecX dq = JW.transpose() * y;
            double turn = 0.0;
            for (std::size_t b = 0; b < poses.size(); ++b) {
                turn = std::max(turn, dq.segment<3>(static_cast<Eigen::Index>(6 * b + 3)).norm());
            }
            if (turn > 1.0) {
                dq /= turn;
            }
            const double before = norm2(rows, select);
            bool accepted = false;
            double alpha = 1.0;
            for (int k = 0; k < 12 && dq.allFinite(); ++k, alpha *= 0.5) {
                const auto trial = applyStep(poses, dq, alpha);
                system.evaluate(trial, trialRows, trialMeasured);
                if (norm2(trialRows, select) < (1.0 - 1e-4 * alpha) * before) {
                    poses = trial;
                    system.commit(trialMeasured);
                    accepted = true;
                    break;
                }
            }
            if (accepted) {
                mu = mu * 0.1 < 1e-20 * scale ? 0.0 : mu * 0.1;
                continue;
            }
            if (worst <= loose) {
                system.commit(measured);
                return true;
            }
            mu = mu == 0.0 ? 1e-6 * scale : mu * 10.0;
            if (mu > 1e12 * scale) {
                return false;
            }
        }
        evaluate();
        if (maxAbs(rows, select) <= loose) {
            system.commit(measured);
            return true;
        }
        return false;
    }

    // The point of the manifold of the kept equations closest to 'target' in
    // the metric W: restoration, then steps along the manifold (the target
    // direction with its component normal to the manifold taken out), until
    // that step vanishes or no longer brings the poses closer.
    bool project(const std::vector<Pose>& target, const VecX& W, const std::vector<int>& select)
    {
        const VecX Winv = W.cwiseInverse();
        for (int round = 0; round < maxRounds; ++round) {
            if (!restore(select)) {
                return false;
            }
            evaluate();
            VecX d0 = difference(target, poses);
            for (std::size_t b = 0; b < poses.size(); ++b) {
                if (system.pinned[b]) {
                    d0.segment<6>(static_cast<Eigen::Index>(6 * b)).setZero();
                }
            }
            VecX t = d0;
            if (!select.empty()) {
                const MatX J = jacobian(rows, select, poses.size());
                const MatX JW = J * Winv.asDiagonal();
                MatX A = JW * J.transpose();
                const double scale = std::max(1.0, A.diagonal().maxCoeff());
                A.diagonal().array() += 1e-14 * scale;
                const VecX y = A.ldlt().solve(J * d0);
                t = d0 - JW.transpose() * y;
            }
            if (!t.allFinite() || t.lpNorm<Eigen::Infinity>() <= stepTolerance) {
                return true;
            }
            const double f0 = objective(poses, target, W);
            const auto saved = poses;
            const auto savedAngles = system.angles;
            // Along the manifold, with the poses restored onto it after the
            // step; taken when it brings them closer to the target, halved
            // until it does. The projected gradient converges only linearly
            // where the manifold curves within the distance to the target, so
            // after a few rounds the reduced Newton step is tried first.
            auto tryStep = [&](VecX step, int halvings) {
                // At most a radian per body, as in restore().
                double turn = 0.0;
                for (std::size_t b = 0; b < poses.size(); ++b) {
                    const auto i = static_cast<Eigen::Index>(6 * b + 3);
                    turn = std::max(turn, step.segment<3>(i).norm());
                }
                if (turn > 1.0) {
                    step /= turn;
                }
                double beta = 1.0;
                for (int k = 0; k < halvings; ++k, beta *= 0.5) {
                    poses = applyStep(saved, step, beta);
                    if (restore(select) && objective(poses, target, W) < f0) {
                        return true;
                    }
                    poses = saved;
                    system.angles = savedAngles;
                }
                return false;
            };
            VecX newton;
            if (round >= newtonAfter && newtonStep(W, select, d0, newton) && tryStep(newton, 8)) {
                continue;
            }
            if (!tryStep(t, 30)) {
                return true;  // as close as rounding lets it get
            }
        }
        return true;
    }

    // The reduced Newton step towards the target at a point of the manifold
    // (evaluate() first): the Hessian of the Lagrangian -- the metric, less
    // the multipliers times the equations' second derivatives, by central
    // differences of the analytic gradients -- on the null space of the kept
    // equations. False when that is not positive definite (then the
    // projected gradient goes on).
    bool newtonStep(const VecX& W, const std::vector<int>& select, const VecX& d0, VecX& out)
    {
        const auto N = static_cast<Eigen::Index>(6 * poses.size());
        const VecX Winv = W.cwiseInverse();
        const MatX J = jacobian(rows, select, poses.size());
        const VecX gradient = -W.cwiseProduct(d0);
        VecX lambda = VecX::Zero(J.rows());
        if (J.rows() > 0) {
            const MatX JW = J * Winv.asDiagonal();
            MatX A = JW * J.transpose();
            const double scale = std::max(1.0, A.diagonal().maxCoeff());
            A.diagonal().array() += 1e-14 * scale;
            lambda = A.ldlt().solve(JW * gradient);
        }
        auto weighted = [&](const std::vector<Pose>& p) {
            std::vector<Row> rr;
            System::Angles scratch;
            system.evaluate(p, rr, scratch);
            return VecX(jacobian(rr, select, p.size()).transpose() * lambda);
        };
        MatX H = W.asDiagonal();
        const double h = 1e-6;
        for (Eigen::Index k = 0; k < N; ++k) {
            if (system.pinned[static_cast<std::size_t>(k / 6)]) {
                continue;
            }
            VecX e = VecX::Zero(N);
            e[k] = h;
            H.col(k) -= (weighted(applyStep(poses, e, 1.0)) - weighted(applyStep(poses, e, -1.0)))
                / (2.0 * h);
        }
        for (Eigen::Index k = 0; k < N; ++k) {
            if (system.pinned[static_cast<std::size_t>(k / 6)]) {
                H.row(k).setZero();
                H.col(k).setZero();
                H(k, k) = W[k];
            }
        }
        H = 0.5 * (H + H.transpose()).eval();
        MatX Z;
        if (J.rows() > 0) {
            Eigen::FullPivLU<MatX> lu(J);
            Z = lu.kernel();
        }
        else {
            Z = MatX::Identity(N, N);
        }
        if (Z.cols() == 0 || (Z.cols() == 1 && Z.norm() == 0.0)) {
            return false;
        }
        const MatX Hr = Z.transpose() * H * Z;
        Eigen::LDLT<MatX> ldlt(Hr);
        if (ldlt.info() != Eigen::Success || !ldlt.isPositive()
            || !(ldlt.vectorD().minCoeff() > 1e-12 * std::max(1.0, ldlt.vectorD().maxCoeff()))) {
            return false;
        }
        out = Z * ldlt.solve(-(Z.transpose() * gradient));
        return out.allFinite();
    }

    // The equations solved to the target, with the active limit bounds as
    // equations: the rows independent at the start, then any row that the
    // solution shows to be independent and unsatisfied.
    bool constrained(const std::vector<Pose>& target, const VecX& W)
    {
        evaluate();
        std::vector<int> select = independentSelection();
        for (int pass = 0; pass < 4; ++pass) {
            if (!project(target, W, select)) {
                return false;
            }
            evaluate();
            std::vector<int> now = independentSelection();
            bool more = false;
            for (int i : now) {
                if (!std::binary_search(select.begin(), select.end(), i)
                    && std::abs(rows[i].s.v) > tight) {
                    more = true;
                }
            }
            if (!more) {
                return true;
            }
            select = std::move(now);
        }
        return true;
    }

    // The multipliers of the kept equations at a solution:
    // W (q - target) = J^T lambda.
    VecX multipliers(const std::vector<Pose>& target, const VecX& W, const std::vector<int>& select)
    {
        const MatX J = jacobian(rows, select, poses.size());
        const VecX Winv = W.cwiseInverse();
        const MatX JW = J * Winv.asDiagonal();
        MatX A = JW * J.transpose();
        const double scale = std::max(1.0, A.diagonal().maxCoeff());
        A.diagonal().array() += 1e-14 * scale;
        const VecX gradient = W.cwiseProduct(-difference(target, poses));  // W (q - target)
        return A.ldlt().solve(JW * gradient);
    }

    // Whether a limit bound is violated at the current poses.
    bool violated(const Limit& limit, int side)
    {
        const std::optional<double>& bound = side == 0 ? limit.min : limit.max;
        if (!bound) {
            return false;
        }
        System::Angles scratch = system.angles;
        const double value = system.limitValue(limit, poses, scratch).v;
        const double tol = std::max(limit.tolerance, 0.0);
        return side == 0 ? value < *bound - tol : value > *bound + tol;
    }

    // The solve: the pose closest to the target under the equations and the
    // limits (active set).
    Status minimise(
        const std::vector<Pose>& target,
        const std::vector<double>& bodyWeight,
        std::string& message
    )
    {
        if (!system.error.empty()) {
            message = system.error;
            return Status::Failed;
        }
        const VecX W = weights(bodyWeight);
        std::fill(system.activeBound.begin(), system.activeBound.end(), 0);
        const int passes = 2 * static_cast<int>(model.limits.size()) + 2;
        for (int pass = 0; pass < passes; ++pass) {
            if (!constrained(target, W)) {
                message = "the equations could not be satisfied";
                return Status::NotConverged;
            }
            bool changed = false;
            for (const auto& limit : model.limits) {
                for (int side = 0; side < 2; ++side) {
                    if (!system.activeBound[2 * limit.id + side] && violated(limit, side)) {
                        system.activeBound[2 * limit.id + side] = 1;
                        changed = true;
                    }
                }
            }
            if (!changed) {
                // Release the bound pulling hardest the wrong way, if any.
                evaluate();
                std::vector<int> select = independentSelection();
                bool anyActive = false;
                for (int i : select) {
                    anyActive = anyActive || rows[i].owner == Owner::Limit;
                }
                if (anyActive) {
                    const VecX lambda = multipliers(target, W, select);
                    int worst = -1;
                    double worstValue = 1e-9;
                    for (std::size_t r = 0; r < select.size(); ++r) {
                        const Row& row = rows[select[r]];
                        if (row.owner != Owner::Limit) {
                            continue;
                        }
                        // A min bound holds with lambda >= 0, a max bound with <= 0.
                        const double wrong = (row.bound % 2 == 0 ? -1.0 : 1.0)
                            * lambda[static_cast<Eigen::Index>(r)];
                        if (wrong > worstValue) {
                            worstValue = wrong;
                            worst = row.bound;
                        }
                    }
                    if (worst >= 0) {
                        system.activeBound[worst] = 0;
                        changed = true;
                    }
                }
            }
            if (!changed) {
                return Status::Ok;
            }
        }
        for (const auto& limit : model.limits) {
            for (int side = 0; side < 2; ++side) {
                if (violated(limit, side)) {
                    message = "the limits cannot be satisfied";
                    return Status::NotConverged;
                }
            }
        }
        return Status::Ok;
    }

    std::vector<BodyPose> movedSince(const std::vector<Pose>& before) const
    {
        std::vector<BodyPose> moved;
        for (std::size_t b = 0; b < poses.size(); ++b) {
            const Base::Placement now = toPlacement(poses[b]);
            if (b >= before.size() || !now.isSame(toPlacement(before[b]))) {
                moved.push_back({static_cast<Id>(b), now});
            }
        }
        return moved;
    }

    double residual()
    {
        evaluate();
        double worst = 0.0;
        for (const auto& row : rows) {
            if (row.owner != Owner::Limit) {
                worst = std::max(worst, std::abs(row.s.v));
            }
        }
        return worst;
    }

    Result run(const std::vector<Pose>& target, const std::vector<double>& bodyWeight)
    {
        Result result;
        if (!loaded) {
            result.message = "no model loaded";
            return result;
        }
        const auto before = poses;
        const auto beforeAngles = system.angles;
        iterations = 0;
        std::string message;
        try {
            result.status = minimise(target, bodyWeight, message);
        }
        catch (const std::exception& e) {
            result.status = Status::Failed;
            message = e.what();
        }
        result.message = message;
        result.iterations = iterations;
        if (result.status != Status::Ok) {
            poses = before;
            system.angles = beforeAngles;
        }
        std::fill(system.activeBound.begin(), system.activeBound.end(), 0);
        result.residual = residual();
        result.moved = movedSince(before);
        return result;
    }

    // Rows without limits, for the diagnostics.
    std::vector<Row> diagnosticRows()
    {
        std::fill(system.activeBound.begin(), system.activeBound.end(), 0);
        evaluate();
        return rows;
    }
};

ReferenceSession::ReferenceSession()
    : d(std::make_unique<Impl>())
{}

ReferenceSession::~ReferenceSession() = default;

Status ReferenceSession::load(const Model& model)
{
    d->model = model;
    d->poses.clear();
    d->poses.reserve(model.bodies.size());
    for (const auto& body : d->model.bodies) {
        d->poses.push_back(toPose(body.placement));
    }
    d->dragBodies.clear();
    try {
        d->system.pin(d->model, d->poses, true);
        d->system.init(d->model, d->poses);
    }
    catch (const std::exception&) {
        d->loaded = false;
        return Status::Failed;
    }
    d->loaded = true;
    return Status::Ok;
}

Status ReferenceSession::update(const Model& model)
{
    if (!d->loaded || !sameExceptDragWeights(d->model, model)) {
        return Status::NeedsReload;
    }
    for (std::size_t i = 0; i < d->model.bodies.size(); ++i) {
        d->model.bodies[i].dragWeight = model.bodies[i].dragWeight;
    }
    return Status::Ok;
}

Result ReferenceSession::solve(const SolveOptions& options)
{
    if (options.maxIterations > 0) {
        d->maxRestore = options.maxIterations;
    }
    if (options.tolerance > 0.0) {
        d->loose = std::max(options.tolerance, d->tight);
    }
    // The pose closest to the start, every body weighted alike.
    const auto target = d->poses;
    Result result = d->run(target, {});
    if (result.status == Status::Ok && options.diagnostics) {
        result.diagnostics = analyse();
    }
    return result;
}

Status ReferenceSession::beginDrag(const std::vector<Id>& bodies, const DragOptions& options)
{
    (void)options;
    if (!d->loaded) {
        return Status::Failed;
    }
    d->dragBodies = bodies;
    return Status::Ok;
}

Result ReferenceSession::dragStep(const std::vector<BodyPose>& targets)
{
    if (!d->loaded) {
        Result result;
        result.message = "no model loaded";
        return result;
    }
    // The dragged bodies pulled to their targets with their drag weights; the
    // others held where they are with weight 1.
    auto target = d->poses;
    std::vector<double> weight(d->poses.size(), 1.0);
    for (const auto& t : targets) {
        if (t.body >= target.size()) {
            continue;
        }
        target[t.body] = toPose(t.placement);
        const double w = d->model.bodies[t.body].dragWeight;
        weight[t.body] = w > 0.0 ? w : 1.0;
    }
    Result result = d->run(target, weight);
    if (result.status != Status::Ok) {
        // Half the way, as OndselSolver retries a step that does not converge.
        auto half = d->poses;
        for (const auto& t : targets) {
            if (t.body >= half.size()) {
                continue;
            }
            const Pose goal = toPose(t.placement);
            Pose& p = half[t.body];
            const Vec3 w = logRotation(goal.q * p.q.inverse());
            p.x = 0.5 * (p.x + goal.x);
            p.q = expRotation(0.5 * w) * p.q;
            p.normalise();
        }
        Result retry = d->run(half, weight);
        if (retry.status == Status::Ok) {
            return retry;
        }
    }
    return result;
}

Result ReferenceSession::endDrag()
{
    d->dragBodies.clear();
    // A static solve at the dragged state.
    const auto target = d->poses;
    return d->run(target, {});
}

std::vector<BodyPose> ReferenceSession::poses() const
{
    std::vector<BodyPose> out;
    out.reserve(d->poses.size());
    for (std::size_t b = 0; b < d->poses.size(); ++b) {
        out.push_back({static_cast<Id>(b), toPlacement(d->poses[b])});
    }
    return out;
}

Diagnostics ReferenceSession::analyse()
{
    Diagnostics diagnostics;
    if (!d->loaded) {
        return diagnostics;
    }
    const std::vector<Row> rows = d->diagnosticRows();
    const std::size_t bodies = d->poses.size();
    const auto order = analysisOrder(rows);
    const auto independent = independentRows(rows, order, bodies);
    const int rank = qrRank(rows, bodies);
    int kept = 0;
    for (char c : independent) {
        kept += c ? 1 : 0;
    }
    if (kept != rank) {
        Base::Console().log(
            "Assembly: the Reference solver's rank by QR (%d) and by Gram-Schmidt (%d) differ\n",
            rank,
            kept
        );
    }
    // OndselSession's convention (sec 6.6, deviation 3): 6 x (1 + bodies)
    // less the equations kept, the assembly origin counted as a body and
    // each grounded body held by six equations of its first Ground.
    diagnostics.dof = static_cast<int>(6 * (1 + bodies - d->system.pinnedCount())) - rank;

    std::vector<int> total(d->model.joints.size(), 0);
    std::vector<int> dependent(d->model.joints.size(), 0);
    std::vector<char> conflict(d->model.joints.size(), 0);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        if (row.owner != Owner::Joint || row.id >= total.size()) {
            continue;
        }
        ++total[row.id];
        if (!independent[i]) {
            ++dependent[row.id];
            if (!(std::abs(row.s.v) <= conflictTolerance)) {
                conflict[row.id] = 1;
            }
        }
    }
    for (std::size_t j = 0; j < total.size(); ++j) {
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
        | Diagnostics::KnownConflicting | Diagnostics::KnownPartiallyRedundant;
    return diagnostics;
}

Status ReferenceSession::simulate(const SimulationSpec& spec, TrajectorySink& sink)
{
    // Position solves at each output time, as simulateByPositionSolves does,
    // but in one session: each frame starts from the last one's poses and
    // counts of turns, so a driver turning more than half a turn between two
    // output times, and a gear it turns, follow the motion and not the
    // nearest pose.
    if (!d->loaded) {
        return Status::Failed;
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
    const auto steps = static_cast<std::size_t>(
        std::floor((spec.tEnd - spec.tStart) / spec.tOutput + 1e-9)
    );
    d->system.time = spec.tStart;
    d->system.alignDrivers(d->poses);
    Status status = Status::Ok;
    for (std::size_t k = 0; k <= steps; ++k) {
        const double time = spec.tStart + static_cast<double>(k) * spec.tOutput;
        d->system.time = time;
        // Outside a formula's domain the run stops (sec 4.6.1).
        for (const auto& driver : d->model.drivers) {
            for (const Formula* formula : {&driver.rotation, &driver.translation}) {
                double value = 0.0;
                if (d->system.driverValue(*formula, value) && !std::isfinite(value)) {
                    status = Status::Failed;
                }
            }
        }
        if (status != Status::Ok) {
            break;
        }
        const auto target = d->poses;
        Result result = d->run(target, {});
        if (result.status != Status::Ok) {
            status = result.status;
            break;
        }
        if (!sink.frame(time, poses())) {
            status = Status::Cancelled;
            break;
        }
    }
    d->system.time = 0.0;
    return status;
}

Status ReferenceSession::exportNative(const std::string& path)
{
    if (!d->loaded) {
        return Status::Failed;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return Status::Failed;
    }
    out.precision(17);
    const Model& m = d->model;
    auto plc = [&](const Base::Placement& p) {
        double x, y, z, w;
        p.getRotation().getValue(x, y, z, w);
        const Base::Vector3d& b = p.getPosition();
        out << b.x << ' ' << b.y << ' ' << b.z << ' ' << x << ' ' << y << ' ' << z << ' ' << w;
    };
    auto id = [&](Id i) -> std::ostream& {
        if (i == NoId) {
            return out << '-';
        }
        return out << i;
    };
    out << "# FreeCAD Assembly neutral model, version " << m.version << "\n";
    for (const auto& b : m.bodies) {
        out << "body " << b.id << " \"" << m.sourceName(b.source) << "\" "
            << (b.grounded ? 1 : 0) << ' ';
        plc(toPlacement(d->poses[b.id]));
        out << "\n";
    }
    for (const auto& f : m.frames) {
        out << "frame " << f.id << ' ';
        id(f.body) << " \"" << m.sourceName(f.label) << "\" ";
        plc(f.local);
        out << "\n";
    }
    for (const auto& g : m.grounds) {
        out << "ground " << g.id << " \"" << m.sourceName(g.source) << "\" " << g.frameI << ' '
            << g.frameJ << "\n";
    }
    for (const auto& j : m.joints) {
        out << "joint " << j.id << " \"" << m.sourceName(j.source) << "\" "
            << static_cast<int>(j.kind) << ' ';
        id(j.frameI) << ' ';
        id(j.frameJ) << ' ';
        id(j.frameK) << ' ' << j.value << ' ' << j.value2;
        if (j.curve != NoId) {
            out << " curve " << j.curve;
        }
        out << "\n";
    }
    for (const auto& c : m.curves) {
        out << "curve " << c.id << " \"" << m.sourceName(c.source) << "\" "
            << (c.geometry ? c.geometry->describe() : std::string("-")) << "\n";
    }
    for (const auto& l : m.limits) {
        out << "limit " << l.id << " \"" << m.sourceName(l.source) << "\" "
            << static_cast<int>(l.kind) << ' '
            << l.frameI << ' ' << l.frameJ << ' ';
        if (l.min) {
            out << *l.min;
        }
        else {
            out << '-';
        }
        out << ' ';
        if (l.max) {
            out << *l.max;
        }
        else {
            out << '-';
        }
        out << ' ' << l.tolerance << "\n";
    }
    for (const auto& dr : m.drivers) {
        out << "driver " << dr.id << " \"" << m.sourceName(dr.source) << "\" "
            << static_cast<int>(dr.kind)
            << ' ' << dr.frameI << ' ' << dr.frameJ << " \"" << dr.rotation.text << "\" \""
            << dr.translation.text << "\"\n";
    }
    return out ? Status::Ok : Status::Failed;
}

std::vector<Reference::EquationInfo> ReferenceSession::equations() const
{
    std::vector<Reference::EquationInfo> out;
    if (!d->loaded) {
        return out;
    }
    std::vector<Row> rows = d->diagnosticRows();
    const auto order = analysisOrder(rows);
    const auto independent = independentRows(rows, order, d->poses.size());
    for (int i : order) {
        Reference::EquationInfo info;
        info.owner = rows[i].owner;
        info.id = rows[i].id;
        info.category = static_cast<Reference::EquationInfo::Category>(rows[i].category);
        info.value = rows[i].s.v;
        info.independent = independent[i] != 0;
        out.push_back(info);
    }
    return out;
}

int ReferenceSession::rank() const
{
    if (!d->loaded) {
        return 0;
    }
    return qrRank(d->diagnosticRows(), d->poses.size());
}

namespace
{

std::vector<Pose> posesOf(const std::vector<Base::Placement>& placements, std::size_t bodies)
{
    std::vector<Pose> poses(bodies);
    for (std::size_t b = 0; b < bodies && b < placements.size(); ++b) {
        poses[b] = toPose(placements[b]);
    }
    return poses;
}

}  // namespace

std::vector<double>
ReferenceSession::residualsAt(const std::vector<Base::Placement>& placements) const
{
    std::vector<double> out;
    if (!d->loaded) {
        return out;
    }
    std::vector<Row> rows;
    System::Angles measured;
    d->system.evaluate(posesOf(placements, d->poses.size()), rows, measured);
    for (const auto& row : rows) {
        out.push_back(row.s.v);
    }
    return out;
}

std::vector<std::vector<double>>
ReferenceSession::jacobianAt(const std::vector<Base::Placement>& placements) const
{
    std::vector<std::vector<double>> out;
    if (!d->loaded) {
        return out;
    }
    std::vector<Row> rows;
    System::Angles measured;
    d->system.evaluate(posesOf(placements, d->poses.size()), rows, measured);
    for (const auto& row : rows) {
        std::vector<double> gradient(6 * d->poses.size(), 0.0);
        for (int k = 0; k < row.s.g.n; ++k) {
            for (int c = 0; c < 6; ++c) {
                gradient[6 * row.s.g.body[k] + c] = row.s.g.g[k][c];
            }
        }
        out.push_back(std::move(gradient));
    }
    return out;
}
