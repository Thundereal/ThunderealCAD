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
#include <cmath>
#include <cstring>
#include <iomanip>
#include <map>
#include <numbers>
#include <set>
#include <sstream>

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_TShape.hxx>
#include <gp_Trsf.hxx>

#include <Base/Exception.h>
#include <Mod/Part/App/PartFeature.h>

#include "Body.h"
#include "FeatureSheetMetalConvert.h"
#include "FeatureSheetMetalFlatPattern.h"
#include "FeatureSheetMetalHem.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Flat.h"
#include "SheetMetalCheck.h"
#include "SheetMetalExtension.h"
#include "SheetMetalFlat.h"
#include "SolidBody.h"

using Part::TopoShape;
using PartDesign::SheetMetal::Element;

namespace PartDesign
{

namespace
{

constexpr double LengthTolerance = 1e-7;
/// mm^2: two elements sharing less than this lie side by side
constexpr double AreaTolerance = 1e-6;

std::string format(double value)
{
    std::ostringstream str;
    str << std::setprecision(4) << value;
    return str.str();
}

/// The feature that made the element of that name: the first down the
/// sheet's chain whose model has it
const App::DocumentObject* makerOf(const SheetMetalExtension* sheet, const std::string& name)
{
    const App::DocumentObject* maker = sheet->getExtendedObject();
    for (auto before = SheetMetalExtension::findBefore(maker);
         before && before->SheetModel.getValue().find(name) >= 0;
         before = SheetMetalExtension::findBefore(maker)) {
        maker = before->getExtendedObject();
    }
    return maker;
}

/// Where a bend's radius comes from: the feature's own, or the rule's. A
/// thin part converted keeps the radii it has; one thickened takes the rule's
const char* radiusSource(const App::DocumentObject* maker, const SheetMetalRule& rule)
{
    if (maker->isDerivedFrom<SheetMetalHem>()) {
        return "feature";
    }
    if (auto convert = dynamic_cast<const SheetMetalConvert*>(maker)) {
        if (std::strcmp(convert->Kind.getValue(), "Thin part") == 0) {
            return "feature";
        }
    }
    auto useRule = dynamic_cast<App::PropertyBool*>(maker->getPropertyByName("UseRuleRadius"));
    if (useRule && !useRule->getValue()) {
        return "feature";
    }
    return rule.valueSource(rule.BendRadius);
}

/// Whether a face is a plane across Z, a skin of the flat; at z 0 if bottom
bool isSkin(const TopoDS_Face& face, bool bottom)
{
    BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane) {
        return false;
    }
    const gp_Pln plane = surface.Plane();
    return std::abs(std::abs(plane.Axis().Direction().Z()) - 1) < 1e-9
        && (!bottom || std::abs(plane.Location().Z()) < 1e-6);
}

double areaOf(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape, props);
    return props.Mass();
}

}  // namespace

namespace
{

/// What a check is of: the sheet's solid, the part checked (its tip for the
/// solid or its flat object), the sheet metal feature it is, and its rule
struct Checked
{
    const SolidBody* solid = nullptr;
    const App::DocumentObject* part = nullptr;
    const SheetMetalExtension* sheet = nullptr;
    SheetMetalRule* rule = nullptr;
};

Checked checkedOf(const App::DocumentObject* obj)
{
    if (!obj || !obj->isAttachedToDocument()) {
        THROWM(Base::ValueError, "No object to check");
    }
    Checked res;
    res.solid = Base::freecad_dynamic_cast<const SolidBody>(obj);
    // the flat object checks its solid
    if (auto flatObj = Base::freecad_dynamic_cast<const SheetMetalFlatPattern>(obj)) {
        res.solid = flatObj->getSheet();
    }
    res.part = obj;
    if (res.solid) {
        res.part = res.solid->Tip.getValue();
    }
    else {
        auto body = Body::findBodyOf(obj);
        res.solid = body ? body->getSolidBody(obj) : nullptr;
    }
    if (res.part) {
        res.sheet = res.part->getExtensionByType<SheetMetalExtension>(true);
        if (!res.sheet) {
            res.sheet = SheetMetalExtension::findBefore(res.part);
        }
    }
    res.rule = res.sheet ? res.sheet->getRule() : nullptr;
    if (!res.solid || !res.solid->isSheetMetal() || !res.rule
        || res.sheet->SheetModel.getValue().elements.empty()) {
        FC_THROWM(Base::ValueError, obj->getFullLabel() << " is of no sheet metal solid");
    }
    return res;
}

/// A bend's row of the table, its values and their sources: no direction,
/// no faces
SheetBendRow bendRow(const SheetMetalExtension* sheet, const SheetMetalRule& rule,
                     const Element& element)
{
    const auto maker = makerOf(sheet, element.name);
    SheetBendRow row;
    row.name = element.name;
    row.feature = maker->getNameInDocument();
    row.angle = std::abs(element.bend.angle) * 180 / std::numbers::pi;
    row.radius = element.bend.radius;
    row.radiusSource = radiusSource(maker, rule);
    row.kFactor = element.bend.k;
    row.method = rule.AllowanceMethod.getValueAsString();
    row.methodSource = rule.valueSource(rule.AllowanceMethod);
    // another method gives a K of its own for each bend; the bend's own,
    // kept on the feature that made it, beats both
    auto makerExt = maker->getExtensionByType<SheetMetalExtension>(true);
    if (makerExt && makerExt->bendKFactor(element.name)) {
        row.kFactorSource = "bend";
    }
    else {
        row.kFactorSource =
            row.method == "KFactor" ? rule.valueSource(rule.KFactor) : row.method;
    }
    row.allowance = std::abs(element.bend.angle) * element.bend.neutral();
    return row;
}

}  // namespace

