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
#include <set>

#include <TopoDS.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShape.h>

#include "Body.h"
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
                                "rule's, by the bend's number, 3 for Bend3. Check "
                                "Sheet's bend table edits it");
    EXTENSION_ADD_PROPERTY_TYPE(WallNumbers,
                                (std::vector<long>()),
                                "Sheet Metal",
                                App::PropertyType(App::Prop_Hidden | App::Prop_Output
                                                  | App::Prop_NoRecompute),
                                "The number of each wall this feature makes, in the order it "
                                "makes them, kept from when it first made it");
    EXTENSION_ADD_PROPERTY_TYPE(BendNumbers,
                                (std::vector<long>()),
                                "Sheet Metal",
                                App::PropertyType(App::Prop_Hidden | App::Prop_Output
                                                  | App::Prop_NoRecompute),
                                "The number of each bend this feature makes, in the order it "
                                "makes them, kept from when it first made it");
    EXTENSION_ADD_PROPERTY_TYPE(WallKeys,
                                (std::vector<std::string>()),
                                "Sheet Metal",
                                App::PropertyType(App::Prop_Hidden | App::Prop_Output
                                                  | App::Prop_NoRecompute),
                                "What each of WallNumbers is of, where the feature says: the "
                                "edge its wall is on");
    EXTENSION_ADD_PROPERTY_TYPE(BendKeys,
                                (std::vector<std::string>()),
                                "Sheet Metal",
                                App::PropertyType(App::Prop_Hidden | App::Prop_Output
                                                  | App::Prop_NoRecompute),
                                "What each of BendNumbers is of, where the feature says: the "
                                "edge its bend is on");
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
        int number = 0;
        auto [ptr, ec] = std::from_chars(key.data(), key.data() + key.size(), number);
        if (ec != std::errc() || ptr != key.data() + key.size() || number < 1) {
            FC_THROWM(Base::ValueError,
                      "BendKFactors: '" << key << "' is no bend's number, as 3 is Bend3's");
        }
        const auto k = parseNumber(text);
        if (!k || !validK(*k)) {
            FC_THROWM(Base::ValueError,
                      "BendKFactors: the K of bend " << key << ", '" << text
                                                     << "', is no number from 0 to 1");
        }
        for (int index : own) {
            if (SheetMetal::numberOf(model.elements[index].name, "Bend") == number) {
                model.elements[index].bend.k = *k;
            }
        }
    }
}

