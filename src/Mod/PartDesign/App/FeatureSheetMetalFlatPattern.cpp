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

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <gp_Trsf.hxx>

#include <App/Document.h>
#include <App/GeoFeature.h>
#include <App/GroupExtension.h>
#include <Base/Exception.h>

#include "FeatureSheetMetalFlatPattern.h"
#include "SolidBody.h"

using namespace PartDesign;
using Part::TopoShape;

const char* SheetMetalFlatPattern::OutOfDate = "Out of date: it is laid flat when shown";

PROPERTY_SOURCE(PartDesign::SheetMetalFlatPattern, Part::Feature)

SheetMetalFlatPattern::SheetMetalFlatPattern()
{
    ADD_PROPERTY_TYPE(Sheet,
                      (nullptr),
                      "Base",
                      static_cast<App::PropertyType>(App::Prop_Hidden | App::Prop_ReadOnly),
                      "The sheet's solid this is the flat of");
    ADD_PROPERTY_TYPE(Root,
                      (""),
                      "Flat",
                      App::Prop_None,
                      "The wall laid on XY, by its name (Wall1); the first wall if empty");
    ADD_PROPERTY_TYPE(SecondSkin,
                      (false),
                      "Flat",
                      App::Prop_None,
                      "The root wall's second skin on XY, not its first");
    ADD_PROPERTY_TYPE(AlignEdge,
                      (""),
                      "Flat",
                      App::Prop_None,
                      "An edge of the solid, by its element name, the flat is turned to run\n"
                      "along X by, where the feature laid flat has it");
    ADD_PROPERTY_TYPE(TangentLines,
                      (false),
                      "Flat",
                      App::Prop_None,
                      "Each bend's two tangent lines in the flat, where it meets its walls");
    ADD_PROPERTY_TYPE(Grain, (false), "Flat", App::Prop_None, "The grain arrow on the flat's top");
    ADD_PROPERTY_TYPE(GrainAngle,
                      (0.0),
                      "Flat",
                      App::Prop_None,
                      "The grain's direction, from the flat's X");
    ADD_PROPERTY_TYPE(Markings,
                      (nullptr),
                      "Flat",
                      App::Prop_None,
                      "Objects whose edges are drawn on the blank's top, the flat's only: a\n"
                      "sketch parallel to XY, read where one taken from the tip's flat lies");
    ADD_PROPERTY_TYPE(Notch,
                      (0L),
                      "Flat",
                      App::Prop_None,
                      "A notch where each bend's line meets the blank's outline, the flat's\n"
                      "only: None, V or Slot");
    static const char* notches[] = {"None", "V", "Slot", nullptr};
    Notch.setEnums(notches);
    ADD_PROPERTY_TYPE(NotchDepth,
                      (1.0),
                      "Flat",
                      App::Prop_None,
                      "How far a notch goes in along the bend's line");
    ADD_PROPERTY_TYPE(NotchWidth,
                      (1.0),
                      "Flat",
                      App::Prop_None,
                      "How wide a notch is at the outline");

    const auto out = static_cast<App::PropertyType>(App::Prop_ReadOnly | App::Prop_Output);
    ADD_PROPERTY_TYPE(FlatLength, (0.0), "Manufacturing", out, "The flat's size on XY, its longer side");
    ADD_PROPERTY_TYPE(FlatWidth, (0.0), "Manufacturing", out, "The flat's size on XY, its shorter side");
    ADD_PROPERTY_TYPE(FlatArea, (0.0), "Manufacturing", out, "The blank's area");
    ADD_PROPERTY_TYPE(Thickness, (0.0), "Manufacturing", out, "The sheet's thickness");
    ADD_PROPERTY_TYPE(MaterialName, (""), "Manufacturing", out, "The name of the rule's material card");
    ADD_PROPERTY_TYPE(BendCount, (0L), "Manufacturing", out, "How many bends the flat has");
    ADD_PROPERTY_TYPE(MaxBendAngle, (0.0), "Manufacturing", out, "The largest bend angle");
    ADD_PROPERTY_TYPE(BendDirections,
                      (),
                      "Manufacturing",
                      out,
                      "Each bend, Up or Down: Up where the material comes up toward one\n"
                      "looking down on the flat");
    ADD_PROPERTY_TYPE(Status,
                      (""),
                      "Manufacturing",
                      out,
                      "Empty when the shape is the solid's tip's flat; else why not: out of\n"
                      "date while hidden, or why the tip does not lie flat");
    // Nothing laid yet
    Status.setValue(OutOfDate);
}

