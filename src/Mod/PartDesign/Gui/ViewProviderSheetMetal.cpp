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
#include <iomanip>
#include <numbers>
#include <regex>
#include <sstream>

#include <QTimer>

#include <Inventor/SoFullPath.h>
#include <Inventor/SoPickedPoint.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoSwitch.h>

#include <Precision.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <App/Document.h>
#include <App/DocumentObserver.h>
#include <App/ElementNamingUtils.h>
#include <Gui/Application.h>
#include <Gui/Control.h>
#include <Gui/Document.h>
#include <Mod/PartDesign/App/FeatureSheetMetalFlatPattern.h>
#include <Mod/PartDesign/App/SheetMetalExtension.h>
#include <Mod/PartDesign/App/SolidBody.h>

#include "TaskSheetMetalParameters.h"
#include "TaskSheetMetalRule.h"
#include "ViewProviderSheetMetal.h"

using namespace PartDesignGui;

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalBaseWall, PartDesignGui::ViewProviderSketchBased)

ViewProviderSheetMetalBaseWall::ViewProviderSheetMetalBaseWall()
{
    sPixmap = "PartDesign_SheetMetalBaseWall";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalBaseWall::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalBaseWall",
                                     QObject::tr("Base wall parameters"),
                                     {"MaterialSide",
                                      "ClosedProfile",
                                      "Length",
                                      "Reversed",
                                      "Midplane",
                                      "UseRuleRadius",
                                      "BendRadius"}));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalFlange, PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalFlange::ViewProviderSheetMetalFlange()
{
    sPixmap = "PartDesign_SheetMetalFlange";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalFlange::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(
            this,
            "PartDesign_SheetMetalFlange",
            QObject::tr("Flange parameters"),
            {"Angle",
             "Length",
             "LengthType",
             "BendPosition",
             "Offset",
             "Gap1",
             "Gap2",
             "Reversed",
             "UseRuleRadius",
             "BendRadius",
             "Profile",
             "ReliefShape",
             "ReliefWidth",
             "ReliefDepth"},
            "Edges",
            "Edge"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalContourFlange,
                PartDesignGui::ViewProviderSheetMetalFlange)

ViewProviderSheetMetalContourFlange::ViewProviderSheetMetalContourFlange()
{
    sPixmap = "PartDesign_SheetMetalContourFlange";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalContourFlange::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(
            this,
            "PartDesign_SheetMetalContourFlange",
            QObject::tr("Contour flange parameters"),
            {"Profile",
             "Gap1",
             "Gap2",
             "UseRuleRadius",
             "BendRadius",
             "ReliefShape",
             "ReliefWidth",
             "ReliefDepth"},
            "Edges",
            "Edge"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalJoin, PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalJoin::ViewProviderSheetMetalJoin()
{
    sPixmap = "PartDesign_SheetMetalJoin";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalJoin::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalJoin",
                                     QObject::tr("Join by bend parameters"),
                                     {"UseRuleRadius",
                                      "BendRadius",
                                      "ReliefShape",
                                      "ReliefWidth",
                                      "ReliefDepth"},
                                     "Edges",
                                     "Edge"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalExtendWall, PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalExtendWall::ViewProviderSheetMetalExtendWall()
{
    sPixmap = "PartDesign_SheetMetalExtendWall";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalExtendWall::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalExtendWall",
                                     QObject::tr("Extend wall parameters"),
                                     {"Length", "Gap1", "Gap2"},
                                     "Faces",
                                     "Face"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalHem, PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalHem::ViewProviderSheetMetalHem()
{
    sPixmap = "PartDesign_SheetMetalHem";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalHem::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalHem",
                                     QObject::tr("Hem parameters"),
                                     {"HemType",
                                      "Length",
                                      "Gap",
                                      "Radius",
                                      "Angle",
                                      "Position",
                                      "Gap1",
                                      "Gap2",
                                      "Reversed"},
                                     "Edges",
                                     "Edge"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalCornerClosure,
                PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalCornerClosure::ViewProviderSheetMetalCornerClosure()
{
    sPixmap = "PartDesign_SheetMetalCornerClosure";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalCornerClosure::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalCornerClosure",
                                     QObject::tr("Corner closure parameters"),
                                     {"CornerType"},
                                     "Faces",
                                     "Face"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalRip, PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalRip::ViewProviderSheetMetalRip()
{
    sPixmap = "PartDesign_SheetMetalRip";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalRip::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalRip",
                                     QObject::tr("Rip parameters"),
                                     {},
                                     "Faces",
                                     "Face"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalBendCuts, PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalBendCuts::ViewProviderSheetMetalBendCuts()
{
    sPixmap = "PartDesign_SheetMetalBendCuts";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalBendCuts::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalBendCuts",
                                     QObject::tr("Bend cuts parameters"),
                                     {"Pattern",
                                      "SlotLength",
                                      "BridgeLength",
                                      "EndMargin",
                                      "SlotWidth",
                                      "Rows",
                                      "Tile"},
                                     "Bends",
                                     "Face"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalCutout,
                PartDesignGui::ViewProviderSketchBased)

ViewProviderSheetMetalCutout::ViewProviderSheetMetalCutout()
{
    sPixmap = "PartDesign_SheetMetalCutout";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalCutout::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalCutout",
                                     QObject::tr("Cutout parameters"),
                                     {"Profile", "Wrap", "WrapFrame", "Reversed", "UpToFirst"}));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalFold, PartDesignGui::ViewProviderSketchBased)

ViewProviderSheetMetalFold::ViewProviderSheetMetalFold()
{
    sPixmap = "PartDesign_SheetMetalFold";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalFold::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalFold",
                                     QObject::tr("Fold parameters"),
                                     {"Profile",
                                      "Angle",
                                      "LinePosition",
                                      "Reversed",
                                      "UseRuleRadius",
                                      "BendRadius"},
                                     "Moving",
                                     "Face"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalJog, PartDesignGui::ViewProviderSketchBased)

ViewProviderSheetMetalJog::ViewProviderSheetMetalJog()
{
    sPixmap = "PartDesign_SheetMetalJog";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalJog::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalJog",
                                     QObject::tr("Jog parameters"),
                                     {"Profile",
                                      "Offset",
                                      "OffsetMeasure",
                                      "Angle",
                                      "LinePosition",
                                      "FixProjectedLength",
                                      "Reversed",
                                      "UseRuleRadius",
                                      "BendRadius"},
                                     "Moving",
                                     "Face"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalTab, PartDesignGui::ViewProviderSketchBased)

ViewProviderSheetMetalTab::ViewProviderSheetMetalTab()
{
    sPixmap = "PartDesign_SheetMetalTab";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalTab::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalTab",
                                     QObject::tr("Tab parameters"),
                                     {"Profile"}));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalConvert, PartDesignGui::ViewProvider)

ViewProviderSheetMetalConvert::ViewProviderSheetMetalConvert()
{
    sPixmap = "PartDesign_SheetMetalConvert";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalConvert::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalConvert",
                                     QObject::tr("Convert parameters"),
                                     {"Side"},
                                     {{"FixedFace", "Face", true},
                                      {"OpenFaces", "Face"},
                                      {"RipEdges", "Edge"}}));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalRule, PartGui::ViewProviderPart)

ViewProviderSheetMetalRule::ViewProviderSheetMetalRule() = default;

bool ViewProviderSheetMetalRule::doubleClicked()
{
    auto doc = Gui::Application::Instance->getDocument(getObject()->getDocument());
    return doc && doc->setEdit(this, ViewProvider::Default);
}

bool ViewProviderSheetMetalRule::setEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        return PartGui::ViewProviderPart::setEdit(ModNum);
    }
    if (Gui::Control().activeDialog()) {
        return false;
    }
    Gui::Control().showDialog(new TaskDlgSheetMetalRule(this));
    return true;
}

