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

#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalJoin.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::CylBend;
using SheetMetal::Element;
using SheetMetal::Model;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalJoin, PartDesign::FeatureAddSub)

const char* SheetMetalJoin::ReliefShapeEnums[] = {"Rule", "Rectangle", "Obround", "Tear", nullptr};

SheetMetalJoin::SheetMetalJoin()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(Additive);
    ADD_PROPERTY_TYPE(Edges,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "Two edges, one at each wall's end where a skin meets it");
    ADD_PROPERTY_TYPE(ReliefShape,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "The bend relief where a wall is trimmed under the bend and runs on beside "
                      "it: Rule follows the rule's; another shape is the join's own, from the "
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
                      "How far the bend relief runs past the bend, when ReliefShape is not Rule");
    showReliefValues();
    ADD_PROPERTY_TYPE(UseRuleRadius,
                      (true),
                      "Sheet Metal",
                      App::Prop_None,
                      "Bend at the rule's radius; off, at BendRadius");
    ADD_PROPERTY_TYPE(BendRadius,
                      (1.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "The bends' inner radius, when not the rule's");
    BendRadius.setStatus(App::Property::ReadOnly, true);
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
}

short SheetMetalJoin::mustExecute() const
{
    if (Edges.isTouched() || ReliefShape.isTouched() || ReliefWidth.isTouched()
        || ReliefDepth.isTouched() || UseRuleRadius.isTouched() || BendRadius.isTouched()) {
        return 1;
    }
    return FeatureAddSub::mustExecute();
}

