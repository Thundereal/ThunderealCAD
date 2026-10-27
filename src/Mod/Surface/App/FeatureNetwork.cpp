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
#include <sstream>
#include <TopoDS.hxx>
#endif

#include <App/Document.h>
#include <App/ElementNamingUtils.h>
#include <Base/Tools.h>

#include "ElementNames.h"
#include "FeatureNetwork.h"
#include "Geom/EdgeCondition.h"
#include "Geom/NetworkSurface.h"


using namespace Surface;

PROPERTY_SOURCE(Surface::Network, Part::Spline)

namespace
{
const char* ContinuityEnums[] = {"G0", "G1", "G2", nullptr};
const App::PropertyFloatConstraint::Constraints WeightRange = {0.01, 100.0, 0.1};

/// Whether `edge` lies on `other`: both ends and the middle
bool sameEdge(const TopoDS_Edge& edge, const TopoDS_Edge& other, double tol)
{
    BRepAdaptor_Curve a(edge), b(other);
    for (double f : {0.0, 0.5, 1.0}) {
        gp_Pnt p = a.Value(a.FirstParameter() + (a.LastParameter() - a.FirstParameter()) * f);
        bool on = false;
        for (double g : {0.0, 0.5, 1.0}) {
            on = on
                || p.Distance(
                       b.Value(b.FirstParameter() + (b.LastParameter() - b.FirstParameter()) * g))
                    < tol;
        }
        if (!on) {
            return false;
        }
    }
    return true;
}
}  // namespace

std::array<Network::Side, 4> Network::sides()
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

#define NETWORK_SIDE(_name, _doc)                                                                  \
    ADD_PROPERTY_TYPE(_name##Continuity,                                                           \
                      ((long)0),                                                                   \
                      "Conditions",                                                                \
                      App::Prop_None,                                                              \
                      "How the surface meets its neighbour along the " _doc                       \
                      ": G0 position, G1 tangent, G2 curvature");                                  \
    _name##Continuity.setEnums(ContinuityEnums);                                                   \
    ADD_PROPERTY_TYPE(_name##Weight,                                                               \
                      (1.0),                                                                       \
                      "Conditions",                                                                \
                      App::Prop_None,                                                              \
                      "Scales the surface's derivative across the " _doc ": 1 keeps its own");    \
    _name##Weight.setConstraints(&WeightRange);                                                    \
    ADD_PROPERTY_TYPE(_name##Support,                                                              \
                      (nullptr),                                                                   \
                      "Conditions",                                                                \
                      App::Prop_None,                                                              \
                      "The face to meet along the " _doc                                           \
                      "; empty takes the face the curve is an edge of");                          \
    _name##Support.setScope(App::LinkScope::Global);

    NETWORK_SIDE(FirstProfile, "first profile")
    NETWORK_SIDE(LastProfile, "last profile")
    NETWORK_SIDE(FirstGuide, "first guide")
    NETWORK_SIDE(LastGuide, "last guide")
#undef NETWORK_SIDE
    ADD_PROPERTY_TYPE(Achieved,
                      (),
                      "Conditions",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Output),
                      "What each edge condition achieved");
}

App::DocumentObjectExecReturn* Network::execute()
{
    std::vector<Part::TopoShape> inputs;
    std::vector<std::pair<App::DocumentObject*, std::string>> sources;
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
            sources.emplace_back(objs[i], subs[i]);
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

    // the edge conditions, on the outer curves as the network ordered them
    Geom::Conditions conditions;
    auto all = sides();
    const int nprofiles = (int)curves[0].size();
    for (int b = 0; b < 4; ++b) {
        const auto& side = all[b];
        auto& c = conditions[b];
        c.order = side.continuity->getValue();
        c.weight = side.weight->getValue();
        if (c.order == 0) {
            continue;
        }
        if (side.support->getValue()) {
            auto face = Part::Feature::getTopoShape(side.support->getValue(),
                                                    side.support->getSubValues().empty()
                                                        ? ""
                                                        : side.support->getSubValues()[0].c_str(),
                                                    true);
            if (face.isNull() || face.getShape().ShapeType() != TopAbs_FACE) {
                return new App::DocumentObjectExecReturn(std::string("The support of the ")
                                                         + side.name + " is not a face");
            }
            c.support = TopoDS::Face(face.getShape());
            continue;
        }
        // implicit: the one face of the curve's own shape it is an edge of
        bool guide = b == int(Geom::Boundary::UMin) || b == int(Geom::Boundary::UMax);
        const auto& order = guide ? result.guideOrder : result.profileOrder;
        bool last = b == int(Geom::Boundary::UMax) || b == int(Geom::Boundary::VMax);
        int input = order[last ? order.size() - 1 : 0] + (guide ? nprofiles : 0);
        const auto& source = sources[input];
        auto owner = Part::Feature::getTopoShape(source.first,
                                                 Data::noElementName(source.second.c_str()).c_str(),
                                                 false);
        const TopoDS_Edge& edge = TopoDS::Edge(inputs[input].getShape());
        std::vector<TopoDS_Face> faces;
        for (const auto& f : owner.getSubTopoShapes(TopAbs_FACE)) {
            for (const auto& e : f.getSubTopoShapes(TopAbs_EDGE)) {
                if (sameEdge(edge, TopoDS::Edge(e.getShape()), Precision::Confusion())) {
                    faces.push_back(TopoDS::Face(f.getShape()));
                    break;
                }
            }
        }
        if (faces.size() != 1) {
            return new App::DocumentObjectExecReturn(
                std::string("G") + std::to_string(c.order) + " on the " + side.name
                + " needs a support face: "
                + (faces.empty() ? "the curve is an edge of no face"
                                 : "the curve is an edge of more than one face")
                + "; pick one as its support");
        }
        c.support = faces.front();
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
        std::ostringstream ss;
        ss << all[b].name << ": G" << conditions[b].order << ", worst angle "
           << Base::toDegrees(r.maxAngle) << " deg";
        if (conditions[b].order >= 2) {
            ss << ", worst curvature gap " << r.maxCurvatureGap;
        }
        if (!r.achieved) {
            ss << "; not met"
               << (r.firstCorner ? " at its first corner"
                                 : (r.lastCorner ? " at its last corner" : ""));
        }
        achieved.push_back(ss.str());
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