void ViewProviderSheetMetalRule::unsetEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        PartGui::ViewProviderPart::unsetEdit(ModNum);
        return;
    }
    Gui::Control().closeDialog();
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalFlatPattern, PartGui::ViewProviderPart)

namespace
{
enum
{
    BendUp,
    BendDown,
    Tangent,
    GrainKind
};
const App::PropertyFloatConstraint::Constraints widthRange = {0.5, 64.0, 0.5};
/// As a Part view's DrawStyle has them
const char* styleEnums[] = {"Solid", "Dashed", "Dotted", "Dashdot", nullptr};
}  // namespace

ViewProviderSheetMetalFlatPattern::ViewProviderSheetMetalFlatPattern()
{
    sPixmap = "PartDesign_SheetMetalFlatten";
    // As the SVG draws them (docs/SheetMetal.md sec 12): up and down dashed
    // in two colours, the tangent lines dotted, the grain solid
    const char* group = "Markings";
    ADD_PROPERTY_TYPE(ShowBendUp, (true), group, App::Prop_None, "Draw the bend lines bent up");
    ADD_PROPERTY_TYPE(BendUpColor, (0.0f, 0x50 / 255.0f, 0xd0 / 255.0f), group, App::Prop_None,
                      "The colour of the bend lines bent up");
    ADD_PROPERTY_TYPE(BendUpStyle, (1L), group, App::Prop_None,
                      "The line style of the bend lines bent up");
    ADD_PROPERTY_TYPE(BendUpWidth, (2.0), group, App::Prop_None,
                      "The line width of the bend lines bent up");
    ADD_PROPERTY_TYPE(ShowBendDown, (true), group, App::Prop_None,
                      "Draw the bend lines bent down");
    ADD_PROPERTY_TYPE(BendDownColor, (0xd0 / 255.0f, 0x30 / 255.0f, 0x30 / 255.0f), group,
                      App::Prop_None, "The colour of the bend lines bent down");
    ADD_PROPERTY_TYPE(BendDownStyle, (1L), group, App::Prop_None,
                      "The line style of the bend lines bent down");
    ADD_PROPERTY_TYPE(BendDownWidth, (2.0), group, App::Prop_None,
                      "The line width of the bend lines bent down");
    ADD_PROPERTY_TYPE(ShowTangentLines, (true), group, App::Prop_None,
                      "Draw the tangent lines, where the flat has them");
    ADD_PROPERTY_TYPE(TangentLineColor, (0x80 / 255.0f, 0x80 / 255.0f, 0x80 / 255.0f), group,
                      App::Prop_None, "The colour of the tangent lines");
    ADD_PROPERTY_TYPE(TangentLineStyle, (2L), group, App::Prop_None,
                      "The line style of the tangent lines");
    ADD_PROPERTY_TYPE(TangentLineWidth, (1.0), group, App::Prop_None,
                      "The line width of the tangent lines");
    ADD_PROPERTY_TYPE(ShowGrain, (true), group, App::Prop_None,
                      "Draw the grain arrow, where the flat has it");
    ADD_PROPERTY_TYPE(GrainColor, (0x20 / 255.0f, 0x80 / 255.0f, 0x20 / 255.0f), group,
                      App::Prop_None, "The colour of the grain arrow");
    ADD_PROPERTY_TYPE(GrainStyle, (0L), group, App::Prop_None,
                      "The line style of the grain arrow");
    ADD_PROPERTY_TYPE(GrainWidth, (2.0), group, App::Prop_None,
                      "The line width of the grain arrow");
    kinds = {Kind {&ShowBendUp, &BendUpColor, &BendUpStyle, &BendUpWidth, {}, {}},
             Kind {&ShowBendDown, &BendDownColor, &BendDownStyle, &BendDownWidth, {}, {}},
             Kind {&ShowTangentLines, &TangentLineColor, &TangentLineStyle, &TangentLineWidth,
                   {}, {}},
             Kind {&ShowGrain, &GrainColor, &GrainStyle, &GrainWidth, {}, {}}};
    for (auto& kind : kinds) {
        kind.style->setEnums(styleEnums);
        kind.width->setConstraints(&widthRange);
    }
}

