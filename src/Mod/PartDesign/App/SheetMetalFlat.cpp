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
#include <cstring>
#include <numbers>
#include <set>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>
#include <Base/Tools.h>
#include <Mod/Part/App/PartFeature.h>

#include "Body.h"
#include "Feature.h"
#include "FeatureSheetMetalFlatPattern.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Dxf.h"
#include "SheetMetal/Flat.h"
#include "SheetMetal/Pieces.h"
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

TopoShape flatShape(const TopoShape& solid, const SheetMetal::FlatResult& flat, bool tangentLines)
{
    const auto& hasher = solid.Hasher;
    std::vector<TopoShape> lines;
    std::vector<TopoShape> tangents;
    // each line as far as it lies on the blank, a piece after the first
    // named Bend1Part2, Bend1Part3, ...
    auto pieces = [](const std::vector<TopoDS_Edge>& segments, const std::string& name,
                     std::vector<TopoShape>& out) {
        for (std::size_t k = 0; k < segments.size(); ++k) {
            TopoShape edge(segments[k]);
            const std::string part = k ? name + "Part" + std::to_string(k + 1) : name;
            edge.setElementName(Data::IndexedName::fromConst("Edge", 1), Data::MappedName(part));
            // its ends too: Bend1V1, Bend1V2
            SheetMetal::nameRest(edge, part);
            out.push_back(edge);
        }
    };
    for (const auto& line : flat.bendLines) {
        pieces(line.segments, line.name, lines);
        for (std::size_t k = 0; k < line.tangents.size() && tangentLines; ++k) {
            pieces(line.tangentSegments[k], line.name + "Tangent" + std::to_string(k + 1),
                   tangents);
        }
    }
    std::vector<TopoShape> parts {
        solid, TopoShape(0, hasher).makECompound(lines, nullptr, /*force*/ true)};
    if (!tangents.empty()) {
        parts.push_back(TopoShape(0, hasher).makECompound(tangents, nullptr, /*force*/ true));
    }
    // each marking alone, named in turn
    auto named = [&](const std::vector<TopoDS_Edge>& edges, auto&& nameOf) {
        std::vector<TopoShape> res;
        for (std::size_t k = 0; k < edges.size(); ++k) {
            TopoShape edge(edges[k]);
            edge.setElementName(Data::IndexedName::fromConst("Edge", 1),
                                Data::MappedName(nameOf(k)));
            SheetMetal::nameRest(edge, nameOf(k));
            res.push_back(edge);
        }
        if (!res.empty()) {
            parts.push_back(TopoShape(0, hasher).makECompound(res, nullptr, /*force*/ true));
        }
    };
    named(flat.grain, [](std::size_t k) {
        return k ? "GrainHead" + std::to_string(k) : std::string("Grain");
    });
    std::vector<TopoDS_Edge> crossEdges;
    std::vector<std::string> crossNames;
    for (const auto& cross : flat.crossBreaks) {
        for (std::size_t k = 0; k < cross.diagonals.size(); ++k) {
            crossEdges.push_back(cross.diagonals[k]);
            crossNames.push_back(cross.name + "CrossBreak" + std::to_string(k + 1));
        }
    }
    named(crossEdges, [&](std::size_t k) { return crossNames[k]; });
    // each form's outline, Form1Outline1, ..., or its cross, Form1Mark1, 2
    std::vector<TopoDS_Edge> formEdges;
    std::vector<std::string> formNames;
    for (const auto& form : flat.forms) {
        for (std::size_t k = 0; k < form.edges.size(); ++k) {
            formEdges.push_back(form.edges[k]);
            formNames.push_back(form.name + (form.centre ? "Mark" : "Outline")
                                + std::to_string(k + 1));
        }
    }
    named(formEdges, [&](std::size_t k) { return formNames[k]; });
    named(flat.markings, [](std::size_t k) { return "Marking" + std::to_string(k + 1); });
    TopoShape res(0, hasher);
    res.makECompound(parts, nullptr, /*force*/ true);
    return res;
}

