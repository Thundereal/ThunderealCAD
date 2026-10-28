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

#ifndef _PreComp_
#include <algorithm>

#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Geom_RectangularTrimmedSurface.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#endif

#include <App/Document.h>

#include "ElementNames.h"
#include "FeatureTrim.h"
#include "Geom/Trim.h"


using namespace Surface;

PROPERTY_SOURCE(Surface::Split, Part::Feature)
PROPERTY_SOURCE(Surface::Trim, Surface::Split)
PROPERTY_SOURCE(Surface::Untrim, Part::Spline)

namespace
{

/// The faces of each link, a whole object's all of them, with the shape
/// they were taken from
void gatherFaces(const App::PropertyLinkSubList& prop,
                 std::vector<Part::TopoShape>& faces,
                 std::vector<Part::TopoShape>& curves)
{
    const auto& objs = prop.getValues();
    const auto& subs = prop.getSubValues();
    for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
        auto shape = Part::Feature::getTopoShape(objs[i], subs[i].c_str(), true);
        if (shape.isNull()) {
            continue;
        }
        if (shape.countSubShapes(TopAbs_FACE) > 0) {
            for (auto& f : shape.getSubTopoShapes(TopAbs_FACE)) {
                faces.push_back(f);
            }
        }
        else if (shape.countSubShapes(TopAbs_EDGE) > 0) {
            curves.push_back(shape);
        }
    }
}

/// Points along the edges of `face`, which lie on any face it was cut from
std::vector<gp_Pnt> edgePoints(const TopoDS_Face& face)
{
    std::vector<gp_Pnt> pts;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(face, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge& e = TopoDS::Edge(edges(i));
        if (BRep_Tool::Degenerated(e)) {
            continue;
        }
        BRepAdaptor_Curve c(e);
        pts.push_back(c.Value((c.FirstParameter() + c.LastParameter()) / 2));
    }
    return pts;
}

double distance(const gp_Pnt& p, const TopoDS_Shape& s)
{
    BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(p).Vertex(), s);
    return d.IsDone() ? d.Value() : 1e100;
}

}  // namespace

// -- Split -------------------------------------------------------------------

Split::Split()
{
    ADD_PROPERTY_TYPE(Base,
                      (nullptr),
                      "Split",
                      App::Prop_None,
                      "Faces to cut: faces, shells, or whole objects (all their faces)");
    Base.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Tools,
                      (nullptr),
                      "Split",
                      App::Prop_None,
                      "What cuts them: curves (edges, wires, a sketch), laid onto the faces, "
                      "or surfaces");
    Tools.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Direction,
                      (Base::Vector3d()),
                      "Split",
                      App::Prop_None,
                      "Along what the curves are laid onto the faces; none, along the "
                      "faces' normals");
    ADD_PROPERTY_TYPE(Mutual,
                      (false),
                      "Split",
                      App::Prop_None,
                      "The tools' surfaces are cut by the faces as well, and are pieces too");
    ADD_PROPERTY_TYPE(Pieces,
                      (0),
                      "Split",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Output),
                      "How many pieces the cut made");
}

const char* Split::opCode() const
{
    return OpCodes::Split;
}

std::vector<Part::TopoShape> Split::baseFaces() const
{
    std::vector<Part::TopoShape> faces, curves;
    gatherFaces(Base, faces, curves);
    return faces;
}

