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

#include <limits>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include <App/Document.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalRule.h"
#include "FeatureSheetMetalTabSlot.h"
#include "SheetMetal/Pieces.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::Element;
using SheetMetal::Model;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalLocatingTabs, PartDesign::Feature)
PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalSlots, PartDesign::Feature)

namespace
{

const App::PropertyIntegerConstraint::Constraints countRange = {1, 1000, 1};

double volumeOf(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

/// Where an object's body is in the document, or nowhere moved
Base::Placement bodyPlacement(const App::DocumentObject* obj)
{
    auto group = App::GeoFeatureGroupExtension::getGroupOfObject(obj);
    auto feature = Base::freecad_dynamic_cast<App::GeoFeature>(group);
    return feature ? feature->globalPlacement() : Base::Placement();
}

}  // namespace

SheetMetalLocatingTabs::SheetMetalLocatingTabs()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(Face,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A wall's end face, across its thickness, the tabs go on");
    ADD_PROPERTY_TYPE(Count, (2), "Sheet Metal", App::Prop_None, "How many tabs");
    Count.setConstraints(&countRange);
    ADD_PROPERTY_TYPE(Width, (6.0), "Sheet Metal", App::Prop_None, "Each tab along the end face");
    ADD_PROPERTY_TYPE(Length,
                      (1.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "How far out of the end face each tab goes: the slotted part's thickness, "
                      "or more");
    ADD_PROPERTY_TYPE(Margin,
                      (3.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "The tabs kept this clear of the end face's ends");
    ADD_PROPERTY_TYPE(TabShape,
                      (TopoShape()),
                      "Sheet Metal",
                      App::PropertyType(App::Prop_Output | App::Prop_Hidden),
                      "The tabs alone, for slots");
}

short SheetMetalLocatingTabs::mustExecute() const
{
    if (Face.isTouched() || Count.isTouched() || Width.isTouched() || Length.isTouched()
        || Margin.isTouched()) {
        return 1;
    }
    return PartDesign::Feature::mustExecute();
}

void SheetMetalLocatingTabs::onChanged(const App::Property* prop)
{
    // the sizes start from the sheet's thickness, once, when the feature
    // takes its rule, as a form's do; any set by then are kept
    if (isRestoring()) {
        sized = true;
    }
    else if (prop == &Rule && !sized) {
        if (auto rule = getRule()) {
            sized = true;
            const double T = rule->Thickness.getValue();
            const std::pair<App::PropertyLength*, double> sizes[] = {
                {&Width, 6},
                {&Length, 1},
                {&Margin, 3}};
            for (const auto& [p, k] : sizes) {
                if (p->getValue() == k) {
                    p->setValue(k * T);
                }
            }
        }
    }
    // the face is of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Face.getValue()
        && Face.getValue() != BaseFeature.getValue()) {
        auto subs = Face.getSubValues(false);
        auto shadows = Face.getShadowSubs();
        Face.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    PartDesign::Feature::onChanged(prop);
}

App::DocumentObjectExecReturn* SheetMetalLocatingTabs::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    try {
        Model model = modelBefore(rule->Thickness.getValue());
        if (model.elements.empty()) {
            THROWM(Base::ValueError, "Tabs build on a sheet metal feature");
        }
        const double T = model.thickness;
        const TopoShape base = getBaseShape();

        const auto shadows = Face.getShadowSubs();
        if (shadows.size() != 1) {
            THROWM(Base::ValueError, "Tabs take one face: a wall's end");
        }
        const std::string& ref =
            shadows.front().first.empty() ? shadows.front().second : shadows.front().first;
        const TopoDS_Shape sub = base.getSubShape(ref.c_str(), true);
        if (sub.IsNull() || sub.ShapeType() != TopAbs_FACE) {
            FC_THROWM(Base::ValueError, "No face " << shadows.front().second);
        }
        const TopoDS_Face face = TopoDS::Face(sub);
        const int wall = model.owner(face);
        BRepAdaptor_Surface surface(face);
        gp_Dir out;
        gp_Dir across;
        bool end = wall >= 0 && model.elements[wall].kind == Element::Kind::Wall
            && surface.GetType() == GeomAbs_Plane;
        if (end) {
            out = surface.Plane().Axis().Direction();
            if (face.Orientation() == TopAbs_REVERSED) {
                out.Reverse();
            }
            across = model.elements[wall].frame.Direction();
            end = std::abs(out.Dot(across)) < Precision::Angular();
        }
        if (!end) {
            FC_THROWM(Base::ValueError,
                      "Face " << shadows.front().second
                              << " is no wall's end: tabs go on the end of a wall, across its "
                                 "thickness");
        }
        const gp_Dir along(gp_Vec(across).Crossed(gp_Vec(out)));

        // the end face's run along it and across the sheet
        const TopExp_Explorer first(face, TopAbs_VERTEX);
        const gp_Pnt o = BRep_Tool::Pnt(TopoDS::Vertex(first.Current()));
        double s0 = std::numeric_limits<double>::max(), s1 = -s0, z0 = s0, z1 = -s0;
        for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
            const gp_Vec v(o, BRep_Tool::Pnt(TopoDS::Vertex(it.Current())));
            s0 = std::min(s0, v.Dot(gp_Vec(along)));
            s1 = std::max(s1, v.Dot(gp_Vec(along)));
            z0 = std::min(z0, v.Dot(gp_Vec(across)));
            z1 = std::max(z1, v.Dot(gp_Vec(across)));
        }
        if (std::abs(z1 - z0 - T) > 1e-4) {
            FC_THROWM(Base::ValueError,
                      "Face " << shadows.front().second
                              << " is not across the wall's thickness: tabs go on a wall's end");
        }

        const int count = Count.getValue();
        const double w = Width.getValue(), len = Length.getValue(), margin = Margin.getValue();
        if (count < 1 || w <= 0 || len <= 0) {
            THROWM(Base::ValueError, "Tabs' count, width and length are above 0");
        }
        const double room = s1 - s0 - 2 * margin;
        if (count * w > room + Precision::Confusion()) {
            FC_THROWM(Base::ValueError,
                      count << " tabs " << w << " mm wide do not fit in the " << room
                            << " mm between the end face's margins");
        }
        BRep_Builder builder;
        TopoDS_Compound tabs;
        builder.MakeCompound(tabs);
        std::vector<TopoShape> fuse {base};
        for (int i = 0; i < count; ++i) {
            // their middles evenly apart, the outer ones at the margins; one
            // in the middle
            const double c =
                count == 1 ? (s0 + s1) / 2 : s0 + margin + w / 2 + i * (room - w) / (count - 1);
            auto at = [&](double s, double z) {
                return o.Translated(gp_Vec(along) * s + gp_Vec(across) * z);
            };
            BRepBuilderAPI_MakePolygon outline(at(c - w / 2, z0),
                                               at(c + w / 2, z0),
                                               at(c + w / 2, z1),
                                               at(c - w / 2, z1),
                                               true);
            const TopoDS_Shape tab =
                BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(outline.Wire(), true).Face(),
                                      gp_Vec(out) * len)
                    .Shape();
            // the wall's: in its slab, short of its limits
            const gp_Pnt middle = at(c, (z0 + z1) / 2).Translated(gp_Vec(out) * (len / 2));
            if (!model.contains(wall, middle, 1e-6)) {
                FC_THROWM(Base::ValueError,
                          "Tab " << i + 1 << " runs off " << model.elements[wall].name);
            }
            builder.Add(tabs, tab);
            // named, and this feature's
            TopoShape piece(tab);
            SheetMetal::nameRest(piece, "Tab" + std::to_string(i + 1));
            piece.Tag = -getID();
            fuse.push_back(piece);
        }
        const auto hasher = getDocument()->getStringHasher();
        TopoShape res(0, hasher);
        res.makEBoolean(Part::OpCodes::Fuse, fuse);
        if (res.countSubShapes(TopAbs_SOLID) != 1) {
            THROWM(Base::ValueError, "The tabs left the sheet in pieces");
        }
        TabShape.setValue(TopoShape(tabs));
        Shape.setValue(getSolid(res));
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

SheetMetalSlots::SheetMetalSlots()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(Tabs,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "The Locating Tabs, in this body or another, the slots are for");
    Tabs.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Clearance,
                      (0.1),
                      "Sheet Metal",
                      App::Prop_None,
                      "Each slot this much larger than its tab all round");
    ADD_PROPERTY_TYPE(TabsBody,
                      (nullptr),
                      "Sheet Metal",
                      App::PropertyType(App::Prop_Hidden | App::Prop_ReadOnly),
                      "The tabs' body, followed as Tabs is");
    TabsBody.setScope(App::LinkScope::Global);
}

