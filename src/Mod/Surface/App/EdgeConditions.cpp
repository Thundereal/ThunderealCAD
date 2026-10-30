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
#include <sstream>

#include <BRepAdaptor_Curve.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>
#endif

#include <App/ElementNamingUtils.h>
#include <Base/Tools.h>
#include <Mod/Part/App/PartFeature.h>

#include "EdgeConditions.h"

namespace Surface
{

const char* ContinuityEnums[] = {"G0", "G1", "G2", nullptr};
const App::PropertyFloatConstraint::Constraints WeightRange = {0.01, 100.0, 0.1};
const App::PropertyFloatConstraint::Constraints WeightEndRange = {0.0, 100.0, 0.1};

namespace
{
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

std::string conditionOf(const ConditionProperties& props,
                        App::DocumentObject* source,
                        const std::string& sub,
                        const TopoDS_Edge& edge,
                        Geom::EdgeCondition& c)
{
    c.order = props.continuity->getValue();
    c.weight = props.weight->getValue();
    c.weightEnd = props.weightEnd ? props.weightEnd->getValue() : 0.0;
    if (c.order == 0) {
        return {};
    }
    if (props.support->getValue()) {
        const auto& subs = props.support->getSubValues();
        auto face = Part::Feature::getTopoShape(props.support->getValue(),
                                                subs.empty() ? "" : subs[0].c_str(),
                                                true);
        if (face.isNull() || face.getShape().ShapeType() != TopAbs_FACE) {
            return std::string("The support of the ") + props.name + " is not a face";
        }
        c.support = TopoDS::Face(face.getShape());
        return {};
    }
    // implicit: the one face of the curve's own shape it is an edge of
    auto owner =
        Part::Feature::getTopoShape(source, Data::noElementName(sub.c_str()).c_str(), false);
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
        return std::string("G") + std::to_string(c.order) + " on the " + props.name
            + " needs a support face: "
            + (faces.empty() ? "the curve is an edge of no face"
                             : "the curve is an edge of more than one face")
            + "; pick one as its support";
    }
    c.support = faces.front();
    return {};
}

std::string describe(const ConditionProperties& props,
                     const Geom::EdgeCondition& c,
                     const Geom::MatchReport& r)
{
    std::ostringstream ss;
    ss << props.name << ": G" << c.order << ", worst angle " << Base::toDegrees(r.maxAngle)
       << " deg";
    if (c.order >= 2) {
        ss << ", worst curvature gap " << r.maxCurvatureGap;
    }
    if (!r.achieved) {
        ss << "; not met"
           << (r.firstCorner ? " at its first corner" : (r.lastCorner ? " at its last corner" : ""));
    }
    return ss.str();
}

}  // namespace Surface
