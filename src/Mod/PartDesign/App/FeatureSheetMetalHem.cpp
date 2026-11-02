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

const char* SheetMetalHem::HemTypeEnums[] = {"Closed", "Open", nullptr};
const char* SheetMetalHem::PositionEnums[] = {"Inside", "Outside", nullptr};

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
                      "Closed: the skins the rule's Gap apart inside the fold; Open: Gap apart");
    HemType.setEnums(HemTypeEnums);
    ADD_PROPERTY_TYPE(Length,
                      (6.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "From the fold's outer face to the leg's end");
    ADD_PROPERTY_TYPE(Gap,
                      (1.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "An open hem's gap between the skins inside the fold");
    ADD_PROPERTY_TYPE(Position,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Inside: the fold's outer face where the wall ended, the wall cut back; "
                      "Outside: the fold past the wall's end");
    Position.setEnums(PositionEnums);
    ADD_PROPERTY_TYPE(Reversed,
                      (false),
                      "Sheet Metal",
                      App::Prop_None,
                      "Fold toward the other skin than the one the edge is on");
    showGapValue();
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
}

short SheetMetalHem::mustExecute() const
{
    if (Edges.isTouched() || HemType.isTouched() || Length.isTouched() || Gap.isTouched()
        || Position.isTouched() || Reversed.isTouched()) {
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
        showGapValue();
    }
    FeatureAddSub::onChanged(prop);
}

void SheetMetalHem::showGapValue()
{
    Gap.setStatus(App::Property::ReadOnly, HemType.getValue() == 0);
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
        int walls = 0, bends = 0;
        for (const Element& e : model.elements) {
            ++(e.kind == Element::Kind::Wall ? walls : bends);
        }
        TopoShape base = getBaseShape();
        const auto shadows = Edges.getShadowSubs();
        if (shadows.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A hem needs an edge"));
        }
        const auto hasher = getDocument()->getStringHasher();

        // the skins' gap inside the fold, and the fold's inner radius
        const double gap = HemType.getValue() == 0 ? rule->Gap.getValue() : Gap.getValue();
        if (gap < Precision::Confusion()) {
            THROWM(Base::ValueError, "A hem's skins are kept a gap apart, more than nothing");
        }
        const double R = gap / 2;
        const double A = std::numbers::pi;
        const double leg = Length.getValue() - (R + T);
        if (leg < Precision::Confusion()) {
            THROWM(Base::ValueError, "A hem's length is more than its fold, the gap and the sheet");
        }

        std::vector<SheetMetal::WallEdge> edges;
        for (const auto& shadow : shadows) {
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            edges.push_back(SheetMetal::wallEdge(base, model, ref, shadow.second));
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
        for (const SheetMetal::WallEdge& edge : edges) {
            // folding toward up, its inner skin the one facing that way
            gp_Dir up = edge.skinOut;
            gp_Pnt inner = edge.p0;
            if (Reversed.getValue()) {
                up.Reverse();
                inner = inner.Translated(gp_Vec(up) * T);
            }
            const gp_Vec e(edge.endOut), u(up);
            const gp_Vec along = gp_Vec(edge.p0, edge.p1);
            const std::string bendName = "Bend" + std::to_string(++bends);
            const std::string legName = "Wall" + std::to_string(++walls);

            // Inside, the fold's outer face where the wall ended
            gp_Pnt bendStart = inner;
            if (Position.getValue() == 0) {
                bendStart = inner.Translated(-e * (R + T));
                cuts.push_back(TopoShape(0, hasher).makEPrism(
                    SheetMetal::wallSection(bendStart, edge.endOut, R + T, up.Reversed(), T,
                                            bendName + "Cut"),
                    along));
            }
            CylBend bend;
            bend.frame =
                gp_Ax3(bendStart.Translated(u * R), edge.endOut.Crossed(up), up.Reversed());
            bend.radius = R;
            bend.thickness = T;
            bend.angle = A;
            bend.k = rule->modelK(A, R);

            // the leg goes on from the bend's end, back over the wall
            const gp_Dir x2 = bend.frame.XDirection().Rotated(
                gp_Ax1(bend.frame.Location(), bend.frame.Direction()), A);
            const gp_Dir legAlong = bend.frame.Direction().Crossed(x2);
            const gp_Pnt legStart = bend.frame.Location().Translated(gp_Vec(x2) * R);
            const int legWall = model.addWall(legName, gp_Ax3(legStart, x2, legAlong));
            model.addBend(bendName, bend, edge.wall, legWall);

            pieces.push_back(TopoShape(0, hasher).makEPrism(
                SheetMetal::bendSection(bend, bendName), along));
            pieces.push_back(TopoShape(0, hasher).makEPrism(
                SheetMetal::wallSection(legStart, legAlong, leg, x2, T, legName), along));
        }

        if (!cuts.empty()) {
            // what the cut leaves of their faces is the part's from then on
            for (const TopoShape& cut : cuts) {
                SheetMetal::storePCurves(cut);
            }
            base = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                                    {base, TopoShape().makECompound(cuts)});
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
        Shape.setValue(makeBoolean(base, tool, true));
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
