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
        if (auto ext = obj->getExtensionByType<SheetMetalExtension>(true)) {
            return ext;
        }
        auto base = dynamic_cast<PartDesign::Feature*>(obj);
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
