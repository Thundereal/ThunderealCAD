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
#include <limits>
#include <numbers>

#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalCornerClosure.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"
#include "SheetMetal/Refold.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::CylBend;
using SheetMetal::Element;
using SheetMetal::Model;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalCornerClosure, PartDesign::FeatureAddSub)

const char* SheetMetalCornerClosure::CornerTypeEnums[] = {"Butt", "Overlap", "Underlap", nullptr};

SheetMetalCornerClosure::SheetMetalCornerClosure()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(Additive);
    ADD_PROPERTY_TYPE(Faces,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "The end of a flange's leg on each side of the corner, the first then the "
                      "second");
    ADD_PROPERTY_TYPE(CornerType,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Butt: both on to the plane halving the corner; Overlap: the first leg over "
                      "the second's end; Underlap: the second over the first's");
    CornerType.setEnums(CornerTypeEnums);
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
}

short SheetMetalCornerClosure::mustExecute() const
{
    if (Faces.isTouched() || CornerType.isTouched()) {
        return 1;
    }
    return FeatureAddSub::mustExecute();
}

void SheetMetalCornerClosure::onChanged(const App::Property* prop)
{
    // The faces are of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Faces.getValue()
        && Faces.getValue() != BaseFeature.getValue()) {
        auto subs = Faces.getSubValues(false);
        auto shadows = Faces.getShadowSubs();
        Faces.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    FeatureAddSub::onChanged(prop);
}

namespace
{

/// One side of the corner: a flange's bend and leg, as the model has them
struct Side
{
    int bend = -1, leg = -1;
    CylBend b;
    /// the bend's axis, the way toward the other side
    gp_Dir toward;
    /// the way the sheet ran out of the wall, and where the bend starts
    gp_Dir out;
    gp_Pnt start;
    /// the leg's inner skin where it starts, its way out from the bend and
    /// its normal out of the corner, and its straight length
    gp_Pnt legStart;
    gp_Dir legAlong, legOut;
    double legLength = 0;
    /// the picked face's middle, and how far back along toward its
    /// material near the corner reaches
    gp_Pnt middle;
    double back = 0;
};

double along(const gp_Pnt& p, const gp_Dir& d)
{
    return gp_Vec(p.XYZ()).Dot(gp_Vec(d));
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalCornerClosure::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    const double T = rule->Thickness.getValue();
    const double gap = rule->Gap.getValue();
    try {
        Model model = modelBefore(T);
        if (model.elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception",
                                  "A corner closure builds on a sheet metal feature"));
        }
        TopoShape base = getBaseShape();
        const auto shadows = Faces.getShadowSubs();
        if (shadows.size() != 2) {
            THROWM(Base::ValueError,
                   "A corner closure takes two faces, the end of a leg on each side of the "
                   "corner");
        }
        const auto hasher = getDocument()->getStringHasher();

        Side sides[2];
        TopoDS_Face faces[2];
        for (int i = 0; i < 2; ++i) {
            const auto& shadow = shadows[i];
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            const TopoDS_Shape sub = base.getSubShape(ref.c_str(), true);
            if (sub.IsNull() || sub.ShapeType() != TopAbs_FACE) {
                FC_THROWM(Base::ValueError, "No face " << shadow.second);
            }
            faces[i] = TopoDS::Face(sub);
            Side& side = sides[i];
            side.leg = model.owner(faces[i]);
            for (std::size_t k = 0; side.leg >= 0 && k < model.elements.size(); ++k) {
                if (model.elements[k].kind == Element::Kind::Bend
                    && model.elements[k].after == side.leg) {
                    side.bend = static_cast<int>(k);
                }
            }
            if (side.bend < 0
                || model.elements[side.bend].bend.angle > std::numbers::pi
                       - Precision::Angular()) {
                FC_THROWM(Base::ValueError,
                          "Face " << shadow.second << " is not the end of a flange's leg");
            }
            GProp_GProps props;
            BRepGProp::SurfaceProperties(faces[i], props);
            side.middle = props.CentreOfMass();
        }
        if (model.elements[sides[0].bend].before != model.elements[sides[1].bend].before) {
            THROWM(Base::ValueError,
                   "A corner closure's two flanges are bent from the same wall");
        }

