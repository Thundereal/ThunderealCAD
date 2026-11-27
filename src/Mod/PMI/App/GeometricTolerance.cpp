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
#include <cstring>

#include "AnnotationSet.h"
#include "Check.h"
#include "Datum.h"
#include "GeometricTolerance.h"
#include "TolerancedFeature.h"

using namespace PMI;

const char* GeometricTolerance::CharacteristicEnums[] = {"Straightness",
                                                         "Flatness",
                                                         "Circularity",
                                                         "Cylindricity",
                                                         "ProfileOfLine",
                                                         "ProfileOfSurface",
                                                         "Angularity",
                                                         "Perpendicularity",
                                                         "Parallelism",
                                                         "Position",
                                                         "Concentricity",
                                                         "Coaxiality",
                                                         "Symmetry",
                                                         "CircularRunout",
                                                         "TotalRunout",
                                                         nullptr};
const char* GeometricTolerance::ZoneShapeEnums[] = {"None", "Diameter", "SphericalDiameter", nullptr};
const char* GeometricTolerance::MaterialEnums[] = {"None", "Maximum", "Least", nullptr};
const char* GeometricTolerance::UnitBasisEnums[] = {"None",      "Length", "Square",
                                                    "Rectangle", "Circle", nullptr};

PROPERTY_SOURCE(PMI::GeometricTolerance, PMI::Callout)

GeometricTolerance::GeometricTolerance()
{
    ADD_PROPERTY_TYPE(Characteristic, (1L), "PMI", App::Prop_None, "What is toleranced");
    Characteristic.setEnums(CharacteristicEnums);
    ADD_PROPERTY_TYPE(Feature, (nullptr), "PMI", App::Prop_None, "The toleranced feature");
    ADD_PROPERTY_TYPE(ZoneShape, (0L), "PMI", App::Prop_None,
                      "The zone's shape: between two surfaces or lines (None), within a "
                      "cylinder or circle (Diameter), within a sphere (SphericalDiameter)");
    ZoneShape.setEnums(ZoneShapeEnums);
    ADD_PROPERTY_TYPE(Value, (0.1), "PMI", App::Prop_None, "The zone's size");
    ADD_PROPERTY_TYPE(MaterialCondition, (0L), "PMI", App::Prop_None,
                      "Maximum (M) or least (L) material requirement");
    MaterialCondition.setEnums(MaterialEnums);
    ADD_PROPERTY_TYPE(Modifiers, (), "PMI", App::Prop_None, "Zone and tolerance modifiers");
    ADD_PROPERTY_TYPE(ProjectedLength, (0.0), "PMI", App::Prop_None,
                      "The length of a projected zone (modifier P)");
    ADD_PROPERTY_TYPE(UnequalValue, (0.0), "PMI", App::Prop_None,
                      "The offset of an unequally disposed zone (modifier U)");
    ADD_PROPERTY_TYPE(MaxValue, (0.0), "PMI", App::Prop_None,
                      "A maximum the zone may not exceed (0.1 MAX 0.3); 0 for none");
    ADD_PROPERTY_TYPE(UnitBasis, (0L), "PMI", App::Prop_None,
                      "What the value holds on: the whole feature (None), or any part of it "
                      "of the unit size -- a length, a square, a rectangle, a circle");
    UnitBasis.setEnums(UnitBasisEnums);
    ADD_PROPERTY_TYPE(UnitSize, (0.0), "PMI", App::Prop_None,
                      "The unit's length, side or diameter (0.01/25)");
    ADD_PROPERTY_TYPE(UnitSize2, (0.0), "PMI", App::Prop_None,
                      "A rectangular unit's second side (0.01/25X30)");
    ADD_PROPERTY_TYPE(DatumSystem, (nullptr), "PMI", App::Prop_None,
                      "The datum reference frame; none for a form tolerance");
    ADD_PROPERTY_TYPE(Composite, (nullptr), "PMI", App::Prop_None,
                      "The upper frame this is a lower segment of, for a composite frame");
}

const std::vector<std::string>& GeometricTolerance::modifierNames()
{
    static const std::vector<std::string> names = {
        "F",  "P",  "T",  "U",  "ST", "CZ", "SZ", "UF",        "OZ",
        "ACS", "LE", "NC", "LD", "MD", "PD", "AllAround", "AllOver"};
    return names;
}

