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

#ifndef PMI_GEOMETRICTOLERANCE_H
#define PMI_GEOMETRICTOLERANCE_H

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include "Callout.h"

namespace PMI
{

class DatumReferenceFrame;
class TolerancedFeature;

/** A feature control frame (docs/GDandT.md sec 4.5). Upstream's
 * PMI_GeometricTolerance. The enumerations follow OCCT's
 * XCAFDimTolObjects_* so STEP is a table, not a translation.
 */
class PMIExport GeometricTolerance: public Callout
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::GeometricTolerance);

public:
    GeometricTolerance();

    App::PropertyEnumeration Characteristic;
    App::PropertyLink Feature;
    App::PropertyEnumeration ZoneShape;
    App::PropertyLength Value;
    App::PropertyEnumeration MaterialCondition;
    /// Zone and tolerance modifiers: F, P, T, U, ST, CZ, SZ, UF, OZ, ACS,
    /// LE, NC, LD, MD, PD, AllAround, AllOver
    App::PropertyStringList Modifiers;
    /// With modifier P
    App::PropertyLength ProjectedLength;
    /// With modifier U: the offset of an unequally disposed zone
    App::PropertyDistance UnequalValue;
    /// A refinement "0.1 MAX 0.3"; 0 for none
    App::PropertyLength MaxValue;
    /// What the value holds on: the whole feature (None), or any part of it
    /// of UnitSize -- per unit length, or area: a square, a rectangle
    /// UnitSize by UnitSize2, a circle of diameter UnitSize ("0.01/25X25")
    App::PropertyEnumeration UnitBasis;
    App::PropertyLength UnitSize;
    App::PropertyLength UnitSize2;
    App::PropertyLink DatumSystem;
    /// The upper frame this is a lower segment of (a composite frame)
    App::PropertyLink Composite;

    enum CharacteristicValue
    {
        Straightness,
        Flatness,
        Circularity,
        Cylindricity,
        ProfileOfLine,
        ProfileOfSurface,
        Angularity,
        Perpendicularity,
        Parallelism,
        Position,
        Concentricity,
        Coaxiality,
        Symmetry,
        CircularRunout,
        TotalRunout,
    };
    enum ZoneShapeValue
    {
        NoZoneShape,
        DiameterZone,
        SphericalDiameterZone,
    };
    enum UnitBasisValue
    {
        WholeFeature,
        PerLength,
        PerSquare,
        PerRectangle,
        PerCircle,
    };
    static const char* UnitBasisEnums[];
    enum MaterialValue
    {
        NoMaterial,
        Maximum,
        Least,
    };
    enum Category
    {
        Form,
        Profile,
        Orientation,
        Location,
        Runout,
    };

    CharacteristicValue getCharacteristic() const
    {
        return static_cast<CharacteristicValue>(Characteristic.getValue());
    }
    static Category category(CharacteristicValue c);
    TolerancedFeature* getFeature() const;
    DatumReferenceFrame* getDatumSystem() const;
    /// The upper segment of the composite frame this is a lower segment of,
    /// or null: none, itself, or one that is a lower segment itself
    GeometricTolerance* compositeUpper() const;
    /// The lower segments of its composite frame, top down: in the set's
    /// order, else by name
    std::vector<GeometricTolerance*> lowerSegments() const;

    App::DocumentObjectExecReturn* execute() override;
    TolerancedFeature* calloutFeature() const override;
    void layout(Drawing& d, double h) const override;
    std::u32string equivalentText() const override;
    Callout* composedUnder(double h, double& below) const override;

    static const char* CharacteristicEnums[];
    static const char* ZoneShapeEnums[];
    static const char* MaterialEnums[];
    static const std::vector<std::string>& modifierNames();
};

}  // namespace PMI

#endif  // PMI_GEOMETRICTOLERANCE_H
