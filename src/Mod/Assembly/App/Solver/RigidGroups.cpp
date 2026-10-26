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
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ReferenceBackend.h"
#include "RigidGroups.h"

using namespace Assembly::Solver;

namespace
{

// The equations a joint of the kind holds (Model.h); -1 for a kind whose
// equations are not functions of the two poses alone (the couplers count the
// turns they made, the cam follows its closest point), which is never taken
// out.
int equationsOf(JointKind kind)
{
    switch (kind) {
        case JointKind::Fixed:
            return 6;
        case JointKind::Revolute:
        case JointKind::Prismatic:
            return 5;
        case JointKind::Cylindrical:
            return 4;
        case JointKind::Spherical:
        case JointKind::Planar:
        case JointKind::LineLineDistance:
            return 3;
        case JointKind::ParallelZ:
        case JointKind::LineInPlane:
        case JointKind::PointOnLine:
            return 2;
        case JointKind::PerpendicularZ:
        case JointKind::AngleZ:
        case JointKind::PointInPlane:
        case JointKind::PointPointDistance:
        case JointKind::PointLineDistance:
            return 1;
        default:
            return -1;
    }
}

enum class Role : std::uint8_t
{
    Kept,  // a body of the smaller model (alone, or a group's first body)
    Placed,  // held by the ground: placed, and its frames on the ground
    Member  // held by a group: goes with the group's first body
};

struct BodyMap
{
    Role role = Role::Kept;
    Id reduced = NoId;  // Kept and Member: the body of the smaller model
    // Placed: the pose it was placed at; Member: its pose in the frame of the
    // group's first body
    Base::Placement pose;
};

class GroupedSession: public Session
{
public:
    explicit GroupedSession(std::unique_ptr<Session> session)
        : inner(std::move(session))
    {}