ViewProviderSheetMetalFlatPattern::~ViewProviderSheetMetalFlatPattern() = default;

void ViewProviderSheetMetalFlatPattern::attach(App::DocumentObject* obj)
{
    PartGui::ViewProviderPart::attach(obj);
    // The markings beside the mode switch, under one of their own that
    // follows Visibility: hiding the view switches the mode switch off only
    pcMarkings = new SoSwitch;
    pcMarkings->whichChild = Visibility.getValue() ? SO_SWITCH_ALL : SO_SWITCH_NONE;
    pcRoot->addChild(pcMarkings);
    for (int k = 0; k < static_cast<int>(kinds.size()); ++k) {
        auto& kind = kinds[k];
        kind.view = std::make_unique<PartGui::ViewProviderPart>();
        kind.view->forceUpdate();
        kind.view->setStatus(Gui::SecondaryView, true);
        kind.view->attach(obj);
        kind.view->DisplayMode.setValue("Wireframe");
        kind.view->setDisplayMode("Wireframe");
        kind.view->setGivenShape(Part::TopoShape());
        // drawn whatever this view's display mode
        pcMarkings->addChild(kind.view->getRoot());
        applyLook(k);
    }
    refreshMarkings();
}

void ViewProviderSheetMetalFlatPattern::reattach(App::DocumentObject* obj)
{
    PartGui::ViewProviderPart::reattach(obj);
    for (auto& kind : kinds) {
        if (kind.view) {
            kind.view->reattach(obj);
        }
    }
}

