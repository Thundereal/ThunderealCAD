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
#include <limits>
#include <vector>

#include <BRepAdaptor_Curve.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>

#include "FeatureSheetMetalRule.h"
#include "FeatureSheetMetalTab.h"
#include "SheetMetal/Pieces.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::Element;
using SheetMetal::Model;
using SheetMetal::skinFaces;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalTab, PartDesign::ProfileBased)

SheetMetalTab::SheetMetalTab()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(FeatureAddSub::Additive);
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
    for (App::Property* prop : std::initializer_list<App::Property*> {&Reversed,
                                                                      &Midplane,
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

namespace
{

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

/// Points telling where a solid lies: its vertices, its edges' middles and
/// its faces' centres
std::vector<gp_Pnt> samples(const TopoDS_Shape& shape)
{
    std::vector<gp_Pnt> res;
    for (TopExp_Explorer it(shape, TopAbs_VERTEX); it.More(); it.Next()) {
        res.push_back(BRep_Tool::Pnt(TopoDS::Vertex(it.Current())));
    }
    for (TopExp_Explorer it(shape, TopAbs_EDGE); it.More(); it.Next()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
        res.push_back(curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2));
    }
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(it.Current(), props);
        res.push_back(props.CentreOfMass());
    }
    return res;
}

/** The bend of the wall a point lies past, -1 for none: beyond the line a
 * bend from the wall starts at, or short of where one into it ends, as the
 * wall lies flat. A bend's side() goes by angle about its axis, so a point
 * straight on in the wall's plane past a bend turning up is not past it;
 * no part of a sheet lies there, but a tab could.
 */
int pastBend(const Model& model, int wall, const gp_Pnt& point, double tol)
{
    for (std::size_t i = 0; i < model.elements.size(); ++i) {
        const Element& el = model.elements[i];
        if (el.kind != Element::Kind::Bend) {
            continue;
        }
        // the frame's Y is the way the wall before runs into the bend
        const gp_Ax3& frame = el.bend.frame;
        if (el.before == wall
            && gp_Vec(frame.Location(), point).Dot(gp_Vec(frame.YDirection())) > tol) {
            return static_cast<int>(i);
        }
        if (el.after == wall) {
            const gp_Pnt flat = point.Transformed(el.bend.unbend());
            const double ba = el.bend.angle * el.bend.neutral();
            if (gp_Vec(frame.Location(), flat).Dot(gp_Vec(frame.YDirection())) < ba - tol) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalTab::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    const double T = rule->Thickness.getValue();
    try {
        TopoShape base = getBaseShape();
        auto invObjLoc = positionByPrevious();
        base.move(invObjLoc);
        const Model model = modelBefore(T);
        if (model.elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A tab builds on a sheet metal feature"));
        }
        TopoShape face = getVerifiedFace();
        face.move(invObjLoc);
        const Base::Vector3d sketchNormal = getProfileNormal();
        gp_Vec n(sketchNormal.x, sketchNormal.y, sketchNormal.z);
        n.Transform(invObjLoc.Transformation());
        const gp_Dir normal(n);
        gp_Pnt on;
        {
            TopExp_Explorer v(face.getShape(), TopAbs_VERTEX);
            if (!v.More()) {
                THROWM(Base::ValueError, "A tab's sketch is empty");
            }
            on = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
        }
        const TopoDS_Shape& shape = base.getShape();

        // the wall: parallel to the sketch, the sketch taken onto its first
        // skin meeting the wall's there, the nearest
        int wall = -1;
        gp_Trsf onto;
        double nearest = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < model.elements.size(); ++i) {
            const Element& el = model.elements[i];
            if (el.kind != Element::Kind::Wall
                || !el.frame.Direction().IsParallel(normal, Precision::Angular())) {
                continue;
            }
            const gp_Dir z = el.frame.Direction();
            // from the sketch to the first skin, along z
            const double d = gp_Vec(on, el.frame.Location()).Dot(gp_Vec(z));
            const auto skins =
                skinFaces(shape, model, static_cast<int>(i), gp_Pln(el.frame.Location(), z));
            if (skins.empty()) {
                continue;
            }
            gp_Trsf move;
            move.SetTranslation(gp_Vec(z) * d);
            BRepExtrema_DistShapeShape dist(face.makETransform(move).getShape(),
                                            compoundOf(skins));
            if (!dist.IsDone() || dist.Value() > Precision::Confusion()) {
                continue;
            }
            // how far the sketch lies off the wall's slab
            const double away = d >= 0 ? d : std::max(0.0, -d - T);
            if (away < nearest) {
                nearest = away;
                wall = static_cast<int>(i);
                onto = move;
            }
        }
        if (wall < 0) {
            THROWM(Base::ValueError,
                   "A tab's sketch meets no wall of the sheet in its plane or parallel to it");
        }
        const Element& el = model.elements[wall];

        const auto hasher = getDocument()->getStringHasher();
        TopoShape tab = TopoShape(0, hasher).makEPrism(face.makETransform(onto),
                                                       gp_Vec(el.frame.Direction()) * T);
        // all of it the wall's: short of each bend from it and its limits
        for (const gp_Pnt& p : samples(tab.getShape())) {
            const int bend = pastBend(model, wall, p, Precision::Confusion());
            if (bend >= 0) {
                FC_THROWM(Base::ValueError,
                          "A tab on " << el.name << " runs past " << model.elements[bend].name);
            }
            if (model.contains(wall, p, Precision::Confusion())) {
                continue;
            }
            const int owner = model.owner(p, Precision::Confusion());
            if (owner >= 0) {
                FC_THROWM(Base::ValueError,
                          "A tab on " << el.name << " runs into " << model.elements[owner].name);
            }
            FC_THROWM(Base::ValueError, "A tab runs off " << el.name << " past its bends");
        }
        tab.fixSolidOrientation();
        AddSubShape.setValue(tab);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        TopoShape fused = makeBoolean(base, tab);
        if (fused.countSubShapes(TopAbs_SOLID) != 1) {
            THROWM(Base::ValueError, "A tab left the sheet in pieces");
        }
        Shape.setValue(getSolid(fused));
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