void SheetMetalExtension::storeModel(SheetMetal::Model model, bool remade,
                                     const std::vector<std::string>& keys)
{
    auto obj = getExtendedObject();
    SheetMetal::Model earlier;
    if (auto before = findBefore(obj)) {
        earlier = before->SheetModel.getValue();
    }
    // the highest number held: by the sheet this builds on, by what this
    // keeps of it, and by every sheet metal feature of the body, this one too
    int top[2] = {0, 0};
    earlier.numbers(top[0], top[1]);
    auto hold = [&top](const SheetMetalExtension& ext) {
        const App::PropertyIntegerList* lists[2] = {&ext.WallNumbers, &ext.BendNumbers};
        for (int kind = 0; kind < 2; ++kind) {
            for (long number : lists[kind]->getValues()) {
                top[kind] = std::max(top[kind], static_cast<int>(number));
            }
        }
    };
    hold(*this);
    auto body = obj ? Body::findBodyOf(obj) : nullptr;
    if (body) {
        for (auto other : body->Group.getValues()) {
            if (auto ext = other ? other->getExtensionByType<SheetMetalExtension>(true) : nullptr) {
                hold(*ext);
            }
        }
    }
    // A new number: the next of the body's count of the kind (0 its walls,
    // 1 its bends), which is the file's (Document::nextObjectCount()) --
    // above every number held, and above every one a feature since deleted
    // had, so a deletion taken back, out of order too, brings back no name
    // another has
    auto doc = obj ? obj->getDocument() : nullptr;
    struct
    {
        App::Document* doc;
        long operator()(const App::DocumentObject* body, int kind, int floor) const
        {
            return body && doc ? doc->nextObjectCount(*body, kind, floor) : floor + 1;
        }
    } nextNumber {doc};
    const char* prefixes[2] = {"Wall", "Bend"};
    App::PropertyIntegerList* lists[2] = {&WallNumbers, &BendNumbers};
    App::PropertyStringList* keyLists[2] = {&WallKeys, &BendKeys};
    for (int kind = 0; kind < 2; ++kind) {
        const std::string prefix = prefixes[kind];
        std::vector<int> own;
        std::set<long> taken;
        if (remade) {
            // none of the sheet before it is in the model, its names are held still
            for (const auto& element : earlier.elements) {
                if (const int number = SheetMetal::numberOf(element.name, prefix)) {
                    taken.insert(number);
                }
            }
        }
        for (std::size_t i = 0; i < model.elements.size(); ++i) {
            const std::string& name = model.elements[i].name;
            const int number = SheetMetal::numberOf(name, prefix);
            if (!number) {
                continue;
            }
            if (remade || earlier.find(name) < 0) {
                own.push_back(static_cast<int>(i));
            }
            else {
                taken.insert(number);
                top[kind] = std::max(top[kind], number);
            }
        }
        // what the feature held: each number and what it was of
        const std::vector<long> held = lists[kind]->getValues();
        std::vector<std::string> heldKeys = keyLists[kind]->getValues();
        heldKeys.resize(held.size());
        std::vector<bool> used(held.size(), false);
        std::vector<long> numbers(own.size(), 0);
        std::vector<std::string> ownKeys(own.size());
        // a number held, if it is one and nothing else of the sheet has it
        auto take = [&](std::size_t k, std::size_t h) {
            used[h] = true;
            if (held[h] >= 1 && taken.insert(held[h]).second) {
                numbers[k] = held[h];
            }
        };
        // first by what each is of, then the rest in the order made
        for (std::size_t k = 0; k < own.size(); ++k) {
            const std::size_t index = static_cast<std::size_t>(own[k]);
            ownKeys[k] = index < keys.size() ? keys[index] : std::string();
            if (ownKeys[k].empty()) {
                continue;
            }
            for (std::size_t h = 0; h < held.size(); ++h) {
                if (!used[h] && heldKeys[h] == ownKeys[k]) {
                    take(k, h);
                    break;
                }
            }
        }
        std::size_t next = 0;
        for (std::size_t k = 0; k < own.size(); ++k) {
            if (numbers[k] >= 1) {
                continue;
            }
            // one whose own was found is not another's; nor is one the
            // feature has the edge of still
            while (next < held.size()
                   && (used[next]
                       || (!heldKeys[next].empty()
                           && std::find(ownKeys.begin(), ownKeys.end(), heldKeys[next])
                                  != ownKeys.end()))) {
                ++next;
            }
            if (next < held.size()) {
                take(k, next);
            }
        }
        for (std::size_t k = 0; k < own.size(); ++k) {
            if (numbers[k] < 1) {
                numbers[k] = nextNumber(body, kind, top[kind]);
                top[kind] = static_cast<int>(numbers[k]);
                taken.insert(numbers[k]);
            }
            model.elements[own[k]].name = prefix + std::to_string(numbers[k]);
        }
        // and what it makes no more is kept, for when it does again
        for (std::size_t h = 0; h < held.size(); ++h) {
            if (!used[h] && held[h] >= 1) {
                numbers.push_back(held[h]);
                ownKeys.push_back(heldKeys[h]);
            }
        }
        if (numbers != held) {
            lists[kind]->setValues(numbers);
        }
        if (std::all_of(ownKeys.begin(), ownKeys.end(), [](const std::string& key) {
                return key.empty();
            })) {
            ownKeys.clear();
        }
        if (ownKeys != keyLists[kind]->getValues()) {
            keyLists[kind]->setValues(ownKeys);
        }
    }
    applyBendOverrides(model);
    SheetModel.setValue(model);
}

std::vector<std::string>
SheetMetalExtension::edgeKeys(const SheetMetal::Model& model,
                              const std::vector<std::pair<std::size_t, std::string>>& starts)
{
    std::vector<std::string> keys(model.elements.size());
    for (std::size_t edge = 0; edge < starts.size(); ++edge) {
        const std::size_t end =
            edge + 1 < starts.size() ? starts[edge + 1].first : model.elements.size();
        int places[2] = {0, 0};
        for (std::size_t i = starts[edge].first; i < end && i < keys.size(); ++i) {
            const bool wall = model.elements[i].kind == SheetMetal::Element::Kind::Wall;
            keys[i] = starts[edge].second + "#" + std::to_string(++places[wall ? 0 : 1]);
        }
    }
    return keys;
}

namespace
{

/// The key of one of a feature's bends in BendKFactors, its number, empty
/// if it made none of that name
std::string placeOf(const SheetMetalExtension& ext, const std::string& bend)
{
    const SheetMetal::Model model = ext.SheetModel.getValue();
    const std::vector<int> own = ext.ownBends(model);
    for (int index : own) {
        if (model.elements[index].name == bend) {
            return std::to_string(SheetMetal::numberOf(bend, "Bend"));
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
    if (const int form = model.formOf(TopoDS::Face(face.getShape())); form >= 0) {
        return model.forms[form].name;
    }
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
