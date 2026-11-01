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
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
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
        || UpToFace.isTouched()) {
        return 1;
    }
    return FeatureAddSub::mustExecute();
}

void SheetMetalFlange::onChanged(const App::Property* prop)
{
    // The edges are of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Edges.getValue()
        && Edges.getValue() != BaseFeature.getValue()) {
        auto subs = Edges.getSubValues(false);
        auto shadows = Edges.getShadowSubs();
        Edges.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    FeatureAddSub::onChanged(prop);
}

namespace
{

/// A face's outward normal, at a planar face; false for another face
bool planeNormal(const TopoDS_Face& face, gp_Dir& normal)
{
    BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane) {
        return false;
    }
    normal = surface.Plane().Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    return true;
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
    const double R = rule->BendRadius.getValue();
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
        std::vector<TopoShape> pieces, cuts;
        // each edge's ends, its way out and up, and its pieces, for the miters
        struct Side
        {
            gp_Pnt p0, p1;
            gp_Dir endOut, up;
            std::size_t first, last;
            int bend, leg;
        };
        std::vector<Side> sides;
        for (const auto& shadow : shadows) {
            const std::size_t first = pieces.size();
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            const TopoDS_Shape sub = base.getSubShape(ref.c_str(), true);
            if (sub.IsNull() || sub.ShapeType() != TopAbs_EDGE) {
                FC_THROWM(Base::ValueError, "No edge " << shadow.second);
            }
            const TopoDS_Edge& edge = TopoDS::Edge(sub);
            if (BRepAdaptor_Curve(edge).GetType() != GeomAbs_Line) {
                FC_THROWM(Base::ValueError, "Edge " << shadow.second << " is not straight");
            }
            TopoDS_Vertex v0, v1;
            TopExp::Vertices(edge, v0, v1);
            const gp_Pnt p0 = BRep_Tool::Pnt(v0), p1 = BRep_Tool::Pnt(v1);
            const gp_Dir d(gp_Vec(p0, p1));

            // the skin the edge is on, the wall it belongs to, and its end
            int wall = -1;
            gp_Dir skinOut, endOut;
            bool haveEnd = false;
            for (const auto& f : base.findAncestorsShapes(edge, TopAbs_FACE)) {
                const TopoDS_Face& face = TopoDS::Face(f);
                gp_Dir n;
                if (!planeNormal(face, n)) {
                    continue;
                }
                const int owner = model.owner(face);
                if (owner >= 0 && model.elements[owner].kind == Element::Kind::Wall
                    && n.IsParallel(model.elements[owner].frame.Direction(),
                                    Precision::Angular())) {
                    wall = owner;
                    skinOut = n;
                }
                else {
                    endOut = n;
                    haveEnd = true;
                }
            }
            if (wall < 0 || !haveEnd || std::abs(endOut.Dot(skinOut)) > Precision::Angular()) {
                FC_THROWM(Base::ValueError,
                          "Edge " << shadow.second << " is not where a wall's skin meets its end");
            }

            // bending toward up, its inner skin the one facing that way
            gp_Dir up = skinOut;
            gp_Pnt inner = p0;
            if (Reversed.getValue()) {
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

            const double width = p0.Distance(p1) - Gap1.getValue() - Gap2.getValue();
            if (width < Precision::Confusion()) {
                FC_THROWM(Base::ValueError,
                          "The gaps leave nothing of edge " << shadow.second);
            }
            const gp_Vec e(endOut), u(up), along = gp_Vec(d) * width;
            const gp_Pnt edgeStart = inner.Translated(gp_Vec(d) * Gap1.getValue());
            const gp_Pnt bendStart = edgeStart.Translated(-e * setback);

            const std::string bendName = "Bend" + std::to_string(++bends);
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
            if (leg < Precision::Confusion()) {
                FC_THROWM(Base::ValueError,
                          "Edge " << shadow.second << ": the flange is too short for its bend");
            }
            const int legWall = model.addWall(legName, gp_Ax3(legStart, x2, legAlong));
            const int bendIndex = model.addBend(bendName, bend, wall, legWall);

            pieces.push_back(TopoShape(0, hasher).makEPrism(
                SheetMetal::bendSection(bend, bendName), along));
            pieces.push_back(TopoShape(0, hasher).makEPrism(
                SheetMetal::wallSection(legStart, legAlong, leg, x2, T, legName), along));
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
                    SheetMetal::wallSection(bendStart, endOut, setback, up.Reversed(), T,
                                            bendName + "Cut"),
                    along));
            }
            sides.push_back({p0, p1, endOut, up, first, pieces.size(), bendIndex, legWall});
        }
        // A miter where two edges meet at a corner, bent the same way: each
        // side stops at the plane halving the corner, the rule's gap between
        // them. Exact for two flanges alike; any two are kept apart.
        if (sides.size() > 1) {
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
                    for (int k : {a.bend, a.leg}) {
                        model.elements[k].limits.emplace_back(corner, m);
                    }
                    for (int k : {b.bend, b.leg}) {
                        model.elements[k].limits.emplace_back(corner, m.Reversed());
                    }
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
            base = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                                    {base, TopoShape().makECompound(cuts)});
            base = getSolid(base);
        }
        TopoShape tool = pieces.size() == 1 ? pieces.front()
                                             : TopoShape(0, hasher).makEFuse(pieces);
        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        Shape.setValue(makeBoolean(base, tool));
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
