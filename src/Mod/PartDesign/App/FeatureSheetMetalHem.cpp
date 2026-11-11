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

#include <cmath>
#include <numbers>

#include <Precision.hxx>
#include <gp_Ax1.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalHem.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::CylBend;
using SheetMetal::Element;
using SheetMetal::Model;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalHem, PartDesign::FeatureAddSub)

const char* SheetMetalHem::HemTypeEnums[] = {"Closed", "Open", "Teardrop", "Rolled", nullptr};

namespace
{
const App::PropertyQuantityConstraint::Constraints angleRange = {90.0, 330.0, 1.0};
}
const char* SheetMetalHem::PositionEnums[] = {"Inside", "Outside", nullptr};
const char* SheetMetalHem::ReliefShapeEnums[] = {"Rule", "Rectangle", "Obround", "Tear", nullptr};

SheetMetalHem::SheetMetalHem()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(Additive);
    ADD_PROPERTY_TYPE(Edges,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "The edges, each where a wall's skin meets its end");
    ADD_PROPERTY_TYPE(HemType,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Closed: the skins the rule's Gap apart inside the fold; Open: Gap apart; "
                      "Teardrop: a bend past 180 deg, its leg stopping Gap short of the skin; "
                      "Rolled: the bend alone");
    HemType.setEnums(HemTypeEnums);
    ADD_PROPERTY_TYPE(Length,
                      (6.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "From the fold's outer face to the leg's end, a closed or open hem's");
    ADD_PROPERTY_TYPE(Gap,
                      (1.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "An open hem's gap between the skins inside the fold; a teardrop's "
                      "between its leg and the skin");
    ADD_PROPERTY_TYPE(Radius,
                      (1.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "A teardrop's or a rolled hem's inner radius");
    ADD_PROPERTY_TYPE(Angle,
                      (225.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "A teardrop's or a rolled hem's bend angle; a teardrop's past 180 deg, up "
                      "to 270");
    Angle.setConstraints(&angleRange);
    ADD_PROPERTY_TYPE(Position,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Inside: the fold's outer face where the wall ended, the wall cut back; "
                      "Outside: the fold past the wall's end");
    Position.setEnums(PositionEnums);
    ADD_PROPERTY_TYPE(Gap1,
                      (0.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "Taken off the hem's width at the edge's first vertex; a negative one "
                      "carries the hem past it");
    ADD_PROPERTY_TYPE(Gap2,
                      (0.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "Taken off the hem's width at the edge's last vertex; a negative one "
                      "carries the hem past it");
    ADD_PROPERTY_TYPE(Reversed,
                      (false),
                      "Sheet Metal",
                      App::Prop_None,
                      "Fold toward the other skin than the one the edge is on");
    ADD_PROPERTY_TYPE(ReliefShape,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "The bend relief where the hem ends inside its wall's end, the wall cut "
                      "back: Rule follows the rule's; another shape is the hem's own, from the "
                      "rule's width and depth when chosen; a tear is the rule's Gap wide");
    ReliefShape.setEnums(ReliefShapeEnums);
    ADD_PROPERTY_TYPE(ReliefWidth,
                      (1.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "The bend relief's width, when ReliefShape is not Rule");
    ADD_PROPERTY_TYPE(ReliefDepth,
                      (1.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "How far the bend relief runs past the fold, when ReliefShape is not Rule");
    showReliefValues();
    showTypeValues();
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
}

short SheetMetalHem::mustExecute() const
{
    if (Edges.isTouched() || HemType.isTouched() || Length.isTouched() || Gap.isTouched()
        || Position.isTouched() || Reversed.isTouched() || Radius.isTouched()
        || Angle.isTouched() || Gap1.isTouched() || Gap2.isTouched() || ReliefShape.isTouched()
        || ReliefWidth.isTouched() || ReliefDepth.isTouched()) {
        return 1;
    }
    return FeatureAddSub::mustExecute();
}

void SheetMetalHem::onChanged(const App::Property* prop)
{
    // The edges are of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Edges.getValue()
        && Edges.getValue() != BaseFeature.getValue()) {
        auto subs = Edges.getSubValues(false);
        auto shadows = Edges.getShadowSubs();
        Edges.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    if (prop == &HemType) {
        showTypeValues();
    }
    if (prop == &ReliefShape) {
        // From the rule to the hem's own: it starts from the rule's
        auto rule = getRule();
        if (!isRestoring() && ReliefShape.getValue() != 0
            && ReliefWidth.testStatus(App::Property::ReadOnly) && rule) {
            ReliefWidth.setValue(rule->ReliefWidth.getValue());
            ReliefDepth.setValue(rule->ReliefDepth.getValue());
        }
        showReliefValues();
    }
    FeatureAddSub::onChanged(prop);
}

void SheetMetalHem::showReliefValues()
{
    const bool byRule = ReliefShape.getValue() == 0;
    ReliefWidth.setStatus(App::Property::ReadOnly, byRule);
    ReliefDepth.setStatus(App::Property::ReadOnly, byRule);
}

void SheetMetalHem::showTypeValues()
{
    const long type = HemType.getValue();
    const bool bent = type >= 2;
    Length.setStatus(App::Property::ReadOnly, bent);
    Gap.setStatus(App::Property::ReadOnly, type == 0 || type == 3);
    Radius.setStatus(App::Property::ReadOnly, !bent);
    Angle.setStatus(App::Property::ReadOnly, !bent);
}

App::DocumentObjectExecReturn* SheetMetalHem::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    const double T = rule->Thickness.getValue();
    try {
        Model model = modelBefore(T);
        if (model.elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A hem builds on a sheet metal feature"));
        }
        // numbered on from the highest the sheet has (storeModel() gives
        // each the number it keeps)
        int walls = 0, bends = 0;
        model.numbers(walls, bends);
        // its pieces' faces are named by its own count (ownName())
        const int wallsBefore = walls, bendsBefore = bends;
        auto ownNamed = [&](const std::string& name) {
            return SheetMetal::ownName(name, wallsBefore, bendsBefore);
        };
        TopoShape base = getBaseShape();
        const auto shadows = Edges.getShadowSubs();
        if (shadows.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A hem needs an edge"));
        }
        const auto hasher = getDocument()->getStringHasher();

        // the bend's radius and angle, and the leg's length after it
        const long type = HemType.getValue();
        double R = 0, A = std::numbers::pi, leg = 0;
        if (type <= 1) {
            // the skins' gap inside the fold, the inner radius half of it
            const double gap = type == 0 ? rule->Gap.getValue() : Gap.getValue();
            if (gap < Precision::Confusion()) {
                THROWM(Base::ValueError, "A hem's skins are kept a gap apart, more than nothing");
            }
            R = gap / 2;
            leg = Length.getValue() - (R + T);
            if (leg < Precision::Confusion()) {
                THROWM(Base::ValueError,
                       "A hem's length is more than its fold, the gap and the sheet");
            }
        }
        else {
            R = Radius.getValue();
            A = Angle.getValue() * std::numbers::pi / 180;
            if (R < Precision::Confusion()) {
                THROWM(Base::ValueError, "A hem's radius is more than nothing");
            }
            if (type == 2) {
                if (A <= std::numbers::pi + Precision::Angular()
                    || A > 1.5 * std::numbers::pi + Precision::Angular()) {
                    THROWM(Base::ValueError, "A teardrop bends past 180 deg, up to 270");
                }
                // The leg's corner nearest the skin starts R (1 - cos A) over
                // it and comes down at -sin A a length; it stops Gap short
                leg = (R * (1 - std::cos(A)) - Gap.getValue()) / -std::sin(A);
                if (leg < Precision::Confusion()) {
                    THROWM(Base::ValueError,
                           "A teardrop's bend comes within its gap of the skin, with no room "
                           "for a leg");
                }
            }
            else if (A > 1.5 * std::numbers::pi
                     && R - (R + T) * std::cos(A) < rule->Gap.getValue()) {
                // past 270 deg, the roll's end comes back down to the skin
                THROWM(Base::ValueError,
                       "A rolled hem's end comes within the rule's gap of the skin");
            }
        }

        std::vector<SheetMetal::WallEdge> edges;
        std::vector<std::string> labels, refs;
        for (const auto& shadow : shadows) {
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            refs.push_back(ref);
            edges.push_back(SheetMetal::wallEdge(base, model, ref, shadow.second));
            labels.push_back(shadow.second);
            for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
                for (const gp_Pnt& p : {edges[i].p0, edges[i].p1}) {
                    for (const gp_Pnt& q : {edges.back().p0, edges.back().p1}) {
                        if (p.Distance(q) < Precision::Confusion()) {
                            FC_THROWM(Base::ValueError,
                                      "Edge " << shadow.second
                                              << " meets another of the hem's at a corner, where "
                                                 "their folds would run into each other");
                        }
                    }
                }
            }
        }

        std::vector<TopoShape> pieces, cuts;
        // what each edge makes goes by the edge (storeModel())
        std::vector<std::pair<std::size_t, std::string>> starts;
        for (std::size_t k = 0; k < edges.size(); ++k) {
            starts.emplace_back(model.elements.size(), refs[k]);
            const SheetMetal::WallEdge& edge = edges[k];
            // folding toward up, its inner skin the one facing that way
            gp_Dir up = edge.skinOut;
            gp_Pnt inner = edge.p0;
            if (Reversed.getValue()) {
                up.Reverse();
                inner = inner.Translated(gp_Vec(up) * T);
            }
            const gp_Vec e(edge.endOut), u(up);
            // the hem's span along the edge, the gaps taken off its ends
            const double length = edge.p0.Distance(edge.p1);
            const double gap1 = Gap1.getValue(), gap2 = Gap2.getValue();
            const double width = length - gap1 - gap2;
            if (width < Precision::Confusion()) {
                FC_THROWM(Base::ValueError, "The gaps leave nothing of edge " << labels[k]);
            }
            const gp_Vec along = gp_Vec(edge.along) * width;
            const gp_Pnt edgeStart = inner.Translated(gp_Vec(edge.along) * gap1);
            const std::string bendName = "Bend" + std::to_string(++bends);
            const std::string legName = "Wall" + std::to_string(++walls);

            // Inside, the fold's outer face where the wall ended
            gp_Pnt bendStart = edgeStart;
            const bool inside = Position.getValue() == 0;
            if (inside) {
                bendStart = edgeStart.Translated(-e * (R + T));
                cuts.push_back(TopoShape(0, hasher).makEPrism(
                    SheetMetal::wallSection(bendStart, edge.endOut, R + T, up.Reversed(), T,
                                            ownNamed(bendName) + "Cut"),
                    along));
                // Where the hem stops inside the wall's end, the bend relief
                // beside the fold, as a flange's (sec 5.2): the hem's own,
                // or the rule's
                const bool ownRelief = ReliefShape.getValue() != 0;
                SheetMetal::Relief relief;
                relief.shape = static_cast<SheetMetal::Relief::Shape>(
                    ownRelief ? ReliefShape.getValue() - 1 : rule->ReliefShape.getValue());
                relief.width = ownRelief ? ReliefWidth.getValue() : rule->ReliefWidth.getValue();
                relief.depth = R + T
                    + (ownRelief ? ReliefDepth.getValue() : rule->ReliefDepth.getValue());
                if (relief.shape == SheetMetal::Relief::Shape::Slit) {
                    relief.width = rule->Gap.getValue();
                    relief.depth = R + T;
                }
                for (TopoShape& cut : SheetMetal::reliefCuts(relief, inner, edge.along,
                                                             edge.endOut, up, T, gap1, width,
                                                             gap2, ownNamed(bendName) + "Relief", hasher)) {
                    cuts.push_back(std::move(cut));
                }
            }
            CylBend bend;
            bend.frame =
                gp_Ax3(bendStart.Translated(u * R), edge.endOut.Crossed(up), up.Reversed());
            bend.radius = R;
            bend.thickness = T;
            bend.angle = A;
            bend.k = rule->modelK(A, R);

            // the leg goes on from the bend's end, back toward the wall
            const gp_Dir x2 = bend.frame.XDirection().Rotated(
                gp_Ax1(bend.frame.Location(), bend.frame.Direction()), A);
            const gp_Dir legAlong = bend.frame.Direction().Crossed(x2);
            const gp_Pnt legStart = bend.frame.Location().Translated(gp_Vec(x2) * R);
            const int legWall = model.addWall(legName, gp_Ax3(legStart, x2, legAlong));
            const int bendIndex = model.addBend(bendName, bend, edge.wall, legWall);
            // Ending inside the wall's end, the fold stops at its sides, as a
            // flange's bend does: its tube runs on through the wall beside it
            if (gap1 > Precision::Confusion()) {
                model.elements[bendIndex].limits.emplace_back(edgeStart, edge.along.Reversed());
            }
            if (gap2 > Precision::Confusion()) {
                model.elements[bendIndex].limits.emplace_back(edgeStart.Translated(along),
                                                              edge.along);
            }
            if (type >= 2) {
                // A teardrop's leg, or a roll's end with none, stops: its slab
                // would run on through the wall it heads back to
                model.elements[legWall].limits.emplace_back(
                    legStart.Translated(gp_Vec(legAlong) * leg), legAlong);
            }

            pieces.push_back(TopoShape(0, hasher).makEPrism(
                SheetMetal::bendSection(bend, ownNamed(bendName)), along));
            if (leg > Precision::Confusion()) {
                pieces.push_back(TopoShape(0, hasher).makEPrism(
                    SheetMetal::wallSection(legStart, legAlong, leg, x2, T, ownNamed(legName)), along));
            }
        }

        if (!cuts.empty()) {
            // what the cut leaves of their faces is the part's from then on
            for (const TopoShape& cut : cuts) {
                SheetMetal::storePCurves(cut);
            }
            base = getSolid(SheetMetal::cutDroppingScrap(base, cuts, pieces, hasher, getID()));
        }
        TopoShape tool = pieces.size() == 1 ? pieces.front()
                                             : TopoShape(0, hasher).makEFuse(pieces);
        // oriented, so the join need not check its result
        tool.fixSolidOrientation();
        SheetMetal::storePCurves(tool);
        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        Shape.setValue(makeBoolean(base, tool, true));
        storeModel(model, false, edgeKeys(model, starts));
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}
