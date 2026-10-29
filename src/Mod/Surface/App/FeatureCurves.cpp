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
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepFill.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomConvert.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#endif

#include <App/Document.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "ElementNames.h"
#include "FeatureCurves.h"
#include "Geom/Curves.h"
#include "Geom/Trim.h"


using namespace Surface;

PROPERTY_SOURCE(Surface::Isocurve, Part::Feature)
PROPERTY_SOURCE(Surface::CurveOnSurface, Part::Feature)
PROPERTY_SOURCE(Surface::IntersectionCurve, Part::Feature)
PROPERTY_SOURCE(Surface::JoinCurves, Part::Feature)
PROPERTY_SOURCE(Surface::ExtendCurve, Part::Feature)

namespace
{

const App::PropertyIntegerConstraint::Constraints CountRange = {0, 1000, 1};
const char* ExtensionEnums[] = {"Natural", "Linear", nullptr};

/// The shapes a link list names, whole objects whole
std::vector<Part::TopoShape> shapesOf(const App::PropertyLinkSubList& prop)
{
    std::vector<Part::TopoShape> out;
    const auto& objs = prop.getValues();
    const auto& subs = prop.getSubValues();
    for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
        auto s = Part::Feature::getTopoShape(objs[i], subs[i].c_str(), true);
        if (!s.isNull()) {
            out.push_back(s);
        }
    }
    return out;
}

/// Edges named after the input edges they lie on
Part::TopoShape named(const TopoDS_Shape& result,
                      const std::vector<Part::TopoShape>& inputs,
                      App::Document* doc,
                      const char* op,
                      double tol)
{
    return Surface::nameFromEdges(result, inputs, doc->getStringHasher(), op, tol);
}

TopoDS_Compound compoundOf(const std::vector<TopoDS_Shape>& shapes)
{
    BRep_Builder b;
    TopoDS_Compound c;
    b.MakeCompound(c);
    for (const auto& s : shapes) {
        b.Add(c, s);
    }
    return c;
}

/// A ribbon `width` wide along `edge` on `face`: ruled from the edge across
/// it in the face's tangent plane, so it is tangent to the face there
TopoDS_Shape ribbon(const TopoDS_Edge& edge, const TopoDS_Face& face, double width)
{
    BRepAdaptor_Curve c(edge);
    TopLoc_Location loc;
    Handle(Geom_Surface) s = BRep_Tool::Surface(face, loc);
    if (!loc.IsIdentity()) {
        s = Handle(Geom_Surface)::DownCast(s->Transformed(loc.Transformation()));
    }
    const int n = 25;
    Handle(TColgp_HArray1OfPnt) along = new TColgp_HArray1OfPnt(1, n);
    Handle(TColgp_HArray1OfPnt) across = new TColgp_HArray1OfPnt(1, n);
    for (int k = 0; k < n; ++k) {
        double t = c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * k / (n - 1);
        gp_Pnt p;
        gp_Vec d;
        c.D1(t, p, d);
        GeomAPI_ProjectPointOnSurf pr(p, s);
        double u = 0, v = 0;
        if (pr.NbPoints() > 0) {
            pr.LowerDistanceParameters(u, v);
        }
        gp_Pnt q;
        gp_Vec du, dv;
        s->D1(u, v, q, du, dv);
        gp_Vec side = du.Crossed(dv).Crossed(d);
        if (side.Magnitude() > 1e-12) {
            side.Normalize();
        }
        along->SetValue(k + 1, p);
        across->SetValue(k + 1, p.Translated(side * width));
    }
    GeomAPI_Interpolate a(along, false, Precision::Confusion());
    GeomAPI_Interpolate b(across, false, Precision::Confusion());
    a.Perform();
    b.Perform();
    if (!a.IsDone() || !b.IsDone()) {
        return {};
    }
    return BRepFill::Face(BRepBuilderAPI_MakeEdge(a.Curve()), BRepBuilderAPI_MakeEdge(b.Curve()));
}

}  // namespace

// -- Isocurve ----------------------------------------------------------------

Isocurve::Isocurve()
{
    ADD_PROPERTY_TYPE(Face, (nullptr), "Isocurve", App::Prop_None, "The face to draw them on");
    Face.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(NumberU,
                      (3),
                      "Isocurve",
                      App::Prop_None,
                      "How many curves of constant U, evenly inside the face's parameters");
    NumberU.setConstraints(&CountRange);
    ADD_PROPERTY_TYPE(NumberV, (3), "Isocurve", App::Prop_None, "How many of constant V");
    NumberV.setConstraints(&CountRange);
}

