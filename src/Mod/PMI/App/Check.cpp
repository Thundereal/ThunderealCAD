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
#include <map>

#include "AnnotationSet.h"
#include "Check.h"
#include "Datum.h"
#include "Dimension.h"
#include "GeometricTolerance.h"
#include "TolerancedFeature.h"

using namespace PMI;
using namespace PMI::Check;

namespace
{
using GT = GeometricTolerance;

bool hasModifier(const App::PropertyStringList& list, const char* name)
{
    const auto& v = list.getValues();
    return std::find(v.begin(), v.end(), name) != v.end();
}

void checkTolerance(const GT* gt, const AnnotationSet* set, std::vector<Issue>& out)
{
    auto error = [&](const std::string& m) {
        out.push_back({gt, Error, m});
    };
    auto warn = [&](const std::string& m) {
        out.push_back({gt, Warning, m});
    };
    const auto c = gt->getCharacteristic();
    const auto cat = GT::category(c);
    const std::string name = GT::CharacteristicEnums[c];
    const auto feat = gt->getFeature();
    const auto drf = gt->getDatumSystem();
    const auto zone = static_cast<GT::ZoneShapeValue>(gt->ZoneShape.getValue());
    const auto material = static_cast<GT::MaterialValue>(gt->MaterialCondition.getValue());

    if (!feat) {
        error("a geometric tolerance needs a toleranced feature");
    }
    // Zero is a tolerance at MMC or LMC: all of it from the departure from
    // the material condition (ASME Y14.5)
    if (gt->Value.getValue() < 0.0
        || (gt->Value.getValue() == 0.0 && material == GT::NoMaterial)) {
        error(material == GT::NoMaterial && gt->Value.getValue() == 0.0
                  ? "the tolerance value must be greater than zero, but at MMC or LMC"
                  : "the tolerance value must not be negative");
    }
    if (gt->MaxValue.getValue() > 0.0 && gt->MaxValue.getValue() < gt->Value.getValue()) {
        error("the maximum is smaller than the tolerance");
    }
    if (gt->UnitBasis.getValue() != GT::WholeFeature && gt->UnitSize.getValue() <= 0.0) {
        error("a tolerance per unit needs the unit's size");
    }
    if (gt->UnitBasis.getValue() == GT::PerRectangle && gt->UnitSize2.getValue() <= 0.0) {
        error("a tolerance per rectangular unit needs its second side");
    }

    // The standard
    if (set && set->getStandard() == AnnotationSet::ASME2018
        && (c == GT::Concentricity || c == GT::Symmetry)) {
        error(name + " is not in ASME Y14.5-2018: use position, profile or runout");
    }
    if (set && !set->isASME() && c == GT::Concentricity && feat
        && feat->primitive().type != Primitive::Point) {
        warn("ISO 1101 concentricity is of a point; an axis takes coaxiality");
    }

    // Datums
    if (cat == GT::Form && drf) {
        error(name + " is a form tolerance: it takes no datums");
    }
    if ((cat == GT::Orientation || cat == GT::Runout || c == GT::Concentricity
         || c == GT::Coaxiality || c == GT::Symmetry)
        && !drf) {
        error(name + " needs a datum reference frame");
    }
    if (c == GT::Position && !drf) {
        warn("a position tolerance without datums locates a pattern only to itself");
    }

    // The zone's shape
    if (zone == GT::DiameterZone) {
        bool axial = feat && (feat->primitive().isAxial() || feat->getKind() == TolerancedFeature::Axis);
        bool allowed = c == GT::Position || c == GT::Coaxiality || c == GT::Concentricity
            || c == GT::Straightness || cat == GT::Orientation;
        if (!allowed) {
            error("a diameter zone does not apply to " + name);
        }
        else if (feat && !axial && c != GT::Position && c != GT::Concentricity) {
            error("a diameter zone needs an axis: the feature is a "
                  + std::string(feat->primitive().typeName()));
        }
    }
    if (zone == GT::SphericalDiameterZone && c != GT::Position) {
        error("a spherical diameter zone is for position only");
    }

    // Material condition
    if (material != GT::NoMaterial) {
        if (cat == GT::Runout || cat == GT::Profile || c == GT::Circularity
            || c == GT::Cylindricity) {
            error(std::string(material == GT::Maximum ? "MMR" : "LMR") + " does not apply to "
                  + name);
        }
        else if (feat && !feat->IsSize.getValue()) {
            error(std::string(material == GT::Maximum ? "MMR" : "LMR")
                  + " needs a feature of size, and the feature is not one");
        }
    }

    // Modifiers
    for (auto& m : gt->Modifiers.getValues()) {
        const auto& names = GT::modifierNames();
        if (std::find(names.begin(), names.end(), m) == names.end()) {
            error("unknown tolerance modifier '" + m + "'");
        }
    }
    bool projected = hasModifier(gt->Modifiers, "P");
    if (projected != (gt->ProjectedLength.getValue() > 0.0)) {
        error(projected ? "a projected zone (P) needs its length"
                        : "a projected length without the P modifier");
    }
    bool unequal = hasModifier(gt->Modifiers, "U");
    if (unequal && cat != GT::Profile) {
        error("an unequally disposed zone (U) is for profile tolerances");
    }

    // Composite frames
    if (auto upper = freecad_cast<GT*>(gt->Composite.getValue())) {
        if (upper == gt) {
            error("a frame cannot be a segment of itself");
        }
        else if (upper->getCharacteristic() != c || (c != GT::Position && cat != GT::Profile)) {
            error("a composite frame is of position or profile, the same in every segment");
        }
        else if (upper->getFeature() != feat) {
            error("the segments of a composite frame tolerance the same feature");
        }
    }

    if (feat && feat->getState() == Annotation::Broken) {
        error("its feature is broken: " + feat->ConditionMessage.getStrValue());
    }
}

void checkDimension(const Dimension* dim, std::vector<Issue>& out)
{
    auto error = [&](const std::string& m) {
        out.push_back({dim, Error, m});
    };
    double upper = dim->Upper.getValue();
    double lower = dim->Lower.getValue();
    bool hasTolerance = upper != 0.0 || lower != 0.0 || !dim->Fit.getStrValue().empty();
    if (dim->Basic.getValue() && hasTolerance) {
        error("a basic dimension has no tolerance");
    }
    if (upper < lower) {
        error("the upper deviation is below the lower");
    }
    if (!dim->Fit.getStrValue().empty()) {
        if (upper != 0.0 || lower != 0.0) {
            error("a fit or deviations, not both");
        }
        if (!Dimension::isFit(dim->Fit.getStrValue())) {
            error("'" + dim->Fit.getStrValue() + "' is not an ISO 286 fit (e.g. H7, g6)");
        }
        if (!dim->isSize() || dim->isAngular()) {
            error("a fit is for a linear size");
        }
    }
    for (auto& m : dim->Modifiers.getValues()) {
        const auto& names = Dimension::modifierNames();
        if (std::find(names.begin(), names.end(), m) == names.end()) {
            error("unknown size modifier '" + m + "'");
        }
    }
    if (!dim->Modifiers.getValues().empty() && !dim->isSize()) {
        error("ISO 14405 modifiers are for sizes, not locations");
    }
    double value;
    std::string why;
    if (!dim->measure(value, why)) {
        error(why);
    }
}

void checkFeature(const TolerancedFeature* feat, std::vector<Issue>& out)
{
    if (feat->getState() == Annotation::Broken) {
        out.push_back({feat, Error, "broken: " + feat->ConditionMessage.getStrValue()});
    }
    else if (feat->getState() == Annotation::Remapped) {
        out.push_back({feat, Warning, "remapped: " + feat->ConditionMessage.getStrValue()});
    }
    else if (feat->getState() == Annotation::Invalid) {
        out.push_back({feat, Error, feat->ConditionMessage.getStrValue()});
    }
}
}  // namespace

