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
#include <map>
#include <numbers>

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>

#include <BRep_Tool.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Vec.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>

#include "FeatureSheetMetalFlatPattern.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Dxf.h"
#include "SheetMetal/Flat.h"
#include "SheetMetalFlatPatternPy.h"
#include "SheetMetalExtension.h"

using namespace PartDesign;
using Part::TopoShape;

namespace
{
/// The op code of a face laid flat, in its name
constexpr const char* FlatOp = "SMF";
}  // namespace

PROPERTY_SOURCE(PartDesign::SheetMetalFlatPattern, Part::Feature)

SheetMetalFlatPattern::SheetMetalFlatPattern()
{
    ADD_PROPERTY_TYPE(Source,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "The folded part: a feature of the body, sheet metal or one after it");
    ADD_PROPERTY_TYPE(RootFace,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "The wall's skin laid on XY; the first wall's first skin if none");
    ADD_PROPERTY_TYPE(TangentLines,
                      (false),
                      "Sheet Metal",
                      App::Prop_None,
                      "Draw each bend's tangent lines, where the bend meets its walls, in the "
                      "flat and its DXF");
    Placement.setStatus(App::Property::Hidden, true);

    const auto out = App::PropertyType(App::Prop_Output | App::Prop_ReadOnly);
    ADD_PROPERTY_TYPE(FlatLength, (0.0), "Manufacturing", out, "The flat's longer side on XY");
    ADD_PROPERTY_TYPE(FlatWidth, (0.0), "Manufacturing", out, "The flat's shorter side on XY");
    ADD_PROPERTY_TYPE(FlatArea, (0.0), "Manufacturing", out, "The blank's area");
    ADD_PROPERTY_TYPE(Thickness, (0.0), "Manufacturing", out, "The sheet's thickness");
    ADD_PROPERTY_TYPE(MaterialName, (""), "Manufacturing", out, "The rule's material card");
    ADD_PROPERTY_TYPE(BendCount, (0), "Manufacturing", out, "How many bends");
    ADD_PROPERTY_TYPE(MaxBendAngle, (0.0), "Manufacturing", out, "The largest bend angle");
    ADD_PROPERTY_TYPE(BendDirections,
                      (),
                      "Manufacturing",
                      out,
                      "Each bend, Up or Down: Up where the material comes up toward one looking "
                      "down on the flat");
}

SheetMetal::FlatResult SheetMetalFlatPattern::buildFlat(SheetMetalExtension*& sheet,
                                                        TopoShape* foldedOut) const
{
    auto source = Source.getValue();
    if (!source) {
        THROWM(Base::ValueError, "The flat pattern has no part to lay flat");
    }
    {
        // the model of the sheet the part is, in the part's placement
        sheet = source->getExtensionByType<SheetMetalExtension>(true);
        if (!sheet) {
            sheet = SheetMetalExtension::findBefore(source);
        }
        if (!sheet || sheet->SheetModel.getValue().elements.empty()) {
            THROWM(Base::ValueError, "The part is no sheet metal");
        }
        SheetMetal::Model model = sheet->SheetModel.getValue();
        if (auto feature = dynamic_cast<Part::Feature*>(sheet->getExtendedObject())) {
            model.transform(TopoShape::convert(feature->Placement.getValue().toMatrix()));
        }
        const TopoShape folded = Part::Feature::getTopoShape(source);
        if (folded.isNull()) {
            THROWM(Base::ValueError, "The part has no shape");
        }

        int root = -1;
        bool secondSkin = false;
        const auto& subs = RootFace.getSubValues();
        if (RootFace.getValue() && !subs.empty()) {
            const TopoShape face = Part::Feature::getTopoShape(RootFace.getValue(),
                                                               subs.front().c_str(),
                                                               /*needSubElement*/ true);
            if (face.isNull() || face.getShape().ShapeType() != TopAbs_FACE) {
                THROWM(Base::ValueError, "The root face is not a face");
            }
            root = model.owner(TopoDS::Face(face.getShape()));
            if (root < 0 || model.elements[root].kind != SheetMetal::Element::Kind::Wall) {
                THROWM(Base::ValueError, "The root face is on no wall");
            }
            const auto& frame = model.elements[root].frame;
            // any point of the skin tells which of the two it is
            const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(face.getSubShape(TopAbs_VERTEX, 1)));
            const double depth = gp_Vec(frame.Location(), p).Dot(gp_Vec(frame.Direction()));
            secondSkin = std::abs(depth - model.thickness) < std::abs(depth);
        }
        else {
            for (std::size_t i = 0; i < model.elements.size() && root < 0; ++i) {
                if (model.elements[i].kind == SheetMetal::Element::Kind::Wall) {
                    root = static_cast<int>(i);
                }
            }
        }

        if (foldedOut) {
            *foldedOut = folded;
        }
        return SheetMetal::flatten(folded.getShape(), model, root, secondSkin);
    }
}

