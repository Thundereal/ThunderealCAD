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

#include <charconv>
#include <cmath>

#include <TopoDS.hxx>

#include <Base/Exception.h>
#include <Mod/Part/App/TopoShape.h>

#include "Feature.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetalExtension.h"
#include "SheetMetalExtensionPy.h"

using namespace PartDesign;

EXTENSION_PROPERTY_SOURCE(PartDesign::SheetMetalExtension, App::DocumentObjectExtension)

SheetMetalExtension::SheetMetalExtension()
{
    initExtensionType(SheetMetalExtension::getExtensionClassTypeId());
    EXTENSION_ADD_PROPERTY_TYPE(Rule,
                                (nullptr),
                                "Sheet Metal",
                                App::Prop_None,
                                "The rule the sheet follows: thickness, bend radius, allowance");
    EXTENSION_ADD_PROPERTY_TYPE(SheetModel,
                                (),
                                "Sheet Metal",
                                App::PropertyType(App::Prop_Hidden | App::Prop_Output
                                                  | App::Prop_NoRecompute),
                                "The sheet's walls and bends, made with the shape");
    EXTENSION_ADD_PROPERTY_TYPE(BendKFactors,
                                (),
                                "Sheet Metal",
                                App::Prop_Hidden,
                                "The K of each bend this feature makes that does not take the "
                                "rule's, by the bend's place among its own counted from 1. Check "
                                "Sheet's bend table edits it");
}

SheetMetalRule* SheetMetalExtension::getRule() const
{
    return dynamic_cast<SheetMetalRule*>(Rule.getValue());
}

double SheetMetalExtension::bendRadius(const SheetMetalRule& rule,
                                       const App::PropertyBool& useRule,
                                       const App::PropertyLength& own)
{
    return useRule.getValue() ? rule.BendRadius.getValue() : own.getValue();
}

void SheetMetalExtension::radiusSwitched(const App::PropertyBool& useRule,
                                         App::PropertyLength& own) const
{
    auto obj = getExtendedObject();
    auto rule = getRule();
    if (!useRule.getValue() && own.testStatus(App::Property::ReadOnly) && rule && obj
        && !obj->isRestoring()) {
        own.setValue(rule->BendRadius.getValue());
    }
    own.setStatus(App::Property::ReadOnly, useRule.getValue());
}

SheetMetalExtension* SheetMetalExtension::findBefore(const App::DocumentObject* feature)
{
    auto pd = dynamic_cast<const PartDesign::Feature*>(feature);
    for (auto obj = pd ? pd->BaseFeature.getValue() : nullptr; obj;) {
        auto base = dynamic_cast<PartDesign::Feature*>(obj);
        // a suppressed one's shape is its base's, its model not
        auto ext = obj->getExtensionByType<SheetMetalExtension>(true);
        if (ext && !(base && base->Suppress.getValue())) {
            return ext;
        }
        obj = base ? base->BaseFeature.getValue() : nullptr;
    }
    return nullptr;
}

namespace
{

/// What places a feature's shape, as a transform
gp_Trsf placementOf(const App::DocumentObject* obj)
{
    auto feature = dynamic_cast<const Part::Feature*>(obj);
    if (!feature) {
        return {};
    }
    return Part::TopoShape::convert(feature->Placement.getValue().toMatrix());
}

}  // namespace

SheetMetal::Model SheetMetalExtension::modelBefore(double thickness) const
{
    auto before = findBefore(getExtendedObject());
    if (!before) {
        return {};
    }
    SheetMetal::Model model = before->SheetModel.getValue();
    if (model.elements.empty()) {
        return model;
    }
    if (std::abs(model.thickness - thickness) > 1e-6) {
        FC_THROWM(Base::ValueError,
                  "The sheet this builds on is " << model.thickness << " mm thick, its rule says "
                                                 << thickness << " mm");
    }
    // from that feature's frame into this one's
    model.transform(placementOf(getExtendedObject()).Inverted()
                    * placementOf(before->getExtendedObject()));
    return model;
}

namespace
{

/// A number written whole, or nothing
std::optional<double> parseNumber(const std::string& text)
{
    double value = 0;
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc() || ptr != end || !std::isfinite(value)) {
        return {};
    }
    return value;
}

bool validK(double k)
{
    return k >= 0 && k <= 1;
}

}  // namespace

