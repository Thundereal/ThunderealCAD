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
#include <numbers>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Base/Tools.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalForm.h"
#include "FeatureSheetMetalRule.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::Element;
using SheetMetal::Form;
using SheetMetal::Model;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalForm, PartDesign::Feature)

const char* SheetMetalForm::KindEnums[] = {"Tool", "Dimple", nullptr};

namespace
{

const App::PropertyQuantityConstraint::Constraints flankRange = {1.0, 90.0, 1.0};

/// A face's outer normal
gp_Dir outerNormal(const TopoDS_Face& face)
{
    gp_Dir normal = BRepAdaptor_Surface(face).Plane().Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    return normal;
}

/// d projected onto the plane of normal n, or none if it is along it
bool across(const gp_Vec& d, const gp_Dir& n, gp_Dir& res)
{
    const gp_Vec v = d - gp_Vec(n) * d.Dot(gp_Vec(n));
    if (v.Magnitude() < Precision::Confusion()) {
        return false;
    }
    res = gp_Dir(v);
    return true;
}

/// What a form adds and what it cuts, and what it takes of the part
struct Pressed
{
    TopoDS_Shape outside;
    std::vector<TopoDS_Shape> cuts;
    Form form;
};

/// The planar faces of s lying on the plane through frame's origin, normal
/// to its Z
std::vector<TopoDS_Face> onBase(const TopoDS_Shape& s, const gp_Ax3& frame)
{
    std::vector<TopoDS_Face> res;
    for (TopExp_Explorer it(s, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face& face = TopoDS::Face(it.Current());
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) {
            continue;
        }
        const gp_Pln plane = surface.Plane();
        if (!plane.Axis().Direction().IsParallel(frame.Direction(), Precision::Angular())
            || std::abs(gp_Vec(frame.Location(), plane.Location()).Dot(gp_Vec(frame.Direction())))
                > Precision::Confusion()) {
            continue;
        }
        res.push_back(face);
    }
    return res;
}

/// A user's tool: its shape set with its base face on the frame's XY plane,
/// rising along Z, taken as the form's outside and hollowed by T, the hollow
/// opened down through the wall
Pressed pressTool(const TopoDS_Shape& tool,
                  const TopoDS_Face& base,
                  const gp_Dir& toolX,
                  const gp_Ax3& frame,
                  double T)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(base, props);
    const gp_Dir into = outerNormal(base).Reversed();
    gp_Dir x;
    if (!across(gp_Vec(toolX), into, x)) {
        x = BRepAdaptor_Surface(base).Plane().Position().XDirection();
    }
    gp_Trsf move;
    move.SetDisplacement(gp_Ax3(props.CentreOfMass(), into, x), frame);
    const TopLoc_Location loc(move);
    const TopoDS_Shape outside = tool.Moved(loc);
    TopTools_ListOfShape open;
    open.Append(base.Moved(loc));
    BRepOffsetAPI_MakeThickSolid thick;
    thick.MakeThickSolidByJoin(outside, open, -T, 1e-6);
    thick.Build();
    if (!thick.IsDone()) {
        THROWM(Base::CADKernelError, "The tool could not be hollowed to the sheet's thickness");
    }
    BRepAlgoAPI_Cut hollow(outside, thick.Shape());
    hollow.Build();
    if (!hollow.IsDone()) {
        THROWM(Base::CADKernelError, "The tool's hollow could not be made");
    }
    Pressed res;
    res.outside = outside;
    res.cuts.push_back(hollow.Shape());
    // the hollow opened through the wall
    for (const TopoDS_Face& face : onBase(hollow.Shape(), frame)) {
        res.cuts.push_back(
            BRepPrimAPI_MakePrism(face, gp_Vec(frame.Direction()) * (-2 * T)).Shape());
    }
    // what it takes of the part: its box in the frame, down through the wall
    gp_Trsf toFrame;
    toFrame.SetTransformation(frame);
    Bnd_Box box;
    BRepBndLib::AddOptimal(BRepBuilderAPI_Transform(outside, toFrame, true).Shape(), box,
                           false, false);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    res.form.halfLength = std::max(std::abs(x0), std::abs(x1));
    res.form.halfWidth = std::max(std::abs(y0), std::abs(y1));
    res.form.zMin = -T;
    res.form.zMax = z1;
    return res;
}

