/***************************************************************************
 *   Copyright (c) 2017 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GeomConvert.hxx>
#include <Geom_RectangularTrimmedSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepLProp_SLProps.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Precision.hxx>
#include <Standard_Version.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#endif

#include <App/Document.h>
#include <Base/Tools.h>

#include "ElementNames.h"
#include "FeatureExtend.h"
#include "Geom/EdgeCondition.h"
#include "Geom/Extend.h"


using namespace Surface;

const App::PropertyIntegerConstraint::Constraints SampleRange = {2, INT_MAX, 1};
const App::PropertyFloatConstraint::Constraints ToleranceRange = {0.0, 10.0, 0.01};
const App::PropertyFloatConstraint::Constraints ExtendRange = {-0.5, 10.0, 0.01};
const char* ModeEnums[] = {"Face", "Edges", nullptr};
const char* ExtensionEnums[] = {"Natural", "Linear", nullptr};
PROPERTY_SOURCE(Surface::Extend, Part::Spline)

Extend::Extend()
{
    ADD_PROPERTY(Face, (nullptr));
    Face.setScope(App::LinkScope::Global);
    ADD_PROPERTY(Tolerance, (0.1));
    Tolerance.setConstraints(&ToleranceRange);

    ADD_PROPERTY(ExtendUNeg, (0.05));
    ExtendUNeg.setConstraints(&ExtendRange);
    ADD_PROPERTY(ExtendUPos, (0.05));
    ExtendUPos.setConstraints(&ExtendRange);
    ADD_PROPERTY(ExtendUSymetric, (true));

    ADD_PROPERTY(ExtendVNeg, (0.05));
    ExtendVNeg.setConstraints(&ExtendRange);
    ADD_PROPERTY(ExtendVPos, (0.05));
    ExtendVPos.setConstraints(&ExtendRange);
    ADD_PROPERTY(ExtendVSymetric, (true));

    ADD_PROPERTY(SampleU, (32));
    SampleU.setConstraints(&SampleRange);
    ADD_PROPERTY(SampleV, (32));
    SampleV.setConstraints(&SampleRange);

    ADD_PROPERTY_TYPE(Mode,
                      ((long)0),
                      "Edges",
                      App::Prop_None,
                      "Face: the whole face refitted larger by the U and V fractions.\n"
                      "Edges: the face carried on across each of Edges by Length");
    Mode.setEnums(ModeEnums);
    ADD_PROPERTY_TYPE(Edges,
                      (nullptr),
                      "Edges",
                      App::Prop_None,
                      "Edges of the face to extend across; each has to be on the boundary of "
                      "its surface");
    Edges.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Length,
                      (5.0),
                      "Edges",
                      App::Prop_None,
                      "How far past each edge, along the surface across it, on average");
    ADD_PROPERTY_TYPE(Extension,
                      ((long)0),
                      "Edges",
                      App::Prop_None,
                      "Natural: the same surface carried on, smooth with itself (G2 and "
                      "more).\nLinear: straight on, tangent to the surface (G1)");
    Extension.setEnums(ExtensionEnums);
}

short Extend::mustExecute() const
{
    if (Face.isTouched()) {
        return 1;
    }
    if (ExtendUNeg.isTouched()) {
        return 1;
    }
    if (ExtendUPos.isTouched()) {
        return 1;
    }
    if (ExtendVNeg.isTouched()) {
        return 1;
    }
    if (ExtendVPos.isTouched()) {
        return 1;
    }
    if (Mode.isTouched() || Edges.isTouched() || Length.isTouched() || Extension.isTouched()) {
        return 1;
    }
    return 0;
}

App::DocumentObjectExecReturn* Extend::execute()
{
    App::DocumentObject* part = Face.getValue();
    if (!part || !part->isDerivedFrom<Part::Feature>()) {
        return new App::DocumentObjectExecReturn("No shape linked.");
    }
    const auto& faces = Face.getSubValues();
    if (faces.size() != 1) {
        return new App::DocumentObjectExecReturn("Not exactly one sub-shape linked.");
    }

    auto input = Part::Feature::getTopoShape(part, faces[0].c_str(), /*needSubElement*/ true);
    const TopoDS_Shape& shape = input.getShape();
    if (shape.IsNull() || shape.ShapeType() != TopAbs_FACE) {
        return new App::DocumentObjectExecReturn("Sub-shape is not a face.");
    }

    if (Mode.getValue() == 1) {
        return extendEdges(input);
    }

    const TopoDS_Face& face = TopoDS::Face(shape);
    BRepAdaptor_Surface adapt(face);
    double u1 = adapt.FirstUParameter();
    double u2 = adapt.LastUParameter();
    double v1 = adapt.FirstVParameter();
    double v2 = adapt.LastVParameter();

    double ur = u2 - u1;
    double vr = v2 - v1;
    double eu1 = u1 - ur * ExtendUNeg.getValue();
    double eu2 = u2 + ur * ExtendUPos.getValue();
    double ev1 = v1 - vr * ExtendVNeg.getValue();
    double ev2 = v2 + vr * ExtendVPos.getValue();
    double eur = eu2 - eu1;
    double evr = ev2 - ev1;

    long numU = SampleU.getValue();
    long numV = SampleV.getValue();
    TColgp_Array2OfPnt approxPoints(1, numU, 1, numV);
    for (long u = 0; u < numU; u++) {
        double uu = eu1 + u * eur / (numU - 1);
        for (long v = 0; v < numV; v++) {
            double vv = ev1 + v * evr / (numV - 1);
            BRepLProp_SLProps prop(adapt, uu, vv, 0, Precision::Confusion());
            const gp_Pnt& pnt = prop.Value();
            approxPoints(u + 1, v + 1) = pnt;
        }
    }

    Approx_ParametrizationType ParType = Approx_ChordLength;
    Standard_Integer DegMin = 3;
    Standard_Integer DegMax = 5;
    GeomAbs_Shape Continuity = GeomAbs_C2;
    Standard_Real Tol3d = Tolerance.getValue();

    GeomAPI_PointsToBSplineSurface approx;
    approx.Init(approxPoints, ParType, DegMin, DegMax, Continuity, Tol3d);

    Handle(Geom_BSplineSurface) surface(approx.Surface());
    BRepBuilderAPI_MakeFace mkFace(surface, Precision::Confusion());

    // a refit: the face is the input face modified, the rest is named from it
    Part::ShapeMapper mapper;
    mapper.insert(false, shape, mkFace.Face());
    Part::TopoShape result(0, getDocument()->getStringHasher());
    result.makESHAPE(mkFace.Face(), mapper, {input}, OpCodes::Extend);
    Shape.setValue(result);

    return StdReturn;
}