        for (int i = 0; i < 2; ++i) {
            Side& side = sides[i];
            const Side& other = sides[1 - i];
            side.b = model.elements[side.bend].bend;
            const gp_Dir axis = side.b.frame.Direction();
            side.toward = gp_Vec(side.middle, other.middle).Dot(gp_Vec(axis)) > 0
                ? axis
                : axis.Reversed();
            const gp_Dir up = side.b.frame.XDirection().Reversed();
            side.out = up.Crossed(axis);
            side.start = side.b.frame.Location().Translated(gp_Vec(up) * -side.b.radius);
            const gp_Ax3& leg = model.elements[side.leg].frame;
            side.legStart = leg.Location();
            side.legAlong = leg.XDirection();
            side.legOut = leg.Direction();
            double length = 0, back = std::numeric_limits<double>::max();
            for (TopExp_Explorer v(faces[i], TopAbs_VERTEX); v.More(); v.Next()) {
                const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
                length = std::max(length, gp_Vec(side.legStart, p).Dot(gp_Vec(side.legAlong)));
                back = std::min(back, along(p, side.toward));
            }
            side.legLength = length;
            side.back = back;
        }
        for (int i = 0; i < 2; ++i) {
            Side& side = sides[i];
            const Side& other = sides[1 - i];
            // back far enough that what a miter took is made again: behind
            // where the other side's bend starts, a thickness more
            side.back = std::min(side.back, along(other.start, side.toward)) - T;
            if (side.legLength < Precision::Confusion()) {
                THROWM(Base::ValueError, "A corner closure's legs have a length");
            }
        }

        Bnd_Box bound;
        BRepBndLib::Add(base.getShape(), bound);
        const double reach = std::sqrt(bound.SquareExtent()) + 1;
        const double size = 4 * reach + 1;
        // the half-space on the plane's +n side, as a box
        auto halfSpace = [&](const gp_Pnt& at, const gp_Dir& n) {
            const gp_Ax2 frame(at, n);
            const gp_Pnt corner = at.Translated(gp_Vec(frame.XDirection()) * (-size / 2)
                                                + gp_Vec(frame.YDirection()) * (-size / 2));
            return TopoShape(
                BRepPrimAPI_MakeBox(gp_Ax2(corner, n, frame.XDirection()), size, size, size)
                    .Solid());
        };
        auto cut = [&](const TopoShape& piece, const TopoShape& away) {
            TopoShape res(0, hasher);
            res.makEBoolean(Part::OpCodes::Cut, {piece, away});
            return res.hasSubShape(TopAbs_SOLID) ? getSolid(res) : TopoShape();
        };
        auto common = [&](const TopoShape& a, const TopoShape& b) {
            return TopoShape(0, hasher).makEBoolean(Part::OpCodes::Common, {a, b});
        };