/// A dimple: two cones the thickness apart, the outside's from where it
/// leaves the face, the hollow's opened through the wall; its bends rounded
/// afterwards (roundDimple())
struct Dimple
{
    double r0;      // where the outside leaves the face
    double h;       // its top above the face
    double phi;     // the slant, rad
    double rho;     // the bends' inside radius
    double T;

    double outside(double z) const
    {
        return r0 - z / std::tan(phi);
    }
    double inside(double z) const
    {
        return outside(z) - T / std::sin(phi);
    }
};

TopoDS_Shape cone(const gp_Ax3& frame, double z0, double r0, double z1, double r1)
{
    const gp_Ax2 at(frame.Location().Translated(gp_Vec(frame.Direction()) * z0),
                    frame.Direction(),
                    frame.XDirection());
    if (std::abs(r0 - r1) < Precision::Confusion()) {
        return BRepPrimAPI_MakeCylinder(at, r0, z1 - z0).Shape();
    }
    return BRepPrimAPI_MakeCone(at, r0, r1, z1 - z0).Shape();
}

Pressed pressDimple(const Dimple& d, const gp_Ax3& frame)
{
    if (d.r0 <= 0 || d.h <= 0) {
        THROWM(Base::ValueError, "A dimple's diameter and depth are above 0");
    }
    if (d.outside(d.h) <= Precision::Confusion()) {
        FC_THROWM(Base::ValueError,
                  "A dimple " << 2 * d.r0 << " mm across cannot rise " << d.h
                              << " mm at its slant: it closes to a point first");
    }
    if (d.inside(d.h - d.T) <= Precision::Confusion()) {
        THROWM(Base::ValueError, "A dimple that narrow has no room inside for the thickness");
    }
    Pressed res;
    res.outside = cone(frame, -d.T, d.outside(-d.T), d.h, d.outside(d.h));
    res.cuts.push_back(
        cone(frame, -2 * d.T, d.inside(-2 * d.T), d.h - d.T, d.inside(d.h - d.T)));
    // as far as its rim's bend reaches on either skin: the two are
    // concentric, so both end rho tan(phi / 2) past where it leaves the face
    res.form.halfLength = d.r0 + d.rho * std::tan(d.phi / 2) + 10 * Precision::Confusion();
    res.form.halfWidth = 0;
    res.form.zMin = -d.T;
    res.form.zMax = d.h;
    return res;
}

/// The dimple's four bends rounded, each a circle about its axis -- the rim
/// and the top, outside and in -- added to fillet; how many
int roundDimple(BRepFilletAPI_MakeFillet& fillet,
                const TopoDS_Shape& shape,
                const Dimple& d,
                const gp_Ax3& frame)
{
    if (d.rho <= 0) {
        return 0;
    }
    const struct
    {
        double z, r, radius;
    } bends[] = {
        {0, d.outside(0), d.rho},                 // the rim, on the face
        {-d.T, d.inside(-d.T), d.rho + d.T},      // the rim, beneath
        {d.h, d.outside(d.h), d.rho + d.T},       // the top, outside
        {d.h - d.T, d.inside(d.h - d.T), d.rho},  // the top, inside
    };
    int count = 0;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
        BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
        if (c.GetType() != GeomAbs_Circle) {
            continue;
        }
        const gp_Circ circ = c.Circle();
        if (!circ.Axis().Direction().IsParallel(frame.Direction(), Precision::Angular())) {
            continue;
        }
        const gp_Vec off(frame.Location(), circ.Location());
        const double z = off.Dot(gp_Vec(frame.Direction()));
        if ((off - gp_Vec(frame.Direction()) * z).Magnitude() > 1e-6) {
            continue;
        }
        for (const auto& b : bends) {
            if (std::abs(z - b.z) < 1e-6 && std::abs(circ.Radius() - b.r) < 1e-6) {
                fillet.Add(b.radius, TopoDS::Edge(edges(i)));
                ++count;
            }
        }
    }
    return count;
}

