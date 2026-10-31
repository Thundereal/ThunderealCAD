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

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>
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
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
}

short SheetMetalExtendWall::mustExecute() const
{
    if (Faces.isTouched() || Length.isTouched() || UpToFace.isTouched()) {
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
            TopoShape prism = TopoShape(0, hasher).makEPrism(face, gp_Vec(out) * distance);
            (distance > 0 ? pieces : cuts).push_back(prism);
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
        Shape.setValue(tool.isNull() ? base : makeBoolean(base, tool));
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