FlatSettings SheetMetalFlatPattern::settings() const
{
    FlatSettings res;
    res.root = Root.getValue();
    res.secondSkin = SecondSkin.getValue();
    res.alignEdge = AlignEdge.getValue();
    res.tangentLines = TangentLines.getValue();
    res.grain = Grain.getValue();
    res.grainAngle = GrainAngle.getValue();
    return res;
}

SolidBody* SheetMetalFlatPattern::getSheet() const
{
    return Base::freecad_dynamic_cast<SolidBody>(Sheet.getValue());
}

SheetFlat SheetMetalFlatPattern::withMarkings(const SheetFlat& flat) const
{
    const auto& objs = Markings.getValues();
    if (objs.empty() || flat.result.solid.IsNull()) {
        return flat;
    }
    // where a sketch taken from the tip's flat lies: the tip's own frame
    // (SolidBody::flatSubObject()), as the cutout's WrapFrame Flat reads one
    auto solid = getSheet();
    auto tip = solid ? solid->Tip.getValue() : nullptr;
    const Base::Placement toFlat =
        tip ? App::GeoFeature::getGlobalPlacement(tip).inverse() : Base::Placement();
    Bnd_Box box;
    BRepBndLib::AddOptimal(flat.result.solid, box, /*useTriangulation*/ false,
                           /*useShapeTolerance*/ false);
    const double top = box.CornerMax().Z();

    SheetFlat res = flat;
    bool any = false;
    for (auto obj : objs) {
        if (!obj || !obj->isAttachedToDocument()) {
            continue;
        }
        Base::Matrix4D mat = (toFlat * App::GeoFeature::getGlobalPlacement(obj)).toMatrix();
        const TopoShape shape = Part::Feature::getTopoShape(obj, nullptr, false, &mat, nullptr,
                                                            /*resolveLink*/ true,
                                                            /*transform*/ false);
        if (shape.isNull() || shape.countSubShapes(TopAbs_EDGE) == 0) {
            continue;
        }
        Bnd_Box bound;
        BRepBndLib::AddOptimal(shape.getShape(), bound, false, false);
        if (bound.CornerMax().Z() - bound.CornerMin().Z() > 1e-6) {
            FC_THROWM(Base::ValueError,
                      "The markings of " << obj->Label.getValue()
                                         << " do not lie parallel to the flat's XY");
        }
        // onto the blank's top, where the flat's other markings are
        gp_Trsf up;
        up.SetTranslation(gp_Vec(0, 0, top - bound.CornerMin().Z()));
        const TopoShape moved = shape.makETransform(up);
        for (const TopoShape& edge : moved.getSubTopoShapes(TopAbs_EDGE)) {
            res.result.markings.push_back(TopoDS::Edge(edge.getShape()));
            any = true;
        }
    }
    if (!any) {
        return res;
    }
    // the flat's own compounds, then the markings' (flatShape())
    const auto parts = flat.shape.getSubTopoShapes();
    if (!parts.empty()) {
        res.shape = flatShape(parts.front(), res.result, TangentLines.getValue());
    }
    return res;
}

