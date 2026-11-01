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

#include <algorithm>
#include <limits>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Vertex.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalExtendWall.h"
#include "FeatureSheetMetalRule.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::Element;

namespace
{

/// How far along dir the shape's vertices reach from p: the least and the most
std::pair<double, double>
spanAlong(const TopoDS_Shape& shape, const gp_Pnt& p, const gp_Dir& dir)
{
    std::pair<double, double> res {std::numeric_limits<double>::max(),
                                   -std::numeric_limits<double>::max()};
    for (TopExp_Explorer it(shape, TopAbs_VERTEX); it.More(); it.Next()) {
        const double d = gp_Vec(p, BRep_Tool::Pnt(TopoDS::Vertex(it.Current()))).Dot(gp_Vec(dir));
        res.first = std::min(res.first, d);
        res.second = std::max(res.second, d);
    }
    return res;
}

double volumeOf(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

/// Whether the edge lies on one of the planar ends carried out: a seam the
/// fuse left where the end was
bool onEnd(const TopoDS_Edge& edge, const std::vector<TopoDS_Face>& ends)
{
    BRepAdaptor_Curve curve(edge);
    const double tol = Precision::Confusion();
    for (const auto& end : ends) {
        const gp_Pln plane = BRepAdaptor_Surface(end).Plane();
        bool on = true;
        for (double t : {0.0, 0.5, 1.0}) {
            const gp_Pnt p = curve.Value(curve.FirstParameter()
                                         + t * (curve.LastParameter() - curve.FirstParameter()));
            const TopAbs_State state = BRepClass_FaceClassifier(end, p, tol).State();
            if (plane.Distance(p) > tol || (state != TopAbs_IN && state != TopAbs_ON)) {
                on = false;
                break;
            }
        }
        if (on) {
            return true;
        }
    }
    return false;
}

/// The walls made one with their extensions: the seams at the old ends
/// unified and every other edge kept, so the faces of two elements stay apart
/// (no plain refine, docs/SheetMetal.md sec 9)
TopoShape unifySeams(const TopoShape& shape, const std::vector<TopoDS_Face>& ends)
{
    ShapeUpgrade_UnifySameDomain unify(shape.getShape(), true, true, false);
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape.getShape(), TopAbs_EDGE, edges);
    bool seams = false;
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(edges(i));
        if (onEnd(edge, ends)) {
            seams = true;
        }
        else {
            unify.KeepShape(edge);
        }
    }
    if (!seams) {
        return shape;
    }
    unify.Build();
    TopoShape res(shape.Tag, shape.Hasher);
    res.makESHAPE(unify.Shape(), Part::MapperHistory(unify.History()), {shape});
    return res;
}

}  // namespace

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalExtendWall, PartDesign::FeatureAddSub)

