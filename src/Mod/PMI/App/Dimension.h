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

#include <gp_Vec.hxx>
#include <TopoDS_Shape.hxx>

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include "Callout.h"

namespace PMI
{

class TolerancedFeature;

/** A size or location dimension, or a basic one (docs/GDandT.md sec 4.4).
 *
 * Nominal is measured from the features' geometry on every recompute, so a
 * dimension cannot disagree with the model; the tolerance is the user's and
 * is never touched. Upstream's PMI_Dimension.
 */
class PMIExport Dimension: public Callout
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
    /// The tolerance given as the limits it allows, 34.8-35.2, not as the
    /// deviations, 35 +-0.2 (ASME Y14.5 limit dimensioning; STEP's range)
    App::PropertyBool Limits;
    /// A reference dimension, for information only: in parentheses, no
    /// tolerance (STEP: a 'dimensional note' that is 'auxiliary')
    App::PropertyBool Reference;

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
        /// The length along a curve: a size, after the locations to keep
        /// the values saved before it
        CurveLength,
        /// A hole's depth along its axis, to its floor: a size
        Depth,
    };
    TypeValue getType() const
    {
        return static_cast<TypeValue>(Type.getValue());
    }
    bool isSize() const
    {
        return getType() < Distance || getType() >= CurveLength;
    }
    bool isAngular() const
    {
        return getType() == AngularSize || getType() == Angle;
    }

    App::DocumentObjectExecReturn* execute() override;
    TolerancedFeature* calloutFeature() const override;
    void layout(Drawing& d, double h) const override;
    std::u32string equivalentText() const override;
    void onChanged(const App::Property* prop) override;
    /// A linear dimension (a width, a distance) lies in a plane along what
    /// it measures, beside the part, its dimension line between extension
    /// lines from the features
    bool autoPlane(double h, gp_Ax3& plane, gp_Pnt& target) const override;
    LeaderStyle leaderStyle() const override
    {
        return _linear ? NoLeader : ArrowLeader;
    }

    /// What a linear dimension measures along (\a dir), from a point of the
    /// first side to one of the second, and the shapes of the two sides;
    /// false if it is not one
    bool linear(gp_Vec& dir, gp_Pnt& p1, gp_Pnt& p2, TopoDS_Shape& s1, TopoDS_Shape& s2) const;

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
    /// Draws the text (and its box, if basic) with the baseline's start at
    /// (x, y); returns its width
    double label(Drawing& d, double h, double x, double y) const;

    /// Whether the last layout drew dimension and extension lines
    mutable bool _linear = false;
};

}  // namespace PMI

#endif  // PMI_DIMENSION_H
