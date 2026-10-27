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

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#endif

#include "ElementNames.h"

using namespace Surface;

namespace
{

struct EdgeEnds
{
    TopoDS_Vertex v1, v2;
    gp_Pnt p1, p2;
    double tol;

    explicit EdgeEnds(const TopoDS_Edge& e)
    {
        TopExp::Vertices(e, v1, v2);
        if (!v1.IsNull()) {
            p1 = BRep_Tool::Pnt(v1);
        }
        if (!v2.IsNull()) {
            p2 = BRep_Tool::Pnt(v2);
        }
        tol = BRep_Tool::Tolerance(e);
    }
};

gp_Pnt midPoint(const TopoDS_Edge& e)
{
    BRepAdaptor_Curve adapt(e);
    return adapt.Value((adapt.FirstParameter() + adapt.LastParameter()) / 2);
}

}  // namespace

Part::TopoShape Surface::nameFromEdges(const TopoDS_Shape& result,
                                       const std::vector<Part::TopoShape>& edges,
                                       const App::StringHasherRef& hasher,
                                       const char* op,
                                       double tol)
{
    Part::ShapeMapper mapper;
    Part::TopoShape res(0, hasher, result);
    auto resEdges = res.getSubTopoShapes(TopAbs_EDGE);
    std::vector<bool> taken(resEdges.size(), false);
    auto faces = res.getSubTopoShapes(TopAbs_FACE);

    for (const auto& input : edges) {
        if (input.isNull() || input.getShape().ShapeType() != TopAbs_EDGE) {
            continue;
        }
        const TopoDS_Edge& e = TopoDS::Edge(input.getShape());
        EdgeEnds in(e);
        if (in.v1.IsNull() || in.v2.IsNull()) {
            continue;
        }
        gp_Pnt mid = midPoint(e);
        for (std::size_t i = 0; i < resEdges.size(); ++i) {
            if (taken[i]) {
                continue;
            }
            const TopoDS_Edge& r = TopoDS::Edge(resEdges[i].getShape());
            EdgeEnds out(r);
            if (out.v1.IsNull() || out.v2.IsNull()) {
                continue;
            }
            double t = std::max({tol, in.tol, out.tol});
            bool same = in.p1.Distance(out.p1) <= t && in.p2.Distance(out.p2) <= t;
            bool reversed = !same && in.p1.Distance(out.p2) <= t && in.p2.Distance(out.p1) <= t;
            if (!same && !reversed) {
                continue;
            }
            BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(mid).Vertex(), r);
            if (!dist.IsDone() || dist.Value() > t) {
                continue;
            }
            taken[i] = true;
            mapper.insert(false, e, r);
            mapper.insert(false, in.v1, same ? out.v1 : out.v2);
            mapper.insert(false, in.v2, same ? out.v2 : out.v1);
            break;
        }
        for (const auto& face : faces) {
            mapper.insert(true, e, face.getShape());
        }
    }

    Part::TopoShape named(0, hasher);
    named.makESHAPE(result, mapper, edges, op);
    return named;
}
