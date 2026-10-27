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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <utility>

#include <Base/BoundBox.h>

#include "Backend.h"

using namespace Assembly::Solver;

namespace
{

// The driver held at the formula's value: a constant formula, printed so that
// it reads back as the same double. False when the value is not finite (the
// formula is outside its domain at this time, e.g. ln(time - 1) at 0).
bool hold(Formula& formula, double time)
{
    if (formula.text.empty() && !formula.tree && !formula.function) {
        return true;  // the half of a driver that does not drive
    }
    auto evaluate = formula.evaluator();
    if (!evaluate) {
        return true;
    }
    const double value = evaluate(time).value;
    if (!std::isfinite(value)) {
        return false;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.17g", value);
    formula.text = text;
    formula.tree = FormulaTree::parse(formula.text);
    formula.function = Formula::functionOf(formula.tree);
    return true;
}

class CentredSession: public Session
{
public:
    explicit CentredSession(std::unique_ptr<Session> session)
        : inner(std::move(session))
    {}

    Status load(const Model& model) override
    {
        return inner->load(centred(model));
    }
    Status update(const Model& model) override
    {
        return inner->update(centred(model));
    }
    Result solve(const SolveOptions& options) override
    {
        return back(inner->solve(options));
    }
    Status beginDrag(const std::vector<Id>& bodies, const DragOptions& options) override
    {
        return inner->beginDrag(bodies, options);
    }
    Result dragStep(const std::vector<BodyPose>& targets) override
    {
        std::vector<BodyPose> centredTargets = targets;
        for (auto& target : centredTargets) {
            if (target.body < centres.size()) {
                target.placement = target.placement * centres[target.body];
            }
        }
        return back(inner->dragStep(centredTargets));
    }
    Result endDrag() override
    {
        return back(inner->endDrag());
    }
    std::vector<BodyPose> poses() const override
    {
        std::vector<BodyPose> out = inner->poses();
        for (auto& pose : out) {
            pose.placement = back(pose);
        }
        return out;
    }
    Diagnostics analyse() override
    {
        return inner->analyse();
    }
    Status simulate(const SimulationSpec& spec, TrajectorySink& sink) override
    {
        struct Back: TrajectorySink
        {
            const CentredSession& session;
            TrajectorySink& sink;
            Back(const CentredSession& session, TrajectorySink& sink)
                : session(session)
                , sink(sink)
            {}
            bool frame(double time, const std::vector<BodyPose>& poses) override
            {
                std::vector<BodyPose> out = poses;
                for (auto& pose : out) {
                    pose.placement = session.back(pose);
                }
                return sink.frame(time, out);
            }
        } backSink(*this, sink);
        return inner->simulate(spec, backSink);
    }
    Status exportNative(const std::string& path) override
    {
        return inner->exportNative(path);
    }
    std::vector<BodyMetric> bodyMetric() const override
    {
        return metric;
    }

private:
    Model centred(const Model& model)
    {
        const std::size_t n = model.bodies.size();
        metric = jointMetric(model);
        Model out = model;
        centres.assign(n, Base::Placement());
        shifted.assign(n, false);
        starts.assign(n, {});
        for (std::size_t b = 0; b < n; ++b) {
            Body& body = out.bodies[b];
            if (!metric[b].sized) {
                continue;
            }
            const double inertia = metric[b].inertia;
            body.mass.principal = {inertia, inertia, inertia};
            body.mass.mass = metric[b].mass;
            // The body's frame goes to the centre
            body.mass.com = Base::Vector3d();
            const Base::Vector3d& centre = metric[b].centre;
            if (centre == Base::Vector3d()) {
                continue;
            }
            centres[b] = Base::Placement(centre, Base::Rotation());
            shifted[b] = true;
            body.placement = body.placement * centres[b];
            starts[b] = {body.placement, model.bodies[b].placement};
        }
        for (auto& frame : out.frames) {
            if (frame.body < n && shifted[frame.body]) {
                frame.local = centres[frame.body].inverse() * frame.local;
            }
        }
        return out;
    }

    // A pose back in the body's own frame; the start itself exactly, so that
    // a body the solve did not move is not moved by the rounding of the way
    // there and back.
    Base::Placement back(const BodyPose& pose) const
    {
        if (pose.body >= shifted.size() || !shifted[pose.body]) {
            return pose.placement;
        }
        const auto& [centred, own] = starts[pose.body];
        if (pose.placement.isSame(centred)) {
            return own;
        }
        return pose.placement * centres[pose.body].inverse();
    }

    Result back(Result result) const
    {
        for (auto& pose : result.moved) {
            pose.placement = back(pose);
        }
        return result;
    }

    std::unique_ptr<Session> inner;
    std::vector<Base::Placement> centres;  // per body: its centre in its own frame
    std::vector<bool> shifted;
    std::vector<BodyMetric> metric;  // per body of the model given
    std::vector<std::pair<Base::Placement, Base::Placement>> starts;  // centred, own
};

}  // namespace

std::vector<BodyMetric> Assembly::Solver::jointMetric(const Model& model)
{
    const std::size_t n = model.bodies.size();
    std::vector<Base::BoundBox3d> boxes(n);
    auto add = [&](Id frame) {
        if (frame >= model.frames.size()) {
            return;
        }
        const Frame& f = model.frames[frame];
        if (f.body < n) {
            boxes[f.body].Add(f.local.getPosition());
        }
    };
    for (const auto& joint : model.joints) {
        add(joint.frameI);
        add(joint.frameJ);
        add(joint.frameK);
    }
    std::vector<BodyMetric> metric(n);
    for (std::size_t b = 0; b < n; ++b) {
        metric[b].body = static_cast<Id>(b);
        const Base::BoundBox3d& box = boxes[b];
        const MassProps& mass = model.bodies[b].mass;
        if (mass.sized && !model.bodies[b].grounded) {
            metric[b] = {true, mass.com, mass.principal[0], static_cast<Id>(b), mass.mass};
            continue;
        }
        if (model.bodies[b].grounded || !box.IsValid()) {
            continue;
        }
        // A turn weighs as much as moving the box's corner along its arc
        // (Euler parameters move by half the angle, so 4 r^2), r at least 1 mm.
        const double r = std::max(0.5 * box.CalcDiagonalLength(), 1.0);
        metric[b] = {true, box.GetCenter(), 4.0 * r * r, static_cast<Id>(b)};
    }
    return metric;
}

std::unique_ptr<Session> Assembly::Solver::centredOnJoints(std::unique_ptr<Session> session)
{
    if (!session) {
        return session;
    }
    return std::make_unique<CentredSession>(std::move(session));
}

Status Assembly::Solver::simulateByPositionSolves(
    Session& session,
    const Model& model,
    const SimulationSpec& spec,
    TrajectorySink& sink
)
{
    for (const auto& driver : model.drivers) {
        for (const Formula* formula : {&driver.rotation, &driver.translation}) {
            if (!formula->text.empty() && !formula->tree && !FormulaTree::parse(formula->text)) {
                return Status::Failed;
            }
        }
    }
    if (!(spec.tOutput > 0.0) || spec.tEnd < spec.tStart) {
        return Status::Failed;
    }

    // Output times as an integration with that output step reports them.
    const auto steps = static_cast<std::size_t>(
        std::floor((spec.tEnd - spec.tStart) / spec.tOutput + 1e-9)
    );
    Model frame = model;
    for (std::size_t k = 0; k <= steps; ++k) {
        const double time = spec.tStart + static_cast<double>(k) * spec.tOutput;
        for (std::size_t i = 0; i < frame.drivers.size(); ++i) {
            frame.drivers[i].rotation = model.drivers[i].rotation;
            frame.drivers[i].translation = model.drivers[i].translation;
            if (!hold(frame.drivers[i].rotation, time)
                || !hold(frame.drivers[i].translation, time)) {
                return Status::Failed;
            }
        }
        if (Status status = session.load(frame); status != Status::Ok) {
            return status;
        }
        Result result = session.solve({});
        if (result.status != Status::Ok) {
            return result.status;
        }
        std::vector<BodyPose> poses = session.poses();
        // Warm: the next frame starts where this one ended.
        for (const auto& pose : poses) {
            if (pose.body < frame.bodies.size()) {
                frame.bodies[pose.body].placement = pose.placement;
            }
        }
        if (!sink.frame(time, poses)) {
            return Status::Cancelled;
        }
    }
    return Status::Ok;
}