namespace
{
void checkTarget(const DatumTarget* t, std::vector<Issue>& out)
{
    const auto* obj = t;
    if (!t->getDatum()) {
        out.push_back({obj, Error, "a datum target is a datum's: list it in its Targets"});
    }
    if (t->Number.getValue() < 1) {
        out.push_back({obj, Error, "a datum target is numbered from 1"});
    }
    switch (t->getTargetType()) {
        case DatumTarget::Line:
        case DatumTarget::Circle:
            if (t->Length.getValue() <= 0) {
                bool line = t->getTargetType() == DatumTarget::Line;
                out.push_back({obj, Error, line ? "a line target needs its length"
                                                : "a circle target needs its diameter"});
            }
            break;
        case DatumTarget::Rectangle:
            if (t->Length.getValue() <= 0 || t->Width.getValue() <= 0) {
                out.push_back({obj, Error, "a rectangle target needs its length and width"});
            }
            break;
        case DatumTarget::Area:
            if (!t->calloutFeature()) {
                out.push_back({obj, Error, "an area target needs its faces"});
            }
            break;
        default:
            break;
    }
}
}  // namespace

std::vector<Issue> Check::object(const App::DocumentObject* obj)
{
    std::vector<Issue> out;
    auto anno = freecad_cast<Annotation*>(obj);
    if (!anno) {
        return out;
    }
    const AnnotationSet* set = anno->getSet();
    if (auto gt = freecad_cast<GT*>(obj)) {
        checkTolerance(gt, set, out);
    }
    else if (auto dim = freecad_cast<Dimension*>(obj)) {
        checkDimension(dim, out);
    }
    else if (auto feat = freecad_cast<TolerancedFeature*>(obj)) {
        checkFeature(feat, out);
    }
    else if (auto datum = freecad_cast<Datum*>(obj)) {
        std::string why;
        if (!Datum::isLegalLetter(datum->Letter.getStrValue(), set && set->isASME(), why)) {
            out.push_back({obj, Error, why});
        }
        if (!datum->getFeature() && datum->getTargets().empty()) {
            out.push_back({obj, Error, "a datum needs a datum feature or datum targets"});
        }
    }
    else if (auto target = freecad_cast<DatumTarget*>(obj)) {
        checkTarget(target, out);
    }
    else if (auto drf = freecad_cast<DatumReferenceFrame*>(obj)) {
        std::vector<DatumReferenceFrame::Compartment> comps;
        std::string why;
        if (!drf->parse(comps, why)) {
            out.push_back({obj, Error, why});
        }
    }
    return out;
}