void ViewProviderSheetMetalFlatPattern::beforeDelete()
{
    for (auto& kind : kinds) {
        if (kind.view) {
            kind.view->beforeDelete();
        }
    }
    PartGui::ViewProviderPart::beforeDelete();
}

void ViewProviderSheetMetalFlatPattern::applyLook(int index)
{
    auto& kind = kinds[index];
    if (!kind.view) {
        return;
    }
    kind.view->LineColor.setValue(kind.color->getValue());
    kind.view->PointColor.setValue(kind.color->getValue());
    kind.view->DrawStyle.setValue(kind.style->getValue());
    kind.view->LineWidth.setValue(kind.width->getValue());
    kind.view->PointSize.setValue(kind.width->getValue());
    if (kind.show->getValue() && !kind.edges.empty()) {
        kind.view->show();
    }
    else {
        kind.view->hide();
    }
}

void ViewProviderSheetMetalFlatPattern::onChanged(const App::Property* prop)
{
    if (prop == &Visibility && pcMarkings) {
        pcMarkings->whichChild = Visibility.getValue() ? SO_SWITCH_ALL : SO_SWITCH_NONE;
    }
    for (int k = 0; k < static_cast<int>(kinds.size()); ++k) {
        const auto& kind = kinds[k];
        if (prop == kind.show || prop == kind.color || prop == kind.style || prop == kind.width) {
            applyLook(k);
        }
    }
    PartGui::ViewProviderPart::onChanged(prop);
}

void ViewProviderSheetMetalFlatPattern::updateData(const App::Property* prop)
{
    auto flat = Base::freecad_dynamic_cast<PartDesign::SheetMetalFlatPattern>(getObject());
    if (flat && (prop == &flat->Shape || prop == &flat->BendDirections)) {
        refreshMarkings();
    }
    PartGui::ViewProviderPart::updateData(prop);
}

void ViewProviderSheetMetalFlatPattern::refreshMarkings()
{
    auto flat = Base::freecad_dynamic_cast<PartDesign::SheetMetalFlatPattern>(getObject());
    if (flat) {
        showFlat(flat->Shape.getShape(), flat->BendDirections.getValues());
    }
}

void ViewProviderSheetMetalFlatPattern::copyLook(const ViewProviderSheetMetalFlatPattern& other)
{
    for (std::size_t k = 0; k < kinds.size(); ++k) {
        kinds[k].show->setValue(other.kinds[k].show->getValue());
        kinds[k].color->setValue(other.kinds[k].color->getValue());
        kinds[k].style->setValue(other.kinds[k].style->getValue());
        kinds[k].width->setValue(other.kinds[k].width->getValue());
    }
    LineColor.setValue(other.LineColor.getValue());
    LineWidth.setValue(other.LineWidth.getValue());
}