App::DocumentObjectExecReturn* Isocurve::execute()
{
    const auto& subs = Face.getSubValues();
    auto face = Part::Feature::getTopoShape(Face.getValue(),
                                            subs.empty() ? "" : subs.front().c_str(),
                                            true);
    if (face.isNull() || face.getShape().ShapeType() != TopAbs_FACE) {
        return new App::DocumentObjectExecReturn("Face is not a face");
    }
    try {
        auto edges = Geom::isocurves(TopoDS::Face(face.getShape()),
                                     NumberU.getValue(),
                                     NumberV.getValue());
        if (edges.empty()) {
            return new App::DocumentObjectExecReturn("No curve: the numbers are 0");
        }
        std::vector<TopoDS_Shape> shapes(edges.begin(), edges.end());
        Part::TopoShape result(0, getDocument()->getStringHasher());
        Part::ShapeMapper mapper;
        for (const auto& e : edges) {
            mapper.populate(true, face, std::vector<Part::TopoShape> {Part::TopoShape(e)});
        }
        result.makESHAPE(compoundOf(shapes), mapper, {face}, OpCodes::Isocurve);
        Shape.setValue(result);
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(std::string("Isocurve failed: ") + e.what());
    }
    return StdReturn;
}

// -- CurveOnSurface ----------------------------------------------------------

CurveOnSurface::CurveOnSurface()
{
    ADD_PROPERTY_TYPE(Curves,
                      (nullptr),
                      "CurveOnSurface",
                      App::Prop_None,
                      "Curves to lay on the faces: edges, wires, a sketch");
    Curves.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Faces,
                      (nullptr),
                      "CurveOnSurface",
                      App::Prop_None,
                      "The faces to lay them on");
    Faces.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Direction,
                      (Base::Vector3d()),
                      "CurveOnSurface",
                      App::Prop_None,
                      "Along what they are laid on; none, along the faces' normals");
    ADD_PROPERTY_TYPE(Ribbon,
                      (0.0),
                      "CurveOnSurface",
                      App::Prop_None,
                      "A face this wide along each curve, tangent to the face it is on: a "
                      "support for an edge condition where there is no face; 0, none");
}

App::DocumentObjectExecReturn* CurveOnSurface::execute()
{
    auto curves = shapesOf(Curves);
    auto faces = shapesOf(Faces);
    if (curves.empty() || faces.empty()) {
        return new App::DocumentObjectExecReturn("Curves and faces are both needed");
    }
    const auto& d = Direction.getValue();
    gp_Dir dir;
    const gp_Dir* along = nullptr;
    if (d.Length() > Precision::Confusion()) {
        dir = gp_Dir(d.x, d.y, d.z);
        along = &dir;
    }
    std::vector<TopoDS_Shape> tools;
    for (const auto& c : curves) {
        tools.push_back(c.getShape());
    }
    std::vector<TopoDS_Shape> out;
    std::vector<Part::TopoShape> inputs = curves;
    try {
        for (const auto& f : faces) {
            for (TopExp_Explorer x(f.getShape(), TopAbs_FACE); x.More(); x.Next()) {
                const TopoDS_Face& face = TopoDS::Face(x.Current());
                for (const auto& e : Geom::projectOnto(face, tools, along, 1e-7)) {
                    out.push_back(e);
                    if (Ribbon.getValue() > Precision::Confusion()) {
                        auto r = ribbon(TopoDS::Edge(e), face, Ribbon.getValue());
                        if (!r.IsNull()) {
                            out.push_back(r);
                        }
                    }
                }
            }
            inputs.push_back(f);
        }
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(std::string("Laying the curves failed: ")
                                                 + e.what());
    }
    if (out.empty()) {
        return new App::DocumentObjectExecReturn(
            "The curves miss the faces along the direction given");
    }
    Shape.setValue(named(compoundOf(out), curves, getDocument(), OpCodes::CurveOnSurface, 1e-4));
    return StdReturn;
}

// -- IntersectionCurve -------------------------------------------------------

IntersectionCurve::IntersectionCurve()
{
    ADD_PROPERTY_TYPE(Shape1, (nullptr), "Intersection", App::Prop_None, "The first shape");
    Shape1.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Shape2, (nullptr), "Intersection", App::Prop_None, "The second shape");
    Shape2.setScope(App::LinkScope::Global);
}

App::DocumentObjectExecReturn* IntersectionCurve::execute()
{
    auto a = shapesOf(Shape1), b = shapesOf(Shape2);
    if (a.empty() || b.empty()) {
        return new App::DocumentObjectExecReturn("Two shapes are needed");
    }
    Part::TopoShape first, second;
    first.makECompound(a, nullptr, false);
    second.makECompound(b, nullptr, false);
    try {
        BRepAlgoAPI_Section section(first.getShape(), second.getShape(), false);
        section.Approximation(true);
        section.ComputePCurveOn1(true);
        section.ComputePCurveOn2(true);
        section.Build();
        if (!section.IsDone()) {
            return new App::DocumentObjectExecReturn("The shapes could not be intersected");
        }
        if (TopExp_Explorer(section.Shape(), TopAbs_EDGE).More() == false) {
            return new App::DocumentObjectExecReturn("The shapes do not meet");
        }
        Part::TopoShape result(0, getDocument()->getStringHasher());
        result.makEShape(section, {first, second}, Part::OpCodes::Section);
        Shape.setValue(result);
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(std::string("The intersection failed: ")
                                                 + e.what());
    }
    return StdReturn;
}