/// Points around what a form takes of the part, at the middle of the wall
std::vector<gp_Pnt> around(const Form& form, double T)
{
    std::vector<std::pair<double, double>> xy;
    if (form.halfWidth > 0) {
        for (double x : {-1.0, 0.0, 1.0}) {
            for (double y : {-1.0, 0.0, 1.0}) {
                xy.emplace_back(x * form.halfLength, y * form.halfWidth);
            }
        }
    }
    else {
        const int n = 16;
        for (int k = 0; k < n; ++k) {
            const double a = 2 * std::numbers::pi * k / n;
            xy.emplace_back(form.halfLength * std::cos(a), form.halfLength * std::sin(a));
        }
    }
    std::vector<gp_Pnt> res;
    const gp_Ax3& f = form.frame;
    for (const auto& [x, y] : xy) {
        res.push_back(f.Location().Translated(gp_Vec(f.XDirection()) * x
                                              + gp_Vec(f.YDirection()) * y
                                              + gp_Vec(f.Direction()) * (-T / 2)));
    }
    return res;
}

}  // namespace

SheetMetalForm::SheetMetalForm()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(Kind,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Tool: Tool's shape, set on its face, as the form's outside; Dimple: a "
                      "built-in one");
    Kind.setEnums(KindEnums);
    ADD_PROPERTY_TYPE(Face,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "The wall's skin the forms rise out of");
    ADD_PROPERTY_TYPE(Positions,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A sketch whose points are where the forms are");
    ADD_PROPERTY_TYPE(Angle,
                      (0.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "Each form turned about the face's normal, from the sketch's X");
    ADD_PROPERTY_TYPE(Reversed,
                      (false),
                      "Sheet Metal",
                      App::Prop_None,
                      "Rising into the face, out of the wall's other skin");
    ADD_PROPERTY_TYPE(Tool,
                      (nullptr),
                      "Form",
                      App::Prop_None,
                      "Kind Tool: the tool, and its face that sits on the wall's skin");
    ADD_PROPERTY_TYPE(Diameter,
                      (10.0),
                      "Form",
                      App::Prop_None,
                      "Kind Dimple: across where it leaves the face");
    ADD_PROPERTY_TYPE(Depth, (2.0), "Form", App::Prop_None, "Kind Dimple: its top above the face");
    ADD_PROPERTY_TYPE(Radius,
                      (1.0),
                      "Form",
                      App::Prop_None,
                      "Kind Dimple: the inside radius of its two bends");
    ADD_PROPERTY_TYPE(FlankAngle,
                      (45.0),
                      "Form",
                      App::Prop_None,
                      "Kind Dimple: the slant of its side from the face, up to 90 deg");
    FlankAngle.setConstraints(&flankRange);
    onChanged(&Kind);
}

short SheetMetalForm::mustExecute() const
{
    if (Kind.isTouched() || Face.isTouched() || Positions.isTouched() || Angle.isTouched()
        || Reversed.isTouched() || Tool.isTouched() || Diameter.isTouched() || Depth.isTouched()
        || Radius.isTouched() || FlankAngle.isTouched()) {
        return 1;
    }
    return PartDesign::Feature::mustExecute();
}

void SheetMetalForm::onChanged(const App::Property* prop)
{
    // what the other kind takes, greyed out
    if (prop == &Kind) {
        const bool tool = Kind.getValue() == 0;
        Tool.setStatus(App::Property::ReadOnly, !tool);
        for (App::Property* p :
             std::initializer_list<App::Property*> {&Diameter, &Depth, &Radius, &FlankAngle}) {
            p->setStatus(App::Property::ReadOnly, tool);
        }
    }
    // The face is of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Face.getValue()
        && Face.getValue() != BaseFeature.getValue()) {
        auto subs = Face.getSubValues(false);
        auto shadows = Face.getShadowSubs();
        Face.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    PartDesign::Feature::onChanged(prop);
}

App::DocumentObjectExecReturn* SheetMetalForm::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    try {
        Model model = modelBefore(rule->Thickness.getValue());
        if (model.elements.empty()) {
            THROWM(Base::ValueError, "A form builds on a sheet metal feature");
        }
        const double T = model.thickness;
        const TopoShape base = getBaseShape();

        // the wall's skin
        const auto shadows = Face.getShadowSubs();
        if (shadows.size() != 1) {
            THROWM(Base::ValueError, "A form takes one face: a wall's skin it rises out of");
        }
        const std::string& ref =
            shadows.front().first.empty() ? shadows.front().second : shadows.front().first;
        const TopoDS_Shape sub = base.getSubShape(ref.c_str(), true);
        if (sub.IsNull() || sub.ShapeType() != TopAbs_FACE) {
            FC_THROWM(Base::ValueError, "No face " << shadows.front().second);
        }
        const TopoDS_Face face = TopoDS::Face(sub);
        const int wall = model.owner(face);
        if (wall < 0 || model.elements[wall].kind != Element::Kind::Wall
            || BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane) {
            FC_THROWM(Base::ValueError,
                      "Face " << shadows.front().second
                              << " is no wall's skin: a form goes on a wall");
        }
        const gp_Dir normal = outerNormal(face);
        const gp_Pln plane = BRepAdaptor_Surface(face).Plane();
        const gp_Dir rise = Reversed.getValue() ? normal.Reversed() : normal;

        // where: the sketch's points, its X turned by Angle for the forms'
        auto sketch = Base::freecad_dynamic_cast<Part::Feature>(Positions.getValue());
        if (!sketch) {
            THROWM(Base::ValueError, "A form takes a sketch of its points: Positions");
        }
        std::vector<gp_Pnt> points;
        for (TopExp_Explorer it(Part::Feature::getTopoShape(sketch).getShape(),
                                TopAbs_VERTEX,
                                TopAbs_EDGE);
             it.More();
             it.Next()) {
            points.push_back(BRep_Tool::Pnt(TopoDS::Vertex(it.Current())));
        }
        if (points.empty()) {
            FC_THROWM(Base::ValueError, sketch->getFullLabel() << " has no point for a form");
        }
        Base::Vector3d sx(1, 0, 0);
        sketch->Placement.getValue().getRotation().multVec(sx, sx);
        gp_Dir xDir;
        if (!across(gp_Vec(sx.x, sx.y, sx.z), rise, xDir)) {
            xDir = plane.Position().XDirection();
        }
        xDir.Rotate(gp_Ax1(gp_Pnt(), rise), Base::toRadians(Angle.getValue()));

        // the tool, Kind Tool
        TopoDS_Shape tool;
        TopoDS_Face toolBase;
        gp_Dir toolX(1, 0, 0);
        const bool isTool = Kind.getValue() == 0;
        if (isTool) {
            auto obj = Tool.getValue();
            const auto subs = Tool.getSubValues(true);
            if (!obj || subs.size() != 1) {
                THROWM(Base::ValueError, "A form of Kind Tool takes a tool and its face");
            }
            const TopoShape toolShape = Part::Feature::getTopoShape(obj);
            TopExp_Explorer solid(toolShape.getShape(), TopAbs_SOLID);
            const TopoDS_Shape baseSub = toolShape.getSubShape(subs.front().c_str(), true);
            if (!solid.More() || baseSub.IsNull() || baseSub.ShapeType() != TopAbs_FACE
                || BRepAdaptor_Surface(TopoDS::Face(baseSub)).GetType() != GeomAbs_Plane) {
                FC_THROWM(Base::ValueError,
                          "The tool " << obj->getFullLabel()
                                      << " takes a solid and a plane face of it");
            }
            tool = solid.Current();
            toolBase = TopoDS::Face(baseSub);
            if (auto feature = Base::freecad_dynamic_cast<App::GeoFeature>(obj)) {
                Base::Vector3d tx(1, 0, 0);
                feature->Placement.getValue().getRotation().multVec(tx, tx);
                toolX = gp_Dir(tx.x, tx.y, tx.z);
            }
        }

        std::vector<TopoShape> adds;
        std::vector<TopoShape> cuts;
        const auto hasher = getDocument()->getStringHasher();
        std::vector<std::pair<Dimple, gp_Ax3>> dimples;
        for (const gp_Pnt& p : points) {
            // onto the face, and in it
            const double off = gp_Vec(plane.Location(), p).Dot(gp_Vec(plane.Axis().Direction()));
            const gp_Pnt on = p.Translated(gp_Vec(plane.Axis().Direction()) * -off);
            BRepClass_FaceClassifier inside(face, on, 1e-6);
            if (inside.State() != TopAbs_IN) {
                FC_THROWM(Base::ValueError,
                          "A point of " << sketch->getFullLabel() << " at (" << p.X() << ", "
                                        << p.Y() << ", " << p.Z() << ") is off the face");
            }
            const gp_Pnt origin =
                Reversed.getValue() ? on.Translated(gp_Vec(normal) * -T) : on;
            const gp_Ax3 frame(origin, rise, xDir);
            Pressed pressed;
            if (isTool) {
                pressed = pressTool(tool, toolBase, toolX, frame, T);
            }
            else {
                const Dimple d {Diameter.getValue() / 2,
                                Depth.getValue(),
                                Base::toRadians(FlankAngle.getValue()),
                                Radius.getValue(),
                                T};
                pressed = pressDimple(d, frame);
                dimples.emplace_back(d, frame);
            }
            // all of it on the wall: short of its bends and its limits
            Form& form = pressed.form;
            form.frame = frame;
            for (const gp_Pnt& q : around(form, T)) {
                if (!model.contains(wall, q, 1e-6)) {
                    FC_THROWM(Base::ValueError,
                              "A form at (" << p.X() << ", " << p.Y() << ", " << p.Z()
                                            << ") runs off " << model.elements[wall].name);
                }
            }
            form.name = "Form" + std::to_string(model.forms.size() + 1);
            form.wall = wall;
            model.forms.push_back(form);
            adds.emplace_back(pressed.outside);
            for (const TopoDS_Shape& cut : pressed.cuts) {
                cuts.emplace_back(cut);
            }
        }

        std::vector<TopoShape> fuse {base};
        fuse.insert(fuse.end(), adds.begin(), adds.end());
        TopoShape fused(0, hasher);
        fused.makEBoolean(Part::OpCodes::Fuse, fuse);
        std::vector<TopoShape> cut {fused};
        cut.insert(cut.end(), cuts.begin(), cuts.end());
        TopoShape res(0, hasher);
        res.makEBoolean(Part::OpCodes::Cut, cut);
        if (res.countSubShapes(TopAbs_SOLID) != 1) {
            THROWM(Base::ValueError, "The forms left the sheet in pieces");
        }
        res = getSolid(res);
        if (!dimples.empty()) {
            BRepFilletAPI_MakeFillet fillet(res.getShape());
            int count = 0;
            for (const auto& [d, frame] : dimples) {
                count += roundDimple(fillet, res.getShape(), d, frame);
            }
            if (count > 0) {
                fillet.Build();
                if (!fillet.IsDone()) {
                    THROWM(Base::ValueError,
                           "The dimple's bends cannot be rounded that much: a smaller Radius, "
                           "or a wider dimple");
                }
                TopoShape rounded(0, hasher);
                rounded.makESHAPE(fillet.Shape(), Part::MapperMaker(fillet), {res},
                                  Part::OpCodes::Fillet);
                res = getSolid(rounded);
            }
        }
        Shape.setValue(res);
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