App::DocumentObjectExecReturn* Extend::extendEdges(const Part::TopoShape& input)
{
    const TopoDS_Face& face = TopoDS::Face(input.getShape());
    // the face's surface over its parameter box, as a B-spline where it is
    // placed; its boundary is what can be extended
    double u1, u2, v1, v2;
    BRepTools::UVBounds(face, u1, u2, v1, v2);
    TopLoc_Location loc;
    Handle(Geom_Surface) base = BRep_Tool::Surface(face, loc);
    Handle(Geom_BSplineSurface) surface = Handle(Geom_BSplineSurface)::DownCast(base);
    try {
        if (surface.IsNull()) {
            surface = GeomConvert::SurfaceToBSplineSurface(
                new Geom_RectangularTrimmedSurface(base, u1, u2, v1, v2));
        }
        else {
            surface = Handle(Geom_BSplineSurface)::DownCast(surface->Copy());
        }
        if (!loc.IsIdentity()) {
            surface->Transform(loc.Transformation());
        }
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(
            std::string("The face's surface cannot be had as a B-spline: ") + e.what());
    }

    const auto& objs = Edges.getValues();
    const auto& subs = Edges.getSubValues();
    if (objs.empty()) {
        return new App::DocumentObjectExecReturn("No edge to extend across");
    }
    const auto mode = Extension.getValue() == 1 ? Geom::Extension::Linear
                                                : Geom::Extension::Natural;
    std::vector<Part::TopoShape> inputs {input};
    for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
        auto edge = Part::Feature::getTopoShape(objs[i], subs[i].c_str(), true);
        const std::string name = std::string(objs[i]->Label.getValue()) + "." + subs[i];
        if (edge.isNull() || edge.getShape().ShapeType() != TopAbs_EDGE) {
            return new App::DocumentObjectExecReturn(name + " is not an edge");
        }
        BRepAdaptor_Curve adapt(TopoDS::Edge(edge.getShape()));
        Handle(Geom_Curve) curve = new Geom_TrimmedCurve(adapt.Curve().Curve(),
                                                         adapt.FirstParameter(),
                                                         adapt.LastParameter());
        if (!edge.getShape().Location().IsIdentity()) {
            curve->Transform(edge.getShape().Location().Transformation());
        }
        int b = Geom::boundaryOf(surface, curve, 1e-6 + BRep_Tool::Tolerance(face));
        if (b < 0) {
            return new App::DocumentObjectExecReturn(
                name + " is not on the boundary of the face's surface; untrim the face "
                       "to extend across it");
        }
        auto error = Geom::extendBoundary(surface, Geom::Boundary(b), Length.getValue(), mode);
        if (!error.empty()) {
            return new App::DocumentObjectExecReturn(name + ": " + error);
        }
        inputs.push_back(edge);
    }

    BRepBuilderAPI_MakeFace mkFace(surface, Precision::Confusion());
    // the face carried on: the input face modified, the rest named from it
    Part::ShapeMapper mapper;
    mapper.insert(false, input.getShape(), mkFace.Face());
    Part::TopoShape result(0, getDocument()->getStringHasher());
    result.makESHAPE(mkFace.Face(), mapper, {input}, OpCodes::Extend);
    Shape.setValue(result);
    return StdReturn;
}

