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
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Base/Tools.h>

#include "FeatureSheetMetalCrossBreak.h"
#include "FeatureSheetMetalRule.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::Element;
using SheetMetal::Model;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalCrossBreak, PartDesign::Feature)

SheetMetalCrossBreak::SheetMetalCrossBreak()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(Faces,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A face of each wall to break: the break rises out of it");
    ADD_PROPERTY_TYPE(Angle,
                      (3.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "The angle each diagonal bends by, above 0 and up to 45 deg");
    ADD_PROPERTY_TYPE(Reversed,
                      (false),
                      "Sheet Metal",
                      App::Prop_None,
                      "The break rises into the face picked, not out of it");
}

short SheetMetalCrossBreak::mustExecute() const
{
    if (Faces.isTouched() || Angle.isTouched() || Reversed.isTouched()) {
        return 1;
    }
    return PartDesign::Feature::mustExecute();
}

void SheetMetalCrossBreak::onChanged(const App::Property* prop)
{
    // The faces are of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Faces.getValue()
        && Faces.getValue() != BaseFeature.getValue()) {
        auto subs = Faces.getSubValues(false);
        auto shadows = Faces.getShadowSubs();
        Faces.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    PartDesign::Feature::onChanged(prop);
}

App::DocumentObjectExecReturn* SheetMetalCrossBreak::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    try {
        Model model = modelBefore(rule->Thickness.getValue());
        if (model.elements.empty()) {
            THROWM(Base::ValueError, "A cross break builds on a sheet metal feature");
        }
        const double angle = Angle.getValue();
        if (angle <= 0 || angle > 45) {
            FC_THROWM(Base::ValueError,
                      "A cross break's angle is above 0 and up to 45 deg, not " << angle);
        }
        const TopoShape base = getBaseShape();
        const auto shadows = Faces.getShadowSubs();
        if (shadows.empty()) {
            THROWM(Base::ValueError, "A cross break takes a face of each wall to break");
        }
        for (const auto& shadow : shadows) {
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            const TopoDS_Shape sub = base.getSubShape(ref.c_str(), true);
            if (sub.IsNull() || sub.ShapeType() != TopAbs_FACE) {
                FC_THROWM(Base::ValueError, "No face " << shadow.second);
            }
            const TopoDS_Face& face = TopoDS::Face(sub);
            const int owner = model.owner(face);
            if (owner < 0) {
                FC_THROWM(Base::ValueError, "Face " << shadow.second << " is in no wall");
            }
            Element& wall = model.elements[owner];
            BRepAdaptor_Surface surface(face);
            if (wall.kind != Element::Kind::Wall || surface.GetType() != GeomAbs_Plane) {
                FC_THROWM(Base::ValueError,
                          "Face " << shadow.second << " is of " << wall.name
                                  << ": a cross break goes on a wall's skin");
            }
            // rising out of the face, along its outer normal
            gp_Dir normal = surface.Plane().Axis().Direction();
            if (face.Orientation() == TopAbs_REVERSED) {
                normal.Reverse();
            }
            double rise = Base::toRadians(angle);
            if ((normal.Dot(wall.frame.Direction()) < 0) != Reversed.getValue()) {
                rise = -rise;
            }
            wall.crossBreak = rise;
        }
        // the solid as it was
        Shape.setValue(base);
        storeModel(model);
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}
