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
#include "FeatureTwoRailSweep.h"
#include "Geom/Sweep.h"


using namespace Surface;

PROPERTY_SOURCE(Surface::TwoRailSweep, Part::Spline)

namespace
{
const App::PropertyIntegerConstraint::Constraints StationRange = {2, 200, 1};

/// The edge `sub` of `obj` as a curve where it is, and the edge
Handle(Geom_Curve) curveOf(App::DocumentObject* obj, const std::string& sub, Part::TopoShape& edge)
{
    edge = Part::Feature::getTopoShape(obj, sub.c_str(), true);
    if (!obj || edge.isNull() || edge.getShape().ShapeType() != TopAbs_EDGE) {
        return {};
    }
    BRepAdaptor_Curve adapt(TopoDS::Edge(edge.getShape()));
    Handle(Geom_Curve) curve = new Geom_TrimmedCurve(adapt.Curve().Curve(),
                                                     adapt.FirstParameter(),
                                                     adapt.LastParameter());
    if (!edge.getShape().Location().IsIdentity()) {
        curve->Transform(edge.getShape().Location().Transformation());
    }
    return curve;
}
}  // namespace

TwoRailSweep::TwoRailSweep()
{
    ADD_PROPERTY_TYPE(Rail1, (nullptr), "Sweep", App::Prop_None, "The first rail");
    Rail1.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Rail2, (nullptr), "Sweep", App::Prop_None, "The second rail");
    Rail2.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Sections,
                      (nullptr),
                      "Sweep",
                      App::Prop_None,
                      "Curves from the first rail to the second, swept between them");
    Sections.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Stations,
                      (9),
                      "Sweep",
                      App::Prop_None,
                      "How many places along the rails a section is put");
    Stations.setConstraints(&StationRange);
}

App::DocumentObjectExecReturn* TwoRailSweep::execute()
{
    std::vector<Part::TopoShape> inputs;
    Handle(Geom_Curve) rails[2];
    App::PropertyLinkSub* links[2] = {&Rail1, &Rail2};
    for (int k = 0; k < 2; ++k) {
        const auto& subs = links[k]->getSubValues();
        Part::TopoShape edge;
        rails[k] = curveOf(links[k]->getValue(), subs.empty() ? "" : subs.front(), edge);
        if (rails[k].IsNull()) {
            return new App::DocumentObjectExecReturn("Rail " + std::to_string(k + 1)
                                                     + " is not an edge");
        }
        inputs.push_back(edge);
    }
    std::vector<Handle(Geom_Curve)> sections;
    const auto& objs = Sections.getValues();
    const auto& subs = Sections.getSubValues();
    for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
        Part::TopoShape edge;
        auto c = curveOf(objs[i], subs[i], edge);
        if (c.IsNull()) {
            return new App::DocumentObjectExecReturn("Section " + std::to_string(i + 1)
                                                     + " is not an edge");
        }
        sections.push_back(c);
        inputs.push_back(edge);
    }

    Geom::SweepOptions options;
    options.stations = Stations.getValue();
    auto result = Geom::twoRailSweep(rails[0], rails[1], sections, options);
    if (!result.isDone()) {
        return new App::DocumentObjectExecReturn(result.error);
    }
    BRepBuilderAPI_MakeFace mkFace(result.surface, Precision::Confusion());
    Shape.setValue(nameFromEdges(mkFace.Face(),
                                 inputs,
                                 getDocument()->getStringHasher(),
                                 OpCodes::TwoRailSweep,
                                 2 * options.fitTolerance + Precision::Confusion()));
    return StdReturn;
}