void SheetMetalJoin::onChanged(const App::Property* prop)
{
    if (prop == &UseRuleRadius) {
        radiusSwitched(UseRuleRadius, BendRadius);
    }
    // The edges are of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Edges.getValue()
        && Edges.getValue() != BaseFeature.getValue()) {
        auto subs = Edges.getSubValues(false);
        auto shadows = Edges.getShadowSubs();
        Edges.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    if (prop == &ReliefShape) {
        // From the rule to the join's own: it starts from the rule's
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

void SheetMetalJoin::showReliefValues()
{
    const bool byRule = ReliefShape.getValue() == 0;
    ReliefWidth.setStatus(App::Property::ReadOnly, byRule);
    ReliefDepth.setStatus(App::Property::ReadOnly, byRule);
}

namespace
{

double volumeOf(const TopoShape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape.getShape(), props);
    return props.Mass();
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalJoin::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    const double T = rule->Thickness.getValue();
    const double R = bendRadius(*rule, UseRuleRadius, BendRadius);
    if (R <= 0.0) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The bend radius is not more than 0"));
    }
    const double tol = Precision::Confusion();
    try {
        Model model = modelBefore(T);
        if (model.elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A join by bend builds on sheet metal walls"));
        }
        int bends = 0;
        for (const Element& e : model.elements) {
            bends += e.kind == Element::Kind::Bend ? 1 : 0;
        }

        // In place, as a pad of the current version is (shouldApplyPlacement())
        TopoShape base = getBaseShape();
        const auto hasher = getDocument()->getStringHasher();

        const auto shadows = Edges.getShadowSubs();
        if (shadows.size() != 2) {
            THROWM(Base::ValueError, "A join by bend takes two edges, one on each wall");
        }
        std::vector<SheetMetal::WallEdge> edges;
        std::vector<std::string> labels;
        for (const auto& shadow : shadows) {
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            edges.push_back(SheetMetal::wallEdge(base, model, ref, shadow.second));
            labels.push_back(shadow.second);
        }
        const SheetMetal::WallEdge &a = edges[0], &b = edges[1];
        const std::string both = "Edges " + labels[0] + " and " + labels[1];
        if (a.wall == b.wall) {
            FC_THROWM(Base::ValueError, both << " are on one wall: a join by bend joins two");
        }
        bool joined = true;
        try {
            model.placement(b.wall, a.wall);
        }
        catch (const Base::IndexError&) {
            joined = false;
        }
        if (joined) {
            FC_THROWM(Base::ValueError,
                      both << " are on walls of one sheet already: a loop of bends does not "
                              "lie flat");
        }
        if (!a.along.IsParallel(b.along, Precision::Angular())) {
            FC_THROWM(Base::ValueError, both << " do not lie parallel");
        }
        const gp_Dir d = a.along;
        if (a.endOut.IsParallel(b.endOut, Precision::Angular())) {
            FC_THROWM(Base::ValueError,
                      both << " are on parallel walls: a join by bend turns between walls that "
                              "meet, a jog steps a wall");
        }

        // Out of a's wall along its end's normal and into b's against b's:
        // each wall's inner skin is the one facing the turn's inside
        const gp_Vec eA(a.endOut), eB(b.endOut);
        auto across = [](const gp_Vec& v, const gp_Vec& from) {
            return gp_Dir(v - from * v.Dot(from));
        };
        const gp_Dir inA = across(-eB, eA), inB = across(-eA, eB);
        // how far the inner skin lies from the edge's skin along it
        const double cA = inA.Dot(a.skinOut) > 0 ? 0 : T;
        const double cB = inB.Dot(b.skinOut) > 0 ? 0 : T;
        // the bend's centre R in from both inner skins, in the plane across
        // the edges through a's first vertex
        const gp_Vec iA(inA), iB(inB);
        const double x =
            (cB + R - gp_Vec(b.p0, a.p0).Dot(iB) - (cA + R) * iA.Dot(iB)) / eA.Dot(iB);
        const gp_Pnt center = a.p0.Translated(eA * x + iA * (cA + R));
        const double angle = std::atan2(eA.Crossed(-eB).Magnitude(), eA.Dot(-eB));

        // the length both edges share along the bend, from a's first vertex
        const double tb0 = gp_Vec(a.p0, b.p0).Dot(gp_Vec(d));
        const double tb1 = gp_Vec(a.p0, b.p1).Dot(gp_Vec(d));
        const double s0 = std::max(0.0, std::min(tb0, tb1));
        const double s1 = std::min(a.p0.Distance(a.p1), std::max(tb0, tb1));
        const double width = s1 - s0;
        if (width < tol) {
            FC_THROWM(Base::ValueError, both << " share no length along the bend");
        }
        const gp_Vec along = gp_Vec(d) * width;

        const std::string bendName = "Bend" + std::to_string(bends + 1);
        CylBend bend;
        bend.frame =
            gp_Ax3(center.Translated(gp_Vec(d) * s0), gp_Dir(eA.Crossed(iA)), inA.Reversed());
        bend.radius = R;
        bend.thickness = T;
        bend.angle = angle;
        bend.k = rule->modelK(angle, R);
        const int bendIndex = model.addBend(bendName, bend, a.wall, b.wall);
        // stopping inside either edge's length, the bend ends at its sides
        const double lb0 = std::min(tb0, tb1), lb1 = std::max(tb0, tb1);
        if (s0 > tol || lb0 < s0 - tol) {
            model.elements[bendIndex].limits.emplace_back(center.Translated(gp_Vec(d) * s0),
                                                          d.Reversed());
        }
        if (s1 < a.p0.Distance(a.p1) - tol || lb1 > s1 + tol) {
            model.elements[bendIndex].limits.emplace_back(center.Translated(gp_Vec(d) * s1), d);
        }

        std::vector<TopoShape> pieces, cuts;
        pieces.push_back(
            TopoShape(0, hasher).makEPrism(SheetMetal::bendSection(bend, bendName), along));

        // Each wall extended or trimmed from its end to where the bend starts
        const bool own = ReliefShape.getValue() != 0;
        const int reliefShape = own ? ReliefShape.getValue() - 1 : rule->ReliefShape.getValue();
        struct Side
        {
            const SheetMetal::WallEdge& edge;
            const std::string& label;
            gp_Dir in;
            double c;
            const char* role;
        };
        for (const Side& side : {Side {a, labels[0], inA, cA, "Before"},
                                 Side {b, labels[1], inB, cB, "After"}}) {
            const SheetMetal::WallEdge& edge = side.edge;
            const gp_Vec e(edge.endOut);
            // the edge's skin at the shared length's start, and where the
            // bend's inner skin starts
            const gp_Pnt q =
                edge.p0.Translated(gp_Vec(d) * gp_Vec(edge.p0, a.p0.Translated(gp_Vec(d) * s0))
                                                   .Dot(gp_Vec(d)));
            const gp_Pnt start = center.Translated(-gp_Vec(side.in) * R);
            const double reach = gp_Vec(edge.p0, start).Dot(e);
            const std::string name = bendName + side.role;
            if (reach > tol) {
                pieces.push_back(TopoShape(0, hasher).makEPrism(
                    SheetMetal::wallSection(q, edge.endOut, reach, edge.skinOut.Reversed(), T,
                                            name + "Ext"),
                    along));
                continue;
            }
            if (reach > -tol) {
                continue;
            }
            const double setback = -reach;
            // what is left of the wall where the bend starts, in the middle
            // of the shared length
            const gp_Pnt probe = q.Translated(-e * setback - gp_Vec(edge.skinOut) * (T / 2)
                                              + along / 2);
            BRepClass3d_SolidClassifier classifier(base.getShape(), probe, tol);
            if (classifier.State() != TopAbs_IN || !model.contains(edge.wall, probe)) {
                FC_THROWM(Base::ValueError,
                          "Edge " << side.label << ": trimmed back to the bend, nothing is left "
                                  "of its wall");
            }
            cuts.push_back(TopoShape(0, hasher).makEPrism(
                SheetMetal::wallSection(q.Translated(-e * setback), edge.endOut, setback,
                                        edge.skinOut.Reversed(), T, name + "Cut"),
                along));
            // the wall running on past the shared length: the relief beside
            // the bend, as a flange's
            const double t0 = gp_Vec(edge.p0, q).Dot(gp_Vec(edge.along));
            const double t1 = gp_Vec(edge.p0, q.Translated(along)).Dot(gp_Vec(edge.along));
            const double gap1 = std::min(t0, t1);
            const double gap2 = edge.p0.Distance(edge.p1) - std::max(t0, t1);
            if (gap1 <= tol && gap2 <= tol) {
                continue;
            }
            SheetMetal::Relief relief;
            relief.shape = static_cast<SheetMetal::Relief::Shape>(reliefShape);
            relief.width = own ? ReliefWidth.getValue() : rule->ReliefWidth.getValue();
            relief.depth = setback + (own ? ReliefDepth.getValue() : rule->ReliefDepth.getValue());
            if (reliefShape == 2) {
                relief.width = rule->Gap.getValue();
                relief.depth = setback;
            }
            const gp_Pnt inner = edge.p0.Translated(gp_Vec(side.in) * side.c);
            for (TopoShape& cut : SheetMetal::reliefCuts(relief, inner, edge.along, edge.endOut,
                                                         side.in, T, gap1, width, gap2,
                                                         name + "Relief", hasher)) {
                cuts.push_back(std::move(cut));
            }
        }

        if (!cuts.empty()) {
            // what the cut leaves of their faces is the part's from then on
            for (const TopoShape& cut : cuts) {
                SheetMetal::storePCurves(cut);
            }
            const int solids = base.countSubShapes(TopAbs_SOLID);
            base = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                                    {base, TopoShape().makECompound(cuts)});
            if (base.countSubShapes(TopAbs_SOLID) > solids) {
                FC_THROWM(Base::ValueError,
                          both << ": trimming the walls back to the bend cuts off what they "
                                  "carry");
            }
            base = getSolid(base);
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
        const TopoShape result = makeBoolean(base, tool, true);
        // the bend and the extensions add to the part, nothing of it under them
        const double sum = volumeOf(base) + volumeOf(tool);
        if (std::abs(volumeOf(result) - sum) > 1e-7 * sum) {
            FC_THROWM(Base::ValueError, both << ": the bend between them runs into the sheet");
        }
        Shape.setValue(result);
        SheetModel.setValue(model);
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}
