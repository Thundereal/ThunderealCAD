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

#include <cmath>
#include <cstdio>

#include "Backend.h"

using namespace Assembly::Solver;

namespace
{

// The driver held at the formula's value: a constant formula, printed so that
// it reads back as the same double. False when the value is not finite (the
// formula is outside its domain at this time, e.g. ln(time - 1) at 0).
bool hold(Formula& formula, double time)
{
    if (formula.text.empty() && !formula.tree) {
        return true;  // the half of a driver that does not drive
    }
    auto tree = formula.tree ? formula.tree : FormulaTree::parse(formula.text);
    if (!tree) {
        return true;
    }
    const double value = tree->value(time);
    if (!std::isfinite(value)) {
        return false;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.17g", value);
    formula.text = text;
    formula.tree = FormulaTree::parse(formula.text);
    return true;
}

}  // namespace

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