void measureFlat(SheetFlat& flat)
{
    const TopoDS_Shape& solid = flat.result.solid;
    if (solid.IsNull()) {
        return;
    }
    Bnd_Box box;
    BRepBndLib::AddOptimal(solid, box, /*useTriangulation*/ false, /*useShapeTolerance*/ false);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    flat.length = std::max(x1 - x0, y1 - y0);
    flat.width = std::min(x1 - x0, y1 - y0);
    if (flat.thickness > 0) {
        flat.area = props.Mass() / flat.thickness;
    }
}

SheetFlat layFlat(const App::DocumentObject* part, const FlatSettings& settings)
{
    if (!part) {
        THROWM(Base::ValueError, "There is no part to lay flat");
    }
    // the model of the sheet the part is, in the part's placement
    auto sheet = sheetOf(part);
    if (!sheet || sheet->SheetModel.getValue().elements.empty()) {
        THROWM(Base::ValueError, "The part is no sheet metal");
    }
    SheetMetal::Model model = sheetModelOf(part);
    const TopoShape folded = Part::Feature::getTopoShape(part);
    if (folded.isNull()) {
        THROWM(Base::ValueError, "The part has no shape");
    }

    int root = -1;
    if (!settings.root.empty()) {
        root = model.find(settings.root);
        if (root < 0 || model.elements[root].kind == SheetMetal::Element::Kind::Bend) {
            FC_THROWM(Base::ValueError, "The sheet has no wall " << settings.root << " to lay on");
        }
    }
    else {
        // the first wall; a sheet of none, a lofted flange's cone, on itself
        for (auto kind : {SheetMetal::Element::Kind::Wall, SheetMetal::Element::Kind::Cone}) {
            for (std::size_t i = 0; i < model.elements.size() && root < 0; ++i) {
                if (model.elements[i].kind == kind) {
                    root = static_cast<int>(i);
                }
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
        SheetMetal::flatten(folded.getShape(), model, root, settings.secondSkin, alongX,
                            settings.formDisplay);
    if (settings.grain) {
        flat.grain = SheetMetal::grainArrow(flat.solid, Base::toRadians(settings.grainAngle));
    }

    SheetFlat res;
    auto hasher = part->getDocument() ? part->getDocument()->getStringHasher()
                                      : App::StringHasherRef();
    for (const auto& line : flat.bendLines) {
        res.directions[line.name] = line.up ? "Up" : "Down";
        res.maxBendAngle = std::max(res.maxBendAngle, line.angle * 180 / std::numbers::pi);
    }
    for (const auto& cross : flat.crossBreaks) {
        res.crossBreaks[cross.name] = cross.up ? "Up" : "Down";
    }

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
    res.shape = flatShape(solid, flat, settings.tangentLines);
    res.result = std::move(flat);
    measureFlat(res);
    return res;
}

SheetMetalExtension* sheetOf(const App::DocumentObject* part)
{
    if (!part) {
        return nullptr;
    }
    // a suppressed feature's shape is its base's, its model not
    auto feature = dynamic_cast<const PartDesign::Feature*>(part);
    auto sheet = feature && feature->Suppress.getValue()
        ? nullptr
        : part->getExtensionByType<SheetMetalExtension>(true);
    return sheet ? sheet : SheetMetalExtension::findBefore(part);
}

namespace
{

/// A body's tip, and the body's placement, which its shape has the tip's
/// shape in
const App::DocumentObject* tipOf(const App::DocumentObject* part, Base::Placement& placement)
{
    auto body = dynamic_cast<const Part::BodyBase*>(part);
    if (!body) {
        return part;
    }
    placement = body->Placement.getValue();
    return body->Tip.getValue();
}

}  // namespace

SheetMetal::Model sheetModelOf(const App::DocumentObject* part)
{
    Base::Placement outer;
    auto sheet = sheetOf(tipOf(part, outer));
    if (!sheet) {
        return {};
    }
    SheetMetal::Model model = sheet->SheetModel.getValue();
    Base::Placement placement;
    if (auto feature = dynamic_cast<const Part::Feature*>(sheet->getExtendedObject())) {
        placement = feature->Placement.getValue();
    }
    model.transform(TopoShape::convert((outer * placement).toMatrix()));
    return model;
}

bool hasCrossBreaks(const App::DocumentObject* part)
{
    Base::Placement outer;
    auto sheet = sheetOf(tipOf(part, outer));
    if (!sheet) {
        return false;
    }
    const auto& elements = sheet->SheetModel.getValue().elements;
    return std::any_of(elements.begin(), elements.end(), [](const SheetMetal::Element& e) {
        return e.kind == SheetMetal::Element::Kind::Wall && e.crossBreak != 0;
    });
}

std::vector<TopoDS_Edge> crossBreakLines(const App::DocumentObject* part)
{
    std::vector<TopoDS_Edge> res;
    if (!hasCrossBreaks(part)) {
        return res;
    }
    const SheetMetal::Model model = sheetModelOf(part);
    const TopoShape shape = Part::Feature::getTopoShape(part);
    if (shape.isNull()) {
        return res;
    }
    for (std::size_t i = 0; i < model.elements.size(); ++i) {
        const TopoDS_Face face =
            SheetMetal::crossBreakFace(shape.getShape(), model, static_cast<int>(i));
        for (const TopoDS_Edge& e : SheetMetal::diagonals(face)) {
            res.push_back(e);
        }
    }
    return res;
}

std::vector<SheetMetal::CrossBreakPatch> crossBreakPatches(const App::DocumentObject* part)
{
    if (!hasCrossBreaks(part)) {
        return {};
    }
    const TopoShape shape = Part::Feature::getTopoShape(part);
    if (shape.isNull()) {
        return {};
    }
    return SheetMetal::crossBreakPatches(shape.getShape(), sheetModelOf(part));
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
    SheetFlat flat = solid->flatOf(part);
    // the sheet's flat, its tip's, has the flat object's notches and markings
    auto flatObj = solid->flatPattern();
    if (flatObj && (!part || part == solid->Tip.getValue())) {
        flat = flatObj->finished(flat);
    }
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

namespace
{

/// A label made a file name: no separator or character a file system
/// refuses, nor a trailing dot or space
std::string fileNameOf(const std::string& label)
{
    std::string res;
    for (char c : label) {
        const auto u = static_cast<unsigned char>(c);
        res += u < 0x20 || std::strchr("/\\:*?\"<>|", c) ? '_' : c;
    }
    while (!res.empty() && (res.back() == '.' || res.back() == ' ')) {
        res.pop_back();
    }
    return res.empty() ? std::string("Sheet") : res;
}

}  // namespace

std::vector<FlatExport>
exportFlats(const App::Document* doc, const char* folder, const char* suffix)
{
    if (!doc) {
        THROWM(Base::ValueError, "No document");
    }
    std::string ext = boost::algorithm::to_lower_copy(std::string(suffix ? suffix : ""));
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(0, 1);
    }
    if (ext != "dxf" && ext != "svg") {
        FC_THROWM(Base::ValueError,
                  "Export Flat writes dxf or svg, not '" << (suffix ? suffix : "") << "'");
    }
    Base::FileInfo dir(folder ? folder : "");
    if (!dir.isDir()) {
        FC_THROWM(Base::ValueError, "No folder " << (folder ? folder : ""));
    }

    std::vector<FlatExport> res;
    // names taken, as a case-blind file system sees them
    std::set<std::string> taken;
    for (auto obj : doc->getObjectsOfType(Body::getClassTypeId())) {
        auto body = static_cast<Body*>(obj);
        std::vector<SolidBody*> sheets;
        for (auto solid : body->getSolidBodies()) {
            if (solid && solid->isSheetMetal()) {
                sheets.push_back(solid);
            }
        }
        for (auto solid : sheets) {
            std::string name = body->Label.getStrValue();
            if (sheets.size() > 1) {
                name += "-" + solid->Label.getStrValue();
            }
            name = fileNameOf(name);
            std::string unique = name;
            for (int i = 2; !taken.insert(boost::algorithm::to_lower_copy(unique)).second; ++i) {
                unique = name + "-" + std::to_string(i);
            }
            FlatExport item;
            item.sheet = solid;
            const std::string path = dir.filePath() + "/" + unique + "." + ext;
            try {
                exportFlat(solid, path.c_str());
                item.path = path;
            }
            catch (const Base::Exception& e) {
                item.error = e.what();
            }
            catch (const Standard_Failure& e) {
                item.error = e.what();
            }
            res.push_back(std::move(item));
        }
    }
    return res;
}

}  // namespace PartDesign
