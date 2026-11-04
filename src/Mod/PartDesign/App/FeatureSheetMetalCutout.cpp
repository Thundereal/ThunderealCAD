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

#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "Body.h"
#include "FeatureSheetMetalCutout.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"
#include "SheetMetal/Refold.h"
#include "SheetMetalFlat.h"
#include "SolidBody.h"

using namespace PartDesign;
using Part::TopoShape;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalCutout, PartDesign::ProfileBased)

SheetMetalCutout::SheetMetalCutout()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(FeatureAddSub::Subtractive);
    ADD_PROPERTY_TYPE(UpToFirst,
                      (false),
                      "Sheet Metal",
                      App::Prop_None,
                      "Only the first wall or bend the sketch meets, not every one in its way");
    ADD_PROPERTY_TYPE(Wrap,
                      (false),
                      "Sheet Metal",
                      App::Prop_None,
                      "The sketch as if drawn on the flat: the wall parallel to it and nearest "
                      "it stays, and what lies on the flat is cut and folded back");
    ADD_PROPERTY_TYPE(WrapFrame,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Where a wrap's sketch is read: Folded, as if the flat lay on from the "
                      "wall nearest it where it stands; Flat, in the frame of the feature's flat "
                      "as the sheet lays it, where geometry taken from a flat is");
    static const char* frames[] = {"Folded", "Flat", nullptr};
    WrapFrame.setEnums(frames);
    WrapFrame.setStatus(App::Property::ReadOnly, true);
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

short SheetMetalCutout::mustExecute() const
{
    if (UpToFirst.isTouched() || Wrap.isTouched() || WrapFrame.isTouched()) {
        return 1;
    }
    return ProfileBased::mustExecute();
}

void SheetMetalCutout::onChanged(const App::Property* prop)
{
    if (prop == &Wrap) {
        // a wrap is cut through the flat: no way, no first only
        Reversed.setStatus(App::Property::ReadOnly, Wrap.getValue());
        UpToFirst.setStatus(App::Property::ReadOnly, Wrap.getValue());
        WrapFrame.setStatus(App::Property::ReadOnly, !Wrap.getValue());
    }
    ProfileBased::onChanged(prop);
}

App::DocumentObjectExecReturn* SheetMetalCutout::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    try {
        TopoShape base = getBaseShape();
        auto invObjLoc = positionByPrevious();
        base.move(invObjLoc);
        const SheetMetal::Model model = modelBefore(rule->Thickness.getValue());
        if (model.elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A cutout builds on a sheet metal feature"));
        }
        TopoShape face = getVerifiedFace();
        face.move(invObjLoc);
        const Base::Vector3d sketchNormal = getProfileNormal();
        gp_Vec n(sketchNormal.x, sketchNormal.y, sketchNormal.z);
        n.Transform(invObjLoc.Transformation());
        // against the sketch's normal, as a pocket's
        const gp_Dir way = Reversed.getValue() ? gp_Dir(n) : gp_Dir(n.Reversed());

        std::vector<TopoShape> tools;
        if (Wrap.getValue() && WrapFrame.getValue() == 1) {
            // the flat as the sheet lays it: its root, second skin, the
            // edge it is turned by, as the feature before's flat is
            FlatSettings settings;
            auto body = Body::findBodyOf(this);
            if (auto solid = body ? body->getSolidBody(this) : nullptr) {
                settings = solid->flatSettings();
            }
            int root = -1;
            if (!settings.root.empty()) {
                root = model.find(settings.root);
                if (root < 0 || model.elements[root].kind != SheetMetal::Element::Kind::Wall) {
                    FC_THROWM(Base::ValueError,
                              "The sheet has no wall " << settings.root << " to lay on");
                }
            }
            for (std::size_t i = 0; i < model.elements.size() && root < 0; ++i) {
                if (model.elements[i].kind == SheetMetal::Element::Kind::Wall) {
                    root = static_cast<int>(i);
                }
            }
            TopoDS_Edge alongX;
            if (!settings.alignEdge.empty()) {
                const TopoShape edge =
                    base.getSubTopoShape(settings.alignEdge.c_str(), /*silent*/ true);
                if (!edge.isNull() && edge.getShape().ShapeType() == TopAbs_EDGE) {
                    alongX = TopoDS::Edge(edge.getShape());
                }
            }
            tools = SheetMetal::wrapCutoutToolsFlat(model,
                                                    base.getShape(),
                                                    face.getShape(),
                                                    gp_Dir(n),
                                                    root,
                                                    settings.secondSkin,
                                                    alongX);
        }
        else {
            tools = Wrap.getValue()
                ? SheetMetal::wrapCutoutTools(model, base.getShape(), face.getShape(), gp_Dir(n))
                : SheetMetal::cutoutTools(model,
                                          base.getShape(),
                                          face.getShape(),
                                          way,
                                          UpToFirst.getValue());
        }
        TopoShape tool = TopoShape().makECompound(tools);
        SheetMetal::storePCurves(tool);
        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        TopoShape result = getSolid(makeBoolean(base, tool));
        if (result.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A cutout leaves nothing of the sheet"));
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
