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

PROPERTY_SOURCE(PMI::GeometricTolerance, PMI::Annotation)

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

TolerancedFeature* GeometricTolerance::getFeature() const
{
    return freecad_cast<TolerancedFeature*>(Feature.getValue());
}

DatumReferenceFrame* GeometricTolerance::getDatumSystem() const
{
    return freecad_cast<DatumReferenceFrame*>(DatumSystem.getValue());
}

App::DocumentObjectExecReturn* GeometricTolerance::execute()
{
    // Its own rules; the set's (datum letters defined, ...) are PMI.check's
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
