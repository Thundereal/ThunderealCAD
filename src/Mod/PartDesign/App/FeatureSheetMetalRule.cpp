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
#include <cmath>
#include <numbers>
#include <optional>

#include <App/Document.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Quantity.h>
#include <Base/Tools.h>
#include <Mod/Material/App/MaterialValue.h>
#include <Mod/Material/App/Materials.h>
#include <Mod/Material/App/ModelUuids.h>

#include "Body.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetalRulePy.h"

FC_LOG_LEVEL_INIT("PartDesign", true, true)

using namespace PartDesign;

namespace
{

// In the order of SheetMetal::Allowance::Method
const char* MethodEnums[] = {"KFactor",
                             "KFactorDIN",
                             "DIN6935",
                             "KTable",
                             "AllowanceTable",
                             "DeductionTable",
                             nullptr};
const char* ReliefEnums[] = {"Rectangle", "Obround", "Tear", nullptr};
const char* CornerReliefEnums[] = {"None", "Circle", "Square", "Tear", nullptr};

// Two thicknesses this close are one gauge
constexpr double GaugeTolerance = 1e-6;

const QString& sheetMetalModel()
{
    return Materials::ModelUUIDs::ModelUUID_SheetMetal_SheetMetal;
}

/// The card's 2D array named, or null where it has none or an empty one
std::shared_ptr<Materials::Array2D> arrayOf(const Materials::Material& mat, const char* name)
{
    if (!mat.hasPhysicalModel(sheetMetalModel())
        || !mat.hasPhysicalProperty(QString::fromLatin1(name))) {
        return {};
    }
    auto prop = mat.getPhysicalProperty(QString::fromLatin1(name));
    if (!prop || prop->isNull() || prop->getType() != Materials::MaterialValue::Array2D) {
        return {};
    }
    auto array = std::static_pointer_cast<Materials::Array2D>(prop->getMaterialValue());
    if (!array || array->rows() == 0) {
        return {};
    }
    return array;
}

/// A cell as a number: a length in mm, an angle in degrees, a plain number
double numberOf(const QVariant& value)
{
    if (value.userType() == qMetaTypeId<Base::Quantity>()) {
        return value.value<Base::Quantity>().getValue();
    }
    bool ok = false;
    double res = value.toDouble(&ok);
    if (!ok) {
        // a cell given as text, "1.5 mm" or "0.42"
        res = Base::Quantity::parse(value.toString().toStdString()).getValue();
    }
    return res;
}

struct Gauge
{
    double radius;
    double k;
    double minFlange;
};

/// The card's gauge row of \a thickness
std::optional<Gauge> gaugeOf(const Materials::Material& mat, double thickness, bool* hasGauges)
{
    auto array = arrayOf(mat, "Gauges");
    if (hasGauges) {
        *hasGauges = bool(array);
    }
    if (!array) {
        return {};
    }
    for (int r = 0; r < array->rows(); ++r) {
        if (std::fabs(numberOf(array->getValue(r, 0)) - thickness) <= GaugeTolerance) {
            return Gauge {numberOf(array->getValue(r, 1)),
                          numberOf(array->getValue(r, 2)),
                          numberOf(array->getValue(r, 3))};
        }
    }
    return {};
}

}  // namespace

PROPERTY_SOURCE(PartDesign::SheetMetalRule, Part::Feature)

