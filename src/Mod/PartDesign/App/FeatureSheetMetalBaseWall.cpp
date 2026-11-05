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

#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include <App/Document.h>
#include <Base/Exception.h>

#include "FeatureSheetMetalBaseWall.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"
#include "SheetMetal/Profile.h"

using namespace PartDesign;
using Part::TopoShape;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalBaseWall, PartDesign::ProfileBased)

const char* SheetMetalBaseWall::MaterialSideEnums[] = {"Outside", "Inside", "Middle", nullptr};
const char* SheetMetalBaseWall::ClosedProfileEnums[] = {"Plate", "Tube", nullptr};

SheetMetalBaseWall::SheetMetalBaseWall()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(MaterialSide,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Where the material is against the sketch: Outside, the sketch on the first "
                      "bend's inner skin (a plate: on the side of the sketch's normal); Inside, on "
                      "its outer skin (a plate: against the normal); Middle");
    MaterialSide.setEnums(MaterialSideEnums);
    ADD_PROPERTY_TYPE(Length,
                      (20.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "How far a profile wall goes along the sketch's normal");
    ADD_PROPERTY_TYPE(ClosedProfile,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "What a closed sketch makes: Plate, the sketch's face the rule's thickness "
                      "through; Tube, a wall along each line and a bend at every corner, which "
                      "lies flat once ripped");
    ClosedProfile.setEnums(ClosedProfileEnums);
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
    for (App::Property* prop : std::initializer_list<App::Property*> {&UpToFace,
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

short SheetMetalBaseWall::mustExecute() const
{
    if (MaterialSide.isTouched() || Length.isTouched() || ClosedProfile.isTouched()
        || UseRuleRadius.isTouched()
        || BendRadius.isTouched()) {
        return 1;
    }
    return ProfileBased::mustExecute();
}

void SheetMetalBaseWall::onChanged(const App::Property* prop)
{
    if (prop == &UseRuleRadius) {
        radiusSwitched(UseRuleRadius, BendRadius);
    }
    ProfileBased::onChanged(prop);
}

namespace
{

using SheetMetal::Element;

}  // namespace

App::DocumentObjectExecReturn* SheetMetalBaseWall::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    SheetMetal::Model model;
    int walls = 0, bends = 0;
    SheetMetal::ProfileBuilder build(model, walls, bends);
    build.k = [rule](double angle, double radius) {
        return rule->modelK(angle, radius);
    };
    build.thickness = rule->Thickness.getValue();
    build.radius = bendRadius(*rule, UseRuleRadius, BendRadius);
    if (build.radius <= 0.0) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The bend radius is not more than 0"));
    }
    if (build.thickness < Precision::Confusion()) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no thickness"));
    }
    try {
        model = modelBefore(build.thickness);
        model.thickness = build.thickness;
        for (const Element& e : model.elements) {
            ++(e.kind == Element::Kind::Wall ? walls : bends);
        }

        TopoShape base = getBaseShape(/*silent*/ true);
        auto invObjLoc = positionByPrevious();
        base.move(invObjLoc);
        const Base::Vector3d sketchNormal = getProfileNormal();
        gp_Vec n(sketchNormal.x, sketchNormal.y, sketchNormal.z);
        n.Transform(invObjLoc.Transformation());
        const gp_Dir normal(n);
        const int side = MaterialSide.getValue();

        TopoShape profile = getProfileShape();
        profile.move(invObjLoc);
        bool closed = profile.hasSubShape(TopAbs_WIRE);
        for (const auto& wire : profile.getSubTopoShapes(TopAbs_WIRE)) {
            closed = closed && BRep_Tool::IsClosed(wire.getShape());
        }
        const bool tube = closed && ClosedProfile.getValue() == 1;
        // a plate is the rule's thickness through the sketch's face: the
        // profile wall's values say nothing of it
        closed = closed && !tube;
        for (App::Property* prop : std::initializer_list<App::Property*> {&Length,
                                                                          &Reversed,
                                                                          &Midplane}) {
            prop->setStatus(App::Property::ReadOnly, closed);
        }

        const auto hasher = getDocument()->getStringHasher();
        TopoShape tool;
        if (closed) {
            // a plate: the sketch's face through the thickness
            TopoShape face = getVerifiedFace();
            face.move(invObjLoc);
            gp_Vec along = gp_Vec(normal) * build.thickness;
            gp_Pnt origin = BRep_Tool::Pnt(TopoDS::Vertex(face.getSubShape(TopAbs_VERTEX, 1)));
            gp_Dir into = normal;
            if (side == 1) {
                along.Reverse();
                into.Reverse();
            }
            else if (side == 2) {
                gp_Trsf half;
                half.SetTranslation(gp_Vec(normal) * (-build.thickness / 2));
                face = face.makETransform(half);
                origin.Transform(half);
            }
            model.addWall(build.nextWall(), gp_Ax3(origin, into));
            tool = TopoShape(0, hasher).makEPrism(face, along);
        }
        else {
            auto wires = profile.getSubTopoShapes(TopAbs_WIRE);
            if (wires.empty()) {
                wires = profile.makEWires().getSubTopoShapes(TopAbs_WIRE);
            }
            if (wires.size() != 1) {
                return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                    "Exception",
                    tube ? "A tube takes one closed line chain"
                         : "A profile wall takes one open line chain, or closed profiles"));
            }
            auto points = SheetMetal::linePoints(TopoDS::Wire(wires.front().getShape()),
                                                 tube ? "A tube" : "A profile wall");
            if (tube) {
                build.closedProfile(points, normal, side);
            }
            else {
                SheetMetal::dropStraight(points);
                build.profile(points, normal, side);
            }

            const double length = Length.getValue();
            if (length < Precision::Confusion()) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Length too small"));
            }
            gp_Vec along = gp_Vec(normal) * (Reversed.getValue() ? -length : length);
            gp_Trsf back;
            if (Midplane.getValue()) {
                back.SetTranslation(along * -0.5);
            }
            std::vector<TopoShape> pieces;
            for (auto& face : build.faces) {
                if (Midplane.getValue()) {
                    face = face.makETransform(back);
                }
                pieces.push_back(TopoShape(0, hasher).makEPrism(face, along));
            }
            if (Midplane.getValue()) {
                model.transform(back);
            }
            if (pieces.size() == 1) {
                tool = pieces.front();
            }
            else {
                tool = TopoShape(0, hasher).makEFuse(pieces);
                tool = getSolid(tool);
            }
        }
        if (tool.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "The sheet came out empty"));
        }

        // oriented, so the join need not check its result
        tool.fixSolidOrientation();
        SheetMetal::storePCurves(tool);
        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        Shape.setValue(makeBoolean(base, tool, true));
        applyBendOverrides(model);
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
