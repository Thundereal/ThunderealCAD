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
#include "FeatureNetwork.h"
#include "Geom/NetworkSurface.h"


using namespace Surface;

PROPERTY_SOURCE(Surface::Network, Part::Spline)

Network::Network()
{
    ADD_PROPERTY_TYPE(Profiles,
                      (nullptr),
                      "Network",
                      App::Prop_None,
                      "Curves one way across the surface: edges, in any order, either way round");
    ADD_PROPERTY_TYPE(Guides,
                      (nullptr),
                      "Network",
                      App::Prop_None,
                      "Curves the other way, each crossing every profile once");
    ADD_PROPERTY_TYPE(Tolerance,
                      (1e-4),
                      "Network",
                      App::Prop_None,
                      "How far a profile and a guide may pass each other and still cross");
    ADD_PROPERTY_TYPE(FitTolerance,
                      (1e-4),
                      "Network",
                      App::Prop_None,
                      "How far the surface may stray from the curves");
    ADD_PROPERTY_TYPE(MaxGap,
                      (0.0),
                      "Network",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Output),
                      "The widest crossing: the surface passes midway");
    ADD_PROPERTY_TYPE(MaxFitDeviation,
                      (0.0),
                      "Network",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Output),
                      "How far the surface strays from the curves, at most");
    Profiles.setScope(App::LinkScope::Global);
    Guides.setScope(App::LinkScope::Global);
}

App::DocumentObjectExecReturn* Network::execute()
{
    std::vector<Part::TopoShape> inputs;
    std::vector<Handle(Geom_Curve)> curves[2];
    int index = 0;
    for (auto prop : {&Profiles, &Guides}) {
        const auto& objs = prop->getValues();
        const auto& subs = prop->getSubValues();
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            auto edge = Part::Feature::getTopoShape(objs[i], subs[i].c_str(), true);
            if (edge.isNull() || edge.getShape().ShapeType() != TopAbs_EDGE) {
                return new App::DocumentObjectExecReturn(
                    std::string(prop->getName()) + " " + std::to_string(i + 1)
                    + " is not an edge");
            }
            BRepAdaptor_Curve adapt(TopoDS::Edge(edge.getShape()));
            Handle(Geom_Curve) curve = new Geom_TrimmedCurve(adapt.Curve().Curve(),
                                                             adapt.FirstParameter(),
                                                             adapt.LastParameter());
            const TopLoc_Location& loc = edge.getShape().Location();
            if (!loc.IsIdentity()) {
                curve->Transform(loc.Transformation());
            }
            curves[index].push_back(curve);
            inputs.push_back(edge);
        }
        ++index;
    }

    Geom::NetworkOptions options;
    options.tolerance = Tolerance.getValue();
    options.fitTolerance = FitTolerance.getValue();
    auto result = Geom::networkSurface(curves[0], curves[1], options);
    MaxGap.setValue(result.maxGap);
    MaxFitDeviation.setValue(result.maxFitDeviation);
    if (!result.isDone()) {
        return new App::DocumentObjectExecReturn(result.error);
    }

    BRepBuilderAPI_MakeFace mkFace(result.surface, Precision::Confusion());
    // the outer curves are its boundary, within the fit
    Shape.setValue(nameFromEdges(mkFace.Face(),
                                 inputs,
                                 getDocument()->getStringHasher(),
                                 OpCodes::Network,
                                 2 * options.fitTolerance + Precision::Confusion()));
    return StdReturn;
}
