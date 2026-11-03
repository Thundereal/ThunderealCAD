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
#include <numbers>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalFlange.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"
#include "SheetMetal/Profile.h"
#include "SheetMetal/Refold.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::CylBend;
using SheetMetal::Element;
using SheetMetal::Model;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalFlange, PartDesign::FeatureAddSub)

const char* SheetMetalFlange::LengthTypeEnums[] = {"Leg",
                                                   "InnerSharp",
                                                   "OuterSharp",
                                                   "Tangent",
                                                   nullptr};
const char* SheetMetalFlange::BendPositionEnums[] = {"MaterialInside",
                                                     "MaterialOutside",
                                                     "BendOutside",
                                                     nullptr};
const char* SheetMetalFlange::ReliefShapeEnums[] = {"Rule",
                                                    "Rectangle",
                                                    "Obround",
                                                    "Tear",
                                                    nullptr};

namespace
{
const App::PropertyQuantityConstraint::Constraints angleRange = {0.01, 179.99, 1.0};
}

SheetMetalFlange::SheetMetalFlange()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(Additive);
    ADD_PROPERTY_TYPE(Edges,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "The edges, each where a wall's skin meets its end");
    ADD_PROPERTY_TYPE(Angle, (90.0), "Sheet Metal", App::Prop_None, "The bend's angle");
    Angle.setConstraints(&angleRange);
    ADD_PROPERTY_TYPE(Length, (10.0), "Sheet Metal", App::Prop_None, "The flange's length");
    ADD_PROPERTY_TYPE(LengthType,
                      (2L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Leg: the straight part; InnerSharp, OuterSharp: from where the inner or "
                      "outer skins of the wall and the leg meet; Tangent: from the plane across "
                      "the leg that touches the outside of the bend");
    LengthType.setEnums(LengthTypeEnums);
    ADD_PROPERTY_TYPE(BendPosition,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "MaterialInside: the leg's outer skin where the wall ended; MaterialOutside: "
                      "its inner skin there; BendOutside: the bend starting there");
    BendPosition.setEnums(BendPositionEnums);
    ADD_PROPERTY_TYPE(Offset,
                      (0.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "Moves the bend out from where BendPosition puts it, the wall carried on");
    ADD_PROPERTY_TYPE(Gap1,
                      (0.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "Taken off the flange's width at the edge's first vertex; a negative one "
                      "carries the flange past it");
    ADD_PROPERTY_TYPE(Gap2,
                      (0.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "Taken off the flange's width at the edge's last vertex; a negative one "
                      "carries the flange past it");
    ADD_PROPERTY_TYPE(ParallelTo,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A plane face the leg runs parallel to, in place of Angle");
    ADD_PROPERTY_TYPE(UpToFace,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A plane face the leg runs up to, in place of Length");
    ADD_PROPERTY_TYPE(Reversed,
                      (false),
                      "Sheet Metal",
                      App::Prop_None,
                      "Bend toward the other skin than the one the edge is on");
    ADD_PROPERTY_TYPE(Profile,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A closed sketch parallel to the leg, standing on the outer virtual sharp: "
                      "a rectangle through the bend, which sets its width, and the leg's shape "
                      "above it; on one edge, in place of Length, LengthType and the gaps");
    ADD_PROPERTY_TYPE(ReliefShape,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "The bend relief where the flange ends inside its wall's end: Rule follows "
                      "the rule's; another shape is the flange's own, from the rule's width and "
                      "depth when chosen; a tear is the rule's Gap wide");
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

bool SheetMetalFlange::shouldApplyPlacement()
{
    // Built where it is, its Placement never applied: a location made of an
    // identity placement is not an empty one, and each feature down a chain
    // stacked another on its shape, every boolean keying the edges' curves
    // on their surfaces to it. A 64-flange chain took 26 s to rebuild.
    return false;
}

short SheetMetalFlange::mustExecute() const
{
    if (Edges.isTouched() || Angle.isTouched() || Length.isTouched() || LengthType.isTouched()
        || BendPosition.isTouched() || Offset.isTouched() || Gap1.isTouched()
        || Gap2.isTouched() || Reversed.isTouched() || ParallelTo.isTouched()
        || UpToFace.isTouched() || Profile.isTouched() || ReliefShape.isTouched()
        || ReliefWidth.isTouched() || ReliefDepth.isTouched() || UseRuleRadius.isTouched()
        || BendRadius.isTouched()) {
        return 1;
    }
    return FeatureAddSub::mustExecute();
}

void SheetMetalFlange::onChanged(const App::Property* prop)
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
    if (prop == &Profile) {
        showProfileValues();
    }
    if (prop == &ReliefShape) {
        // From the rule to the flange's own: it starts from the rule's
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

void SheetMetalFlange::showReliefValues()
{
    const bool byRule = ReliefShape.getValue() == 0;
    ReliefWidth.setStatus(App::Property::ReadOnly, byRule);
    ReliefDepth.setStatus(App::Property::ReadOnly, byRule);
}

void SheetMetalFlange::showProfileValues()
{
    // a contour flange's profile is its chain: its gaps stay its own
    const bool shaped = Profile.getValue() != nullptr && !contour();
    for (App::Property* prop :
         std::initializer_list<App::Property*> {&Length, &LengthType, &Gap1, &Gap2}) {
        prop->setStatus(App::Property::ReadOnly, shaped);
    }
}

namespace
{

double areaOf(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape, props);
    return props.Mass();
}

/// What a profile makes of a flange: the bend's span along the edge, from
/// the edge's first vertex, and the leg's faces on its outer skin
struct LegShape
{
    double s0 = 0, s1 = 0;
    std::vector<TopoShape> faces;
};

/** A closed profile's face taken onto the leg's outer skin. sharp is the
 * outer virtual sharp at the edge's first vertex, along the edge's way,
 * legUp the leg's way out from the bend and rise how far the bend reaches up
 * the outer skin from the sharp. Throws Base::ValueError for a profile not
 * parallel to the leg, not standing on the sharp, reaching past it, other
 * than a rectangle through the bend, or with nothing above it.
 */
/** The bend of an earlier feature an edge's end meets at a corner: one of
 * the first earlier elements, from the same wall, its sheet starting on the
 * plane through q across out (the way the edge runs on past q), with its
 * material in base just beyond q, back from the edge's end face; -1 for none
 */
int earlierBendAt(const Model& model, int earlier, const TopoDS_Shape& base, int wall,
                  const gp_Pnt& q, const gp_Dir& out, const gp_Dir& endOut)
{
    for (int i = 0; i < earlier; ++i) {
        const Element& element = model.elements[i];
        // a flange's bend; a hem's fold, at 180 deg and past, has no leg to meet
        if (element.kind != Element::Kind::Bend || element.before != wall
            || element.bend.angle > std::numbers::pi - Precision::Angular()) {
            continue;
        }
        const CylBend& b = element.bend;
        // the way its sheet ran out of the wall, and where the bend starts
        const gp_Dir up = b.frame.XDirection().Reversed();
        const gp_Dir e = up.Crossed(b.frame.Direction());
        if (e.Dot(out) < 1 - Precision::Angular()) {
            continue;
        }
        const gp_Pnt start = b.frame.Location().Translated(gp_Vec(up) * -b.radius);
        if (std::abs(gp_Vec(start, q).Dot(gp_Vec(e))) > Precision::Confusion()) {
            continue;
        }
        const gp_Ax1 axis(b.frame.Location(), b.frame.Direction());
        const double at = gp_Vec(b.frame.Location(), q).Dot(gp_Vec(axis.Direction()));
        const gp_Dir radial = b.frame.XDirection().Rotated(axis, std::min(0.2, b.angle / 2));
        const gp_Pnt probe = b.frame.Location().Translated(
            gp_Vec(axis.Direction()) * at + gp_Vec(radial) * (b.radius + b.thickness / 2)
            - gp_Vec(endOut) * (b.thickness / 10));
        BRepClass3d_SolidClassifier classifier(base, probe, Precision::Confusion());
        if (classifier.State() == TopAbs_IN) {
            return i;
        }
    }
    return -1;
}

LegShape legShape(const TopoShape& face, const gp_Pnt& sharp, const gp_Dir& along,
                  const gp_Dir& legUp, double rise, const App::StringHasherRef& hasher)
{
    // the leg's frame: x along the edge, y up the leg, z across the sheet
    const gp_Ax3 frame(sharp, along.Crossed(legUp), along);
    gp_Pln plane;
    if (!face.findPlane(plane)
        || !plane.Axis().Direction().IsParallel(frame.Direction(), Precision::Angular())) {
        THROWM(Base::ValueError, "A flange's profile lies parallel to the leg");
    }
    gp_Trsf toLocal;
    toLocal.SetTransformation(frame);
    // and down onto the leg's outer skin, z = 0
    gp_Trsf onto;
    onto.SetTranslation(gp_Vec(0, 0, -plane.Location().Transformed(toLocal).Z()));
    onto.Multiply(toLocal);
    const TopoShape local = face.makETransform(onto);

    const double tol = Precision::Confusion();
    Bnd_Box box;
    BRepBndLib::AddOptimal(local.getShape(), box, false, false);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    if (y0 < -tol) {
        THROWM(Base::ValueError, "A flange's profile reaches past the outer virtual sharp");
    }
    if (y0 > tol) {
        THROWM(Base::ValueError, "A flange's profile does not stand on the outer virtual sharp");
    }

    // within the bend, a rectangle the bend's height
    auto part = [&](double from, double to) {
        TopoDS_Shape slab =
            BRepPrimAPI_MakeBox(gp_Pnt(x0 - 1, from, -1), gp_Pnt(x1 + 1, to, 1)).Solid();
        return TopoShape(0, hasher).makEBoolean(Part::OpCodes::Common, {local, TopoShape(slab)});
    };
    const TopoShape bend = part(0, rise);
    double bx0 = 0, by0, bz0, bx1 = 0, by1, bz1;
    bool rectangle = bend.countSubShapes(TopAbs_FACE) == 1;
    if (rectangle) {
        Bnd_Box bendBox;
        BRepBndLib::AddOptimal(bend.getShape(), bendBox, false, false);
        bendBox.Get(bx0, by0, bz0, bx1, by1, bz1);
        const double area = (bx1 - bx0) * rise;
        rectangle = std::abs(areaOf(bend.getShape()) - area) <= 1e-7 * area;
    }
    if (!rectangle) {
        THROWM(Base::ValueError,
               "A flange's profile is a rectangle through the bend, from the outer virtual "
               "sharp to where the leg starts");
    }

    LegShape result;
    result.s0 = bx0;
    result.s1 = bx1;
    if (y1 > rise + tol) {
        const TopoShape leg = part(rise, y1 + 1).makETransform(toLocal.Inverted());
        result.faces = leg.getSubTopoShapes(TopAbs_FACE);
    }
    if (result.faces.empty()) {
        THROWM(Base::ValueError, "A flange's profile has no leg above the bend");
    }
    return result;
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalFlange::execute()
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
    try {
        Model model = modelBefore(T);
        if (model.elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A flange builds on a sheet metal feature"));
        }
        int walls = 0, bends = 0;
        for (const Element& e : model.elements) {
            ++(e.kind == Element::Kind::Wall ? walls : bends);
        }

        // In place, as a pad of the current version is (shouldApplyPlacement())
        TopoShape base = getBaseShape();

        const auto shadows = Edges.getShadowSubs();
        if (shadows.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A flange needs an edge"));
        }

        // a plane face, if one is linked
        auto planeOf = [](const App::PropertyLinkSub& link, gp_Pln& plane) {
            if (!link.getValue()) {
                return false;
            }
            const auto& subs = link.getSubValues();
            TopoShape face = Part::Feature::getTopoShape(link.getValue(),
                                                         subs.empty() ? "" : subs.front().c_str(),
                                                         true);
            if (face.isNull() || !face.findPlane(plane)) {
                FC_THROWM(Base::ValueError,
                          "A flange's " << link.getName() << " is a plane face only");
            }
            return true;
        };
        gp_Pln parallel, upTo;
        const bool byParallel = planeOf(ParallelTo, parallel);
        const bool byUpTo = planeOf(UpToFace, upTo);
        const auto hasher = getDocument()->getStringHasher();
        // the elements of the features before, and how far past an edge's
        // end a flange meeting one of them at a corner is carried
        const int earlier = static_cast<int>(model.elements.size());
        Bnd_Box baseBound;
        BRepBndLib::Add(base.getShape(), baseBound);
        const double reach = std::sqrt(baseBound.SquareExtent()) + 1;

        // the leg's shape from a closed sketch, on one edge
        TopoShape profile;
        if (Profile.getValue() && !contour()) {
            if (shadows.size() != 1) {
                THROWM(Base::ValueError, "A flange shaped by a profile is on one edge only");
            }
            if (byUpTo) {
                THROWM(Base::ValueError,
                       "A flange shaped by a profile takes its length from it, not UpToFace");
            }
            const auto& subs = Profile.getSubValues();
            const TopoShape sketch = Part::Feature::getTopoShape(
                Profile.getValue(), subs.empty() ? "" : subs.front().c_str(), true);
            const auto wires = sketch.getSubTopoShapes(TopAbs_WIRE);
            bool closed = !wires.empty();
            for (const auto& wire : wires) {
                closed = closed && BRep_Tool::IsClosed(wire.getShape());
            }
            if (!closed) {
                THROWM(Base::ValueError, "A flange's profile is a closed sketch");
            }
            profile = TopoShape(0, hasher).makEFace(wires);
        }
        // A contour's lines from where it starts, each point's way along the
        // end's normal and the skin's of the edge it starts on: the same on
        // every edge
        std::vector<std::pair<double, double>> contourAt;
        if (contour()) {
            if (!Profile.getValue()) {
                THROWM(Base::ValueError, "A contour flange needs a sketch");
            }
            const auto& subs = Profile.getSubValues();
            const TopoShape sketch = Part::Feature::getTopoShape(
                Profile.getValue(), subs.empty() ? "" : subs.front().c_str(), true);
            auto wires = sketch.getSubTopoShapes(TopAbs_WIRE);
            if (wires.empty() && sketch.hasSubShape(TopAbs_EDGE)) {
                wires = sketch.makEWires().getSubTopoShapes(TopAbs_WIRE);
            }
            if (wires.size() != 1 || BRep_Tool::IsClosed(wires.front().getShape())) {
                THROWM(Base::ValueError, "A contour flange's sketch is one open chain of lines");
            }
            auto points = SheetMetal::linePoints(TopoDS::Wire(wires.front().getShape()),
                                                 "A contour flange's sketch");
            // the edge it starts on, from either end
            for (const auto& shadow : shadows) {
                const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
                const SheetMetal::WallEdge edge =
                    SheetMetal::wallEdge(base, model, ref, shadow.second);
                auto on = [&](const gp_Pnt& p) {
                    return gp_Vec(edge.p0, p).Crossed(gp_Vec(edge.along)).Magnitude()
                        < Precision::Confusion();
                };
                if (!on(points.front()) && on(points.back())) {
                    std::reverse(points.begin(), points.end());
                }
                if (!on(points.front())) {
                    continue;
                }
                for (const gp_Pnt& p : points) {
                    const gp_Vec v(points.front(), p);
                    if (std::abs(v.Dot(gp_Vec(edge.along))) > Precision::Confusion()) {
                        FC_THROWM(Base::ValueError,
                                  "A contour flange's sketch lies square to edge "
                                      << shadow.second << ", which it starts on");
                    }
                    contourAt.emplace_back(v.Dot(gp_Vec(edge.endOut)),
                                           v.Dot(gp_Vec(edge.skinOut)));
                }
                break;
            }
            if (contourAt.empty()) {
                THROWM(Base::ValueError,
                       "A contour flange's sketch starts on one of its edges, at either end");
            }
        }
        std::vector<TopoShape> pieces, cuts;
        // each edge's ends, its way out and up, and its pieces, for the miters
        struct Side
        {
            gp_Pnt p0, p1;
            gp_Dir endOut, up;
            std::size_t first, last;
            // the first bend (-1 for none) and the first wall after it
            int bend, leg;
            // the earlier bend each end meets at a corner, -1 for none
            int meets[2];
            // every element the side made
            std::vector<int> elements;
        };
        std::vector<Side> sides;
        for (const auto& shadow : shadows) {
            const std::size_t first = pieces.size();
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            const SheetMetal::WallEdge edge = SheetMetal::wallEdge(base, model, ref, shadow.second);
            const gp_Pnt p0 = edge.p0, p1 = edge.p1;
            const gp_Dir d = edge.along, endOut = edge.endOut;
            const int wall = edge.wall;

            // bending toward up, its inner skin the one facing that way; a
            // contour the way its first turn goes
            gp_Dir up = edge.skinOut;
            gp_Pnt inner = p0;
            bool away = Reversed.getValue();
            if (contour()) {
                away = false;
                for (const auto& at : contourAt) {
                    if (std::abs(at.second) > Precision::Confusion()) {
                        away = at.second < 0;
                        break;
                    }
                }
            }
            if (away) {
                up.Reverse();
                inner = p0.Translated(gp_Vec(up) * T);
            }
            // the angle, given or from the face the leg runs parallel to
            double A = Angle.getValue() * std::numbers::pi / 180;
            if (byParallel) {
                const gp_Dir n = parallel.Axis().Direction();
                A = std::atan2(-n.Dot(endOut), n.Dot(up));
                if (A <= 0) {
                    A += std::numbers::pi;
                }
                if (A < 1e-4 || A > std::numbers::pi - 1e-4) {
                    FC_THROWM(Base::ValueError,
                              "Edge " << shadow.second
                                      << ": a leg parallel to that face does not bend");
                }
            }
            double setback = 0;
            switch (BendPosition.getValue()) {
                case 0:
                    setback = (R + T) * std::tan(A / 2);
                    break;
                case 1:
                    setback = R * std::tan(A / 2);
                    break;
                default:
                    break;
            }
            setback -= Offset.getValue();

            const gp_Vec e(endOut), u(up);
            double gap1 = Gap1.getValue();
            double width = p0.Distance(p1) - gap1 - Gap2.getValue();
            LegShape shape;
            if (!profile.isNull()) {
                // where the leg's outer skin meets the wall's, at the first vertex
                const gp_Dir axis = endOut.Crossed(up);
                const gp_Dir legOut = up.Reversed().Rotated(gp_Ax1(p0, axis), A);
                const double rise = (R + T) * std::tan(A / 2);
                const gp_Pnt sharp = inner.Translated(-e * setback + u * R
                                                      + gp_Vec(legOut) * (R + T)
                                                      - gp_Vec(axis.Crossed(legOut)) * rise);
                shape = legShape(profile, sharp, d, axis.Crossed(legOut), rise, hasher);
                gap1 = shape.s0;
                width = shape.s1 - shape.s0;
            }
            if (width < Precision::Confusion()) {
                FC_THROWM(Base::ValueError,
                          "The gaps leave nothing of edge " << shadow.second);
            }
            // An end meeting an earlier flange at a corner (ruled 2026-11-02):
            // the edge stops where the earlier bend starts, its material just
            // beyond. The flange is carried on past the corner, to be mitered
            // against it below; the wall is cut back under the edge only.
            const gp_Vec cutAlong = gp_Vec(d) * width;
            const gp_Pnt cutStart = inner.Translated(gp_Vec(d) * gap1);
            int meets[2] = {-1, -1};
            if (profile.isNull()) {
                if (gap1 <= Precision::Confusion()) {
                    meets[0] = earlierBendAt(model, earlier, base.getShape(), wall, p0,
                                             d.Reversed(), endOut);
                }
                if (Gap2.getValue() <= Precision::Confusion()) {
                    meets[1] = earlierBendAt(model, earlier, base.getShape(), wall, p1, d, endOut);
                }
                for (int end : meets) {
                    if (end >= 0 && contour()) {
                        FC_THROWM(Base::ValueError,
                                  "Edge " << shadow.second << " meets "
                                          << model.elements[end].name
                                          << " at a corner: a contour flange is not mitered "
                                             "against an earlier flange, give it a gap there");
                    }
                }
                if (meets[0] >= 0) {
                    gap1 -= reach;
                    width += reach;
                }
                if (meets[1] >= 0) {
                    width += reach;
                }
            }
            const gp_Vec along = gp_Vec(d) * width;
            const gp_Pnt edgeStart = inner.Translated(gp_Vec(d) * gap1);
            // the edge's length left beyond the flange at its last vertex
            const double gap2 = p0.Distance(p1) - gap1 - width;

            int bendIndex = -1, legWall = -1;
            std::vector<int> elements;
            // what the wall carried on or cut back under it is named after
            std::string bendName;
            if (contour()) {
                // The wall's skin through the edge runs on as the lines: the
                // wall the chain's first line, its material from -T to 0 along
                // its left (n x its way), the skin's outward normal
                const gp_Pnt start = p0.Translated(gp_Vec(d) * gap1);
                std::vector<gp_Pnt> chain {start.Translated(-e), start};
                for (std::size_t i = 1; i < contourAt.size(); ++i) {
                    chain.push_back(start.Translated(e * contourAt[i].first
                                                     + gp_Vec(edge.skinOut) * contourAt[i].second));
                }
                SheetMetal::dropStraight(chain);
                SheetMetal::ProfileBuilder build(model, walls, bends);
                build.k = [rule](double angle, double radius) {
                    return rule->modelK(angle, radius);
                };
                build.thickness = T;
                build.radius = R;
                build.first = wall;
                build.profileAt(chain, endOut.Crossed(edge.skinOut), -T);
                // the wall's straight part ends short of the edge, or past it
                setback = gp_Vec(build.firstEnd, start).Dot(e);
                for (const TopoShape& face : build.faces) {
                    pieces.push_back(TopoShape(0, hasher).makEPrism(face, along));
                }
                bendIndex = build.firstBend;
                elements = build.added;
                for (int k : elements) {
                    if (legWall < 0 && model.elements[k].kind == Element::Kind::Wall) {
                        legWall = k;
                    }
                }
                bendName = bendIndex >= 0 ? model.elements[bendIndex].name
                                          : model.elements[wall].name;
            }
            else {
                const gp_Pnt bendStart = edgeStart.Translated(-e * setback);

                bendName = "Bend" + std::to_string(++bends);
                const std::string legName = "Wall" + std::to_string(++walls);
                CylBend bend;
                bend.frame = gp_Ax3(bendStart.Translated(u * R), endOut.Crossed(up), up.Reversed());
                bend.radius = R;
                bend.thickness = T;
                bend.angle = A;
                bend.k = rule->modelK(A, R);

                // the leg goes on from the bend's end
                const gp_Dir x2 = bend.frame.XDirection().Rotated(
                    gp_Ax1(bend.frame.Location(), bend.frame.Direction()), A);
                const gp_Dir legAlong = bend.frame.Direction().Crossed(x2);
                const gp_Pnt legStart = bend.frame.Location().Translated(gp_Vec(x2) * R);

                // the leg's straight part, by the length type or up to a face
                double leg = Length.getValue();
                if (byUpTo) {
                    const double across = legAlong.Dot(upTo.Axis().Direction());
                    if (std::abs(across) < Precision::Angular()) {
                        FC_THROWM(Base::ValueError,
                                  "Edge " << shadow.second << ": the leg never meets that face");
                    }
                    leg = gp_Vec(legStart, upTo.Location()).Dot(gp_Vec(upTo.Axis().Direction()))
                        / across;
                }
                else if (LengthType.getValue() == 1) {
                    leg -= R * std::tan(A / 2);
                }
                else if (LengthType.getValue() == 2) {
                    leg -= (R + T) * std::tan(A / 2);
                }
                else if (LengthType.getValue() == 3) {
                    leg -= (R + T) * (A >= std::numbers::pi / 2 ? 1.0 : std::sin(A));
                }
                if (shape.faces.empty() && leg < Precision::Confusion()) {
                    FC_THROWM(Base::ValueError,
                              "Edge " << shadow.second << ": the flange is too short for its bend");
                }
                legWall = model.addWall(legName, gp_Ax3(legStart, x2, legAlong));
                bendIndex = model.addBend(bendName, bend, wall, legWall);
                elements = {bendIndex, legWall};

                pieces.push_back(TopoShape(0, hasher).makEPrism(
                    SheetMetal::bendSection(bend, bendName), along));
                if (shape.faces.empty()) {
                    pieces.push_back(TopoShape(0, hasher).makEPrism(
                        SheetMetal::wallSection(legStart, legAlong, leg, x2, T, legName), along));
                }
                for (const TopoShape& face : shape.faces) {
                    // from the outer skin in
                    pieces.push_back(TopoShape(0, hasher).makEPrism(face, gp_Vec(x2) * -T));
                }
            }
            // Ending inside the wall's end, the bend stops at its sides: its
            // tube runs on through the wall beside it, the relief's faces
            // there among it
            if (bendIndex >= 0 && gap1 > Precision::Confusion()) {
                model.elements[bendIndex].limits.emplace_back(edgeStart, d.Reversed());
            }
            if (bendIndex >= 0 && gap2 > Precision::Confusion()) {
                model.elements[bendIndex].limits.emplace_back(edgeStart.Translated(along), d);
            }
            if (setback < -Precision::Confusion()) {
                // the wall carried on to where the bend starts
                pieces.push_back(TopoShape(0, hasher).makEPrism(
                    SheetMetal::wallSection(edgeStart, endOut, -setback, up.Reversed(), T,
                                            bendName + "Ext"),
                    along));
            }
            else if (setback > Precision::Confusion()) {
                // the wall cut back under the flange to where the bend starts
                cuts.push_back(TopoShape(0, hasher).makEPrism(
                    SheetMetal::wallSection(cutStart.Translated(-e * setback), endOut, setback,
                                            up.Reversed(), T, bendName + "Cut"),
                    cutAlong));
                // Where the flange ends inside the wall's end, the wall left
                // beside it meets the bend's side face to face, and only a
                // relief parts them for the flat: beside the bend, from the
                // wall's end to the relief's depth past where the bend starts.
                // A tear is a slit the rule's gap wide, to where the bend starts.
                // The flange's own relief, or the rule's.
                const bool own = ReliefShape.getValue() != 0;
                const int reliefShape =
                    own ? ReliefShape.getValue() - 1 : rule->ReliefShape.getValue();
                const bool tear = reliefShape == 2;
                double reliefWidth = own ? ReliefWidth.getValue() : rule->ReliefWidth.getValue();
                double depth =
                    setback + (own ? ReliefDepth.getValue() : rule->ReliefDepth.getValue());
                if (tear) {
                    reliefWidth = rule->Gap.getValue();
                    depth = setback;
                }
                SheetMetal::Relief relief;
                relief.shape = static_cast<SheetMetal::Relief::Shape>(reliefShape);
                relief.width = reliefWidth;
                relief.depth = depth;
                for (TopoShape& cut : SheetMetal::reliefCuts(relief, inner, d, endOut, up, T, gap1,
                                                             width, gap2, bendName + "Relief",
                                                             hasher)) {
                    cuts.push_back(std::move(cut));
                }
            }
            sides.push_back({p0,
                             p1,
                             endOut,
                             up,
                             first,
                             pieces.size(),
                             bendIndex,
                             legWall,
                             {meets[0], meets[1]},
                             elements});
        }
        // the bends meeting at each miter, for the corner relief
        std::vector<std::pair<int, int>> mitered;
        // A miter where two edges meet at a corner, bent the same way: each
        // side stops at the plane halving the corner, the rule's gap between
        // them. Exact for two flanges alike; any two are kept apart.
        const bool corners = std::any_of(sides.begin(), sides.end(), [](const Side& side) {
            return side.meets[0] >= 0 || side.meets[1] >= 0;
        });
        if (sides.size() > 1 || corners) {
            Bnd_Box bound;
            for (const auto& piece : pieces) {
                BRepBndLib::Add(piece.getShape(), bound);
            }
            const double size = 4 * std::sqrt(bound.SquareExtent()) + 1;
            const double gap = rule->Gap.getValue();
            // the half-space on the plane's +m side, offset along m, as a box
            auto halfSpace = [&](const gp_Pnt& at, const gp_Dir& m) {
                const gp_Ax2 frame(at, m);
                const gp_Pnt corner = at.Translated(gp_Vec(frame.XDirection()) * (-size / 2)
                                                    + gp_Vec(frame.YDirection()) * (-size / 2));
                return TopoShape(BRepPrimAPI_MakeBox(gp_Ax2(corner, m, frame.XDirection()),
                                                     size,
                                                     size,
                                                     size)
                                     .Solid());
            };
            auto cutAway = [&](const Side& side, const TopoShape& box) {
                for (std::size_t k = side.first; k < side.last; ++k) {
                    TopoShape cut(0, hasher);
                    cut.makEBoolean(Part::OpCodes::Cut, {pieces[k], box});
                    pieces[k] = cut.hasSubShape(TopAbs_SOLID) ? getSolid(cut) : TopoShape();
                }
            };
            const double tol = Precision::Confusion();
            for (std::size_t i = 0; i < sides.size(); ++i) {
                for (std::size_t j = i + 1; j < sides.size(); ++j) {
                    const Side &a = sides[i], &b = sides[j];
                    gp_Pnt corner;
                    bool meet = false;
                    for (const gp_Pnt& p : {a.p0, a.p1}) {
                        for (const gp_Pnt& q : {b.p0, b.p1}) {
                            if (!meet && p.Distance(q) < tol) {
                                corner = p;
                                meet = true;
                            }
                        }
                    }
                    const gp_Vec halving = gp_Vec(a.endOut) + gp_Vec(b.endOut);
                    if (!meet || !a.up.IsEqual(b.up, Precision::Angular())
                        || halving.Magnitude() < tol) {
                        continue;
                    }
                    gp_Dir m(gp_Vec(a.up).Crossed(halving));
                    // a on the -m side
                    const gp_Pnt middle((a.p0.XYZ() + a.p1.XYZ()) / 2);
                    if (gp_Vec(corner, middle).Dot(gp_Vec(m)) > 0) {
                        m.Reverse();
                    }
                    cutAway(a, halfSpace(corner.Translated(gp_Vec(m) * (-gap / 2)), m));
                    cutAway(b,
                            halfSpace(corner.Translated(gp_Vec(m) * (gap / 2)), m.Reversed()));
                    // and in the model, where the two sides' slabs and tubes
                    // overlap at the corner
                    for (int k : a.elements) {
                        model.elements[k].limits.emplace_back(corner, m);
                    }
                    for (int k : b.elements) {
                        model.elements[k].limits.emplace_back(corner, m.Reversed());
                    }
                    if (a.bend >= 0 && b.bend >= 0) {
                        mitered.emplace_back(a.bend, b.bend);
                    }
                }
            }
            // Against an earlier flange at a corner: the plane mirroring the
            // earlier leg's outer skin onto this one's, which for two flanges
            // alike is the plane halving the corner, as one flange on both
            // edges has it. This side's pieces stop on their side of it; the
            // earlier flange's material in front of where its bend starts, on
            // this side, is cut from the part; the rule's gap between.
            for (const Side& side : sides) {
                for (int end : side.meets) {
                    if (end < 0) {
                        continue;
                    }
                    const Element& before = model.elements[end];
                    const gp_Ax3& legA = model.elements[before.after].frame;
                    const gp_Ax3& legB = model.elements[side.leg].frame;
                    const gp_Vec n1(legA.Direction()), n2(legB.Direction());
                    const gp_Vec normal = n1 - n2;
                    if (normal.Magnitude() < tol) {
                        continue;
                    }
                    // n1 . p = c1 on the one outer skin, n2 . p = c2 on the other
                    const double c1 = n1.Dot(gp_Vec(legA.Location().XYZ())) + T;
                    const double c2 = n2.Dot(gp_Vec(legB.Location().XYZ())) + T;
                    gp_Dir m(normal);
                    const gp_Pnt on(normal.XYZ() * ((c1 - c2) / normal.SquareMagnitude()));
                    // this side on the +m side
                    const gp_Pnt middle((side.p0.XYZ() + side.p1.XYZ()) / 2);
                    if (gp_Vec(on, middle).Dot(gp_Vec(m)) < 0) {
                        m.Reverse();
                    }
                    cutAway(side, halfSpace(on.Translated(gp_Vec(m) * (gap / 2)), m.Reversed()));
                    const CylBend& b = before.bend;
                    const gp_Dir bUp = b.frame.XDirection().Reversed();
                    const gp_Dir bOut = bUp.Crossed(b.frame.Direction());
                    const gp_Pnt bStart = b.frame.Location().Translated(gp_Vec(bUp) * -b.radius);
                    // in two, the bend's and the leg's, so that the face the
                    // cut leaves on each is that element's own
                    auto common = [&](const TopoShape& a, const TopoShape& b) {
                        return TopoShape(0, hasher).makEBoolean(Part::OpCodes::Common, {a, b});
                    };
                    const TopoShape beyond =
                        common(halfSpace(on.Translated(gp_Vec(m) * (-gap / 2)), m),
                               halfSpace(bStart, bOut));
                    cuts.push_back(
                        common(beyond, halfSpace(legA.Location(), legA.XDirection().Reversed())));
                    cuts.push_back(common(beyond, halfSpace(legA.Location(), legA.XDirection())));
                    for (int k : {end, before.after}) {
                        model.elements[k].limits.emplace_back(on, m);
                    }
                    for (int k : {side.bend, side.leg}) {
                        model.elements[k].limits.emplace_back(on, m.Reversed());
                    }
                    mitered.emplace_back(end, side.bend);
                }
            }
            pieces.erase(std::remove_if(pieces.begin(),
                                        pieces.end(),
                                        [](const TopoShape& piece) {
                                            return piece.isNull();
                                        }),
                         pieces.end());
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
        TopoShape result = makeBoolean(base, tool, true);
        if (contour()) {
            // a chain turning back into the sheet: the join less than both
            auto volume = [](const TopoShape& shape) {
                GProp_GProps props;
                BRepGProp::VolumeProperties(shape.getShape(), props);
                return props.Mass();
            };
            const double both = volume(base) + volume(tool);
            if (std::abs(volume(result) - both) > 1e-7 * both) {
                THROWM(Base::ValueError, "A contour flange runs into the sheet");
            }
        }
        // The corner relief at each miter, drawn in the flat and folded back
        // (sec 5.2); none for None, and a tear is the miter's gap itself
        const std::vector<TopoShape> reliefs = SheetMetal::cornerReliefs(
            model, mitered, rule->CornerReliefShape.getValue(), rule->CornerReliefSize.getValue(),
            rule->CornerReliefCover.getValue());
        if (!reliefs.empty()) {
            result = getSolid(TopoShape(0, hasher).makEBoolean(
                Part::OpCodes::Cut, {result, TopoShape().makECompound(reliefs)}));
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

PROPERTY_SOURCE(PartDesign::SheetMetalContourFlange, PartDesign::SheetMetalFlange)

SheetMetalContourFlange::SheetMetalContourFlange()
{
    // the sketch says the angle, the length and where the bend sits
    for (App::Property* prop : std::initializer_list<App::Property*> {&Angle,
                                                                      &Length,
                                                                      &LengthType,
                                                                      &BendPosition,
                                                                      &Offset,
                                                                      &ParallelTo,
                                                                      &UpToFace,
                                                                      &Reversed}) {
        prop->setStatus(App::Property::Hidden, true);
    }
}