namespace
{

/** The kind of marking an edge of a flat is, by its mapped name -- Bend1,
 * Bend1Part2, Bend1Tangent1, Grain, GrainHead1 (layFlat()) with what
 * followed it after the first ';' -- each bend Up or Down by directions; -1
 * for none
 */
int markingKind(std::string name, const std::map<std::string, std::string>& directions)
{
    name = name.substr(0, name.find(';'));
    static const std::regex bend("(Bend[0-9]+)(Tangent[0-9]+)?(Part[0-9]+)?");
    static const std::regex grain("Grain(Head[0-9]+)?");
    std::smatch m;
    if (std::regex_match(name, grain)) {
        return GrainKind;
    }
    if (!std::regex_match(name, m, bend)) {
        return -1;
    }
    if (m[2].matched) {
        return Tangent;
    }
    auto it = directions.find(m[1].str());
    return it != directions.end() && it->second == "Down" ? BendDown : BendUp;
}

}  // namespace

namespace
{

/// A number as a note writes it: no more decimals than it has, at most 2
std::string noteNumber(double v)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(2) << v;
    std::string text = s.str();
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

}  // namespace

bool ViewProviderSheetMetalFlatPattern::getEdgeInfo(const char* mappedName, EdgeInfo& info) const
{
    auto flat = Base::freecad_dynamic_cast<PartDesign::SheetMetalFlatPattern>(getObject());
    if (!flat || !mappedName) {
        return false;
    }
    const auto& directions = flat->BendDirections.getValues();
    const int k = markingKind(mappedName, directions);
    if (k < 0) {
        return false;
    }
    const auto& kind = kinds[k];
    info.visible = kind.show->getValue();
    // the styles, Solid Dashed Dotted Dashdot, as Qt numbers them
    info.style = static_cast<int>(kind.style->getValue()) + 1;
    info.color = kind.color->getValue();
    info.width = 0;
    // A bend line's own name, not a piece's nor a tangent line's, has the
    // bend's note: which way, how far, at what radius, from the sheet's tip
    std::string name(mappedName);
    name = name.substr(0, name.find(';'));
    auto direction = directions.find(name);
    auto solid = flat->getSheet();
    auto tip = solid ? solid->Tip.getValue() : nullptr;
    auto ext = tip ? tip->getExtensionByType<PartDesign::SheetMetalExtension>(true) : nullptr;
    if ((k == BendUp || k == BendDown) && direction != directions.end() && ext) {
        const PartDesign::SheetMetal::Model model = ext->SheetModel.getValue();
        const int index = model.find(name);
        if (index >= 0) {
            const auto& bend = model.elements[index].bend;
            // the degree sign, UTF-8
            info.label = (direction->second == "Down" ? "DOWN " : "UP ")
                + noteNumber(std::abs(bend.angle) * 180 / std::numbers::pi) + "\xC2\xB0 R"
                + noteNumber(bend.radius);
        }
    }
    return true;
}

void ViewProviderSheetMetalFlatPattern::showFlat(
    const Part::TopoShape& flatShape,
    const std::map<std::string, std::string>& directions)
{
    if (!kinds[0].view) {
        return;
    }
    // The shape is the blank, then a compound of each kind (layFlat()): the
    // blank's edges are the shape's first, the markings' after them
    // in the object's own frame: this view's transform places it
    const Part::TopoShape shape = flatShape.located();
    Part::TopoShape blank;
    if (shape.countSubShapes(TopAbs_SOLID) > 0) {
        blank = shape.getSubTopoShape(TopAbs_SOLID, 1);
    }
    blankEdges = blank.isNull() ? 0 : static_cast<int>(blank.countSubShapes(TopAbs_EDGE));
    // a marking on the blank's underside is drawn on its top, where one
    // looking down sees it: a flat stored before flatten() put every marking
    // there has a down bend's line on its inner skin
    Base::BoundBox3d bound;
    if (!blank.isNull()) {
        bound = blank.getBoundBox();
    }
    const double tol = Precision::Confusion();
    gp_Trsf up;
    if (bound.IsValid() && bound.LengthZ() > tol) {
        up.SetTranslation(gp_Vec(0, 0, bound.LengthZ()));
    }
    std::array<std::vector<Part::TopoShape>, 4> edges;
    for (auto& kind : kinds) {
        kind.edges.clear();
    }
    const int count = static_cast<int>(shape.countSubShapes(TopAbs_EDGE));
    for (int i = blankEdges + 1; i <= count; ++i) {
        const Data::IndexedName indexed("Edge", i);
        int k = markingKind(shape.getMappedName(indexed).toString(), directions);
        if (k < 0) {
            k = BendUp;
        }
        Part::TopoShape edge = shape.getSubTopoShape(TopAbs_EDGE, i);
        if (up.Form() != gp_Identity && edge.getBoundBox().MaxZ < bound.MinZ + tol) {
            edge = edge.makETransform(up);
        }
        edges[k].push_back(edge);
        kinds[k].edges.push_back(i);
    }
    setGivenShape(blank);
    for (int k = 0; k < static_cast<int>(kinds.size()); ++k) {
        Part::TopoShape compound;
        if (!edges[k].empty()) {
            compound.makECompound(edges[k], nullptr, /*force*/ true);
        }
        kinds[k].view->setGivenShape(compound);
        applyLook(k);
    }
}