    Status load(const Model& model) override
    {
        original = model;
        build();
        return inner->load(reduced);
    }
    Status update(const Model& model) override
    {
        if (!sameExceptDragWeights(original, model)) {
            return Status::NeedsReload;
        }
        original = model;
        // A group weighs what its heaviest body does.
        for (auto& body : reduced.bodies) {
            body.dragWeight = 0.0;
        }
        for (std::size_t b = 0; b < map.size(); ++b) {
            if (map[b].role != Role::Placed) {
                double& w = reduced.bodies[map[b].reduced].dragWeight;
                w = std::max(w, model.bodies[b].dragWeight);
            }
        }
        return inner->update(reduced);
    }
    Result solve(const SolveOptions& options) override
    {
        return back(inner->solve(options));
    }
    Status beginDrag(const std::vector<Id>& bodies, const DragOptions& options) override
    {
        std::vector<Id> mapped;
        for (Id b : bodies) {
            if (b < map.size() && map[b].role != Role::Placed
                && std::ranges::find(mapped, map[b].reduced) == mapped.end()) {
                mapped.push_back(map[b].reduced);
            }
        }
        return inner->beginDrag(mapped, options);
    }
    Result dragStep(const std::vector<BodyPose>& targets) override
    {
        std::vector<BodyPose> mapped;
        for (const auto& target : targets) {
            if (target.body >= map.size() || map[target.body].role == Role::Placed) {
                continue;
            }
            const BodyMap& m = map[target.body];
            if (std::ranges::any_of(mapped, [&](const auto& t) { return t.body == m.reduced; })) {
                continue;
            }
            mapped.push_back(
                {m.reduced,
                 m.role == Role::Member ? target.placement * m.pose.inverse() : target.placement}
            );
        }
        return back(inner->dragStep(mapped));
    }
    Result endDrag() override
    {
        return back(inner->endDrag());
    }
    std::vector<BodyPose> poses() const override
    {
        return expand(inner->poses());
    }
    Diagnostics analyse() override
    {
        Diagnostics d = inner->analyse();
        auto joints = [&](std::vector<Id>& ids) {
            for (Id& id : ids) {
                if (id < jointOrigin.size()) {
                    id = jointOrigin[id];
                }
            }
        };
        joints(d.redundant);
        joints(d.conflicting);
        joints(d.partiallyRedundant);
        joints(d.malformed);
        joints(d.unsupported);
        for (auto& group : d.overDefinedGroups) {
            joints(group);
        }
        if (!d.bodyDof.empty()) {
            std::vector<int> bodyDof(map.size(), 0);
            for (std::size_t b = 0; b < map.size(); ++b) {
                if (map[b].role != Role::Placed && map[b].reduced < d.bodyDof.size()) {
                    bodyDof[b] = d.bodyDof[map[b].reduced];
                }
            }
            d.bodyDof = std::move(bodyDof);
        }
        return d;
    }
    Status simulate(const SimulationSpec& spec, TrajectorySink& sink) override
    {
        struct Expand: TrajectorySink
        {
            const GroupedSession& session;
            TrajectorySink& sink;
            Expand(const GroupedSession& session, TrajectorySink& sink)
                : session(session)
                , sink(sink)
            {}
            bool frame(double time, const std::vector<BodyPose>& poses) override
            {
                return sink.frame(time, session.expand(poses));
            }
        } expandSink(*this, sink);
        return inner->simulate(spec, expandSink);
    }
    Status exportNative(const std::string& path) override
    {
        return inner->exportNative(path);
    }
    std::vector<BodyMetric> bodyMetric() const override
    {
        const std::vector<BodyMetric> metric = inner->bodyMetric();
        if (metric.size() != reduced.bodies.size()) {
            return {};
        }
        std::vector<BodyMetric> out(map.size());
        for (std::size_t b = 0; b < map.size(); ++b) {
            const BodyMap& m = map[b];
            if (m.role == Role::Placed) {
                out[b].body = static_cast<Id>(b);
                continue;
            }
            out[b] = metric[m.reduced];
            out[b].body = keptOrigin[m.reduced];
            if (m.role == Role::Member) {
                m.pose.inverse().multVec(metric[m.reduced].centre, out[b].centre);
            }
        }
        return out;
    }

private:
    Base::Placement world(const Frame& frame) const
    {
        return frame.body == NoId ? frame.local : pose[frame.body] * frame.local;
    }

    // Where the joints place body b against the bodies they reach, which are
    // held: true when they leave it no freedom and have no dependent equation.
    bool place(Id b, const std::vector<Id>& joints, Base::Placement& placed) const
    {
        if (joints.size() == 1 && original.joints[joints[0]].kind == JointKind::Fixed) {
            const Joint& joint = original.joints[joints[0]];
            const Frame& fi = original.frames[joint.frameI];
            const Frame& fj = original.frames[joint.frameJ];
            placed = fj.body == b ? world(fi) * fj.local.inverse() : world(fj) * fi.local.inverse();
            return true;
        }

        // The body alone, the frames of the others on the ground
        Model one;
        Body body;
        body.placement = pose[b];
        one.addBody(body);
        std::unordered_map<Id, Id> frames;
        auto frameOf = [&](Id f) -> Id {
            if (f == NoId) {
                return NoId;
            }
            if (auto it = frames.find(f); it != frames.end()) {
                return it->second;
            }
            Frame frame = original.frames[f];
            frame.label = NoId;
            if (frame.body == b) {
                frame.body = 0;
            }
            else {
                frame.local = world(frame);
                frame.body = NoId;
            }
            return frames[f] = one.addFrame(frame);
        };
        for (Id j : joints) {
            Joint joint = original.joints[j];
            joint.source = NoId;
            joint.frameI = frameOf(joint.frameI);
            joint.frameJ = frameOf(joint.frameJ);
            joint.frameK = frameOf(joint.frameK);
            one.addJoint(joint);
        }

        ReferenceSession session;
        if (session.load(one) != Status::Ok || session.solve({}).status != Status::Ok) {
            return false;
        }
        const Diagnostics d = session.analyse();
        if (!(d.known & Diagnostics::KnownDof) || d.dof != 0 || !d.redundant.empty()
            || !d.conflicting.empty() || !d.partiallyRedundant.empty()) {
            return false;
        }
        placed = session.poses().at(0).placement;
        return true;
    }