SheetMetalRule::SheetMetalRule()
{
    const char* group = "Sheet Metal";
    ADD_PROPERTY_TYPE(Thickness, (1.0), group, App::Prop_None, "The sheet's thickness");
    ADD_PROPERTY_TYPE(BendRadius, (1.0), group, App::Prop_None,
                      "The inner radius of a bend; from the card's gauge row for the thickness");
    ADD_PROPERTY_TYPE(MinFlange, (0.0), group, App::Prop_None,
                      "The shortest flange the tooling makes; from the card's gauge row");
    ADD_PROPERTY_TYPE(MinHoleToBend, (0.0), group, App::Prop_None,
                      "The least distance from a hole to a bend line, checked on demand;\n"
                      "0 for 2.5 x the thickness plus the bend's radius");
    ADD_PROPERTY_TYPE(AllowanceMethod, (0L), group, App::Prop_None,
                      "How a bend's flat length is found; from the card");
    AllowanceMethod.setEnums(MethodEnums);
    ADD_PROPERTY_TYPE(KFactor, (0.5), group, App::Prop_None,
                      "Where the neutral fibre lies, as a share of the thickness from the "
                      "inside; from the card's gauge row");
    ADD_PROPERTY_TYPE(Gap, (0.1), group, App::Prop_None,
                      "The gap between two walls meeting at a corner");
    ADD_PROPERTY_TYPE(ReliefShape, (0L), group, App::Prop_None,
                      "The relief cut where a bend ends inside a wall");
    ReliefShape.setEnums(ReliefEnums);
    ADD_PROPERTY_TYPE(ReliefWidth, (1.0), group, App::Prop_None, "The bend relief's width");
    ADD_PROPERTY_TYPE(ReliefDepth, (1.0), group, App::Prop_None,
                      "How far the bend relief runs past the bend");
    ADD_PROPERTY_TYPE(CornerReliefShape, (1L), group, App::Prop_None,
                      "The relief cut where bends meet at a corner");
    CornerReliefShape.setEnums(CornerReliefEnums);
    ADD_PROPERTY_TYPE(CornerReliefSize, (2.0), group, App::Prop_None,
                      "The corner relief's size");
    ADD_PROPERTY_TYPE(CornerReliefCover, (1.0), group, App::Prop_None,
                      "How much of the bends' crossing a corner relief covers at least: 1 all\n"
                      "of it, so the flat does not overlap there; the size is used where\n"
                      "larger. 0 for the size alone");
    static const App::PropertyFloatConstraint::Constraints coverRange = {0.0, 10.0, 0.05};
    CornerReliefCover.setConstraints(&coverRange);
    ADD_PROPERTY_TYPE(Overridden, (), group, App::Prop_Hidden,
                      "The values set by hand over the card's");

    // Nothing to place: the rule has no geometry of its own yet
    Placement.setStatus(App::Property::Hidden, true);
}

App::DocumentObjectExecReturn* SheetMetalRule::execute()
{
    if (!Shape.getShape().isNull()) {
        Shape.setValue(Part::TopoShape());
    }
    return App::DocumentObject::StdReturn;
}

App::DocumentObjectExecReturn* SheetMetalRule::recompute()
{
    takeBodyMaterial();
    return inherited::recompute();
}

void SheetMetalRule::takeBodyMaterial()
{
    // As a feature does (Feature::recompute()); a new rule is not touched,
    // so the body hands it over as the rule comes in too
    if (auto body = Body::findBodyOf(this)) {
        copyMaterial(body);
    }
}

bool SheetMetalRule::isCardValue(const App::Property* prop) const
{
    return prop == &BendRadius || prop == &KFactor || prop == &MinFlange
        || prop == &AllowanceMethod;
}

void SheetMetalRule::onChanged(const App::Property* prop)
{
    auto doc = getDocument();
    if (doc && !isRestoring() && !doc->testStatus(App::Document::Restoring)
        && !doc->isPerformingTransaction() && !takingCard) {
        if (prop == &ShapeMaterial || prop == &Thickness || prop == &Overridden) {
            takeCard();
        }
        else if (isCardValue(prop)) {
            auto names = Overridden.getValues();
            if (std::find(names.begin(), names.end(), prop->getName()) == names.end()) {
                names.emplace_back(prop->getName());
                Base::StateLocker guard(takingCard);
                Overridden.setValues(names);
            }
        }
    }
    inherited::onChanged(prop);
}

void SheetMetalRule::onDocumentRestored()
{
    Placement.setStatus(App::Property::Hidden, true);
    inherited::onDocumentRestored();
}