std::vector<SheetBendRow> sheetBends(const App::DocumentObject* obj)
{
    const Checked checked = checkedOf(obj);
    std::map<std::string, std::string> directions;
    try {
        directions = checked.solid->flatOf(checked.part).directions;
    }
    catch (const Base::Exception&) {
        // no direction where the sheet does not lie flat
    }
    catch (const Standard_Failure&) {
    }
    std::vector<SheetBendRow> rows;
    for (const Element& element : checked.sheet->SheetModel.getValue().elements) {
        if (element.kind != Element::Kind::Bend) {
            continue;
        }
        SheetBendRow row = bendRow(checked.sheet, *checked.rule, element);
        auto it = directions.find(element.name);
        if (it != directions.end()) {
            row.direction = it->second;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

SheetCheck checkSheet(const App::DocumentObject* obj)
{
    const Checked checked = checkedOf(obj);
    const SolidBody* solid = checked.solid;
    const App::DocumentObject* part = checked.part;
    const SheetMetalExtension* sheet = checked.sheet;
    SheetMetalRule* rule = checked.rule;

    // the model of the sheet, in the part's placement, as layFlat() has it
    SheetMetal::Model model = sheet->SheetModel.getValue();
    if (auto feature = dynamic_cast<const Part::Feature*>(sheet->getExtendedObject())) {
        model.transform(TopoShape::convert(feature->Placement.getValue().toMatrix()));
    }
    const TopoShape folded = Part::Feature::getTopoShape(part);
    const int faceCount = folded.countSubShapes(TopAbs_FACE);
    std::vector<int> owners(faceCount + 1, -1);
    for (int i = 1; i <= faceCount; ++i) {
        owners[i] = model.owner(TopoDS::Face(folded.getSubShape(TopAbs_FACE, i)));
    }

    SheetCheck res;
    const SheetFlat* flat = nullptr;
    auto noFlat = [&](const std::string& why) {
        SheetProblem problem;
        problem.kind = "NoFlat";
        problem.feature = part->getNameInDocument();
        problem.message = "It does not lie flat: " + why;
        res.problems.push_back(std::move(problem));
    };
    try {
        flat = &solid->flatOf(part);
    }
    catch (const Base::Exception& e) {
        noFlat(e.what());
    }
    catch (const Standard_Failure& e) {
        noFlat(e.GetMessageString());
    }

    // each folded face by its index, with the faces of the flat it lies as;
    // found by its TShape: the shape the flat was laid from is the part's
    // under a location of its own
    std::map<const TopoDS_TShape*, int> indices;
    for (int i = 1; i <= faceCount; ++i) {
        indices[folded.getSubShape(TopAbs_FACE, i).TShape().get()] = i;
    }
    std::vector<std::pair<int, TopoDS_Face>> laid;
    if (flat) {
        for (const auto& [from, to] : flat->result.faces) {
            auto it = indices.find(from.TShape().get());
            const int index = it == indices.end() ? 0 : it->second;
            if (index <= 0 || to.IsNull()) {
                continue;
            }
            for (TopExp_Explorer it(to, TopAbs_FACE); it.More(); it.Next()) {
                laid.emplace_back(index, TopoDS::Face(it.Current()));
            }
        }
    }
    auto addFaces = [&](const std::set<int>& indices, std::vector<std::string>& elements,
                        std::vector<std::string>& flatElements) {
        for (int index : indices) {
            elements.push_back("Face" + std::to_string(index));
            for (const auto& [from, face] : laid) {
                const int i = from == index ? flat->shape.findShape(face) : 0;
                if (i > 0) {
                    flatElements.push_back("FlatFace" + std::to_string(i));
                }
            }
        }
    };
    // the flat's faces by the element they lay out, all and on XY
    std::map<int, std::vector<std::pair<int, TopoDS_Face>>> byOwner;
    std::map<int, std::vector<std::pair<int, TopoDS_Face>>> bottom;
    for (const auto& [index, face] : laid) {
        const int owner = owners[index];
        if (owner < 0) {
            continue;
        }
        byOwner[owner].emplace_back(index, face);
        if (isSkin(face, true)) {
            bottom[owner].emplace_back(index, face);
        }
    }

    // the bend table
    for (std::size_t i = 0; i < model.elements.size(); ++i) {
        const Element& element = model.elements[i];
        if (element.kind != Element::Kind::Bend) {
            continue;
        }
        SheetBendRow row = bendRow(sheet, *rule, element);
        if (flat) {
            auto it = flat->directions.find(element.name);
            if (it != flat->directions.end()) {
                row.direction = it->second;
            }
        }
        std::set<int> faces;
        for (int f = 1; f <= faceCount; ++f) {
            if (owners[f] == static_cast<int>(i)) {
                faces.insert(f);
            }
        }
        addFaces(faces, row.elements, row.flatElements);
        res.bends.push_back(std::move(row));
    }

    // a feature's own relief smaller than the rule's, earliest first
    std::vector<const App::DocumentObject*> chain;
    for (auto s = sheet; s; s = SheetMetalExtension::findBefore(s->getExtendedObject())) {
        chain.insert(chain.begin(), s->getExtendedObject());
    }
    for (auto feature : chain) {
        auto shape = dynamic_cast<App::PropertyEnumeration*>(
            feature->getPropertyByName("ReliefShape"));
        auto width = dynamic_cast<App::PropertyLength*>(feature->getPropertyByName("ReliefWidth"));
        auto depth = dynamic_cast<App::PropertyLength*>(feature->getPropertyByName("ReliefDepth"));
        if (!shape || !width || !depth || shape->isValue("Rule")) {
            continue;
        }
        auto check = [&](const App::PropertyLength& own, const App::PropertyLength& ruled,
                         const char* how) {
            if (own.getValue() >= ruled.getValue() - LengthTolerance) {
                return;
            }
            SheetProblem problem;
            problem.kind = "Relief";
            problem.feature = feature->getNameInDocument();
            problem.value = own.getValue();
            problem.limit = ruled.getValue();
            problem.message = feature->Label.getStrValue() + "'s relief is " + format(own.getValue())
                + " mm " + how + ", less than the rule's " + format(ruled.getValue()) + " mm";
            res.problems.push_back(std::move(problem));
        };
        check(*width, rule->ReliefWidth, "wide");
        check(*depth, rule->ReliefDepth, "deep");
    }
    if (!flat) {
        return res;
    }

    // a wall shorter past a bend than the rule's least flange; a hem's
    // return has rules of its own
    const double minFlange = rule->MinFlange.getValue();
    for (const auto& line : flat->result.bendLines) {
        const int b = model.find(line.name);
        if (minFlange <= LengthTolerance || b < 0
            || makerOf(sheet, line.name)->isDerivedFrom<SheetMetalHem>()) {
            continue;
        }
        const Element& bend = model.elements[b];
        const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(line.edge));
        const gp_Pnt e = BRep_Tool::Pnt(TopExp::LastVertex(line.edge));
        const double length = std::hypot(e.X() - a.X(), e.Y() - a.Y());
        if (length < LengthTolerance) {
            continue;
        }
        const double ux = (e.X() - a.X()) / length;
        const double uy = (e.Y() - a.Y()) / length;
        // the tangent lines lie half the bend's flat length either side
        const double half = std::abs(bend.bend.angle) * bend.bend.neutral() / 2;
        for (int wall : {bend.before, bend.after}) {
            auto it = byOwner.find(wall);
            if (it == byOwner.end()) {
                continue;
            }
            double far = 0;
            std::set<int> faces;
            for (const auto& [index, face] : it->second) {
                faces.insert(index);
                for (TopExp_Explorer v(face, TopAbs_VERTEX); v.More(); v.Next()) {
                    const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
                    far = std::max(far, std::abs((q.X() - a.X()) * uy - (q.Y() - a.Y()) * ux));
                }
            }
            const double flange = far - half;
            if (flange >= minFlange - LengthTolerance) {
                continue;
            }
            const std::string& name = model.elements[wall].name;
            SheetProblem problem;
            problem.kind = "ShortFlange";
            problem.feature = makerOf(sheet, name)->getNameInDocument();
            problem.value = flange;
            problem.limit = minFlange;
            problem.message = name + " runs " + format(flange) + " mm past " + line.name
                + ", shorter than the rule's least flange, " + format(minFlange) + " mm";
            addFaces(faces, problem.elements, problem.flatElements);
            res.problems.push_back(std::move(problem));
        }
    }

    // a hole nearer a bend line than the rule asks: the holes of the flat's
    // bottom skin as one face, so a hole across two elements is one
    std::vector<TopoDS_Face> skin;
    try {
        skin = SheetMetal::facesOnXY(flat->result.solid);
    }
    catch (const Base::Exception&) {
    }
    TopoDS_Shape whole;
    if (skin.size() == 1) {
        whole = skin.front();
    }
    else if (skin.size() > 1) {
        TopTools_ListOfShape arguments;
        TopTools_ListOfShape tools;
        arguments.Append(skin.front());
        for (std::size_t k = 1; k < skin.size(); ++k) {
            tools.Append(skin[k]);
        }
        BRepAlgoAPI_Fuse fuse;
        fuse.SetArguments(arguments);
        fuse.SetTools(tools);
        fuse.Build();
        if (fuse.IsDone()) {
            ShapeUpgrade_UnifySameDomain unify(fuse.Shape(), true, true, false);
            unify.Build();
            whole = unify.Shape();
        }
    }
    std::vector<TopoDS_Wire> holes;
    for (TopExp_Explorer f(whole, TopAbs_FACE); f.More(); f.Next()) {
        const TopoDS_Face& face = TopoDS::Face(f.Current());
        const TopoDS_Wire outer = BRepTools::OuterWire(face);
        for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
            if (!w.Current().IsSame(outer)) {
                holes.push_back(TopoDS::Wire(w.Current()));
            }
        }
    }
    const double minHole = rule->MinHoleToBend.getValue();
    for (const auto& hole : holes) {
        Bnd_Box box;
        BRepBndLib::Add(hole, box);
        box.Enlarge(1e-6);
        // its faces: those across the sheet, all within it on XY
        std::set<int> faces;
        for (const auto& [index, face] : laid) {
            if (isSkin(face, false)) {
                continue;
            }
            Bnd_Box faceBox;
            BRepBndLib::Add(face, faceBox);
            double x0, y0, z0, x1, y1, z1;
            faceBox.Get(x0, y0, z0, x1, y1, z1);
            if (!box.IsOut(gp_Pnt(x0, y0, 0)) && !box.IsOut(gp_Pnt(x1, y1, 0))) {
                faces.insert(index);
            }
        }
        for (const auto& line : flat->result.bendLines) {
            const int b = model.find(line.name);
            if (b < 0) {
                continue;
            }
            const double limit = minHole > LengthTolerance
                ? minHole
                : 2.5 * model.thickness + model.elements[b].bend.radius;
            // the line on the bottom skin, where the hole is measured
            gp_Trsf down;
            down.SetTranslation(
                gp_Vec(0, 0, -BRep_Tool::Pnt(TopExp::FirstVertex(line.edge)).Z()));
            BRepExtrema_DistShapeShape distance(
                hole, BRepBuilderAPI_Transform(line.edge, down, true).Shape());
            if (!distance.IsDone() || distance.Value() >= limit - LengthTolerance) {
                continue;
            }
            SheetProblem problem;
            problem.kind = "HoleNearBend";
            problem.value = distance.Value();
            problem.limit = limit;
            problem.message = "A hole is " + format(distance.Value()) + " mm from " + line.name
                + "'s line, nearer than " + format(limit) + " mm";
            addFaces(faces, problem.elements, problem.flatElements);
            res.problems.push_back(std::move(problem));
        }
    }

    // material of two elements lying on each other in the flat
    std::map<int, std::vector<Bnd_Box>> boxes;
    for (const auto& [owner, faces] : bottom) {
        for (const auto& [index, face] : faces) {
            Bnd_Box box;
            BRepBndLib::Add(face, box);
            boxes[owner].push_back(box);
        }
    }
    for (auto first = bottom.begin(); first != bottom.end(); ++first) {
        for (auto second = std::next(first); second != bottom.end(); ++second) {
            double area = 0;
            std::set<int> faces;
            for (std::size_t i = 0; i < first->second.size(); ++i) {
                for (std::size_t j = 0; j < second->second.size(); ++j) {
                    if (boxes[first->first][i].IsOut(boxes[second->first][j])) {
                        continue;
                    }
                    BRepAlgoAPI_Common common(first->second[i].second, second->second[j].second);
                    if (!common.IsDone()) {
                        continue;
                    }
                    const double shared = areaOf(common.Shape());
                    if (shared > AreaTolerance) {
                        area += shared;
                        faces.insert(first->second[i].first);
                        faces.insert(second->second[j].first);
                    }
                }
            }
            if (area <= AreaTolerance) {
                continue;
            }
            SheetProblem problem;
            problem.kind = "Overlap";
            problem.value = area;
            problem.message = model.elements[first->first].name + " and "
                + model.elements[second->first].name + " overlap in the flat, "
                + format(area) + " mm^2";
            addFaces(faces, problem.elements, problem.flatElements);
            res.problems.push_back(std::move(problem));
        }
    }
    return res;
}

}  // namespace PartDesign