std::vector<Issue> Check::set(const AnnotationSet* set)
{
    std::vector<Issue> out;
    if (!set) {
        return out;
    }
    if (!set->getPart()) {
        out.push_back({set, Error, "the set annotates no part"});
    }
    // Datum letters are a solid's own: two solids of a body are two parts,
    // and each has its A (docs/GDandT.md sec 3)
    using Solid = const App::DocumentObject*;
    std::map<Solid, std::map<std::string, const Datum*>> letters;
    std::map<std::string, int> anywhere;
    for (auto obj : set->Group.getValues()) {
        auto issues = object(obj);
        out.insert(out.end(), issues.begin(), issues.end());
        if (auto datum = freecad_cast<Datum*>(obj)) {
            const std::string letter = datum->Letter.getStrValue();
            Solid solid = datum->solid();
            auto res = letters[solid].emplace(letter, datum);
            ++anywhere[letter];
            if (!res.second) {
                out.push_back({obj, Error, "datum " + letter + " is defined twice"});
            }
        }
    }
    for (auto obj : set->Group.getValues()) {
        auto gt = freecad_cast<GT*>(obj);
        auto drf = gt ? gt->getDatumSystem() : freecad_cast<DatumReferenceFrame*>(obj);
        if (!drf) {
            continue;
        }
        std::vector<DatumReferenceFrame::Compartment> comps;
        std::string why;
        if (!drf->parse(comps, why)) {
            continue;
        }
        Solid solid = gt ? AnnotationSet::solidOf(gt->getFeature()) : nullptr;
        for (auto& comp : comps) {
            for (auto& d : comp.datums) {
                if (!anywhere.count(d)) {
                    if (!gt) {
                        out.push_back({obj, Error, "datum " + d + " is not defined"});
                    }
                }
                else if (gt && solid && !letters[solid].count(d)) {
                    out.push_back({obj, Warning, "datum " + d + " is on another solid"});
                }
            }
        }
    }
    return out;
}
