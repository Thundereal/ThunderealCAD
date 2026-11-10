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

#include <optional>
#include <string>
#include <vector>

#include <App/DocumentObjectExtension.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "PropertySheetModel.h"

namespace PartDesign
{

class SheetMetalRule;

/** What every sheet metal feature has (docs/SheetMetal.md sec 4.2, 4.6)
 *
 * The rule it follows, and its model of the sheet. A feature put in a body
 * takes the rule of the sheet feature it builds on, or makes one if it is
 * the first (Body::insertObject()).
 */
class PartDesignExport SheetMetalExtension : public App::DocumentObjectExtension
{
    EXTENSION_PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::SheetMetalExtension);
    using inherited = App::DocumentObjectExtension;

public:
    SheetMetalExtension();

    App::PropertyXLink Rule;
    PropertySheetModel SheetModel;
    /** The K of each bend this feature makes that does not take the rule's
     * (docs/SheetMetal.md sec 4.6, ruled 2026-11-05)
     *
     * Keyed by the bend's place among the feature's own, counted from 1 in
     * the order it makes them, as its number is kept (BendNumbers). A key
     * past the bends the feature makes is kept and changes nothing. Check
     * Sheet's bend table edits it, through setBendKFactor().
     */
    App::PropertyMap BendKFactors;
    /** The numbers of the walls and of the bends this feature makes, each
     * list in the order it makes them (docs/SheetMetal.md sec 12, ruled
     * 2026-11-11): Wall4, Bend3
     *
     * A wall or a bend takes the next number of its body the first time its
     * feature makes it, and keeps it: a feature suppressed, put in or taken
     * out before this one renames nothing of this one's, in the bend table
     * or on the flat's lines. A number past what the feature makes is kept,
     * and taken up again when it makes that many. Filled by storeModel().
     */
    App::PropertyIntegerList WallNumbers;
    App::PropertyIntegerList BendNumbers;

    /// The rule, null if none
    SheetMetalRule* getRule() const;

    /// The sheet metal feature the given feature builds on: the first down
    /// its BaseFeature chain not suppressed, null if none
    static SheetMetalExtension* findBefore(const App::DocumentObject* feature);

    /** The model of the sheet this feature builds on, in this feature's frame
     *
     * Empty if there is none; throws Base::ValueError if its thickness is not
     * the rule's.
     */
    SheetMetal::Model modelBefore(double thickness) const;

    /** The radius a feature's bends take (docs/SheetMetal.md sec 12, ruled
     * 2026-11-03): the rule's, or the feature's own where useRule is off
     */
    static double bendRadius(const SheetMetalRule& rule, const App::PropertyBool& useRule,
                             const App::PropertyLength& own);
    /// useRule changed: own is read only while it is on; turned off, own
    /// starts at the rule's
    void radiusSwitched(const App::PropertyBool& useRule, App::PropertyLength& own) const;

    /// The bends of a model this feature made: those the sheet it builds on
    /// has not, by index, in the order it made them
    std::vector<int> ownBends(const SheetMetal::Model& model) const;
    /** Put BendKFactors on the bends of model this feature made, before it
     * is stored. Throws Base::ValueError for a key or a value that is none.
     */
    void applyBendOverrides(SheetMetal::Model& model) const;
    /** Store the model this feature made as its SheetModel: its own walls
     * and bends -- those the sheet it builds on has not, all of them where
     * it remade the sheet (a convert) -- named by their numbers
     * (WallNumbers, BendNumbers), its bends with their K (BendKFactors).
     *
     * A number is new where the feature has none for that wall or bend yet,
     * or has one the sheet before it holds too (a copy of a feature): one
     * above every number of the sheet before it and of every sheet metal
     * feature of its body, suppressed or not. Throws as
     * applyBendOverrides() does.
     */
    void storeModel(SheetMetal::Model model, bool remade = false);
    /// The K of one of this feature's bends, by its name, where the bend has
    /// one of its own
    std::optional<double> bendKFactor(const std::string& bend) const;
    /** Give one of this feature's bends a K of its own, or none for the
     * rule's. Throws Base::ValueError for a K outside 0..1, and for a bend
     * this feature did not make.
     */
    void setBendKFactor(const std::string& bend, std::optional<double> k);

    /// The name of the element that owns a face of the feature's shape,
    /// empty if none
    std::string elementOf(const char* subname) const;

    PyObject* getExtensionPyObject() override;
};

}  // namespace PartDesign