void Extend::onChanged(const App::Property* prop)
{
    // using a mutex and lock to protect a recursive calling when setting the new values
    if (lockOnChangeMutex) {
        return;
    }
    Base::StateLocker lock(lockOnChangeMutex);

    if (ExtendUSymetric.getValue()) {
        if (prop == &ExtendUNeg || prop == &ExtendUPos) {
            auto changedValue = dynamic_cast<const App::PropertyFloat*>(prop);
            if (changedValue) {
                ExtendUNeg.setValue(changedValue->getValue());
                ExtendUPos.setValue(changedValue->getValue());
            }
        }
    }

    if (ExtendVSymetric.getValue()) {
        if (prop == &ExtendVNeg || prop == &ExtendVPos) {
            auto changedValue = dynamic_cast<const App::PropertyFloat*>(prop);
            if (changedValue) {
                ExtendVNeg.setValue(changedValue->getValue());
                ExtendVPos.setValue(changedValue->getValue());
            }
        }
    }
    Part::Spline::onChanged(prop);
}

void Extend::handleChangedPropertyName(Base::XMLReader& reader,
                                       const char* TypeName,
                                       const char* PropName)
{
    Base::Type type = Base::Type::fromName(TypeName);
    if (App::PropertyFloatConstraint::getClassTypeId() == type
        && strcmp(PropName, "ExtendU") == 0) {
        App::PropertyFloatConstraint v;
        v.Restore(reader);
        ExtendUNeg.setValue(v.getValue());
        ExtendUPos.setValue(v.getValue());
    }
    else if (App::PropertyFloatConstraint::getClassTypeId() == type
             && strcmp(PropName, "ExtendV") == 0) {
        App::PropertyFloatConstraint v;
        v.Restore(reader);
        ExtendVNeg.setValue(v.getValue());
        ExtendVPos.setValue(v.getValue());
    }
    else {
        Part::Spline::handleChangedPropertyName(reader, TypeName, PropName);
    }
}
