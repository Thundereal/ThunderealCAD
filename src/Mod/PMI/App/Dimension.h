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

#ifndef PMI_DIMENSION_H
#define PMI_DIMENSION_H

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include "Annotation.h"

namespace PMI
{

class TolerancedFeature;

/** A size or location dimension, or a basic one (docs/GDandT.md sec 4.4).
 *
 * Nominal is measured from the features' geometry on every recompute, so a
 * dimension cannot disagree with the model; the tolerance is the user's and
 * is never touched. Upstream's PMI_Dimension.
 */
class PMIExport Dimension: public Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::Dimension);

public:
    Dimension();

    App::PropertyEnumeration Type;
    /// One toleranced feature for a size, two for a location
    App::PropertyLinkList Features;
    App::PropertyQuantity Nominal;
    App::PropertyQuantity Upper;
    App::PropertyQuantity Lower;
    /// An ISO 286 fit instead of Upper/Lower, e.g. "H7", "g6"
    App::PropertyString Fit;
    /// A basic dimension (ISO: theoretically exact): no tolerance, boxed
    App::PropertyBool Basic;
    /// ISO 14405 size modifiers: LP, LS, GG, GX, GN, GC, CC, CA, CV, SX, SN,
    /// SA, SM, SD, SR, SQ, ACS, SCS, CT, ... (and E, the envelope)
    App::PropertyStringList Modifiers;
    /// -1: the set's
    App::PropertyInteger DecimalPlaces;

    enum TypeValue
    {
        Diameter,
        Radius,
        SphericalDiameter,
        SphericalRadius,
        Width,
        AngularSize,
        Distance,
        Angle,
    };
    TypeValue getType() const
    {
        return static_cast<TypeValue>(Type.getValue());
    }
    bool isSize() const
    {
        return getType() < Distance;
    }
    bool isAngular() const
    {
        return getType() == AngularSize || getType() == Angle;
    }

    App::DocumentObjectExecReturn* execute() override;
    void onChanged(const App::Property* prop) override;

    /// Measure the nominal; false with \a why if the features do not have
    /// one of this type. Lengths in mm, angles in degrees.
    bool measure(double& value, std::string& why) const;

    static const char* TypeEnums[];
    static const std::vector<std::string>& modifierNames();
    /// Whether \a fit is an ISO 286 fit: a deviation (A..ZC, a..zc, JS, js)
    /// and a grade (01, 0, 1..18)
    static bool isFit(const std::string& fit);

private:
    void setUnits();
};

}  // namespace PMI

#endif  // PMI_DIMENSION_H
