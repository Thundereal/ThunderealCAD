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
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Vec.hxx>

#include <App/Document.h>
#include <Base/Exception.h>

#include "FeatureSheetMetalFlatPattern.h"
#include "SheetMetal/Flat.h"
#include "SheetMetalExtension.h"

using namespace PartDesign;
using Part::TopoShape;

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
    Placement.setStatus(App::Property::Hidden, true);
}

App::DocumentObjectExecReturn* SheetMetalFlatPattern::execute()
{
    auto source = Source.getValue();
    if (!source) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The flat pattern has no part to lay flat"));
    }
    try {
        // the model of the sheet the part is, in the part's placement
        auto sheet = source->getExtensionByType<SheetMetalExtension>(true);
        if (!sheet) {
            sheet = SheetMetalExtension::findBefore(source);
        }
        if (!sheet || sheet->SheetModel.getValue().elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "The part is no sheet metal"));
        }
        SheetMetal::Model model = sheet->SheetModel.getValue();
        if (auto feature = dynamic_cast<Part::Feature*>(sheet->getExtendedObject())) {
            model.transform(TopoShape::convert(feature->Placement.getValue().toMatrix()));
        }
        const TopoShape folded = Part::Feature::getTopoShape(source);
        if (folded.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "The part has no shape"));
        }

        int root = -1;
        bool secondSkin = false;
        const auto& subs = RootFace.getSubValues();
        if (RootFace.getValue() && !subs.empty()) {
            const TopoShape face = Part::Feature::getTopoShape(RootFace.getValue(),
                                                               subs.front().c_str(),
                                                               /*needSubElement*/ true);
            if (face.isNull() || face.getShape().ShapeType() != TopAbs_FACE) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "The root face is not a face"));
            }
            root = model.owner(TopoDS::Face(face.getShape()));
            if (root < 0 || model.elements[root].kind != SheetMetal::Element::Kind::Wall) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "The root face is on no wall"));
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

        const auto flat = SheetMetal::flatten(folded.getShape(), model, root, secondSkin);
        std::vector<TopoShape> lines;
        for (const auto& line : flat.bendLines) {
            TopoShape edge(line.second);
            edge.setElementName(Data::IndexedName::fromConst("Edge", 1),
                                Data::MappedName(line.first));
            lines.push_back(edge);
        }
        TopoShape res(0, getDocument()->getStringHasher());
        res.makECompound({TopoShape(flat.solid),
                          TopoShape(0, getDocument()->getStringHasher())
                              .makECompound(lines, nullptr, /*force*/ true)},
                         nullptr,
                         /*force*/ true);
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