SheetFlat SheetMetalFlatPattern::withNotches(const SheetFlat& flat) const
{
    const auto tools =
        SheetMetal::notchTools(flat.result,
                               static_cast<SheetMetal::NotchKind>(Notch.getValue()),
                               NotchDepth.getValue(),
                               NotchWidth.getValue());
    const auto parts = flat.shape.getSubTopoShapes();
    if (tools.empty() || parts.empty()) {
        return flat;
    }
    const auto& hasher = flat.shape.Hasher;
    std::vector<TopoShape> cutters(tools.begin(), tools.end());
    TopoShape cut(0, hasher);
    cut.makECut({parts.front(), TopoShape(0, hasher).makECompound(cutters, nullptr, true)});
    if (cut.countSubShapes(TopAbs_SOLID) != 1) {
        THROWM(Base::ValueError, "The notches cut the blank apart");
    }
    const TopoShape blank = cut.getSubTopoShape(TopAbs_SOLID, 1);

    SheetFlat res = flat;
    res.result.solid = blank.getShape();
    // each line, and each tangent line, as far as it lies on the blank now
    for (auto& line : res.result.bendLines) {
        line.segments = SheetMetal::onBlank(res.result.solid, line.edge);
        for (std::size_t k = 0; k < line.tangents.size(); ++k) {
            line.tangentSegments[k] = SheetMetal::onBlank(res.result.solid, line.tangents[k]);
        }
    }
    res.shape = flatShape(blank, res.result, TangentLines.getValue());
    measureFlat(res);
    return res;
}

bool SheetMetalFlatPattern::isWanted() const
{
    if (Visibility.getValue()) {
        return true;
    }
    for (auto obj : getInList()) {
        // its body holds it; anything else uses it
        auto group = obj ? obj->getExtensionByType<App::GroupExtension>(true) : nullptr;
        if (obj && !(group && group->hasObject(this))) {
            return true;
        }
    }
    return false;
}

short SheetMetalFlatPattern::mustExecute() const
{
    if (Sheet.isTouched() || Root.isTouched() || SecondSkin.isTouched() || AlignEdge.isTouched()
        || TangentLines.isTouched() || Grain.isTouched() || GrainAngle.isTouched()
        || Markings.isTouched() || Notch.isTouched() || NotchDepth.isTouched()
        || NotchWidth.isTouched()) {
        return 1;
    }
    // Out of date and shown, or taken up by something that uses it since
    if (Status.getStrValue() == OutOfDate && isWanted()) {
        return 1;
    }
    return Part::Feature::mustExecute();
}

bool SheetMetalFlatPattern::skipRecompute()
{
    // Nothing it takes changed, but it is out of date and now wanted
    if (Status.getStrValue() == OutOfDate && isWanted()) {
        return false;
    }
    return Part::Feature::skipRecompute();
}

App::DocumentObjectExecReturn* SheetMetalFlatPattern::execute()
{
    auto solid = getSheet();
    if (!solid) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The flat has no sheet"));
    }
    // Nothing is laid flat unless asked for (ruled 2026-11-03): hidden and
    // unused, what was laid last stays
    if (!isWanted()) {
        Status.setValue(OutOfDate);
        return App::DocumentObject::StdReturn;
    }
    std::string why;
    try {
        const SheetFlat flat = finished(solid->flatOf(nullptr));
        Shape.setValue(flat.shape);
        FlatLength.setValue(flat.length);
        FlatWidth.setValue(flat.width);
        FlatArea.setValue(flat.area);
        Thickness.setValue(flat.thickness);
        MaterialName.setValue(flat.material);
        BendCount.setValue(flat.bendCount);
        MaxBendAngle.setValue(flat.maxBendAngle);
        BendDirections.setValues(flat.directions);
    }
    catch (const Base::Exception& e) {
        why = e.what();
    }
    catch (const Standard_Failure& e) {
        why = e.GetMessageString();
    }
    // A tip that does not lie flat is said, the last flat kept
    Status.setValue(why);
    return App::DocumentObject::StdReturn;
}

void SheetMetalFlatPattern::onChanged(const App::Property* prop)
{
    auto doc = getDocument();
    const bool editing = doc && !isRestoring() && !doc->testStatus(App::Document::Restoring)
        && !doc->isPerformingTransaction();
    if (prop == &Visibility) {
        // Shown out of date, or failed: laid again by the recompute it asks for
        if (editing && Visibility.getValue() && Status.getStrValue().size()) {
            touch();
        }
    }
    else if (prop == &Root || prop == &SecondSkin || prop == &AlignEdge || prop == &TangentLines
             || prop == &Grain || prop == &GrainAngle) {
        if (auto solid = getSheet()) {
            solid->flatSettingsChanged();
        }
    }
    Part::Feature::onChanged(prop);
}