    void build()
    {
        const std::size_t n = original.bodies.size();
        const auto& frames = original.frames;
        auto bodyOf = [&](Id f) { return f < frames.size() ? frames[f].body : NoId; };

        // Bodies a limit or a driver touches, never taken out
        std::vector<bool> timed(n, false);
        auto mark = [&](Id f) {
            const Id b = bodyOf(f);
            if (b < n) {
                timed[b] = true;
            }
        };
        for (const auto& limit : original.limits) {
            mark(limit.frameI);
            mark(limit.frameJ);
        }
        for (const auto& driver : original.drivers) {
            mark(driver.frameI);
            mark(driver.frameJ);
        }

        std::vector<std::vector<Id>> jointsOf(n);
        for (const auto& joint : original.joints) {
            for (Id f : {joint.frameI, joint.frameJ, joint.frameK}) {
                const Id b = bodyOf(f);
                if (b < n && std::ranges::find(jointsOf[b], joint.id) == jointsOf[b].end()) {
                    jointsOf[b].push_back(joint.id);
                }
            }
        }

        map.assign(n, {});
        pose.resize(n);
        for (std::size_t b = 0; b < n; ++b) {
            pose[b] = original.bodies[b].placement;
        }
        std::vector<bool> assigned(n, false);
        std::vector<bool> taken(original.joints.size(), false);
        bool any = false;

        // Places, until none is left, the bodies that the joints to the bodies
        // 'held' says are held (NoId: the ground) leave no freedom.
        auto grow = [&](const std::function<bool(Id)>& held, const std::function<void(Id)>& accept) {
            for (bool changed = true; changed;) {
                changed = false;
                for (Id b = 0; b < n; ++b) {
                    if (assigned[b] || timed[b]) {
                        continue;
                    }
                    std::vector<Id> joints;
                    int count = 0;
                    bool usable = true;
                    for (Id j : jointsOf[b]) {
                        const Joint& joint = original.joints[j];
                        bool reaches = true;
                        bool other = false;
                        for (Id f : {joint.frameI, joint.frameJ, joint.frameK}) {
                            if (f == NoId) {
                                continue;
                            }
                            const Id o = bodyOf(f);
                            if (o != b) {
                                other = true;
                                reaches = reaches && held(o);
                            }
                        }
                        if (!reaches) {
                            continue;
                        }
                        const int e = equationsOf(joint.kind);
                        if (!other || e < 0 || joint.frameK != NoId) {
                            usable = false;
                            break;
                        }
                        count += e;
                        joints.push_back(j);
                    }
                    Base::Placement placed;
                    if (!usable || count < 6 || !place(b, joints, placed)) {
                        continue;
                    }
                    pose[b] = placed;
                    assigned[b] = true;
                    for (Id j : joints) {
                        taken[j] = true;
                    }
                    accept(b);
                    any = true;
                    changed = true;
                }
            }
        };

        // From the ground
        std::vector<bool> fixed(n, false);
        for (Id b = 0; b < n; ++b) {
            if (original.bodies[b].grounded) {
                fixed[b] = assigned[b] = true;
            }
        }
        grow(
            [&](Id o) { return o == NoId || (o < n && fixed[o]); },
            [&](Id b) {
                fixed[b] = true;
                map[b].role = Role::Placed;
                map[b].pose = pose[b];
            }
        );

        // The groups, each from its first body
        std::vector<Id> groupOf(n, NoId);
        for (Id r = 0; r < n; ++r) {
            if (assigned[r]) {
                continue;
            }
            assigned[r] = true;
            groupOf[r] = r;
            grow(
                [&](Id o) { return o < n && groupOf[o] == r; },
                [&](Id b) {
                    groupOf[b] = r;
                    map[b].role = Role::Member;
                }
            );
        }

        jointOrigin.clear();
        keptOrigin.clear();
        if (!any) {
            reduced = original;
            for (Id b = 0; b < n; ++b) {
                map[b].reduced = b;
                keptOrigin.push_back(b);
            }
            for (const auto& joint : original.joints) {
                jointOrigin.push_back(joint.id);
            }
            return;
        }

        reduced = Model();
        reduced.version = original.version;
        reduced.sources = original.sources;
        for (Id b = 0; b < n; ++b) {
            if (map[b].role == Role::Kept) {
                map[b].reduced = reduced.addBody(original.bodies[b]);
                keptOrigin.push_back(b);
            }
        }
        for (Id b = 0; b < n; ++b) {
            if (map[b].role == Role::Member) {
                const Id r = groupOf[b];
                map[b].reduced = map[r].reduced;
                map[b].pose = pose[r].inverse() * pose[b];
            }
        }
        for (Frame frame : original.frames) {
            if (frame.body < n) {
                const BodyMap& m = map[frame.body];
                switch (m.role) {
                    case Role::Kept:
                        frame.body = m.reduced;
                        break;
                    case Role::Placed:
                        frame.local = m.pose * frame.local;
                        frame.body = NoId;
                        break;
                    case Role::Member:
                        frame.local = m.pose * frame.local;
                        frame.body = m.reduced;
                        break;
                }
            }
            reduced.addFrame(frame);
        }
        for (const auto& joint : original.joints) {
            if (!taken[joint.id]) {
                reduced.addJoint(joint);
                jointOrigin.push_back(joint.id);
            }
        }
        reduced.grounds = original.grounds;
        reduced.limits = original.limits;
        reduced.drivers = original.drivers;
        reduced.curves = original.curves;
    }