void ViewProviderSheetMetalFlatPattern::show()
{
    PartGui::ViewProviderPart::show();
    // Shown out of date, the object asks to be recomputed
    // (SheetMetalFlatPattern::onChanged()); that is done for it, once the
    // change that showed it is through
    auto flat = Base::freecad_dynamic_cast<PartDesign::SheetMetalFlatPattern>(getObject());
    if (!flat || !flat->isTouched()) {
        return;
    }
    App::DocumentObjectT objT(flat);
    QTimer::singleShot(0, [objT]() {
        auto obj = objT.getObject();
        auto doc = obj ? obj->getDocument() : nullptr;
        if (doc && obj->isTouched() && !doc->testStatus(App::Document::Recomputing)) {
            doc->recompute({obj});
        }
    });
}

bool ViewProviderSheetMetalFlatPattern::getElementPicked(const SoPickedPoint* pp,
                                                         std::string& subname) const
{
    for (const auto& kind : kinds) {
        if (!kind.view || !pp->getPath()->containsNode(kind.view->getRoot())) {
            continue;
        }
        // a marking's edge, by its index in the object's shape
        std::string local;
        if (!kind.view->getElementPicked(pp, local)) {
            return false;
        }
        Data::IndexedName name(local.c_str());
        if (std::string(name.getType()) != "Edge" || name.getIndex() < 1
            || name.getIndex() > static_cast<int>(kind.edges.size())) {
            return false;
        }
        subname = "Edge" + std::to_string(kind.edges[name.getIndex() - 1]);
        return true;
    }
    return PartGui::ViewProviderPart::getElementPicked(pp, subname);
}

bool ViewProviderSheetMetalFlatPattern::getDetailPath(const char* subname, SoFullPath* path,
                                                      bool append, SoDetail*& det) const
{
    const char* element = Data::findElementName(subname);
    auto flat = Base::freecad_dynamic_cast<PartDesign::SheetMetalFlatPattern>(getObject());
    // by index or by a mapped name (Bend1Part2), in the object's shape: this
    // view draws the blank only
    Data::IndexedName name;
    if (flat && element && *element && element == subname) {
        name = flat->Shape.getShape().getElementName(element).index;
    }
    if (name && std::string(name.getType()) == "Edge" && name.getIndex() > blankEdges) {
        for (const auto& kind : kinds) {
            auto it = std::find(kind.edges.begin(), kind.edges.end(), name.getIndex());
            if (it == kind.edges.end() || !kind.view) {
                continue;
            }
            const auto len = path->getLength();
            if (append) {
                path->append(pcRoot);
                path->append(pcMarkings);
            }
            const std::string local = "Edge" + std::to_string(it - kind.edges.begin() + 1);
            if (!kind.view->getDetailPath(local.c_str(), path, true, det)) {
                path->truncate(len);
                return false;
            }
            return true;
        }
        return false;
    }
    return PartGui::ViewProviderPart::getDetailPath(subname, path, append, det);
}
