/***************************************************************************
 *   Copyright (c) 2020 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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
#include <TColGeom_SequenceOfCurve.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <GeomFill_NSections.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Precision.hxx>
#include <Standard_Version.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#endif

#include <App/Document.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "ElementNames.h"
#include "FeatureSections.h"


using namespace Surface;

PROPERTY_SOURCE(Surface::Sections, Part::Spline)

Sections::Sections()
{
    ADD_PROPERTY_TYPE(NSections, (nullptr), "Sections", App::Prop_None, "Section curves");
    NSections.setScope(App::LinkScope::Global);
    SURFACE_ADD_CONDITION(FirstSection, "the first section");
    SURFACE_ADD_CONDITION(LastSection, "the last section");
    ADD_PROPERTY_TYPE(Achieved,
                      (),
                      "Conditions",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Output),
                      "What each edge condition achieved");
}

App::DocumentObjectExecReturn* Sections::execute()
{
    TColGeom_SequenceOfCurve curveSeq;
    std::vector<Part::TopoShape> inputs;
    std::vector<std::pair<App::DocumentObject*, std::string>> sources;
    auto edge_obj = NSections.getValues();
    auto edge_sub = NSections.getSubValues();
    if (edge_obj.size() == edge_sub.size()) {
        for (std::size_t index = 0; index < edge_obj.size(); index++) {
            auto edge = Part::Feature::getTopoShape(edge_obj[index],
                                                    edge_sub[index].c_str(),
                                                    /*needSubElement*/ true);
            if (!edge.isNull() && edge.getShape().ShapeType() == TopAbs_EDGE) {
                BRepAdaptor_Curve curve_adapt(TopoDS::Edge(edge.getShape()));
                const TopLoc_Location& loc = edge.getShape().Location();
                Handle(Geom_TrimmedCurve) hCurve =
                    new Geom_TrimmedCurve(curve_adapt.Curve().Curve(),
                                          curve_adapt.FirstParameter(),
                                          curve_adapt.LastParameter());
                if (!loc.IsIdentity()) {
                    hCurve->Transform(loc.Transformation());
                }
                curveSeq.Append(hCurve);
                inputs.push_back(edge);
                sources.emplace_back(edge_obj[index], edge_sub[index]);
            }
        }
    }

    if (curveSeq.Length() < 2) {
        return new App::DocumentObjectExecReturn("At least two sections are required.");
    }

    GeomFill_NSections fillOp(curveSeq);
    fillOp.ComputeSurface();

    Handle(Geom_BSplineSurface) aSurf = fillOp.BSplineSurface();
    if (aSurf.IsNull()) {
        return new App::DocumentObjectExecReturn("Failed to create surface from sections.");
    }

    // the end conditions; the boundaries no section is on are the sides,
    // free for the corners
    Geom::Conditions conditions;
    std::vector<std::string> achieved;
    ConditionProperties ends[2] = {
        {"first section", &FirstSectionContinuity, &FirstSectionWeight, &FirstSectionSupport},
        {"last section", &LastSectionContinuity, &LastSectionWeight, &LastSectionSupport}};
    int at[2] = {-1, -1};
    bool any = false;
    for (int k = 0; k < 2; ++k) {
        int input = k == 0 ? 0 : curveSeq.Length() - 1;
        at[k] = Geom::boundaryOf(aSurf, curveSeq(input + 1), 1e-6);
        if (ends[k].continuity->getValue() == 0) {
            continue;
        }
        if (at[k] < 0) {
            return new App::DocumentObjectExecReturn(std::string("The ") + ends[k].name
                                                     + " is not at a boundary of the surface");
        }
        auto error = conditionOf(ends[k],
                                 sources[input].first,
                                 sources[input].second,
                                 TopoDS::Edge(inputs[input].getShape()),
                                 conditions[at[k]]);
        if (!error.empty()) {
            return new App::DocumentObjectExecReturn(error);
        }
        any = true;
    }
    if (any) {
        for (int b = 0; b < 4; ++b) {
            conditions[b].free = b != at[0] && b != at[1];
        }
        // a condition reaches half way to the next section, so the sections
        // between stay on the surface
        double u0, u1, v0, v1;
        aSurf->Bounds(u0, u1, v0, v1);
        for (int k = 0; k < 2; ++k) {
            if (at[k] < 0 || conditions[at[k]].order == 0) {
                continue;
            }
            const bool acrossV = at[k] == int(Geom::Boundary::VMin)
                || at[k] == int(Geom::Boundary::VMax);
            const double c0 = acrossV ? v0 : u0, c1 = acrossV ? v1 : u1;
            const bool atMax = at[k] == int(Geom::Boundary::VMax)
                || at[k] == int(Geom::Boundary::UMax);
            double nearest = c1 - c0;
            for (int j = 1; j <= curveSeq.Length(); ++j) {
                const auto& c = curveSeq(j);
                gp_Pnt mid = c->Value((c->FirstParameter() + c->LastParameter()) / 2);
                GeomAPI_ProjectPointOnSurf pr(mid, aSurf);
                if (pr.NbPoints() == 0) {
                    continue;
                }
                double u, v;
                pr.LowerDistanceParameters(u, v);
                double d = std::abs((acrossV ? v : u) - (atMax ? c1 : c0));
                if (d > Precision::PConfusion()) {
                    nearest = std::min(nearest, d);
                }
            }
            conditions[at[k]].reach = 0.5 * nearest / (c1 - c0);
        }
        auto reports = Geom::matchBoundaries(aSurf, conditions);
        for (int k = 0; k < 2; ++k) {
            if (at[k] < 0 || conditions[at[k]].order == 0) {
                continue;
            }
            if (!reports[at[k]].error.empty()) {
                return new App::DocumentObjectExecReturn(std::string("The ") + ends[k].name + ": "
                                                         + reports[at[k]].error);
            }
            achieved.push_back(describe(ends[k], conditions[at[k]], reports[at[k]]));
        }
    }
    Achieved.setValues(achieved);

    BRepBuilderAPI_MakeFace mkFace(aSurf, Precision::Confusion());

    Shape.setValue(nameFromEdges(mkFace.Face(),
                                 inputs,
                                 getDocument()->getStringHasher(),
                                 Part::OpCodes::ThruSections));
    return StdReturn;
}