void SheetMetalRule::takeCard()
{
    const auto& mat = ShapeMaterial.getValue();
    if (!mat.hasPhysicalModel(sheetMetalModel())) {
        return;
    }
    Base::StateLocker guard(takingCard);
    const auto& names = Overridden.getValues();
    auto overridden = [&](const App::Property& prop) {
        return std::find(names.begin(), names.end(), prop.getName()) != names.end();
    };

    if (!overridden(AllowanceMethod)) {
        std::string method = mat.getPhysicalValueString(QStringLiteral("AllowanceMethod"))
                                 .trimmed()
                                 .toStdString();
        if (method.empty()) {
            method = MethodEnums[0];
        }
        else if (!AllowanceMethod.isPartOf(method.c_str())) {
            FC_WARN(getFullName() << ": the card " << mat.getName().toStdString()
                                  << " names the allowance method '" << method
                                  << "', which there is none of; KFactor is taken");
            method = MethodEnums[0];
        }
        if (method != AllowanceMethod.getValueAsString()) {
            AllowanceMethod.setValue(method.c_str());
        }
    }

    bool hasGauges = false;
    auto gauge = gaugeOf(mat, Thickness.getValue(), &hasGauges);
    if (!gauge) {
        if (hasGauges) {
            FC_WARN(getFullName() << ": the card " << mat.getName().toStdString()
                                  << " has no gauge " << Thickness.getValue()
                                  << " mm thick; the rule keeps its own values");
        }
        return;
    }
    auto set = [&](auto& prop, double value) {
        if (!overridden(prop) && prop.getValue() != value) {
            prop.setValue(value);
        }
    };
    set(BendRadius, gauge->radius);
    set(KFactor, gauge->k);
    set(MinFlange, gauge->minFlange);
}

bool SheetMetalRule::hasGauge() const
{
    return bool(gaugeOf(ShapeMaterial.getValue(), Thickness.getValue(), nullptr));
}

const char* SheetMetalRule::valueSource(const App::Property& prop) const
{
    if (!isCardValue(&prop)) {
        return "rule";
    }
    const auto& names = Overridden.getValues();
    if (std::find(names.begin(), names.end(), prop.getName()) != names.end()) {
        return "rule";
    }
    if (&prop == &AllowanceMethod) {
        return ShapeMaterial.getValue().hasPhysicalModel(sheetMetalModel()) ? "card" : "rule";
    }
    return hasGauge() ? "card" : "rule";
}

void SheetMetalRule::resetValue(const char* name)
{
    auto prop = getPropertyByName(name);
    if (!prop || !isCardValue(prop)) {
        FC_THROWM(Base::ValueError,
                  "The card gives no '" << (name ? name : "") << "': it gives BendRadius, "
                                        << "KFactor, MinFlange and AllowanceMethod");
    }
    auto names = Overridden.getValues();
    auto it = std::find(names.begin(), names.end(), prop->getName());
    if (it == names.end()) {
        return;
    }
    names.erase(it);
    // onChanged() takes the card again
    Overridden.setValues(names);
}

SheetMetal::Bend SheetMetalRule::getBend(double angle, double radius) const
{
    return {Thickness.getValue(), radius < 0 ? BendRadius.getValue() : radius, angle};
}

double SheetMetalRule::modelK(double angle, double radius) const
{
    const double ba = SheetMetal::bendAllowance(getBend(angle, radius), getAllowance());
    return (ba / angle - radius) / Thickness.getValue();
}

SheetMetal::Allowance SheetMetalRule::getAllowance() const
{
    using Method = SheetMetal::Allowance::Method;
    SheetMetal::Allowance allowance;
    allowance.method = static_cast<Method>(AllowanceMethod.getValue());
    allowance.k = KFactor.getValue();

    int column;
    switch (allowance.method) {
        case Method::KTable:
            column = 3;
            break;
        case Method::AllowanceTable:
            column = 4;
            break;
        case Method::DeductionTable:
            column = 5;
            break;
        default:
            return allowance;
    }
    const auto& mat = ShapeMaterial.getValue();
    auto array = arrayOf(mat, "BendTable");
    if (!array) {
        FC_THROWM(Base::ValueError,
                  getFullName() << ": the allowance method " << AllowanceMethod.getValueAsString()
                                << " takes its table from the card, and the card "
                                << mat.getName().toStdString() << " has no bend table");
    }
    std::vector<SheetMetal::Table::Row> rows;
    for (int r = 0; r < array->rows(); ++r) {
        rows.push_back({numberOf(array->getValue(r, 0)),
                        numberOf(array->getValue(r, 1)),
                        numberOf(array->getValue(r, 2)) * std::numbers::pi / 180,
                        numberOf(array->getValue(r, column))});
    }
    allowance.table = SheetMetal::Table::fromRows(rows);
    return allowance;
}

PyObject* SheetMetalRule::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        PythonObject = Py::Object(new SheetMetalRulePy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}