short SheetMetalSlots::mustExecute() const
{
    if (Tabs.isTouched() || Clearance.isTouched() || TabsBody.isTouched()) {
        return 1;
    }
    return PartDesign::Feature::mustExecute();
}

void SheetMetalSlots::onChanged(const App::Property* prop)
{
    // the tabs' body, another than this one's
    if (prop == &Tabs && !isRestoring()) {
        auto body = App::GeoFeatureGroupExtension::getGroupOfObject(Tabs.getValue());
        TabsBody.setValue(body && body != App::GeoFeatureGroupExtension::getGroupOfObject(this)
                              ? body
                              : nullptr);
    }
    PartDesign::Feature::onChanged(prop);
}

App::DocumentObjectExecReturn* SheetMetalSlots::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    try {
        Model model = modelBefore(rule->Thickness.getValue());
        if (model.elements.empty()) {
            THROWM(Base::ValueError, "Slots build on a sheet metal feature");
        }
        const TopoShape base = getBaseShape();
        auto tabs = Base::freecad_dynamic_cast<SheetMetalLocatingTabs>(Tabs.getValue());
        if (!tabs) {
            THROWM(Base::ValueError, "Slots take the Locating Tabs they are for: Tabs");
        }
        TopoShape shape(tabs->TabShape.getShape());
        if (shape.isNull()) {
            FC_THROWM(Base::ValueError, tabs->getFullLabel() << " has no tabs");
        }
        // the tabs where they are in this feature's frame
        const Base::Placement move = bodyPlacement(this).inverse() * bodyPlacement(tabs);
        shape.transformShape(move.toMatrix(), true);
        // each grown by the clearance all round, square
        const double clearance = Clearance.getValue();
        // each named by its place among them, and this feature's
        std::vector<TopoShape> grown;
        auto slot = [&](const TopoDS_Shape& solid) {
            TopoShape piece(solid);
            SheetMetal::nameRest(piece, "Slot" + std::to_string(grown.size() + 1));
            grown.push_back(piece);
        };
        for (TopExp_Explorer it(shape.getShape(), TopAbs_SOLID); it.More(); it.Next()) {
            if (clearance <= Precision::Confusion()) {
                slot(it.Current());
                continue;
            }
            BRepOffsetAPI_MakeOffsetShape offset;
            offset.PerformByJoin(it.Current(),
                                 clearance,
                                 1e-7,
                                 BRepOffset_Skin,
                                 false,
                                 false,
                                 GeomAbs_Intersection);
            if (!offset.IsDone()) {
                THROWM(Base::CADKernelError, "A slot could not be grown by the clearance");
            }
            slot(offset.Shape());
        }
        const auto hasher = getDocument()->getStringHasher();
        TopoShape res(0, hasher);
        res.makEBoolean(Part::OpCodes::Cut, {base, SheetMetal::toolOf(getID(), grown)});
        if (res.countSubShapes(TopAbs_SOLID) != 1) {
            THROWM(Base::ValueError, "The slots left the sheet in pieces");
        }
        res = getSolid(res);
        if (volumeOf(res.getShape()) > volumeOf(base.getShape()) - 1e-9) {
            FC_THROWM(Base::ValueError,
                      "The tabs of " << tabs->getFullLabel() << " miss the part: no slot to cut");
        }
        Shape.setValue(res);
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