    // The poses of the smaller model's bodies as the poses of all bodies
    std::vector<BodyPose> expand(const std::vector<BodyPose>& poses, bool withPlaced = true) const
    {
        std::vector<const Base::Placement*> of(reduced.bodies.size(), nullptr);
        for (const auto& p : poses) {
            if (p.body < of.size()) {
                of[p.body] = &p.placement;
            }
        }
        std::vector<BodyPose> out;
        out.reserve(map.size());
        for (Id b = 0; b < map.size(); ++b) {
            const BodyMap& m = map[b];
            if (m.role == Role::Placed) {
                if (withPlaced) {
                    out.push_back({b, m.pose});
                }
            }
            else if (m.reduced < of.size() && of[m.reduced]) {
                out.push_back({b, m.role == Role::Member ? *of[m.reduced] * m.pose : *of[m.reduced]});
            }
        }
        return out;
    }

    Result back(Result result) const
    {
        std::vector<BodyPose> moved = expand(result.moved, false);
        // A placed body moved when it was placed
        for (Id b = 0; b < map.size(); ++b) {
            if (map[b].role == Role::Placed && map[b].pose != original.bodies[b].placement) {
                moved.push_back({b, map[b].pose});
            }
        }
        result.moved = std::move(moved);
        return result;
    }

    std::unique_ptr<Session> inner;
    Model original;
    Model reduced;
    std::vector<BodyMap> map;  // per body of the original model
    std::vector<Base::Placement> pose;  // per body: its start, or where it was placed
    std::vector<Id> jointOrigin;  // per joint of the smaller model: the original's
    std::vector<Id> keptOrigin;  // per body of the smaller model: the original's
};

}  // namespace

std::unique_ptr<Session> Assembly::Solver::groupedRigidly(std::unique_ptr<Session> session)
{
    if (!session) {
        return session;
    }
    return std::make_unique<GroupedSession>(std::move(session));
}
