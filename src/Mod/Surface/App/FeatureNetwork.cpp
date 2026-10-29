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

#include "EdgeConditions.h"
#include "ElementNames.h"
#include "FeatureNetwork.h"
#include "Geom/EdgeCondition.h"
#include "Geom/NetworkSurface.h"


using namespace Surface;

PROPERTY_SOURCE(Surface::Network, Part::Spline)


std::array<ConditionProperties, 4> Network::sides()
{
    // in Geom::Boundary's order: the guides run along V, so the first and last
    // guide are UMin and UMax; the profiles VMin and VMax
    return {{{"first guide", &FirstGuideContinuity, &FirstGuideWeight, &FirstGuideSupport},
             {"last guide", &LastGuideContinuity, &LastGuideWeight, &LastGuideSupport},
             {"first profile", &FirstProfileContinuity, &FirstProfileWeight, &FirstProfileSupport},
             {"last profile", &LastProfileContinuity, &LastProfileWeight, &LastProfileSupport}}};
}

Network::Network()
{
    ADD_PROPERTY_TYPE(Profiles,
                      (nullptr),
                      "Network",
                      App::Prop_None,
                      "Curves one way across the surface: edges, in any order, either way round,\n"
                      "or whole objects (their edges). With no guides, these are the whole\n"
                      "network and are sorted into its two families");
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

    SURFACE_ADD_CONDITION(FirstProfile, "the first profile");
    SURFACE_ADD_CONDITION(LastProfile, "the last profile");
    SURFACE_ADD_CONDITION(FirstGuide, "the first guide");
    SURFACE_ADD_CONDITION(LastGuide, "the last guide");
    ADD_PROPERTY_TYPE(Achieved,
                      (),
                      "Conditions",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Output),
                      "What each edge condition achieved");
}

std::string Network::gather(std::vector<Curve> families[2], bool sort) const
{
    families[0].clear();
    families[1].clear();
    int index = 0;
    for (auto prop : {&Profiles, &Guides}) {
        const auto& objs = prop->getValues();
        const auto& subs = prop->getSubValues();
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            const auto& sub = subs[i];
            auto shape = Part::Feature::getTopoShape(objs[i], sub.c_str(), true);
            std::vector<std::pair<Part::TopoShape, std::string>> edges;
            if (!shape.isNull() && shape.getShape().ShapeType() == TopAbs_EDGE) {
                edges.emplace_back(shape, sub);
            }
            // an object given whole (or a sub-object, "Body.Sketch.") is its edges
            else if (shape.isNull() || shape.countSubShapes(TopAbs_EDGE) == 0
                     || (!sub.empty() && sub.back() != '.')) {
                return std::string(prop->getName()) + " " + std::to_string(i + 1)
                    + " is not an edge, nor an object with edges";
            }
            else {
                int k = 0;
                for (const auto& edge : shape.getSubTopoShapes(TopAbs_EDGE)) {
                    edges.emplace_back(edge, sub + "Edge" + std::to_string(++k));
                }
            }
            for (auto& [edge, name] : edges) {
                BRepAdaptor_Curve adapt(TopoDS::Edge(edge.getShape()));
                Handle(Geom_Curve) curve = new Geom_TrimmedCurve(adapt.Curve().Curve(),
                                                                 adapt.FirstParameter(),
                                                                 adapt.LastParameter());
                const TopLoc_Location& loc = edge.getShape().Location();
                if (!loc.IsIdentity()) {
                    curve->Transform(loc.Transformation());
                }
                families[index].push_back({edge, objs[i], name, curve});
            }
        }
        ++index;
    }

    // no guides: the profiles are the whole network, sorted into its two
    // families (docs/Surface.md sec 6)
    if (sort && families[1].empty() && !families[0].empty()) {
        std::vector<Handle(Geom_Curve)> curves;
        std::vector<std::string> names;
        for (const auto& c : families[0]) {
            curves.push_back(c.curve);
            names.push_back(std::string(c.object->Label.getValue()) + "." + c.sub);
        }
        auto sorted = Geom::sortFamilies(curves, Tolerance.getValue(), names);
        if (!sorted.isDone()) {
            return sorted.error;
        }
        std::vector<Curve> all;
        all.swap(families[0]);
        for (int family = 0; family < 2; ++family) {
            for (int i : family ? sorted.guides : sorted.profiles) {
                families[family].push_back(all[i]);
            }
        }
    }
    return {};
}

std::string Network::separateFamilies()
{
    if (Guides.getSize() > 0) {
        return "the network has its guides already";
    }
    std::vector<Curve> families[2];
    auto error = gather(families, true);
    if (!error.empty()) {
        return error;
    }
    App::PropertyLinkSubList* props[2] = {&Profiles, &Guides};
    for (int family = 0; family < 2; ++family) {
        std::vector<App::DocumentObject*> objs;
        std::vector<std::string> subs;
        for (const auto& c : families[family]) {
            objs.push_back(c.object);
            subs.push_back(c.sub);
        }
        props[family]->setValues(objs, subs);
    }
    return {};
}

App::DocumentObjectExecReturn* Network::execute()
{
    std::vector<Curve> families[2];
    auto error = gather(families, true);
    if (!error.empty()) {
        return new App::DocumentObjectExecReturn(error);
    }
    std::vector<Part::TopoShape> inputs;
    std::vector<std::pair<App::DocumentObject*, std::string>> sources;
    std::vector<Handle(Geom_Curve)> curves[2];
    for (int family = 0; family < 2; ++family) {
        for (const auto& c : families[family]) {
            curves[family].push_back(c.curve);
            inputs.push_back(c.edge);
            sources.emplace_back(c.object, c.sub);
        }
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

    // the edge conditions, on the outer curves as the network ordered them
    Geom::Conditions conditions;
    auto all = sides();
    const int nprofiles = (int)curves[0].size();
    for (int b = 0; b < 4; ++b) {
        bool guide = b == int(Geom::Boundary::UMin) || b == int(Geom::Boundary::UMax);
        const auto& order = guide ? result.guideOrder : result.profileOrder;
        bool last = b == int(Geom::Boundary::UMax) || b == int(Geom::Boundary::VMax);
        int input = order[last ? order.size() - 1 : 0] + (guide ? nprofiles : 0);
        auto error = conditionOf(all[b],
                                 sources[input].first,
                                 sources[input].second,
                                 TopoDS::Edge(inputs[input].getShape()),
                                 conditions[b]);
        if (!error.empty()) {
            return new App::DocumentObjectExecReturn(error);
        }
    }

    std::vector<std::string> achieved;
    auto reports = Geom::matchBoundaries(result.surface, conditions);
    for (int b = 0; b < 4; ++b) {
        if (conditions[b].order == 0) {
            continue;
        }
        const auto& r = reports[b];
        if (!r.error.empty()) {
            return new App::DocumentObjectExecReturn(std::string("The ") + all[b].name + ": "
                                                     + r.error);
        }
        achieved.push_back(describe(all[b], conditions[b], r));
    }
    Achieved.setValues(achieved);

    BRepBuilderAPI_MakeFace mkFace(result.surface, Precision::Confusion());
    // the outer curves are its boundary, within the fit
    Shape.setValue(nameFromEdges(mkFace.Face(),
                                 inputs,
                                 getDocument()->getStringHasher(),
                                 OpCodes::Network,
                                 2 * options.fitTolerance + Precision::Confusion()));
    return StdReturn;
}
