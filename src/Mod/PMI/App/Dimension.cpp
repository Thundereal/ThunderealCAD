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

#include <cctype>
#include <cmath>
#include <regex>

#include <Base/Unit.h>

#include "Dimension.h"
#include "TolerancedFeature.h"

using namespace PMI;

const char* Dimension::TypeEnums[] = {"Diameter",
                                      "Radius",
                                      "SphericalDiameter",
                                      "SphericalRadius",
                                      "Width",
                                      "AngularSize",
                                      "Distance",
                                      "Angle",
                                      nullptr};

PROPERTY_SOURCE(PMI::Dimension, PMI::Annotation)

Dimension::Dimension()
{
    ADD_PROPERTY_TYPE(Type, (0L), "PMI", App::Prop_None,
                      "A size (Diameter ... AngularSize) of one feature, or a location "
                      "(Distance, Angle) between two");
    Type.setEnums(TypeEnums);
    ADD_PROPERTY_TYPE(Features, (nullptr), "PMI", App::Prop_None,
                      "The toleranced features: one for a size, two for a location");
    ADD_PROPERTY_TYPE(Nominal, (0.0), "PMI", App::Prop_ReadOnly,
                      "Measured from the features on every recompute");
    ADD_PROPERTY_TYPE(Upper, (0.0), "PMI", App::Prop_None, "The upper deviation");
    ADD_PROPERTY_TYPE(Lower, (0.0), "PMI", App::Prop_None, "The lower deviation");
    ADD_PROPERTY_TYPE(Fit, (""), "PMI", App::Prop_None,
                      "An ISO 286 fit instead of the deviations, e.g. H7");
    ADD_PROPERTY_TYPE(Basic, (false), "PMI", App::Prop_None,
                      "A basic (theoretically exact) dimension: no tolerance");
    ADD_PROPERTY_TYPE(Modifiers, (), "PMI", App::Prop_None, "ISO 14405 size modifiers");
    ADD_PROPERTY_TYPE(DecimalPlaces, (-1), "PMI", App::Prop_None,
                      "Decimal places shown; -1 for the set's");
    setUnits();
}

const std::vector<std::string>& Dimension::modifierNames()
{
    static const std::vector<std::string> names = {
        "E",  "LP", "LS", "GG", "GX", "GN", "GC", "CC", "CA",  "CV",  "SX",       "SN",
        "SA", "SM", "SD", "SR", "SQ", "ACS", "SCS", "CT", "F", "Between", "UnitBasis"};
    return names;
}

bool Dimension::isFit(const std::string& fit)
{
    static const std::regex re("^([A-HJKMNPR-Z]|JS|Z[ABC]|[a-hjkmnpr-z]|js|z[abc]|CD|EF|FG|cd|ef|fg)"
                               "(01|0|[1-9]|1[0-8])$");
    return std::regex_match(fit, re);
}

void Dimension::setUnits()
{
    Base::Unit unit = isAngular() ? Base::Unit::Angle : Base::Unit::Length;
    for (App::PropertyQuantity* p : {&Nominal, &Upper, &Lower}) {
        if (p->getUnit() != unit) {
            p->setUnit(unit);
        }
    }
}

void Dimension::onChanged(const App::Property* prop)
{
    if (prop == &Type) {
        setUnits();
    }
    Annotation::onChanged(prop);
}

bool Dimension::measure(double& value, std::string& why) const
{
    std::vector<TolerancedFeature*> feats;
    for (auto obj : Features.getValues()) {
        if (auto f = freecad_cast<TolerancedFeature*>(obj)) {
            feats.push_back(f);
        }
    }
    std::size_t need = isSize() ? 1 : 2;
    if (feats.size() != need) {
        why = isSize() ? "a size dimension needs one toleranced feature"
                       : "a location dimension needs two toleranced features";
        return false;
    }
    const Primitive& p = feats[0]->primitive();
    const auto& faces = feats[0]->facePrimitives();
    switch (getType()) {
        case Diameter:
        case Radius:
            if (p.type != Primitive::Cylinder) {
                why = "a diameter or radius needs a cylindrical feature";
                return false;
            }
            value = getType() == Diameter ? 2 * p.radius : p.radius;
            return true;
        case SphericalDiameter:
        case SphericalRadius:
            if (p.type != Primitive::Sphere) {
                why = "a spherical size needs a spherical feature";
                return false;
            }
            value = getType() == SphericalDiameter ? 2 * p.radius : p.radius;
            return true;
        case Width:
            if (faces.size() != 2 || faces[0].type != Primitive::Plane
                || faces[1].type != Primitive::Plane) {
                why = "a width needs a feature of two parallel planar faces";
                return false;
            }
            return Geom::distance(faces[0], faces[1], value, why);
        case AngularSize:
            if (p.type == Primitive::Cone) {
                value = 2 * p.angle * 180.0 / M_PI;
                return true;
            }
            if (faces.size() == 2 && Geom::angle(faces[0], faces[1], value, why)) {
                value *= 180.0 / M_PI;
                return true;
            }
            why = "an angular size needs a cone or two planar faces";
            return false;
        case Distance:
            return Geom::distance(p, feats[1]->primitive(), value, why);
        case Angle:
            if (Geom::angle(p, feats[1]->primitive(), value, why)) {
                value *= 180.0 / M_PI;
                return true;
            }
            return false;
    }
    return false;
}

App::DocumentObjectExecReturn* Dimension::execute()
{
    setUnits();
    double value = 0.0;
    std::string why;
    if (!measure(value, why)) {
        setState(Invalid, why);
        return App::DocumentObject::StdReturn;
    }
    if (std::fabs(Nominal.getValue() - value) > 1e-12) {
        Nominal.setValue(value);
    }
    for (auto obj : Features.getValues()) {
        auto f = freecad_cast<TolerancedFeature*>(obj);
        if (f && f->getState() == Broken) {
            setState(Broken, "its feature is broken: " + f->ConditionMessage.getStrValue());
            return App::DocumentObject::StdReturn;
        }
    }
    setState(Ok);
    return App::DocumentObject::StdReturn;
}