// -- JoinCurves --------------------------------------------------------------

JoinCurves::JoinCurves()
{
    ADD_PROPERTY_TYPE(Curves,
                      (nullptr),
                      "Join",
                      App::Prop_None,
                      "Edges to join, in any order and either way round, or whole objects");
    Curves.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Tolerance,
                      (1e-4),
                      "Join",
                      App::Prop_None,
                      "How far a joined curve may stray from the edges");
    ADD_PROPERTY_TYPE(CornerAngle,
                      (5.0),
                      "Join",
                      App::Prop_None,
                      "Where the chain turns by more than this, it is broken into two curves");
    ADD_PROPERTY_TYPE(Gap,
                      (1e-4),
                      "Join",
                      App::Prop_None,
                      "How far apart two ends may be and still be joined");
}

App::DocumentObjectExecReturn* JoinCurves::execute()
{
    auto inputs = shapesOf(Curves);
    std::vector<TopoDS_Edge> edges;
    for (const auto& s : inputs) {
        for (TopExp_Explorer x(s.getShape(), TopAbs_EDGE); x.More(); x.Next()) {
            edges.push_back(TopoDS::Edge(x.Current()));
        }
    }
    std::string error;
    auto curves = Geom::joinCurves(edges,
                                   Tolerance.getValue(),
                                   CornerAngle.getValue() * M_PI / 180,
                                   Gap.getValue(),
                                   error);
    if (curves.empty()) {
        return new App::DocumentObjectExecReturn(error);
    }
    std::vector<TopoDS_Shape> out;
    for (const auto& c : curves) {
        out.push_back(BRepBuilderAPI_MakeEdge(c).Edge());
    }
    Shape.setValue(named(compoundOf(out),
                         inputs,
                         getDocument(),
                         OpCodes::JoinCurves,
                         2 * Tolerance.getValue() + Precision::Confusion()));
    return StdReturn;
}

// -- ExtendCurve -------------------------------------------------------------

ExtendCurve::ExtendCurve()
{
    ADD_PROPERTY_TYPE(Curve, (nullptr), "Extend", App::Prop_None, "The edge to extend");
    Curve.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(StartLength, (0.0), "Extend", App::Prop_None, "How far past its start");
    ADD_PROPERTY_TYPE(EndLength, (5.0), "Extend", App::Prop_None, "How far past its end");
    ADD_PROPERTY_TYPE(Extension,
                      ((long)0),
                      "Extend",
                      App::Prop_None,
                      "Natural: the same curve carried on, smooth with itself (G2 and "
                      "more).\nLinear: straight on along its tangent (G1)");
    Extension.setEnums(ExtensionEnums);
}

App::DocumentObjectExecReturn* ExtendCurve::execute()
{
    const auto& subs = Curve.getSubValues();
    auto edge = Part::Feature::getTopoShape(Curve.getValue(),
                                            subs.empty() ? "" : subs.front().c_str(),
                                            true);
    if (edge.isNull() || edge.getShape().ShapeType() != TopAbs_EDGE) {
        return new App::DocumentObjectExecReturn("Curve is not an edge");
    }
    BRepAdaptor_Curve adapt(TopoDS::Edge(edge.getShape()));
    Handle(Geom_Curve) base = new Geom_TrimmedCurve(adapt.Curve().Curve(),
                                                    adapt.FirstParameter(),
                                                    adapt.LastParameter());
    if (!edge.getShape().Location().IsIdentity()) {
        base->Transform(edge.getShape().Location().Transformation());
    }
    const auto mode = Extension.getValue() == 1 ? Geom::Extension::Linear
                                                : Geom::Extension::Natural;
    try {
        Handle(Geom_BSplineCurve) c = GeomConvert::CurveToBSplineCurve(base);
        for (int end = 0; end < 2; ++end) {
            double length = end ? EndLength.getValue() : StartLength.getValue();
            if (length <= Precision::Confusion()) {
                continue;
            }
            auto error = Geom::extendCurve(c, end == 1, length, mode);
            if (!error.empty()) {
                return new App::DocumentObjectExecReturn(error);
            }
        }
        Shape.setValue(named(BRepBuilderAPI_MakeEdge(c).Edge(),
                             {edge},
                             getDocument(),
                             OpCodes::ExtendCurve,
                             1e-6));
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(std::string("The extension failed: ") + e.what());
    }
    return StdReturn;
}
