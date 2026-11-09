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

// The unit contract (docs/AssemblyDynamics.md sec 8.1). The neutral model
// carries FreeCAD's internal units, which are consistent -- mm, kg, s, so a
// force is in mN (kg mm / s^2) -- and a backend that wants SI converts at
// load and back on output, multiplying by these factors: a value in internal
// units times its factor is the value in SI. Chrono's kinematic solves keep
// mm, their tolerances set for it; its dynamics convert, as Chrono's
// defaults (envelopes, margins, recovery speeds) all assume SI-sized numbers.

#pragma once

#include <cmath>

namespace Assembly::Solver::SI
{

inline constexpr double length = 1e-3;              // mm -> m
inline constexpr double mass = 1.0;                 // kg
inline constexpr double time = 1.0;                 // s
inline constexpr double angle = 1.0;                // rad
inline constexpr double velocity = 1e-3;            // mm/s -> m/s
inline constexpr double acceleration = 1e-3;        // mm/s^2 -> m/s^2, gravity too
inline constexpr double angularVelocity = 1.0;      // rad/s
inline constexpr double force = 1e-3;               // mN -> N
inline constexpr double torque = 1e-6;              // mN mm -> N m
inline constexpr double energy = 1e-6;              // mN mm -> J
inline constexpr double power = 1e-6;               // mN mm / s -> W
inline constexpr double linearStiffness = 1.0;      // mN/mm -> N/m
inline constexpr double torsionalStiffness = 1e-6;  // mN mm / rad -> N m / rad
inline constexpr double linearDamping = 1.0;        // mN s / mm -> N s / m
inline constexpr double torsionalDamping = 1e-6;    // mN mm s / rad -> N m s / rad
inline constexpr double inertia = 1e-6;             // kg mm^2 -> kg m^2
inline constexpr double density = 1e9;              // kg/mm^3 -> kg/m^3
inline constexpr double pressure = 1e3;             // kPa (kg / (mm s^2)) -> Pa: Young's modulus

/** An impact law's stiffness k, with its exponent e: a force over a length
 * to the e, so its factor is force's over length's to the e -- the scaling
 * Adams warns about ("stiffness must be unit-scaled by (length
 * factor)^exponent"). 1 for e = 1, the linear stiffness's.
 */
inline double impactStiffness(double exponent)
{
    return force / std::pow(length, exponent);
}

/// An impact law's damping: a force over a velocity
inline constexpr double impactDamping = force / velocity;

}  // namespace Assembly::Solver::SI
