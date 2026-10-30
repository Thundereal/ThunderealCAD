// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

#include <Base/Exception.h>

#include "BendAllowance.h"

using namespace PartDesign::SheetMetal;

namespace
{

double keyOf(Table::Axis axis, const Bend& bend)
{
    switch (axis) {
        case Table::Axis::Thickness:
            return bend.thickness;
        case Table::Axis::Radius:
            return bend.radius;
        case Table::Axis::Angle:
            return bend.angle;
        case Table::Axis::RadiusOverThickness:
            if (bend.thickness <= 0) {
                THROWM(Base::ValueError, "A bend with no thickness has no R/T");
            }
            return bend.radius / bend.thickness;
    }
    return 0;
}

void check(const Table& table)
{
    if (table.axes.empty() || table.axes.size() > 3 || table.keys.size() != table.axes.size()) {
        THROWM(Base::ValueError, "A bend table needs one to three axes, each with its keys");
    }
    std::size_t count = 1;
    for (std::size_t i = 0; i < table.axes.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (table.axes[i] == table.axes[j]) {
                THROWM(Base::ValueError, "A bend table has an axis twice");
            }
        }
        const auto& keys = table.keys[i];
        if (keys.empty()) {
            THROWM(Base::ValueError, "A bend table has an axis with no keys");
        }
        for (std::size_t j = 1; j < keys.size(); ++j) {
            if (!(keys[j] > keys[j - 1])) {
                THROWM(Base::ValueError, "A bend table's keys do not ascend");
            }
        }
        count *= keys.size();
    }
    if (table.values.size() != count) {
        FC_THROWM(Base::ValueError,
                  "A bend table has " << table.values.size() << " values for " << count
                                      << " combinations of its keys");
    }
}

}  // namespace

double Table::at(const Bend& bend, bool* inside) const
{
    check(*this);
    // per axis: the two rows either side and the weight of the upper one
    const std::size_t n = axes.size();
    std::vector<std::size_t> lower(n), upper(n);
    std::vector<double> weight(n);
    bool within = true;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& k = keys[i];
        const double x = keyOf(axes[i], bend);
        if (x < k.front() || x > k.back()) {
            within = false;
        }
        if (k.size() == 1 || x <= k.front()) {
            lower[i] = upper[i] = 0;
            weight[i] = 0;
            continue;
        }
        if (x >= k.back()) {
            lower[i] = upper[i] = k.size() - 1;
            weight[i] = 0;
            continue;
        }
        const auto it = std::upper_bound(k.begin(), k.end(), x);
        upper[i] = static_cast<std::size_t>(it - k.begin());
        lower[i] = upper[i] - 1;
        weight[i] = (x - k[lower[i]]) / (k[upper[i]] - k[lower[i]]);
    }
    if (inside) {
        *inside = within;
    }
    // the corners of the cell, each weighted by its share
    double sum = 0;
    for (unsigned corner = 0; corner < (1u << n); ++corner) {
        double w = 1;
        std::size_t index = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const bool up = corner & (1u << i);
            w *= up ? weight[i] : 1 - weight[i];
            index = index * keys[i].size() + (up ? upper[i] : lower[i]);
        }
        if (w != 0) {
            sum += w * values[index];
        }
    }
    return sum;
}

double PartDesign::SheetMetal::din6935K(double thickness, double radius)
{
    if (thickness <= 0 || radius <= 0) {
        return 0;
    }
    const double ratio = radius / thickness;
    if (ratio >= 5) {
        return 0.5;
    }
    return std::clamp(0.65 + 0.5 * std::log10(ratio), 0.0, 1.0) / 2;
}

double PartDesign::SheetMetal::kFromY(double y)
{
    return 2 * y / std::numbers::pi;
}

double PartDesign::SheetMetal::kOf(const Bend& bend, const Allowance& allowance, bool* inside)
{
    if (inside) {
        *inside = true;
    }
    switch (allowance.method) {
        case Allowance::Method::KFactor:
            return allowance.k;
        case Allowance::Method::KFactorDIN:
            return allowance.k / 2;
        case Allowance::Method::DIN6935:
            return din6935K(bend.thickness, bend.radius);
        case Allowance::Method::KTable:
            return allowance.table.at(bend, inside);
        case Allowance::Method::AllowanceTable:
        case Allowance::Method::DeductionTable:
            break;
    }
    // K from the allowance the table gives
    if (bend.angle <= 0 || bend.thickness <= 0) {
        return 0;
    }
    return (bendAllowance(bend, allowance, inside) / bend.angle - bend.radius) / bend.thickness;
}

double PartDesign::SheetMetal::bendAllowance(const Bend& bend,
                                             const Allowance& allowance,
                                             bool* inside)
{
    switch (allowance.method) {
        case Allowance::Method::AllowanceTable:
            return allowance.table.at(bend, inside);
        case Allowance::Method::DeductionTable:
            return 2 * outsideSetback(bend) - allowance.table.at(bend, inside);
        default:
            return bend.angle * (bend.radius + kOf(bend, allowance, inside) * bend.thickness);
    }
}

double PartDesign::SheetMetal::outsideSetback(const Bend& bend)
{
    if (bend.angle >= 179 * std::numbers::pi / 180) {
        FC_THROWM(Base::ValueError,
                  "A bend of " << bend.angle * 180 / std::numbers::pi
                               << " deg has no outside setback: its walls hardly meet");
    }
    return (bend.radius + bend.thickness) * std::tan(bend.angle / 2);
}

double PartDesign::SheetMetal::bendDeduction(const Bend& bend,
                                             const Allowance& allowance,
                                             bool* inside)
{
    if (allowance.method == Allowance::Method::DeductionTable) {
        outsideSetback(bend);  // refused where it would be
        return allowance.table.at(bend, inside);
    }
    return 2 * outsideSetback(bend) - bendAllowance(bend, allowance, inside);
}
