// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cmath>

#include "FreeFallBackend.h"

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
        if (start.empty()) {
            why = "there is no fluid: no volume holds a point at this resolution";
            return Status::Empty;
        }
        const double g = std::sqrt(model.gravity[0] * model.gravity[0]
                                   + model.gravity[1] * model.gravity[1]
                                   + model.gravity[2] * model.gravity[2]);
        up = g > 0.0 ? Vec3 {-model.gravity[0] / g, -model.gravity[1] / g, -model.gravity[2] / g}
                     : Vec3 {0.0, 0.0, 1.0};
        weight = g * (model.fluids.empty() ? 1000.0 : model.fluids.front().material.density);
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
            for (const auto& p0 : start) {
                Vec3 p;
                bool gone = false;
                for (std::size_t i = 0; i < 3; ++i) {
                    p[i] = p0[i] + 0.5 * model.gravity[i] * frame.time * frame.time;
                    const double low = model.space.min[i] + half;
                    const double high = model.space.max[i] - half;
                    if (p[i] < low) {
                        gone = gone || model.space.open[2 * i];
                        p[i] = low;
                    }
                    else if (p[i] > high) {
                        gone = gone || model.space.open[2 * i + 1];
                        p[i] = high;
                    }
                }
                if (!gone) {
                    frame.points.push_back(p);
                }
            }
            for (const auto& monitor : model.monitors) {
                const double height = heightAt(frame, monitor.point);
                frame.monitors.push_back(monitor.kind == MonitorKind::Height ? height
                                                                              : weight * height);
            }
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
    Vec3 up {0.0, 0.0, 1.0};
    double weight = 0.0;
};
}  // namespace

Capabilities FreeFallBackend::capabilities() const
{
    Capabilities can;
    can.results = ResultKind::Particles;
    can.freeSurface = true;
    can.repeatable = true;
    can.inProcess = true;
    can.testData = true;
    return can;
}

std::unique_ptr<Session> FreeFallBackend::createSession() const
{
    return std::make_unique<FreeFallSession>();
}
