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

#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "SheetMetal/BendAllowance.h"

namespace PartDesign
{

/** The values a sheet's features share (docs/SheetMetal.md sec 4.6)
 *
 * Its own, or from its material card (ShapeMaterial) where that has the
 * SheetMetal model: BendRadius, KFactor and MinFlange from the card's gauge
 * row for Thickness, AllowanceMethod and the bend table from the card. A
 * value set by hand is the rule's from then on (Overridden), until
 * resetValue() gives it back to the card. The features link the rule and
 * depend on it; it depends on nothing of theirs.
 *
 * Its shape is empty, kept for later use.
 */
class PartDesignExport SheetMetalRule : public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::SheetMetalRule);
    using inherited = Part::Feature;

public:
    SheetMetalRule();

    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalRule";
    }

    App::PropertyLength Thickness;
    App::PropertyLength BendRadius;
    App::PropertyLength MinFlange;
    /// The least distance from a hole to a bend line, checked on demand
    /// (checkSheet()); 0 for 2.5 x the thickness plus the bend's radius
    App::PropertyLength MinHoleToBend;
    App::PropertyEnumeration AllowanceMethod;
    App::PropertyFloat KFactor;
    App::PropertyLength Gap;
    App::PropertyEnumeration ReliefShape;
    App::PropertyLength ReliefWidth;
    App::PropertyLength ReliefDepth;
    App::PropertyEnumeration CornerReliefShape;
    App::PropertyLength CornerReliefSize;
    App::PropertyFloatConstraint CornerReliefCover;
    /// The names of the values set by hand over the card's
    App::PropertyStringList Overridden;

    App::DocumentObjectExecReturn *execute() override;
    App::DocumentObjectExecReturn *recompute() override;
    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;
    PyObject *getPyObject() override;

    /// Take the material of the body holding the rule, unless it has its own
    void takeBodyMaterial();

    /// Whether the card has the SheetMetal model and a gauge row for Thickness
    bool hasGauge() const;
    /// Where a value of the rule comes from: "card" where the card gives it
    /// (BendRadius, KFactor and MinFlange from a gauge row, AllowanceMethod
    /// from a card with the SheetMetal model) and it is not set by hand,
    /// "rule" otherwise
    const char* valueSource(const App::Property& prop) const;
    /// Give the value named back to the card. Throws Base::ValueError for a
    /// name the card does not give.
    void resetValue(const char* name);

    /// How a bend's flat length is found under this rule, its table from the card
    SheetMetal::Allowance getAllowance() const;
    /// A bend of the rule's thickness, of its BendRadius unless given another
    SheetMetal::Bend getBend(double angle, double radius = -1) const;
    /** The K of a sheet model's bend (SheetMetal::CylBend): the one that
     * gives this rule's allowance for a bend through angle (rad) of that
     * inner radius, by whatever method
     */
    double modelK(double angle, double radius) const;

private:
    /// Take what the card gives into every value not set by hand
    void takeCard();
    bool isCardValue(const App::Property* prop) const;

    bool takingCard = false;
    bool gaugeFound = false;
};

}  // namespace PartDesign