        // each side made again from back there, its bend's and leg's
        // sections carried on past the corner
        TopoShape bendPiece[2], legPiece[2];
        std::vector<TopoShape> cuts;
        for (int i = 0; i < 2; ++i) {
            const Side& side = sides[i];
            const gp_Vec shift = gp_Vec(side.toward)
                * (side.back - along(side.b.frame.Location(), side.toward));
            CylBend b = side.b;
            b.frame.Translate(shift);
            const gp_Vec run = gp_Vec(side.toward) * (reach + T);
            bendPiece[i] = TopoShape(0, hasher).makEPrism(
                SheetMetal::bendSection(b, model.elements[side.bend].name), run);
            legPiece[i] = TopoShape(0, hasher).makEPrism(
                // the leg starts beside the bend's start, the same way along
                SheetMetal::wallSection(side.legStart.Translated(shift),
                                        side.legAlong,
                                        side.legLength,
                                        side.legOut,
                                        T,
                                        model.elements[side.leg].name),
                run);
            // and what was there taken from the part: the side's sheet, and
            // the wall at the corner, in front of where the bend starts
            const gp_Pnt backAt = gp_Pnt(side.toward.XYZ() * side.back);
            cuts.push_back(
                common(halfSpace(backAt, side.toward), halfSpace(side.start, side.out)));
        }
        // the planes each side stops at, its material against each normal
        std::vector<gp_Pln> bendStops[2], legStops[2];
        const long type = CornerType.getValue();
        if (type == 0) {
            // the plane mirroring the one leg's outer skin onto the other's
            const gp_Vec n1(sides[0].legOut), n2(sides[1].legOut);
            const gp_Vec normal = n1 - n2;
            if (normal.Magnitude() < Precision::Confusion()) {
                THROWM(Base::ValueError, "A corner closure's legs are not at a corner");
            }
            const double c1 = n1.Dot(gp_Vec(sides[0].legStart.XYZ())) + T;
            const double c2 = n2.Dot(gp_Vec(sides[1].legStart.XYZ())) + T;
            gp_Dir m(normal);
            const gp_Pnt on(normal.XYZ() * ((c1 - c2) / normal.SquareMagnitude()));
            // the second side on the +m side
            if (gp_Vec(on, sides[1].middle).Dot(gp_Vec(m)) < 0) {
                m.Reverse();
            }
            for (auto* stops : {&bendStops[0], &legStops[0]}) {
                stops->emplace_back(on.Translated(gp_Vec(m) * (-gap / 2)), m);
            }
            for (auto* stops : {&bendStops[1], &legStops[1]}) {
                stops->emplace_back(on.Translated(gp_Vec(m) * (gap / 2)), m.Reversed());
            }
        }
        else {
            // the one over runs to the other's outer skin; the one under
            // stops the gap short of the over one's inner skin, its bend the
            // gap short of where the over one's bend starts
            const int over = type == 1 ? 0 : 1, under = 1 - over;
            const Side& o = sides[over];
            const Side& u = sides[under];
            const gp_Pln outer(u.legStart.Translated(gp_Vec(u.legOut) * T), u.legOut);
            bendStops[over].push_back(outer);
            legStops[over].push_back(outer);
            legStops[under].emplace_back(o.legStart.Translated(gp_Vec(o.legOut) * -gap),
                                         o.legOut);
            bendStops[under].emplace_back(o.start.Translated(gp_Vec(o.out) * -gap), o.out);
        }

        std::vector<TopoShape> pieces;
        for (int i = 0; i < 2; ++i) {
            for (const gp_Pln& stop : bendStops[i]) {
                bendPiece[i] =
                    cut(bendPiece[i], halfSpace(stop.Location(), stop.Axis().Direction()));
            }
            for (const gp_Pln& stop : legStops[i]) {
                legPiece[i] =
                    cut(legPiece[i], halfSpace(stop.Location(), stop.Axis().Direction()));
            }
            for (const TopoShape& piece : {bendPiece[i], legPiece[i]}) {
                if (!piece.isNull()) {
                    pieces.push_back(piece);
                }
            }
            // In the model: the side's stops toward the corner go, a miter's
            // or a gap's, and the new ones come
            const Side& side = sides[i];
            for (int k : {side.bend, side.leg}) {
                auto& limits = model.elements[k].limits;
                limits.erase(std::remove_if(limits.begin(),
                                            limits.end(),
                                            [&](const gp_Pln& limit) {
                                                return limit.Axis().Direction().Dot(side.toward)
                                                    > Precision::Angular();
                                            }),
                             limits.end());
            }
            for (const gp_Pln& stop : bendStops[i]) {
                model.elements[side.bend].limits.push_back(stop);
            }
            for (const gp_Pln& stop : legStops[i]) {
                model.elements[side.leg].limits.push_back(stop);
            }
        }
        if (pieces.empty()) {
            THROWM(Base::ValueError, "A corner closure leaves nothing of its legs");
        }

        for (const TopoShape& c : cuts) {
            SheetMetal::storePCurves(c);
        }
        base = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                                {base, TopoShape().makECompound(cuts)});
        base = getSolid(base);
        TopoShape tool = pieces.size() == 1 ? pieces.front()
                                             : TopoShape(0, hasher).makEFuse(pieces);
        tool.fixSolidOrientation();
        SheetMetal::storePCurves(tool);
        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        TopoShape result = makeBoolean(base, tool, true);
        if (type == 0) {
            // Butt is a miter: the rule's corner relief, as the automatic
            // miter's (sec 5.2)
            const std::vector<TopoShape> reliefs =
                SheetMetal::cornerReliefs(model,
                                          {{sides[0].bend, sides[1].bend}},
                                          rule->CornerReliefShape.getValue(),
                                          rule->CornerReliefSize.getValue());
            if (!reliefs.empty()) {
                result = getSolid(TopoShape(0, hasher).makEBoolean(
                    Part::OpCodes::Cut, {result, TopoShape().makECompound(reliefs)}));
            }
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