std::string Split::split(Part::TopoShape& pieces, std::vector<Part::TopoShape>& inputs) const
{
    std::vector<Part::TopoShape> bases, ignored, toolFaces, toolCurves;
    gatherFaces(Base, bases, ignored);
    if (bases.empty()) {
        return "No face to cut";
    }
    gatherFaces(Tools, toolFaces, toolCurves);
    if (toolFaces.empty() && toolCurves.empty()) {
        return "Nothing to cut with";
    }

    const Base::Vector3d& dir = Direction.getValue();
    gp_Dir along;
    const gp_Dir* direction = nullptr;
    if (dir.Length() > Precision::Confusion()) {
        along = gp_Dir(dir.x, dir.y, dir.z);
        direction = &along;
    }

    TopTools_ListOfShape args, tools;
    std::vector<TopoDS_Shape> curves;
    for (const auto& c : toolCurves) {
        curves.push_back(c.getShape());
    }
    for (const auto& f : bases) {
        args.Append(f.getShape());
        // the curves laid onto each face they cut
        for (const auto& e :
             Geom::projectOnto(TopoDS::Face(f.getShape()), curves, direction, 1e-7)) {
            tools.Append(e);
        }
    }
    for (const auto& f : toolFaces) {
        tools.Append(f.getShape());
        if (Mutual.getValue()) {
            args.Append(f.getShape());
        }
    }
    if (Mutual.getValue()) {
        for (const auto& f : bases) {
            tools.Append(f.getShape());
        }
    }
    if (tools.IsEmpty()) {
        return "The curves miss the faces: none of them lies over a face along the "
               "direction given";
    }

    inputs = bases;
    inputs.insert(inputs.end(), toolFaces.begin(), toolFaces.end());
    inputs.insert(inputs.end(), toolCurves.begin(), toolCurves.end());
    try {
        BRepAlgoAPI_Splitter splitter;
        splitter.SetArguments(args);
        splitter.SetTools(tools);
        splitter.Build();
        if (!splitter.IsDone()) {
            return "The faces could not be cut";
        }
        pieces = Part::TopoShape(0, getDocument()->getStringHasher());
        pieces.makEShape(splitter, inputs, opCode());
    }
    catch (Standard_Failure& e) {
        return std::string("The cut failed: ") + e.what();
    }
    return {};
}

App::DocumentObjectExecReturn* Split::execute()
{
    Part::TopoShape pieces;
    std::vector<Part::TopoShape> inputs;
    auto error = split(pieces, inputs);
    if (!error.empty()) {
        return new App::DocumentObjectExecReturn(error);
    }
    Pieces.setValue((long)pieces.countSubShapes(TopAbs_FACE));
    Shape.setValue(pieces);
    return StdReturn;
}

// -- Trim --------------------------------------------------------------------

Trim::Trim()
{
    ADD_PROPERTY_TYPE(KeepPoints,
                      (),
                      "Trim",
                      App::Prop_None,
                      "The sides kept: the piece nearest each point");
    ADD_PROPERTY_TYPE(KeepElements,
                      (),
                      "Trim",
                      App::Prop_None,
                      "The names of the pieces when they were picked, one for each of "
                      "KeepPoints: which to keep of two as near");
    ADD_PROPERTY_TYPE(ShowPieces,
                      (false),
                      "Trim",
                      App::PropertyType(App::Prop_Hidden | App::Prop_Transient),
                      "Every piece shown, for the sides to keep to be picked from");
}

const char* Trim::opCode() const
{
    return OpCodes::Trim;
}

App::DocumentObjectExecReturn* Trim::execute()
{
    if (ShowPieces.getValue()) {
        return Split::execute();
    }
    const auto& points = KeepPoints.getValues();
    if (points.empty()) {
        return new App::DocumentObjectExecReturn(
            "Nothing to keep: pick a side of the cut to keep");
    }
    Part::TopoShape pieces;
    std::vector<Part::TopoShape> inputs;
    auto error = split(pieces, inputs);
    if (!error.empty()) {
        return new App::DocumentObjectExecReturn(error);
    }
    const int count = (int)pieces.countSubShapes(TopAbs_FACE);
    Pieces.setValue((long)count);
    std::vector<TopoDS_Face> faces;
    for (int i = 1; i <= count; ++i) {
        faces.push_back(TopoDS::Face(pieces.getSubShape(TopAbs_FACE, i)));
    }

    const auto& names = KeepElements.getValues();
    std::vector<int> kept;
    const double tol = 1e-7;
    for (std::size_t k = 0; k < points.size(); ++k) {
        const auto& p = points[k];
        int tie = -1;
        int best = Geom::nearestPiece(faces, gp_Pnt(p.x, p.y, p.z), tol, &tie);
        if (best < 0) {
            continue;
        }
        // a point as near to two: the one that had the name it was picked by
        if (tie >= 0 && k < names.size() && !names[k].empty()) {
            auto name = [&](int i) {
                return pieces
                    .getMappedName(Data::IndexedName::fromConst("Face", i + 1), false)
                    .toString();
            };
            if (name(tie) == names[k]) {
                best = tie;
            }
        }
        if (std::find(kept.begin(), kept.end(), best) == kept.end()) {
            kept.push_back(best);
        }
    }
    std::sort(kept.begin(), kept.end());
    std::vector<Part::TopoShape> keep;
    for (int i : kept) {
        keep.push_back(pieces.getSubTopoShape(TopAbs_FACE, i + 1));
    }
    Part::TopoShape result(0, getDocument()->getStringHasher());
    if (keep.size() == 1) {
        result = keep.front();
    }
    else {
        result.makECompound(keep, nullptr, false);
    }
    Shape.setValue(result);
    return StdReturn;
}

