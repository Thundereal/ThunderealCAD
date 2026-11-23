// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cmath>

#include "FreeFallBackend.h"
#include "SplishCase.h"

using namespace Fluid::Solver;

namespace
{
class FreeFallSession: public Session
{
public:
    Status prepare(const Model& given, std::string& why) override
    {
        model = given;
        start.clear();
        if (model.resolution <= 0.0 || model.outputInterval <= 0.0) {
            why = "the resolution and the output interval must be positive";
            return Status::Unsupported;
        }
        for (const auto& fluid : model.fluids) {
            const auto points = sampled(fluid.surface, model.resolution);
            start.insert(start.end(), points.begin(), points.end());
        }
        // An inlet's fluid is a layer of points on its face, another each
        // time the last has gone the resolution
        layers.clear();
        for (std::size_t k = 0; k < model.inlets.size(); ++k) {
            Vec3 across {};
            layers.push_back(Splish::inletOf(model, k, across, why));
            if (layers.back().empty() || !(model.inlets[k].speed > 0.0)) {
                why = why.empty() ? model.inlets[k].source + " has no speed" : why;
                return Status::Unsupported;
            }
        }
        if (start.empty() && layers.empty()) {
            why = "there is no fluid: no volume holds a point at this resolution";
            return Status::Empty;
        }
        const double g = std::sqrt(model.gravity[0] * model.gravity[0]
                                   + model.gravity[1] * model.gravity[1]
                                   + model.gravity[2] * model.gravity[2]);
        up = g > 0.0 ? Vec3 {-model.gravity[0] / g, -model.gravity[1] / g, -model.gravity[2] / g}
                     : Vec3 {0.0, 0.0, 1.0};
        weight = g * (model.fluids.empty() ? 1000.0 : model.fluids.front().material.density);
        // the walls that fall with the fluid, each as its box
        moved.clear();
        for (std::size_t wall : freeWalls(model)) {
            Vec3 low {};
            Vec3 high {};
            if (!boxOf(model.walls[wall].surface, low, high)) {
                why = model.walls[wall].source + " has no shape";
                return Status::Unsupported;
            }
            moved.push_back({low, high});
        }
        return Status::Ok;
    }

    Status run(FrameSink& sink, std::string& /*why*/) override
    {
        const auto frames = static_cast<long>(
            std::floor(model.timeEnd / model.outputInterval + 1.0e-9));
        const double half = 0.5 * model.resolution;
        for (long k = 0; k <= frames; ++k) {
            if (sink.cancelled()) {
                return Status::Cancelled;
            }
            Frame frame;
            frame.time = static_cast<double>(k) * model.outputInterval;
            frame.points.reserve(start.size());
            // a point that set off from p0 at that speed, that long ago
            const auto fallen = [&](const Vec3& p0, const Vec3& speed, double age) {
                Vec3 p;
                bool gone = false;
                for (std::size_t i = 0; i < 3; ++i) {
                    p[i] = p0[i] + speed[i] * age + 0.5 * model.gravity[i] * age * age;
                    const double low = model.space.min[i] + half;
                    const double high = model.space.max[i] - half;
                    // through an open side it is gone; one that set off
                    // at a side, as an inlet's on a face there, is not
                    // out by that
                    if (p[i] < low) {
                        gone = gone || (model.space.open[2 * i] && p0[i] >= low);
                        p[i] = low;
                    }
                    else if (p[i] > high) {
                        gone = gone || (model.space.open[2 * i + 1] && p0[i] <= high);
                        p[i] = high;
                    }
                }
                if (!gone) {
                    frame.points.push_back(p);
                }
            };
            for (const auto& p0 : start) {
                fallen(p0, {0.0, 0.0, 0.0}, frame.time);
            }
            // What the inlets have sent by now, and what they press with on
            // a wall square to them that stops it: the momentum a second,
            // density x area x speed^2
            Vec3 push {0.0, 0.0, 0.0};
            Material liquid;
            liquidOf(model, liquid);
            for (std::size_t k = 0; k < layers.size(); ++k) {
                const Inlet& inlet = model.inlets[k];
                const double end = inlet.end > inlet.start ? inlet.end : model.timeEnd;
                const Vec3 speed {inlet.direction[0] * inlet.speed,
                                  inlet.direction[1] * inlet.speed,
                                  inlet.direction[2] * inlet.speed};
                const double every = model.resolution / inlet.speed;
                for (long j = 0;; ++j) {
                    const double sent = inlet.start + static_cast<double>(j) * every;
                    if (sent > std::min(frame.time, end) + 1.0e-12) {
                        break;
                    }
                    for (const Vec3& p0 : layers[k]) {
                        fallen(p0, speed, frame.time - sent);
                    }
                }
                if (frame.time >= inlet.start && frame.time <= end + 1.0e-12) {
                    const double flux = liquid.density * static_cast<double>(layers[k].size())
                        * model.resolution * model.resolution * inlet.speed;
                    for (std::size_t i = 0; i < 3; ++i) {
                        push[i] += flux * speed[i];
                    }
                }
            }
            // a moved wall falls as the fluid does, until its box is at a
            // side of the space
            for (const auto& box : moved) {
                Motion motion;
                for (std::size_t i = 0; i < 3; ++i) {
                    const double fall = 0.5 * model.gravity[i] * frame.time * frame.time;
                    motion.translation[i] = std::clamp(fall,
                                                       std::min(model.space.min[i] - box[0][i], 0.0),
                                                       std::max(model.space.max[i] - box[1][i], 0.0));
                }
                frame.bodies.push_back(motion);
            }
            std::vector<double> values;
            for (const auto& monitor : model.monitors) {
                if (monitor.kind == MonitorKind::Force) {
                    values.push_back(push[0] * monitor.direction[0] + push[1] * monitor.direction[1]
                                     + push[2] * monitor.direction[2]);
                    continue;
                }
                const double height = heightAt(frame, monitor.point);
                values.push_back(monitor.kind == MonitorKind::Height ? height : weight * height);
            }
            sink.sample(frame.time, values);
            sink.frame(frame);
        }
        return Status::Ok;
    }

private:
    // How far above the point the fluid reaches on the line through it
    // along up: the top of the highest particle within a spacing of the
    // line; 0 when there is none above.
    double heightAt(const Frame& frame, const Vec3& point) const
    {
        double height = 0.0;
        for (const auto& p : frame.points) {
            const Vec3 d {p[0] - point[0], p[1] - point[1], p[2] - point[2]};
            const double along = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
            const double off2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along;
            if (off2 <= model.resolution * model.resolution) {
                height = std::max(height, along + 0.5 * model.resolution);
            }
        }
        return height;
    }

    Model model;
    std::vector<Vec3> start;
    std::vector<std::vector<Vec3>> layers;  // one per inlet
    std::vector<std::array<Vec3, 2>> moved;
    Vec3 up {0.0, 0.0, 1.0};
    double weight = 0.0;
};
}  // namespace

Capabilities FreeFallBackend::capabilities() const
{
    Capabilities can;
    can.results = ResultKind::Particles;
    can.freeSurface = true;
    can.freeBodies = true;
    can.inlets = true;
    can.forces = true;
    can.repeatable = true;
    can.inProcess = true;
    can.testData = true;
    return can;
}

std::unique_ptr<Session> FreeFallBackend::createSession() const
{
    return std::make_unique<FreeFallSession>();
}
