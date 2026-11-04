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

#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalConvert.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Recognise.h"

using namespace PartDesign;
using Part::TopoShape;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalConvert, PartDesign::Feature)

SheetMetalConvert::SheetMetalConvert()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(FixedFace,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A face of the part, a wall's skin: the wall the flat lies on");
    // the part is the body's base, which lies outside it
    FixedFace.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Kind,
                      (""),
                      "Sheet Metal",
                      static_cast<App::PropertyType>(App::Prop_Output | App::Prop_ReadOnly),
                      "What the part was taken for");
}

short SheetMetalConvert::mustExecute() const
{
    if (FixedFace.isTouched()) {
        return 1;
    }
    return PartDesign::Feature::mustExecute();
}

namespace
{

/// The face of base that is face: the same, else one lying where it lies
TopoDS_Face faceOf(const TopoShape& base, const TopoDS_Shape& face)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    const gp_Pnt centre = props.CentreOfMass();
    const double area = props.Mass();
    for (TopExp_Explorer it(base.getShape(), TopAbs_FACE); it.More(); it.Next()) {
        if (it.Current().IsPartner(face)) {
            return TopoDS::Face(it.Current());
        }
    }
    for (TopExp_Explorer it(base.getShape(), TopAbs_FACE); it.More(); it.Next()) {
        GProp_GProps p;
        BRepGProp::SurfaceProperties(it.Current(), p);
        if (std::abs(p.Mass() - area) <= 1e-6 * std::max(1.0, area)
            && p.CentreOfMass().Distance(centre) <= 1e-6) {
            return TopoDS::Face(it.Current());
        }
    }
    return {};
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalConvert::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    try {
        const TopoShape base = getBaseShape();
        const auto& subs = FixedFace.getSubValues();
        if (!FixedFace.getValue() || subs.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Convert needs a fixed face, a wall's skin"));
        }
        const TopoShape picked =
            Part::Feature::getTopoShape(FixedFace.getValue(), subs.front().c_str(), true);
        if (picked.isNull() || picked.getShape().ShapeType() != TopAbs_FACE) {
            THROWM(Base::ValueError, "The fixed face is not a face");
        }
        const TopoDS_Face fixed = faceOf(base, picked.getShape());
        if (fixed.IsNull()) {
            THROWM(Base::ValueError, "The fixed face is not a face of the part converted");
        }

        SheetMetal::Recognised found = SheetMetal::recognise(
            base.getShape(), fixed, [rule](double angle, double radius) {
                return rule->modelK(angle, radius);
            });
        const double T = rule->Thickness.getValue();
        if (std::abs(found.model.thickness - T) > 1e-6) {
            FC_THROWM(Base::ValueError,
                      "The part is " << found.model.thickness << " mm thick, the rule "
                                     << T << " mm: set the rule's thickness to the part's");
        }

        // split where each bend meets its walls, every face in one element
        TopoShape result = base;
        if (!found.cutters.empty()) {
            BRepAlgoAPI_Splitter split;
            TopTools_ListOfShape args, tools;
            args.Append(base.getShape());
            for (const TopoDS_Face& cutter : found.cutters) {
                tools.Append(cutter);
            }
            split.SetArguments(args);
            split.SetTools(tools);
            split.Build();
            if (!split.IsDone()) {
                THROWM(Base::CADKernelError, "The part could not be split where its bends meet "
                                             "its walls");
            }
            result = TopoShape(0, getDocument()->getStringHasher()).makEShape(split, {base});
            // The cutters cross the sheet whole and part it into pieces: fused
            // again, the faces between them go, the faces they split stay split
            const auto pieces = result.getSubTopoShapes(TopAbs_SOLID);
            if (pieces.size() > 1) {
                result = TopoShape(0, getDocument()->getStringHasher())
                             .makEBoolean(Part::OpCodes::Fuse, pieces);
            }
            result = getSolid(result);
        }
        // every face one element's
        int index = 0;
        for (TopExp_Explorer it(result.getShape(), TopAbs_FACE); it.More(); it.Next()) {
            ++index;
            if (found.model.owner(TopoDS::Face(it.Current())) < 0) {
                FC_THROWM(Base::ValueError, "Face" << index << " is in no wall or bend");
            }
        }
        Shape.setValue(result);
        SheetModel.setValue(found.model);
        if (Kind.getStrValue() != "Thin part") {
            Kind.setValue("Thin part");
        }
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}
