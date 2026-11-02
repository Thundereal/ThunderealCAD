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
#include <limits>
#include <set>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Base/Tools.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalFold.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"
#include "SheetMetal/Refold.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::CylBend;
using SheetMetal::Element;
using SheetMetal::Model;
using SheetMetal::skinFaces;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalFold, PartDesign::ProfileBased)

const char* SheetMetalFold::LinePositionEnums[] = {"Middle", "Start", "End", nullptr};

namespace
{
const App::PropertyQuantityConstraint::Constraints angleRange = {0.0, 180.0, 1.0};
}

SheetMetalFold::SheetMetalFold()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(FeatureAddSub::Additive);
    ADD_PROPERTY_TYPE(Angle,
                      (90.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "How far the moving side turns");
    Angle.setConstraints(&angleRange);
    ADD_PROPERTY_TYPE(LinePosition,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Where the line lies in the bend: Middle; Start, the bend starting there on "
                      "the moving side; End, the bend ending there");
    LinePosition.setEnums(LinePositionEnums);
    ADD_PROPERTY_TYPE(Moving,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A face on the side that moves; none for the smaller side");
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
    for (App::Property* prop : std::initializer_list<App::Property*> {&Midplane,
                                                                      &UpToFace,
                                                                      &UpToShape,
                                                                      &AllowMultiFace,
                                                                      &Fit,
                                                                      &FitJoin,
                                                                      &InnerFit,
                                                                      &InnerFitJoin,
                                                                      &Linearize}) {
        prop->setStatus(App::Property::Hidden, true);
    }
}

short SheetMetalFold::mustExecute() const
{
    if (Angle.isTouched() || LinePosition.isTouched() || Moving.isTouched()) {
        return 1;
    }
    return ProfileBased::mustExecute();
}

void SheetMetalFold::onChanged(const App::Property* prop)
{
    // The face picked is of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Moving.getValue()
        && Moving.getValue() != BaseFeature.getValue()) {
        auto subs = Moving.getSubValues(false);
        auto shadows = Moving.getShadowSubs();
        Moving.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    ProfileBased::onChanged(prop);
}

namespace
{

double volumeOf(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

gp_Pnt centreOf(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.CentreOfMass();
}

TopoDS_Compound compoundOf(const std::vector<TopoDS_Face>& faces)
{
    TopoDS_Compound res;
    BRep_Builder builder;
    builder.MakeCompound(res);
    for (const TopoDS_Face& f : faces) {
        builder.Add(res, f);
    }
    return res;
}

/// The planar rectangle on the plane through at, along a from a0 to a1 and
/// along b from b0 to b1
TopoDS_Face rectangle(const gp_Pnt& at, const gp_Dir& a, double a0, double a1, const gp_Dir& b,
                      double b0, double b1)
{
    auto p = [&](double s, double t) {
        return at.Translated(gp_Vec(a) * s + gp_Vec(b) * t);
    };
    BRepBuilderAPI_MakePolygon poly(p(a0, b0), p(a1, b0), p(a1, b1), p(a0, b1), true);
    return BRepBuilderAPI_MakeFace(poly.Wire(), true).Face();
}

/// Move an element rigidly
void moveElement(Element& el, const gp_Trsf& trsf)
{
    el.frame.Transform(trsf);
    el.bend.frame.Transform(trsf);
    for (gp_Pln& limit : el.limits) {
        limit.Transform(trsf);
    }
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalFold::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    const double T = rule->Thickness.getValue();
    const double R = rule->BendRadius.getValue();
    const double A = Base::toRadians(Angle.getValue());
    if (A < Precision::Angular()) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "A fold turns through an angle"));
    }
    const char* noun = featureNoun();
    try {
        TopoShape base = getBaseShape();
        auto invObjLoc = positionByPrevious();
        base.move(invObjLoc);
        Model model = modelBefore(T);
        if (model.elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A fold builds on a sheet metal feature"));
        }
        const auto hasher = getDocument()->getStringHasher();
        const TopoDS_Shape& shape = base.getShape();

        // the line
        TopoShape profile = getProfileShape();
        profile.move(invObjLoc);
        gp_Lin line;
        bool have = false;
        for (const TopoShape& edge : profile.getSubTopoShapes(TopAbs_EDGE)) {
            BRepAdaptor_Curve curve(TopoDS::Edge(edge.getShape()));
            if (curve.GetType() != GeomAbs_Line) {
                FC_THROWM(Base::ValueError, "A " << noun << "'s sketch is a line");
            }
            const gp_Lin l = curve.Line();
            if (!have) {
                line = l;
                have = true;
            }
            else if (!l.Direction().IsParallel(line.Direction(), Precision::Angular())
                     || line.Distance(l.Location()) > Precision::Confusion()) {
                FC_THROWM(Base::ValueError, "A " << noun << "'s sketch is one line");
            }
        }
        if (!have) {
            FC_THROWM(Base::ValueError, "A " << noun << "'s sketch is a line");
        }
        const Base::Vector3d sketchNormal = getProfileNormal();
        gp_Vec n(sketchNormal.x, sketchNormal.y, sketchNormal.z);
        n.Transform(invObjLoc.Transformation());
        const gp_Dir normal(n);
        // the moving side turns toward the sketch's normal, the inner skin there
        const gp_Dir up = Reversed.getValue() ? normal.Reversed() : normal;

        Bnd_Box bound;
        BRepBndLib::Add(shape, bound);
        const double size = 4 * (std::sqrt(bound.SquareExtent()) + 10 * T) + 1;
        const double margin = T / 10;
        const gp_Dir along = line.Direction();

        // the wall: parallel to the sketch, the line across its inner skin,
        // the nearest
        int wall = -1;
        gp_Pnt l0;
        gp_Pln inner;
        double nearest = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < model.elements.size(); ++i) {
            const Element& el = model.elements[i];
            if (el.kind != Element::Kind::Wall
                || !el.frame.Direction().IsParallel(normal, Precision::Angular())) {
                continue;
            }
            const gp_Dir z = el.frame.Direction();
            const gp_Pnt on = up.Dot(z) > 0 ? el.frame.Location().Translated(gp_Vec(z) * T)
                                            : el.frame.Location();
            const gp_Pln plane(on, up);
            gp_Pnt at = line.Location();
            at.Translate(gp_Vec(up) * -gp_Vec(on, at).Dot(gp_Vec(up)));
            const auto skins = skinFaces(shape, model, static_cast<int>(i), plane);
            if (skins.empty()) {
                continue;
            }
            const TopoDS_Edge edge = SheetMetal::lineEdge(at.Translated(gp_Vec(along) * -size),
                                                          at.Translated(gp_Vec(along) * size));
            BRepExtrema_DistShapeShape dist(edge, compoundOf(skins));
            if (!dist.IsDone() || dist.Value() > Precision::Confusion()) {
                continue;
            }
            const double away = std::abs(gp_Vec(at, line.Location()).Dot(gp_Vec(up)));
            if (away < nearest) {
                nearest = away;
                wall = static_cast<int>(i);
                l0 = at;
                inner = plane;
            }
        }
        if (wall < 0) {
            FC_THROWM(Base::ValueError,
                      "A " << noun << "'s line lies across no wall of the sheet");
        }
        const std::string wallName = model.elements[wall].name;

        // a slab of the wall across the line, along e from from to to
        auto slab = [&](const gp_Dir& e, double from, double to) {
            // X along e, Y along the line, Z into the sheet
            const gp_Dir y = up.Reversed().Crossed(e);
            const gp_Pnt corner = l0.Translated(gp_Vec(e) * from - gp_Vec(y) * (size / 2)
                                                + gp_Vec(up) * margin);
            const gp_Ax2 frame(corner, up.Reversed(), e);
            return TopoShape(
                BRepPrimAPI_MakeBox(frame, to - from, size, T + 2 * margin).Solid());
        };
        auto solidsOf = [](const TopoShape& s) {
            return s.getSubTopoShapes(TopAbs_SOLID);
        };
        auto ahead = [&](const TopoShape& s, const gp_Dir& e) {
            return gp_Vec(l0, centreOf(s.getShape())).Dot(gp_Vec(e)) > 0;
        };

        // the two sides, split at the line
        const gp_Dir e0 = up.Crossed(along);
        const auto halves = solidsOf(
            TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                             {base, slab(e0, -margin, margin)}));
        if (halves.size() != 2) {
            FC_THROWM(Base::ValueError,
                      "A " << noun << "'s line splits the sheet in "
                          << halves.size()
                          << " pieces, not two: it runs across something else than its wall");
        }
        int moving = volumeOf(halves[0].getShape()) <= volumeOf(halves[1].getShape()) ? 0 : 1;
        const auto shadows = Moving.getShadowSubs();
        if (!shadows.empty()) {
            const auto& shadow = shadows.front();
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            const TopoDS_Shape picked = base.getSubShape(ref.c_str(), true);
            if (picked.IsNull()) {
                FC_THROWM(Base::ValueError, "No face " << shadow.second);
            }
            bool on[2];
            for (int i = 0; i < 2; ++i) {
                BRepExtrema_DistShapeShape dist(picked, halves[i].getShape());
                on[i] = dist.IsDone() && dist.Value() < Precision::Confusion();
            }
            if (on[0] == on[1]) {
                FC_THROWM(Base::ValueError,
                          "Face " << shadow.second << " is not on one side of the fold's line");
            }
            moving = on[0] ? 0 : 1;
        }
        const gp_Dir e = ahead(halves[moving], e0) ? e0 : e0.Reversed();

        // what moves: the elements of the moving side but the wall
        std::set<int> owners[2];
        for (int i = 0; i < 2; ++i) {
            for (const TopoShape& f : halves[i].getSubTopoShapes(TopAbs_FACE)) {
                const int owner = model.owner(TopoDS::Face(f.getShape()));
                if (owner >= 0 && owner != wall) {
                    owners[i].insert(owner);
                }
            }
        }
        for (int k : owners[moving]) {
            if (owners[1 - moving].count(k)) {
                FC_THROWM(Base::ValueError,
                          "A " << noun << "'s line runs across " << model.elements[k].name);
            }
        }

        // the bend: its strip in the flat, its allowance wide, from s0 along
        // e; a jog's run and second bend after it, the wall taken from s0
        // to s1
        CylBend bend;
        bend.radius = R;
        bend.thickness = T;
        bend.angle = A;
        bend.k = rule->modelK(A, R);
        const double ba = A * bend.neutral();
        double run = 0, taken = ba;
        const bool jog = jogged(T, R, A, ba, run, taken);
        const double developed = jog ? 2 * ba + run : ba;
        const long position = LinePosition.getValue();
        const double s0 = position == 0 ? -ba / 2 : (position == 1 ? 0.0 : -ba);
        const double s1 = s0 + taken;
        const gp_Pnt start = l0.Translated(gp_Vec(e) * s0);
        bend.frame = gp_Ax3(start.Translated(gp_Vec(up) * R), e.Crossed(up), up.Reversed());
        const gp_Dir axis = bend.frame.Direction();

        // nothing but the wall in the strip
        {
            BRepAlgoAPI_Common strip(shape, slab(e, s0, s1).getShape());
            if (!strip.IsDone()) {
                FC_THROWM(Base::CADKernelError,
                          "A " << noun << " could not take its strip from the wall");
            }
            for (TopExp_Explorer it(strip.Shape(), TopAbs_FACE); it.More(); it.Next()) {
                const int owner = model.owner(TopoDS::Face(it.Current()));
                if (owner >= 0 && owner != wall) {
                    FC_THROWM(Base::ValueError,
                              "A " << noun << "'s bend runs into " << model.elements[owner].name);
                }
            }
        }

        // the wall's skin in the strip, a little past it each way, and the
        // bend's span along its axis
        const double past = margin;
        const auto skins = skinFaces(shape, model, wall, inner);
        const TopoDS_Face wide = rectangle(start, e, -past, taken + past, axis, -size, size);
        std::vector<TopoDS_Face> pieces;
        {
            BRepAlgoAPI_Common common(compoundOf(skins), wide);
            if (common.IsDone()) {
                for (TopExp_Explorer it(common.Shape(), TopAbs_FACE); it.More(); it.Next()) {
                    pieces.push_back(TopoDS::Face(it.Current()));
                }
            }
        }
        double d0 = std::numeric_limits<double>::max(), d1 = -d0;
        for (const TopoDS_Face& piece : pieces) {
            for (TopExp_Explorer v(piece, TopAbs_VERTEX); v.More(); v.Next()) {
                const double d =
                    gp_Vec(start, BRep_Tool::Pnt(TopoDS::Vertex(v.Current()))).Dot(gp_Vec(axis));
                d0 = std::min(d0, d);
                d1 = std::max(d1, d);
            }
        }
        if (pieces.empty() || d1 - d0 < Precision::Confusion()) {
            FC_THROWM(Base::ValueError,
                      "A " << noun << "'s line lies across no wall of the sheet");
        }

        int walls = 0, bends = 0;
        for (const Element& el : model.elements) {
            ++(el.kind == Element::Kind::Wall ? walls : bends);
        }
        auto unique = [&](const char* prefix, int& count) {
            std::string name;
            do {
                name = prefix + std::to_string(++count);
            } while (model.find(name) >= 0);
            return name;
        };
        const std::string bendName = unique("Bend", bends);
        const std::string bend2Name = jog ? unique("Bend", bends) : std::string();
        const std::string runName = jog ? unique("Wall", walls) : std::string();
        const std::string legName = unique("Wall", walls);

        // what the strip has not of the wall -- a hole, a notch; past where
        // the wall is taken, a jog that adds material carries the wall's
        // section there on, so a hole across that line is stretched with
        // it. One cut, so a hole and its stretch are one face.
        std::vector<TopoDS_Face> missing;
        {
            const bool adds = jog && taken < developed;
            std::vector<TopoDS_Face> present = pieces;
            if (adds) {
                present.clear();
                BRepAlgoAPI_Common upTo(compoundOf(pieces),
                                        rectangle(start, e, -past, taken, axis, -size, size));
                const gp_Pnt end = start.Translated(gp_Vec(e) * taken);
                BRepAlgoAPI_Common section(
                    SheetMetal::lineEdge(end.Translated(gp_Vec(axis) * -size),
                                         end.Translated(gp_Vec(axis) * size)),
                    compoundOf(pieces));
                if (!upTo.IsDone() || !section.IsDone()) {
                    THROWM(Base::CADKernelError, "A jog could not take the wall's section");
                }
                for (TopExp_Explorer it(upTo.Shape(), TopAbs_FACE); it.More(); it.Next()) {
                    present.push_back(TopoDS::Face(it.Current()));
                }
                for (TopExp_Explorer it(section.Shape(), TopAbs_EDGE); it.More(); it.Next()) {
                    double a = std::numeric_limits<double>::max(), b = -a;
                    for (TopExp_Explorer v(it.Current(), TopAbs_VERTEX); v.More(); v.Next()) {
                        const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
                        const double d = gp_Vec(start, p).Dot(gp_Vec(axis));
                        a = std::min(a, d);
                        b = std::max(b, d);
                    }
                    if (b - a > Precision::Confusion()) {
                        present.push_back(
                            rectangle(start, e, taken, developed + past, axis, a, b));
                    }
                }
            }
            BRepAlgoAPI_Cut cut(
                rectangle(start, e, -past, adds ? developed + past : taken + past, axis, d0, d1),
                compoundOf(present));
            if (!cut.IsDone()) {
                FC_THROWM(Base::CADKernelError, "A " << noun << " could not lay out its strip");
            }
            for (TopExp_Explorer it(cut.Shape(), TopAbs_FACE); it.More(); it.Next()) {
                missing.push_back(TopoDS::Face(it.Current()));
            }
        }
        // the missing faces from from to to along e
        auto clip = [&](double from, double to) {
            std::vector<TopoDS_Face> res;
            if (missing.empty()) {
                return res;
            }
            BRepAlgoAPI_Common common(compoundOf(missing),
                                      rectangle(start, e, from, to, axis, -size, size));
            if (common.IsDone()) {
                for (TopExp_Explorer it(common.Shape(), TopAbs_FACE); it.More(); it.Next()) {
                    res.push_back(TopoDS::Face(it.Current()));
                }
            }
            return res;
        };
        // a bend's piece less the faces given, taken onto it along its
        // radius from its own flat, the faces moved there by onto
        auto lessHoles = [&](TopoShape piece, const CylBend& b,
                             const std::vector<TopoDS_Face>& faces, const gp_Vec& onto) {
            gp_Trsf move;
            move.SetTranslation(onto);
            std::vector<TopoShape> holes;
            for (const TopoDS_Face& f : faces) {
                const TopoDS_Face flat =
                    onto.Magnitude() > 0
                        ? TopoDS::Face(BRepBuilderAPI_Transform(f, move, true).Shape())
                        : f;
                const TopoDS_Shape hole =
                    SheetMetal::bendSolid(b, flat, R - margin, R + T + margin);
                if (!hole.IsNull()) {
                    holes.emplace_back(hole);
                }
            }
            if (!holes.empty()) {
                piece = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                                         {piece, TopoShape().makECompound(holes)});
                piece = getSolid(piece);
            }
            return piece;
        };
        // a bend's piece over the wall's span, its section swept along its
        // axis from d0 (from d1 on an axis the other way)
        auto sweep = [&](const CylBend& b, const std::string& name) {
            CylBend span = b;
            const bool same = b.frame.Direction().Dot(axis) > 0;
            span.frame.Translate(gp_Vec(axis) * (same ? d0 : d1));
            return TopoShape(0, hasher).makEPrism(SheetMetal::bendSection(span, name),
                                                  gp_Vec(span.frame.Direction()) * (d1 - d0));
        };

        TopoShape bendPiece = sweep(bend, bendName);
        bendPiece = lessHoles(bendPiece, bend, jog ? clip(-past, ba + past) : missing, gp_Vec());
        std::vector<TopoShape> added {bendPiece};

        // the moving side carried rigidly where the bend leaves it, a
        // jog's past its second bend, which turns back on the far skin,
        // and pushed out by what the jog adds
        const gp_Trsf carry1 = bend.refold();
        gp_Trsf carry = carry1;
        CylBend second;
        if (jog) {
            CylBend back = bend;
            const gp_Pnt start2 = start.Translated(gp_Vec(e) * (ba + run) - gp_Vec(up) * T);
            back.frame =
                gp_Ax3(start2.Translated(gp_Vec(up) * -R), e.Crossed(up.Reversed()), up);
            TopoShape piece2 = sweep(back, bend2Name);
            piece2 = lessHoles(piece2,
                               back,
                               clip(ba + run - past, developed + past),
                               gp_Vec(up) * -T);
            added.push_back(piece2.makETransform(carry1));
            if (run > Precision::Confusion()) {
                BRepAlgoAPI_Cut straight(rectangle(start, e, ba, ba + run, axis, d0, d1),
                                         compoundOf(missing));
                if (!straight.IsDone()) {
                    THROWM(Base::CADKernelError, "A jog could not make its run");
                }
                for (TopExp_Explorer it(straight.Shape(), TopAbs_FACE); it.More(); it.Next()) {
                    added.push_back(TopoShape(0, hasher)
                                        .makEPrism(TopoShape(it.Current()), gp_Vec(up) * -T)
                                        .makETransform(carry1));
                }
            }
            second = back;
            second.frame.Transform(carry1);
            gp_Trsf out;
            out.SetTranslation(gp_Vec(e) * (developed - taken));
            carry = carry1 * back.refold() * out;
        }

        // the part less the strip, each side
        std::vector<TopoShape> parts;
        bool fixedSide = false, movingSide = false;
        for (const TopoShape& s :
             solidsOf(TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                                       {base, slab(e, s0, s1)}))) {
            // the moving side's pieces are clear of the fixed half
            BRepExtrema_DistShapeShape dist(s.getShape(), halves[1 - moving].getShape());
            if (dist.IsDone() && dist.Value() > margin / 2) {
                parts.push_back(s.makETransform(carry));
                movingSide = true;
            }
            else {
                parts.push_back(s);
                fixedSide = true;
            }
        }
        if (!fixedSide || !movingSide) {
            FC_THROWM(Base::ValueError,
                      "A " << noun << "'s strip leaves nothing of the wall on one side");
        }
        for (TopoShape& piece : added) {
            piece.fixSolidOrientation();
            SheetMetal::storePCurves(piece);
        }
        AddSubShape.setValue(added.size() == 1 ? added.front()
                                               : TopoShape(0, hasher).makECompound(added));
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        parts.insert(parts.end(), added.begin(), added.end());
        TopoShape result = getSolid(TopoShape(0, hasher).makEFuse(parts));
        if (result.countSubShapes(TopAbs_SOLID) != 1) {
            FC_THROWM(Base::ValueError, "A " << noun << " left the sheet in pieces");
        }

        // the model: the wall's moving side a wall of its own after the
        // bend (a jog's after its run and second bend), all on that side
        // carried with it
        int before = wall;
        const CylBend* into = &bend;
        if (jog) {
            Element runWall = model.elements[wall];
            moveElement(runWall, carry1);
            const int mid = model.addWall(runName, runWall.frame);
            model.elements[mid].limits = runWall.limits;
            model.addBend(bendName, bend, wall, mid);
            before = mid;
            into = &second;
        }
        Element movedWall = model.elements[wall];
        moveElement(movedWall, carry);
        const int leg = model.addWall(legName, movedWall.frame);
        model.elements[leg].limits = movedWall.limits;
        model.addBend(jog ? bend2Name : bendName, *into, before, leg);
        for (int k : owners[moving]) {
            Element& el = model.elements[k];
            moveElement(el, carry);
            if (el.before == wall) {
                el.before = leg;
            }
            if (el.after == wall) {
                el.after = leg;
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

bool SheetMetalFold::jogged(double, double, double, double, double&, double&) const
{
    return false;
}

PROPERTY_SOURCE(PartDesign::SheetMetalJog, PartDesign::SheetMetalFold)

const char* SheetMetalJog::OffsetMeasureEnums[] = {"Step", "Inside", "Overall", nullptr};

namespace
{
const App::PropertyQuantityConstraint::Constraints jogAngleRange = {0.0, 90.0, 1.0};
}

SheetMetalJog::SheetMetalJog()
{
    Angle.setConstraints(&jogAngleRange);
    ADD_PROPERTY_TYPE(Offset,
                      (10.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "How far the moving side is stepped, as OffsetMeasure says");
    ADD_PROPERTY_TYPE(OffsetMeasure,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "What Offset measures: Step, how far the wall moves; Inside, the gap "
                      "between the facing skins; Overall, the outer skins");
    OffsetMeasure.setEnums(OffsetMeasureEnums);
    ADD_PROPERTY_TYPE(FixProjectedLength,
                      (true),
                      "Sheet Metal",
                      App::Prop_None,
                      "The moving side's end stays where it was, the jog adding its material; "
                      "off, the jog takes it from the wall");
}

short SheetMetalJog::mustExecute() const
{
    if (Offset.isTouched() || OffsetMeasure.isTouched() || FixProjectedLength.isTouched()) {
        return 1;
    }
    return SheetMetalFold::mustExecute();
}

bool SheetMetalJog::jogged(double T, double R, double A, double ba, double& run,
                           double& taken) const
{
    // the step a skin to the same skin; the least, with no run between
    // the bends, as the offset is measured
    const long measure = OffsetMeasure.getValue();
    const double skins = measure == 1 ? -T : (measure == 2 ? T : 0.0);
    const double step = Offset.getValue() - skins;
    const double least = (T + 2 * R) * (1 - std::cos(A));
    run = (step - least) / std::sin(A);
    if (run < -Precision::Confusion()) {
        FC_THROWM(Base::ValueError,
                  "A jog's offset leaves no room between its bends: at this angle it takes at "
                  "least "
                      << least + skins << " mm");
    }
    run = std::max(run, 0.0);
    // across, from where the first bend starts to where the second ends
    const double across = (T + 2 * R) * std::sin(A) + run * std::cos(A);
    taken = FixProjectedLength.getValue() ? across : 2 * ba + run;
    return true;
}