std::vector<int> SheetMetalExtension::ownBends(const SheetMetal::Model& model) const
{
    SheetMetal::Model earlier;
    if (auto before = findBefore(getExtendedObject())) {
        earlier = before->SheetModel.getValue();
    }
    std::vector<int> res;
    for (std::size_t i = 0; i < model.elements.size(); ++i) {
        const auto& element = model.elements[i];
        if (element.kind == SheetMetal::Element::Kind::Bend && earlier.find(element.name) < 0) {
            res.push_back(static_cast<int>(i));
        }
    }
    return res;
}

void SheetMetalExtension::applyBendOverrides(SheetMetal::Model& model) const
{
    const auto& values = BendKFactors.getValues();
    if (values.empty()) {
        return;
    }
    const std::vector<int> own = ownBends(model);
    for (const auto& [key, text] : values) {
        int place = 0;
        auto [ptr, ec] = std::from_chars(key.data(), key.data() + key.size(), place);
        if (ec != std::errc() || ptr != key.data() + key.size() || place < 1) {
            FC_THROWM(Base::ValueError,
                      "BendKFactors: '" << key << "' is no bend's place, counted from 1");
        }
        const auto k = parseNumber(text);
        if (!k || !validK(*k)) {
            FC_THROWM(Base::ValueError,
                      "BendKFactors: the K of bend " << key << ", '" << text
                                                     << "', is no number from 0 to 1");
        }
        if (place <= static_cast<int>(own.size())) {
            model.elements[own[place - 1]].bend.k = *k;
        }
    }
}

namespace
{

/// The key of one of a feature's bends in BendKFactors, empty if it made
/// none of that name
std::string placeOf(const SheetMetalExtension& ext, const std::string& bend)
{
    const SheetMetal::Model model = ext.SheetModel.getValue();
    const std::vector<int> own = ext.ownBends(model);
    for (std::size_t i = 0; i < own.size(); ++i) {
        if (model.elements[own[i]].name == bend) {
            return std::to_string(i + 1);
        }
    }
    return {};
}

}  // namespace

std::optional<double> SheetMetalExtension::bendKFactor(const std::string& bend) const
{
    if (BendKFactors.getValues().empty()) {
        return {};
    }
    const std::string key = placeOf(*this, bend);
    const auto& values = BendKFactors.getValues();
    auto it = key.empty() ? values.end() : values.find(key);
    if (it == values.end()) {
        return {};
    }
    auto k = parseNumber(it->second);
    if (!k || !validK(*k)) {
        return {};
    }
    return k;
}

void SheetMetalExtension::setBendKFactor(const std::string& bend, std::optional<double> k)
{
    if (k && !(std::isfinite(*k) && validK(*k))) {
        FC_THROWM(Base::ValueError, "A K factor is from 0 to 1, not " << *k);
    }
    const std::string key = placeOf(*this, bend);
    if (key.empty()) {
        auto obj = getExtendedObject();
        FC_THROWM(Base::ValueError,
                  (obj ? obj->getFullName() : std::string("This feature")) << " makes no bend '"
                                                                          << bend << "'");
    }
    auto values = BendKFactors.getValues();
    if (k) {
        // the shortest text that reads back as the same number
        char buf[32];
        auto res = std::to_chars(buf, buf + sizeof(buf), *k);
        values[key] = std::string(buf, res.ptr);
    }
    else if (!values.erase(key)) {
        return;
    }
    BendKFactors.setValues(values);
}

std::string SheetMetalExtension::elementOf(const char* subname) const
{
    auto feature = dynamic_cast<const Part::Feature*>(getExtendedObject());
    if (!feature) {
        return {};
    }
    const Part::TopoShape face = feature->Shape.getShape().getSubTopoShape(subname, true);
    if (face.isNull() || face.getShape().ShapeType() != TopAbs_FACE) {
        return {};
    }
    SheetMetal::Model model = SheetModel.getValue();
    model.transform(placementOf(feature));
    const int index = model.owner(TopoDS::Face(face.getShape()));
    return index < 0 ? std::string() : model.elements[index].name;
}

PyObject* SheetMetalExtension::getExtensionPyObject()
{
    if (ExtensionPythonObject.is(Py::_None())) {
        ExtensionPythonObject = Py::Object(new SheetMetalExtensionPy(this), true);
    }
    return Py::new_reference_to(ExtensionPythonObject);
}