// -- Untrim ------------------------------------------------------------------

Untrim::Untrim()
{
    ADD_PROPERTY_TYPE(Face, (nullptr), "Untrim", App::Prop_None, "The face to untrim");
    Face.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Before,
                      (nullptr),
                      "Untrim",
                      App::Prop_None,
                      "A trim or split the face came from: the face as it was before it. "
                      "None: the face's surface over its own bounds, or for a surface "
                      "without any (a plane), over the face's parameter box");
    Before.setScope(App::LinkScope::Global);
}

App::DocumentObjectExecReturn* Untrim::execute()
{
    App::DocumentObject* obj = Face.getValue();
    const auto& subs = Face.getSubValues();
    auto input = Part::Feature::getTopoShape(obj,
                                             subs.empty() ? "" : subs.front().c_str(),
                                             true);
    if (input.isNull() || input.getShape().ShapeType() != TopAbs_FACE) {
        return new App::DocumentObjectExecReturn("Face is not a face");
    }
    const TopoDS_Face& face = TopoDS::Face(input.getShape());

    if (auto before = dynamic_cast<Split*>(Before.getValue())) {
        // the face of the cut's input that this one lies on
        auto pts = edgePoints(face);
        for (const auto& candidate : before->baseFaces()) {
            bool on = !pts.empty();
            for (const auto& p : pts) {
                on = on && distance(p, candidate.getShape()) < 1e-6;
            }
            if (on) {
                Part::ShapeMapper mapper;
                mapper.insert(false, face, candidate.getShape());
                Part::TopoShape result(0, getDocument()->getStringHasher());
                result.makESHAPE(candidate.getShape(), mapper, {input, candidate}, OpCodes::Untrim);
                Shape.setValue(result);
                return StdReturn;
            }
        }
        return new App::DocumentObjectExecReturn(
            std::string("The face is not cut from a face of ") + before->Label.getValue());
    }
    else if (Before.getValue()) {
        return new App::DocumentObjectExecReturn("Before is not a trim or a split");
    }

    // its surface over its own bounds, or a surface without any over the
    // face's parameter box
    TopLoc_Location loc;
    Handle(Geom_Surface) surface = BRep_Tool::Surface(face, loc);
    double u1, u2, v1, v2;
    surface->Bounds(u1, u2, v1, v2);
    double fu1, fu2, fv1, fv2;
    BRepTools::UVBounds(face, fu1, fu2, fv1, fv2);
    if (Precision::IsInfinite(u1) || Precision::IsInfinite(u2)) {
        u1 = fu1, u2 = fu2;
    }
    if (Precision::IsInfinite(v1) || Precision::IsInfinite(v2)) {
        v1 = fv1, v2 = fv2;
    }
    try {
        BRepBuilderAPI_MakeFace mk(surface, u1, u2, v1, v2, Precision::Confusion());
        if (!mk.IsDone()) {
            return new App::DocumentObjectExecReturn("The face's surface makes no face");
        }
        TopoDS_Face untrimmed = mk.Face();
        untrimmed.Location(loc);
        Part::ShapeMapper mapper;
        mapper.insert(false, face, untrimmed);
        Part::TopoShape result(0, getDocument()->getStringHasher());
        result.makESHAPE(untrimmed, mapper, {input}, OpCodes::Untrim);
        Shape.setValue(result);
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(std::string("Untrim failed: ") + e.what());
    }
    return StdReturn;
}
