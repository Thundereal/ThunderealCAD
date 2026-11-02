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

#ifndef _PreComp_
#include <BRepBuilderAPI_Sewing.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#endif

#include <App/Document.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureThicken.h"

using namespace PartDesign;
using Part::TopoShape;

PROPERTY_SOURCE(PartDesign::Thicken, PartDesign::FeatureAddSub)

namespace
{
const char* JoinEnums[] = {"Arc", "Intersection", nullptr};
}

Thicken::Thicken()
{
    ADD_PROPERTY_TYPE(Faces,
                      (nullptr),
                      "Thicken",
                      App::Prop_None,
                      "The faces made into a solid: of a surface, or of a solid; objects given\n"
                      "whole give all their faces. Several are sewn into one skin");
    ADD_PROPERTY_TYPE(Value, (1.0), "Thicken", App::Prop_None, "The thickness");
    ADD_PROPERTY_TYPE(Reversed,
                      (false),
                      "Thicken",
                      App::Prop_None,
                      "Thicken against the faces' normals");
    ADD_PROPERTY_TYPE(Midplane,
                      (false),
                      "Thicken",
                      App::Prop_None,
                      "Half the thickness on each side of the faces");
    ADD_PROPERTY_TYPE(Join,
                      ((long)0),
                      "Thicken",
                      App::Prop_None,
                      "How the offsets of faces meeting at an edge are joined: round it, or\n"
                      "by running on until they meet");
    Join.setEnums(JoinEnums);
    initAddSubType(Additive);
}

short Thicken::mustExecute() const
{
    if (Faces.isTouched() || Value.isTouched() || Reversed.isTouched() || Midplane.isTouched()
        || Join.isTouched()) {
        return 1;
    }
    return FeatureAddSub::mustExecute();
}

App::DocumentObjectExecReturn* Thicken::execute()
{
    try {
        auto hasher = getDocument()->getStringHasher();
        std::vector<TopoShape> faces;
        const auto& objs = Faces.getValues();
        const auto subs = Faces.getSubValues();
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            TopoShape shape = Part::Feature::getTopoShape(objs[i], subs[i].c_str(), !subs[i].empty());
            if (shape.isNull()) {
                return new App::DocumentObjectExecReturn(
                    std::string("Nothing to thicken in ") + objs[i]->getNameInDocument());
            }
            auto found = shape.getSubTopoShapes(TopAbs_FACE);
            if (found.empty()) {
                return new App::DocumentObjectExecReturn(
                    std::string("No face to thicken in ") + objs[i]->getNameInDocument() + "."
                    + subs[i]);
            }
            faces.insert(faces.end(), found.begin(), found.end());
        }
        if (faces.empty()) {
            return new App::DocumentObjectExecReturn("No faces to thicken");
        }
        const double tol = Precision::Confusion();
        if (Value.getValue() <= 2 * tol) {
            return new App::DocumentObjectExecReturn("The thickness is too small");
        }

        // one skin: a face, or the faces sewn
        TopoShape skin(0, hasher);
        if (faces.size() == 1) {
            skin = faces.front();
        }
        else {
            BRepBuilderAPI_Sewing sewer(tol);
            for (const auto& f : faces) {
                sewer.Add(f.getShape());
            }
            sewer.Perform();
            skin.makEShape(sewer, faces, Part::OpCodes::Sewing);
            // one shell of all of them, or they do not meet
            if (skin.countSubShapes(TopAbs_SHELL) != 1
                || skin.getSubTopoShape(TopAbs_SHELL, 1).countSubShapes(TopAbs_FACE)
                    != (int)faces.size()) {
                return new App::DocumentObjectExecReturn(
                    "The faces to thicken do not make one piece: they have to meet at their edges");
            }
            skin = skin.getSubTopoShape(TopAbs_SHELL, 1);
        }

        const auto join =
            Join.getValue() == 1 ? TopoShape::JoinType::Intersection : TopoShape::JoinType::Arc;
        double thickness = Reversed.getValue() ? -Value.getValue() : Value.getValue();
        if (Midplane.getValue()) {
            // from its offset half a thickness back
            skin = skin.makEOffset(-thickness / 2, tol, false, false, 0, join);
        }
        TopoShape tool(0, hasher);
        tool.makEThicken(skin, thickness, tol, join);
        tool = getSolid(tool);
        if (tool.isNull()) {
            return new App::DocumentObjectExecReturn("Thickening made no solid");
        }
        AddSubShape.setValue(tool);

        TopoShape base = getBaseShape(/*silent*/ true);
        Shape.setValue(makeBoolean(base, tool));
        return App::DocumentObject::StdReturn;
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    catch (Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
}