SheetMetalExtendWall::SheetMetalExtendWall()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(Additive);
    ADD_PROPERTY_TYPE(Faces,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "The ends of walls to carry out, each a face across the sheet");
    ADD_PROPERTY_TYPE(Length,
                      (10.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "How far to carry the ends out; a negative length cuts the wall back");
    ADD_PROPERTY_TYPE(UpToFace,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A plane face to carry the ends up to, in place of Length");
    ADD_PROPERTY_TYPE(Gap1,
                      (0.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "How far in from the first end of a wall's end the extension starts");
    ADD_PROPERTY_TYPE(Gap2,
                      (0.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "How far in from the second end of a wall's end the extension stops");
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
}

short SheetMetalExtendWall::mustExecute() const
{
    if (Faces.isTouched() || Length.isTouched() || UpToFace.isTouched() || Gap1.isTouched()
        || Gap2.isTouched()) {
        return 1;
    }
    return FeatureAddSub::mustExecute();
}

void SheetMetalExtendWall::onChanged(const App::Property* prop)
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

App::DocumentObjectExecReturn* SheetMetalExtendWall::execute()
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
                QT_TRANSLATE_NOOP("Exception", "Extend wall builds on a sheet metal feature"));
        }
        TopoShape base = getBaseShape();
        const auto shadows = Faces.getShadowSubs();
        if (shadows.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Extend wall needs a wall's end"));
        }

        // the plane to go up to, if any
        bool upTo = false;
        gp_Pln plane;
        if (UpToFace.getValue()) {
            const auto& subs = UpToFace.getSubValues();
            TopoShape target = Part::Feature::getTopoShape(
                UpToFace.getValue(), subs.empty() ? "" : subs.front().c_str(), true);
            if (target.isNull() || !target.findPlane(plane)) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Extend wall goes up to a plane face only"));
            }
            upTo = true;
        }

        const auto hasher = getDocument()->getStringHasher();
        std::vector<TopoShape> pieces, cuts;
        std::vector<TopoDS_Face> ends;
        for (const auto& shadow : shadows) {
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            TopoShape face = base.getSubTopoShape(ref.c_str(), true);
            if (face.isNull() || face.getShape().ShapeType() != TopAbs_FACE) {
                FC_THROWM(Base::ValueError, "No face " << shadow.second);
            }
            const TopoDS_Face& f = TopoDS::Face(face.getShape());
            BRepAdaptor_Surface surface(f);
            const int owner = model.owner(f);
            if (surface.GetType() != GeomAbs_Plane || owner < 0
                || model.elements[owner].kind != Element::Kind::Wall) {
                FC_THROWM(Base::ValueError, "Face " << shadow.second << " is on no wall");
            }
            gp_Dir out = surface.Plane().Axis().Direction();
            if (f.Orientation() == TopAbs_REVERSED) {
                out.Reverse();
            }
            if (std::abs(out.Dot(model.elements[owner].frame.Direction()))
                > Precision::Angular()) {
                FC_THROWM(Base::ValueError,
                          "Face " << shadow.second << " is a skin, not a wall's end");
            }
            double distance = Length.getValue();
            if (upTo) {
                GProp_GProps props;
                BRepGProp::SurfaceProperties(f, props);
                const gp_Pnt center = props.CentreOfMass();
                const double across = out.Dot(plane.Axis().Direction());
                if (std::abs(across) < Precision::Angular()) {
                    FC_THROWM(Base::ValueError,
                              "Face " << shadow.second << " never meets the face to go up to");
                }
                distance = gp_Vec(center, plane.Location()).Dot(gp_Vec(plane.Axis().Direction()))
                    / across;
            }
            if (std::abs(distance) < Precision::Confusion()) {
                continue;
            }

            // across the end, from its first end to its second
            const gp_Dir normal = model.elements[owner].frame.Direction();
            const gp_Dir across(gp_Vec(out).Crossed(gp_Vec(normal)));
            GProp_GProps props;
            BRepGProp::SurfaceProperties(f, props);
            const gp_Pnt center = props.CentreOfMass();
            const auto [first, second] = spanAlong(f, center, across);
            const double gap1 = Gap1.getValue(), gap2 = Gap2.getValue();
            if (first + gap1 >= second - gap2 - Precision::Confusion()) {
                FC_THROWM(Base::ValueError,
                          "Face " << shadow.second << ": the gaps leave nothing of it");
            }
            // the band between the gaps, deep and long enough for any prism
            auto band = [&](double length) {
                const double reach = std::abs(length) + 1;
                const double depth = 10 * (rule->Thickness.getValue() + second - first);
                const gp_Pnt origin = center.Translated(gp_Vec(out) * (length < 0 ? -reach : -1)
                                                        + gp_Vec(across) * (first + gap1)
                                                        + gp_Vec(normal) * depth);
                // X out, Y across, Z against the normal
                return BRepPrimAPI_MakeBox(gp_Ax2(origin, normal.Reversed(), out),
                                           reach + 1,
                                           second - gap2 - first - gap1,
                                           2 * depth)
                    .Shape();
            };
            auto prismOf = [&](double length) {
                TopoShape prism = TopoShape(0, hasher).makEPrism(face, gp_Vec(out) * length);
                if (gap1 > 0 || gap2 > 0) {
                    prism = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Common,
                                                             {prism, TopoShape(band(length))});
                }
                return prism;
            };

            if (distance > 0) {
                // a wall in the way, or met at the end: stop the rule's gap
                // short of it
                const double clearance = rule->Gap.getValue();
                const TopoShape reach = prismOf(distance + clearance);
                BRepAlgoAPI_Common meet(base.getShape(), reach.getShape());
                if (meet.IsDone() && volumeOf(meet.Shape()) > Precision::Confusion()) {
                    const double nearest = spanAlong(meet.Shape(), center, out).first;
                    if (nearest - clearance <= Precision::Confusion()) {
                        FC_THROWM(Base::ValueError,
                                  "Face " << shadow.second
                                          << " has no room: a wall is in the way, nearer than "
                                             "the rule's gap");
                    }
                    distance = std::min(distance, nearest - clearance);
                }
            }
            TopoShape prism = prismOf(distance);
            (distance > 0 ? pieces : cuts).push_back(prism);
            if (distance > 0) {
                ends.push_back(f);
            }
        }

        if (!cuts.empty()) {
            TopoShape cut(0, hasher);
            cut.makEBoolean(Part::OpCodes::Cut, {base, TopoShape().makECompound(cuts)});
            if (!cut.hasSubShape(TopAbs_SOLID)) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Nothing is left of the wall cut back"));
            }
            base = getSolid(cut);
        }
        TopoShape tool;
        if (!pieces.empty()) {
            tool = pieces.size() == 1 ? pieces.front() : TopoShape(0, hasher).makEFuse(pieces);
        }
        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        Shape.setValue(tool.isNull() ? base : unifySeams(makeBoolean(base, tool), ends));
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