App::DocumentObjectExecReturn* SheetMetalFlatPattern::execute()
{
    try {
        SheetMetalExtension* sheet = nullptr;
        TopoShape folded;
        const auto flat = buildFlat(sheet, &folded);
        const SheetMetal::Model& model = sheet->SheetModel.getValue();
        std::vector<TopoShape> lines;
        std::vector<TopoShape> tangents;
        std::map<std::string, std::string> directions;
        double maxAngle = 0;
        for (const auto& line : flat.bendLines) {
            TopoShape edge(line.edge);
            edge.setElementName(Data::IndexedName::fromConst("Edge", 1),
                                Data::MappedName(line.name));
            lines.push_back(edge);
            for (std::size_t k = 0; k < line.tangents.size() && TangentLines.getValue(); ++k) {
                TopoShape tangent(line.tangents[k]);
                tangent.setElementName(
                    Data::IndexedName::fromConst("Edge", 1),
                    Data::MappedName(line.name + "Tangent" + std::to_string(k + 1)));
                tangents.push_back(tangent);
            }
            directions[line.name] = line.up ? "Up" : "Down";
            maxAngle = std::max(maxAngle, line.angle);
        }

        Bnd_Box box;
        BRepBndLib::AddOptimal(flat.solid, box, /*useTriangulation*/ false, /*useShapeTolerance*/ false);
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        GProp_GProps props;
        BRepGProp::VolumeProperties(flat.solid, props);
        FlatLength.setValue(std::max(x1 - x0, y1 - y0));
        FlatWidth.setValue(std::min(x1 - x0, y1 - y0));
        FlatArea.setValue(props.Mass() / model.thickness);
        Thickness.setValue(model.thickness);
        BendCount.setValue(static_cast<long>(flat.bendLines.size()));
        MaxBendAngle.setValue(maxAngle * 180 / std::numbers::pi);
        BendDirections.setValues(directions);
        auto rule = sheet->getRule();
        MaterialName.setValue(rule ? rule->ShapeMaterial.getValue().getName().toStdString()
                                   : std::string());
        // each face named after the folded face it lays out; its edges and
        // vertices after the faces they bound
        Part::ShapeMapper mapper;
        for (const auto& [from, to] : flat.faces) {
            mapper.insert(/*generated*/ false, from, to);
        }
        TopoShape solid(0, getDocument()->getStringHasher());
        solid.makESHAPE(flat.solid, mapper, {folded}, FlatOp);
        std::vector<TopoShape> parts {solid,
                                      TopoShape(0, getDocument()->getStringHasher())
                                          .makECompound(lines, nullptr, /*force*/ true)};
        if (!tangents.empty()) {
            parts.push_back(TopoShape(0, getDocument()->getStringHasher())
                                .makECompound(tangents, nullptr, /*force*/ true));
        }
        TopoShape res(0, getDocument()->getStringHasher());
        res.makECompound(parts, nullptr, /*force*/ true);
        Shape.setValue(res);
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}

void SheetMetalFlatPattern::exportDXF(const char* path) const
{
    SheetMetalExtension* sheet = nullptr;
    const auto flat = buildFlat(sheet);
    Base::FileInfo fi(path);
    Base::ofstream out(fi, std::ios::out | std::ios::trunc);
    if (!out) {
        FC_THROWM(Base::FileException, "Cannot write " << path);
    }
    SheetMetal::writeDXF(out, flat, TangentLines.getValue());
}

PyObject* SheetMetalFlatPattern::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        PythonObject = Py::Object(new SheetMetalFlatPatternPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}
