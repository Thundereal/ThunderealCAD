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

#pragma once

#include <vector>

#include <Mod/PartDesign/PartDesignGlobal.h>

/** Sheet metal's geometry, no document (docs/SheetMetal.md sec 4.5) */
namespace PartDesign::SheetMetal
{

/** A bend as its flat length depends on it: the sheet's thickness, the inner
 * radius, and the angle the sheet turns through in radians (0 flat, pi / 2 a
 * right angle, pi a hem) -- not the angle left open between the walls
 */
struct Bend
{
    double thickness = 0;
    double radius = 0;
    double angle = 0;
};

/** A table of measured values, interpolated linearly along each of its axes
 * (one to three). The values run through the last axis fastest: for axes
 * (A, B) the value at keys (a_i, b_j) is values[i * |B| + j]. Keys ascend.
 * Outside its keys the table gives its end row's value and says so.
 */
struct PartDesignExport Table
{
    enum class Axis
    {
        Thickness,
        Radius,
        Angle,
        RadiusOverThickness,
    };
    std::vector<Axis> axes;
    std::vector<std::vector<double>> keys;
    std::vector<double> values;

    /** The value at \a bend; \a inside, if given, says whether every axis was
     * within its keys. Throws Base::ValueError on a table that does not say
     * what it means: empty, an axis twice, keys not ascending, or values not
     * one for each combination of keys.
     */
    double at(const Bend& bend, bool* inside = nullptr) const;

    /// One measured bend: its thickness, inner radius, angle (radians) and
    /// the value measured
    struct Row
    {
        double thickness = 0;
        double radius = 0;
        double angle = 0;
        double value = 0;
    };
    /** The table of rows in any order, as a card holds it: axes thickness,
     * radius and angle, each keyed by the values its rows have (an axis of
     * one key kept). Throws Base::ValueError unless the rows fill the grid,
     * each corner once.
     */
    static Table fromRows(const std::vector<Row>& rows);
};

/** How a bend's flat length is found (docs/SheetMetal.md sec 4.6). The table
 * holds K for KTable, the allowance for AllowanceTable and the deduction for
 * DeductionTable.
 */
struct Allowance
{
    enum class Method
    {
        KFactor,     ///< k as ANSI's: the neutral fibre at R + k T
        KFactorDIN,  ///< k as DIN's, measured on half the thickness: R + k T / 2
        DIN6935,     ///< K from R / T by DIN 6935 (din6935K)
        KTable,
        AllowanceTable,
        DeductionTable,
    };
    Method method = Method::KFactor;
    double k = 0.44;
    Table table;
};

/// DIN 6935's K, as ANSI's: (0.65 + 0.5 lg(R / T)) / 2 below R / T = 5, 0.5
/// from there, never below 0
PartDesignExport double din6935K(double thickness, double radius);
/// Creo's Y-factor as K: K = 2 Y / pi
PartDesignExport double kFromY(double y);

/// The K-factor (ANSI) \a allowance gives \a bend, for any method;
/// \a inside as Table::at's
PartDesignExport double kOf(const Bend& bend, const Allowance& allowance, bool* inside = nullptr);
/// The bend's flat length: the arc of its neutral fibre, A (R + K T)
PartDesignExport double bendAllowance(const Bend& bend,
                                      const Allowance& allowance,
                                      bool* inside = nullptr);
/// From each wall's outer face to where the two outer faces meet: (R + T)
/// tan(A / 2). Throws Base::ValueError from 179 deg on, where they hardly meet.
PartDesignExport double outsideSetback(const Bend& bend);
/// What the flat loses against the two outer lengths: 2 OSSB - BA
PartDesignExport double bendDeduction(const Bend& bend,
                                      const Allowance& allowance,
                                      bool* inside = nullptr);

}  // namespace PartDesign::SheetMetal
