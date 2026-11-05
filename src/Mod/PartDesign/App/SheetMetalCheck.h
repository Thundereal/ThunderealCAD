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

#include <string>
#include <vector>

#include <Mod/PartDesign/PartDesignGlobal.h>

namespace App
{
class DocumentObject;
}

namespace PartDesign
{

/// A bend of a sheet as its bend table lists it (docs/SheetMetal.md sec 7)
struct PartDesignExport SheetBendRow
{
    /// The bend's name by role ("Bend1"), and the feature that made it
    std::string name;
    std::string feature;
    /// deg
    double angle = 0;
    /// Its inner radius, and where that comes from: "rule", "card" or
    /// "feature" (the feature's own)
    double radius = 0;
    std::string radiusSource;
    /// The K its flat length is found by, and where that comes from: "rule",
    /// "card", the allowance method where that gives it, or "bend" where the
    /// bend has its own (SheetMetalExtension::BendKFactors)
    double kFactor = 0;
    std::string kFactorSource;
    /// The rule's allowance method, and where that comes from
    std::string method;
    std::string methodSource;
    /// Its flat length
    double allowance = 0;
    /// Up or Down, as it lies flat; empty where the part does not
    std::string direction;
    /// Its faces folded ("Face3"), and those of the flat ("FlatFace3")
    std::vector<std::string> elements;
    std::vector<std::string> flatElements;
};

/// What is wrong with a sheet, found by checkSheet()
struct PartDesignExport SheetProblem
{
    /// Overlap, Relief, ShortFlange, HoleNearBend, or NoFlat
    std::string kind;
    std::string message;
    /// The feature it is of, where it is one feature's
    std::string feature;
    /// What was measured and what it should have been at least, where one is
    double value = 0;
    double limit = 0;
    /// The faces it is about, folded and flat
    std::vector<std::string> elements;
    std::vector<std::string> flatElements;
};

struct PartDesignExport SheetCheck
{
    std::vector<SheetBendRow> bends;
    std::vector<SheetProblem> problems;
};

/** Check a sheet (docs/SheetMetal.md sec 7, ruled 2026-11-03: on demand,
 * never in a recompute): its bend table, and what is wrong with its flat
 * -- a flange shorter than the rule's MinFlange, a hole nearer a bend line
 * than its MinHoleToBend, material overlapping itself, a feature's relief
 * smaller than the rule's, a flat that cannot be laid. \a obj is a feature
 * of a sheet's solid, or the solid (its tip). Nothing is kept.
 *
 * Throws Base::ValueError for an object of no sheet.
 */
PartDesignExport SheetCheck checkSheet(const App::DocumentObject* obj);

/** The bend table alone: each bend's values and their sources, and which way
 * it lies flat (empty where the sheet does not), no faces and no problems --
 * what a drawing's bend table shows (SheetMetalBendTable). Throws as
 * checkSheet() does.
 */
PartDesignExport std::vector<SheetBendRow> sheetBends(const App::DocumentObject* obj);

}  // namespace PartDesign