GeometricTolerance::Category GeometricTolerance::category(CharacteristicValue c)
{
    switch (c) {
        case Straightness:
        case Flatness:
        case Circularity:
        case Cylindricity:
            return Form;
        case ProfileOfLine:
        case ProfileOfSurface:
            return Profile;
        case Angularity:
        case Perpendicularity:
        case Parallelism:
            return Orientation;
        case CircularRunout:
        case TotalRunout:
            return Runout;
        default:
            return Location;
    }
}

TolerancedFeature* GeometricTolerance::calloutFeature() const
{
    return getFeature();
}

TolerancedFeature* GeometricTolerance::getFeature() const
{
    return freecad_cast<TolerancedFeature*>(Feature.getValue());
}

DatumReferenceFrame* GeometricTolerance::getDatumSystem() const
{
    return freecad_cast<DatumReferenceFrame*>(DatumSystem.getValue());
}

GeometricTolerance* GeometricTolerance::compositeUpper() const
{
    auto upper = freecad_cast<GeometricTolerance*>(Composite.getValue());
    if (!upper || upper == this || upper->Composite.getValue()) {
        return nullptr;
    }
    return upper;
}

std::vector<GeometricTolerance*> GeometricTolerance::lowerSegments() const
{
    std::vector<GeometricTolerance*> res;
    auto add = [&](App::DocumentObject* obj) {
        auto gt = freecad_cast<GeometricTolerance*>(obj);
        if (gt && gt->compositeUpper() == this
            && std::find(res.begin(), res.end(), gt) == res.end()) {
            res.push_back(gt);
        }
    };
    if (auto set = getSet()) {
        for (auto obj : set->Group.getValues()) {
            add(obj);
        }
    }
    std::vector<GeometricTolerance*> rest;
    std::swap(res, rest);
    for (auto obj : getInList()) {
        add(obj);
    }
    std::sort(res.begin(), res.end(), [](auto a, auto b) {
        return std::strcmp(a->getNameInDocument(), b->getNameInDocument()) < 0;
    });
    for (auto gt : res) {
        if (std::find(rest.begin(), rest.end(), gt) == rest.end()) {
            rest.push_back(gt);
        }
    }
    return rest;
}

Callout* GeometricTolerance::composedUnder(double h, double& below) const
{
    auto upper = compositeUpper();
    if (!upper) {
        return nullptr;
    }
    // Flush under the upper, a frame's height (twice the text) below each
    // lower segment above it
    auto lowers = upper->lowerSegments();
    auto it = std::find(lowers.begin(), lowers.end(), this);
    below = 2 * h * double(it - lowers.begin());
    return upper;
}

int GeometricTolerance::composedRows() const
{
    auto upper = compositeUpper();
    return int((upper ? upper : this)->lowerSegments().size());
}

void GeometricTolerance::touchFrame(GeometricTolerance* upper) const
{
    if (!upper) {
        return;
    }
    // The upper's symbol cell opens to its lower segments, each lower lies
    // under those above it and the last closes the cell: none depends on the
    // others, so each is told
    upper->touch();
    for (auto lower : upper->lowerSegments()) {
        if (lower != this) {
            lower->touch();
        }
    }
}

void GeometricTolerance::onBeforeChange(const App::Property* prop)
{
    // The frame it leaves
    if (prop == &Composite && !isRestoring()) {
        touchFrame(compositeUpper());
    }
    Callout::onBeforeChange(prop);
}

void GeometricTolerance::onChanged(const App::Property* prop)
{
    // The frame it joins
    if (prop == &Composite && !isRestoring()) {
        touchFrame(compositeUpper());
    }
    Callout::onChanged(prop);
}

App::DocumentObjectExecReturn* GeometricTolerance::execute()
{
    // Its own rules; the set's (datum letters defined, ...) are PMI.check's
    updatePresentation();
    auto issues = Check::object(this);
    for (auto& issue : issues) {
        if (issue.severity == Check::Error) {
            setState(Invalid, issue.message);
            return App::DocumentObject::StdReturn;
        }
    }
    auto feat = getFeature();
    if (feat && feat->getState() == Broken) {
        setState(Broken, "its feature is broken: " + feat->ConditionMessage.getStrValue());
    }
    else {
        setState(Ok);
    }
    return App::DocumentObject::StdReturn;
}
