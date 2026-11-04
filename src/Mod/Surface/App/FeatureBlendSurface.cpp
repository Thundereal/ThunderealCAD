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
#include <BRepBuilderAPI_MakeFace.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>
#endif

#include <App/Document.h>

#include "ElementNames.h"
#include "FeatureBlendSurface.h"
#include "Geom/Blend.h"


using namespace Surface;

PROPERTY_SOURCE(Surface::BlendSurface, Part::Spline)

namespace
{
const App::PropertyIntegerConstraint::Constraints SampleRange = {3, 50, 1};
}

BlendSurface::BlendSurface()
{
    ADD_PROPERTY_TYPE(Edge1, (nullptr), "Blend", App::Prop_None, "The first edge to bridge");
    Edge1.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Edge2, (nullptr), "Blend", App::Prop_None, "The second edge to bridge");
    Edge2.setScope(App::LinkScope::Global);
    SURFACE_ADD_CONDITION(Side1, "the first edge");
    SURFACE_ADD_WEIGHT_END(Side1, "the first edge");
    SURFACE_ADD_CONDITION(Side2, "the second edge");
    SURFACE_ADD_WEIGHT_END(Side2, "the second edge");
    // a blend meets its faces tangent unless told otherwise
    Side1Continuity.setValue(1L);
    Side2Continuity.setValue(1L);
    ADD_PROPERTY_TYPE(Samples,
                      (7),
                      "Blend",
                      App::Prop_None,
                      "How many curves are laid across the gap to shape it");
    Samples.setConstraints(&SampleRange);
    ADD_PROPERTY_TYPE(Achieved,
                      (),
                      "Conditions",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Output),
                      "What each side achieved");
}

App::DocumentObjectExecReturn* BlendSurface::execute()
{
    ConditionProperties sides[2] = {
        {"first edge", &Side1Continuity, &Side1Weight, &Side1Support, &Side1WeightEnd},
        {"second edge", &Side2Continuity, &Side2Weight, &Side2Support, &Side2WeightEnd}};
    App::PropertyLinkSub* links[2] = {&Edge1, &Edge2};
    Geom::BlendSide blend[2];
    std::vector<Part::TopoShape> inputs;
    for (int k = 0; k < 2; ++k) {
        App::DocumentObject* obj = links[k]->getValue();
        const auto& subs = links[k]->getSubValues();
        std::string sub = subs.empty() ? std::string() : subs.front();
        auto edge = Part::Feature::getTopoShape(obj, sub.c_str(), true);
        if (!obj || edge.isNull() || edge.getShape().ShapeType() != TopAbs_EDGE) {
            return new App::DocumentObjectExecReturn(std::string("The ") + sides[k].name
                                                     + " is not an edge");
        }
        BRepAdaptor_Curve adapt(TopoDS::Edge(edge.getShape()));
        Handle(Geom_Curve) curve = new Geom_TrimmedCurve(adapt.Curve().Curve(),
                                                         adapt.FirstParameter(),
                                                         adapt.LastParameter());
        if (!edge.getShape().Location().IsIdentity()) {
            curve->Transform(edge.getShape().Location().Transformation());
        }
        blend[k].curve = curve;
        auto error = conditionOf(sides[k],
                                 obj,
                                 sub,
                                 TopoDS::Edge(edge.getShape()),
                                 blend[k].condition);
        if (!error.empty()) {
            return new App::DocumentObjectExecReturn(error);
        }
        inputs.push_back(edge);
    }

    Geom::BlendOptions options;
    options.samples = Samples.getValue();
    auto result = Geom::blendSurface(blend[0], blend[1], options);
    if (!result.isDone()) {
        return new App::DocumentObjectExecReturn(result.error);
    }
    std::vector<std::string> achieved;
    for (int k = 0; k < 2; ++k) {
        if (blend[k].condition.order > 0) {
            achieved.push_back(describe(sides[k], blend[k].condition, result.reports[k]));
        }
    }
    Achieved.setValues(achieved);

    BRepBuilderAPI_MakeFace mkFace(result.surface, Precision::Confusion());
    Shape.setValue(nameFromEdges(mkFace.Face(),
                                 inputs,
                                 getDocument()->getStringHasher(),
                                 OpCodes::BlendSurface,
                                 1e-4));
    return StdReturn;
}
