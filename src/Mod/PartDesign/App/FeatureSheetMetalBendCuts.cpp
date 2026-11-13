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

#include <set>

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/PartFeature.h>

#include "FeatureSheetMetalBendCuts.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"
#include "SheetMetal/Refold.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::BendCutPattern;
using SheetMetal::Element;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalBendCuts, PartDesign::FeatureAddSub)

const char* SheetMetalBendCuts::PatternEnums[] = {"Slots", "Staggered", "Sketch", nullptr};

namespace
{
const App::PropertyIntegerConstraint::Constraints RowsRange = {1, 1000, 1};
}

SheetMetalBendCuts::SheetMetalBendCuts()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(Subtractive);
    ADD_PROPERTY_TYPE(Bends,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A face of each bend to cut");
    ADD_PROPERTY_TYPE(Pattern,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Slots: one row across the bend, to bend it by hand; Staggered: rows of "
                      "them, every other one shifted, a living hinge; Sketch: a sketch's closed "
                      "shapes tiled along the bend");
    Pattern.setEnums(PatternEnums);
    ADD_PROPERTY_TYPE(SlotLength, (10.0), "Sheet Metal", App::Prop_None,
                      "A slot's length along the bend");
    ADD_PROPERTY_TYPE(BridgeLength, (2.0), "Sheet Metal", App::Prop_None,
                      "What is left between two slots or tiles along the bend");
    ADD_PROPERTY_TYPE(EndMargin, (2.0), "Sheet Metal", App::Prop_None,
                      "What is left whole at each end of the bend");
    ADD_PROPERTY_TYPE(SlotWidth, (0.0), "Sheet Metal", App::Prop_None,
                      "A slot's width across the bend, about its middle: 0 is the whole bend "
                      "for Slots, half of each row's share for Staggered");
    ADD_PROPERTY_TYPE(Rows, (3), "Sheet Metal", App::Prop_None,
                      "Staggered's rows across the bend");
    Rows.setConstraints(&RowsRange);
    ADD_PROPERTY_TYPE(Tile, (nullptr), "Sheet Metal", App::Prop_None,
                      "Sketch's tile: X along the bend, Y across it from its middle");
    showPatternValues();
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
}

short SheetMetalBendCuts::mustExecute() const
{
    if (Bends.isTouched() || Pattern.isTouched() || SlotLength.isTouched()
        || BridgeLength.isTouched() || EndMargin.isTouched() || SlotWidth.isTouched()
        || Rows.isTouched() || Tile.isTouched()) {
        return 1;
    }
    return FeatureAddSub::mustExecute();
}

void SheetMetalBendCuts::onChanged(const App::Property* prop)
{
    // The faces are of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Bends.getValue()
        && Bends.getValue() != BaseFeature.getValue()) {
        auto subs = Bends.getSubValues(false);
        auto shadows = Bends.getShadowSubs();
        Bends.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    if (prop == &Pattern) {
        showPatternValues();
    }
    FeatureAddSub::onChanged(prop);
}

void SheetMetalBendCuts::showPatternValues()
{
    const auto kind = static_cast<BendCutPattern::Kind>(Pattern.getValue());
    SlotLength.setStatus(App::Property::ReadOnly, kind == BendCutPattern::Kind::Sketch);
    SlotWidth.setStatus(App::Property::ReadOnly, kind == BendCutPattern::Kind::Sketch);
    Rows.setStatus(App::Property::ReadOnly, kind != BendCutPattern::Kind::Staggered);
    Tile.setStatus(App::Property::ReadOnly, kind != BendCutPattern::Kind::Sketch);
}

App::DocumentObjectExecReturn* SheetMetalBendCuts::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    try {
        const SheetMetal::Model model = modelBefore(rule->Thickness.getValue());
        if (model.elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Bend cuts build on sheet metal bends"));
        }
        // In place, as a pad of the current version is (shouldApplyPlacement())
        TopoShape base = getBaseShape();

        const auto shadows = Bends.getShadowSubs();
        if (shadows.empty()) {
            THROWM(Base::ValueError, "Bend cuts take a face of each bend to cut");
        }
        std::set<int> bends;
        for (const auto& shadow : shadows) {
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            const TopoDS_Shape sub = base.getSubShape(ref.c_str(), true);
            if (sub.IsNull() || sub.ShapeType() != TopAbs_FACE) {
                FC_THROWM(Base::ValueError, "No face " << shadow.second);
            }
            const int owner = model.owner(TopoDS::Face(sub));
            if (owner < 0 || model.elements[owner].kind != Element::Kind::Bend) {
                FC_THROWM(Base::ValueError, "Face " << shadow.second << " is not a bend's");
            }
            bends.insert(owner);
        }

        BendCutPattern pattern;
        pattern.kind = static_cast<BendCutPattern::Kind>(Pattern.getValue());
        pattern.length = SlotLength.getValue();
        pattern.bridge = BridgeLength.getValue();
        pattern.margin = EndMargin.getValue();
        pattern.width = SlotWidth.getValue();
        pattern.rows = Rows.getValue();
        if (pattern.kind == BendCutPattern::Kind::Sketch) {
            App::DocumentObject* tile = Tile.getValue();
            if (!tile) {
                THROWM(Base::ValueError, "A sketch pattern takes a sketch, its Tile");
            }
            // in the sketch's own frame
            TopoShape shape = Part::Feature::getTopoShape(tile,
                                                          nullptr,
                                                          false,
                                                          nullptr,
                                                          nullptr,
                                                          true,
                                                          false);
            if (shape.isNull()) {
                THROWM(Base::ValueError, "A sketch pattern's tile has no shape");
            }
            if (shape.countSubShapes(TopAbs_FACE) == 0) {
                shape = shape.makEFace();
            }
            pattern.tile = shape.getShape();
        }

        std::vector<TopoShape> tools;
        for (int bend : bends) {
            for (TopoShape& tool : SheetMetal::bendCutTools(model, base.getShape(), bend,
                                                            pattern)) {
                tools.push_back(std::move(tool));
            }
        }
        TopoShape tool = TopoShape().makECompound(tools);
        SheetMetal::storePCurves(tool);
        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        TopoShape result = getSolid(makeBoolean(base, tool));
        if (result.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Bend cuts leave nothing of the sheet"));
        }
        Shape.setValue(result);
        storeModel(model);
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}
