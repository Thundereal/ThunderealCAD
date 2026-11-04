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
#include <numbers>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>
#include <Base/Tools.h>
#include <Mod/Part/App/PartFeature.h>

#include "Body.h"
#include "FeatureSheetMetalFlatPattern.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Dxf.h"
#include "SheetMetal/Flat.h"
#include "SheetMetal/Svg.h"
#include "SheetMetalExtension.h"
#include "SheetMetalFlat.h"
#include "SolidBody.h"
#include <Mod/Material/App/Materials.h>

using Part::TopoShape;

namespace PartDesign
{

namespace
{
constexpr const char* FlatOp = "SMF";
}

SheetFlat layFlat(const App::DocumentObject* part, const FlatSettings& settings)
{
    if (!part) {
        THROWM(Base::ValueError, "There is no part to lay flat");
    }
    // the model of the sheet the part is, in the part's placement
    auto sheet = part->getExtensionByType<SheetMetalExtension>(true);
    if (!sheet) {
        sheet = SheetMetalExtension::findBefore(part);
    }
    if (!sheet || sheet->SheetModel.getValue().elements.empty()) {
        THROWM(Base::ValueError, "The part is no sheet metal");
    }
    SheetMetal::Model model = sheet->SheetModel.getValue();
    if (auto feature = dynamic_cast<Part::Feature*>(sheet->getExtendedObject())) {
        model.transform(TopoShape::convert(feature->Placement.getValue().toMatrix()));
    }
    const TopoShape folded = Part::Feature::getTopoShape(part);
    if (folded.isNull()) {
        THROWM(Base::ValueError, "The part has no shape");
    }

    int root = -1;
    if (!settings.root.empty()) {
        root = model.find(settings.root);
        if (root < 0 || model.elements[root].kind != SheetMetal::Element::Kind::Wall) {
            FC_THROWM(Base::ValueError, "The sheet has no wall " << settings.root << " to lay on");
        }
    }
    else {
        for (std::size_t i = 0; i < model.elements.size() && root < 0; ++i) {
            if (model.elements[i].kind == SheetMetal::Element::Kind::Wall) {
                root = static_cast<int>(i);
            }
        }
    }

    // A feature lacking the edge (one before it, say) is laid flat unturned
    TopoDS_Edge alongX;
    if (!settings.alignEdge.empty()) {
        const TopoShape edge = folded.getSubTopoShape(settings.alignEdge.c_str(), /*silent*/ true);
        if (!edge.isNull() && edge.getShape().ShapeType() == TopAbs_EDGE) {
            alongX = TopoDS::Edge(edge.getShape());
        }
    }

    SheetMetal::FlatResult flat =
        SheetMetal::flatten(folded.getShape(), model, root, settings.secondSkin, alongX);
    if (settings.grain) {
        flat.grain = SheetMetal::grainArrow(flat.solid, Base::toRadians(settings.grainAngle));
    }

    SheetFlat res;
    auto hasher = part->getDocument() ? part->getDocument()->getStringHasher()
                                      : App::StringHasherRef();
    std::vector<TopoShape> lines;
    std::vector<TopoShape> tangents;
    // each line as far as it lies on the blank, a piece after the first
    // named Bend1Part2, Bend1Part3, ...
    auto pieces = [](const std::vector<TopoDS_Edge>& segments, const std::string& name,
                     std::vector<TopoShape>& out) {
        for (std::size_t k = 0; k < segments.size(); ++k) {
            TopoShape edge(segments[k]);
            edge.setElementName(Data::IndexedName::fromConst("Edge", 1),
                                Data::MappedName(k ? name + "Part" + std::to_string(k + 1) : name));
            out.push_back(edge);
        }
    };
    for (const auto& line : flat.bendLines) {
        pieces(line.segments, line.name, lines);
        for (std::size_t k = 0; k < line.tangents.size() && settings.tangentLines; ++k) {
            pieces(line.tangentSegments[k], line.name + "Tangent" + std::to_string(k + 1),
                   tangents);
        }
        res.directions[line.name] = line.up ? "Up" : "Down";
        res.maxBendAngle = std::max(res.maxBendAngle, line.angle * 180 / std::numbers::pi);
    }

    Bnd_Box box;
    BRepBndLib::AddOptimal(flat.solid, box, /*useTriangulation*/ false,
                           /*useShapeTolerance*/ false);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    GProp_GProps props;
    BRepGProp::VolumeProperties(flat.solid, props);
    res.length = std::max(x1 - x0, y1 - y0);
    res.width = std::min(x1 - x0, y1 - y0);
    res.area = props.Mass() / model.thickness;
    res.thickness = model.thickness;
    res.bendCount = static_cast<long>(flat.bendLines.size());
    if (auto rule = sheet->getRule()) {
        res.material = rule->ShapeMaterial.getValue().getName().toStdString();
    }

    // each face named after the folded face it lays out; its edges and
    // vertices after the faces they bound
    Part::ShapeMapper mapper;
    for (const auto& [from, to] : flat.faces) {
        mapper.insert(/*generated*/ false, from, to);
    }
    TopoShape solid(0, hasher);
    solid.makESHAPE(flat.solid, mapper, {folded}, FlatOp);
    std::vector<TopoShape> parts {solid,
                                  TopoShape(0, hasher).makECompound(lines, nullptr, /*force*/ true)};
    if (!tangents.empty()) {
        parts.push_back(TopoShape(0, hasher).makECompound(tangents, nullptr, /*force*/ true));
    }
    std::vector<TopoShape> grain;
    for (std::size_t k = 0; k < flat.grain.size(); ++k) {
        TopoShape edge(flat.grain[k]);
        edge.setElementName(Data::IndexedName::fromConst("Edge", 1),
                            Data::MappedName(k ? "GrainHead" + std::to_string(k) : "Grain"));
        grain.push_back(edge);
    }
    if (!grain.empty()) {
        parts.push_back(TopoShape(0, hasher).makECompound(grain, nullptr, /*force*/ true));
    }
    res.result = std::move(flat);
    res.shape = TopoShape(0, hasher);
    res.shape.makECompound(parts, nullptr, /*force*/ true);
    return res;
}

void exportFlat(const App::DocumentObject* obj, const char* path)
{
    if (!obj || !obj->isAttachedToDocument()) {
        THROWM(Base::ValueError, "No object to lay flat");
    }
    Base::FileInfo fi(path ? path : "");
    const std::string suffix = boost::algorithm::to_lower_copy(fi.extension());
    if (suffix != "dxf" && suffix != "svg") {
        FC_THROWM(Base::ValueError,
                  "Export Flat writes .dxf or .svg, not '" << fi.fileName() << "'");
    }
    auto solid = Base::freecad_dynamic_cast<const SolidBody>(obj);
    const App::DocumentObject* part = nullptr;
    // the flat object writes its solid's tip's
    if (auto flatObj = Base::freecad_dynamic_cast<const SheetMetalFlatPattern>(obj)) {
        solid = flatObj->getSheet();
    }
    else if (!solid) {
        auto body = Body::findBodyOf(obj);
        solid = body ? body->getSolidBody(obj) : nullptr;
        part = obj;
    }
    if (!solid || !solid->isSheetMetal()) {
        FC_THROWM(Base::ValueError, obj->getFullLabel() << " is of no sheet metal solid");
    }
    // laid flat before the file is opened, so a refusal writes nothing
    const SheetFlat& flat = solid->flatOf(part);
    Base::ofstream out(fi, std::ios::out | std::ios::trunc);
    if (!out) {
        FC_THROWM(Base::FileException, "Cannot write " << path);
    }
    if (suffix == "dxf") {
        SheetMetal::writeDXF(out, flat.result, solid->flatSettings().tangentLines);
    }
    else {
        SheetMetal::writeSVG(out, flat.result, solid->flatSettings().tangentLines);
    }
}

}  // namespace PartDesign
