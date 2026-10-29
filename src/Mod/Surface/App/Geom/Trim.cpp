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
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepOffsetAPI_NormalProjection.hxx>
#include <BRepProj_Projection.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#endif

#include "Trim.h"

namespace Surface::Geom
{

namespace
{

/// Whether `edge` lies on `face`: its ends and three points between
bool onFace(const TopoDS_Edge& edge, const TopoDS_Face& face, double tolerance)
{
    BRepAdaptor_Curve c(edge);
    for (int k = 0; k <= 4; ++k) {
        gp_Pnt p = c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * k / 4);
        BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(p).Vertex(), face);
        if (!d.IsDone() || d.Value() > tolerance) {
            return false;
        }
    }
    return true;
}

void appendEdges(const TopoDS_Shape& shape, std::vector<TopoDS_Shape>& out)
{
    for (TopExp_Explorer x(shape, TopAbs_EDGE); x.More(); x.Next()) {
        out.push_back(x.Current());
    }
}

}  // namespace

std::vector<TopoDS_Shape> projectOnto(const TopoDS_Face& face,
                                      const std::vector<TopoDS_Shape>& tools,
                                      const gp_Dir* direction,
                                      double tolerance)
{
    std::vector<TopoDS_Shape> out;
    std::vector<TopoDS_Edge> away;
    for (const auto& tool : tools) {
        for (TopExp_Explorer x(tool, TopAbs_EDGE); x.More(); x.Next()) {
            const TopoDS_Edge& edge = TopoDS::Edge(x.Current());
            if (onFace(edge, face, tolerance)) {
                out.push_back(edge);
            }
            else {
                away.push_back(edge);
            }
        }
    }
    if (away.empty()) {
        return out;
    }
    try {
        if (direction) {
            // one edge at a time: a wire of them would have to be connected
            for (const auto& edge : away) {
                BRepProj_Projection proj(BRepBuilderAPI_MakeWire(edge).Wire(), face, *direction);
                if (proj.IsDone()) {
                    appendEdges(proj.Shape(), out);
                }
            }
        }
        else {
            BRepOffsetAPI_NormalProjection proj(face);
            for (const auto& edge : away) {
                proj.Add(edge);
            }
            proj.SetParams(tolerance, std::max(tolerance, 1e-7), GeomAbs_C2, 14, 16);
            proj.Build();
            if (proj.IsDone()) {
                appendEdges(proj.Projection(), out);
            }
        }
    }
    catch (Standard_Failure&) {
        // what could not be laid on the face is left out, as what misses it
    }
    return out;
}

int nearestPiece(const std::vector<TopoDS_Face>& pieces,
                 const gp_Pnt& point,
                 double tolerance,
                 int* tie)
{
    if (tie) {
        *tie = -1;
    }
    TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(point).Vertex();
    int best = -1;
    double nearest = 0;
    std::vector<double> dist(pieces.size(), -1);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        BRepExtrema_DistShapeShape d(v, pieces[i]);
        if (!d.IsDone()) {
            continue;
        }
        dist[i] = d.Value();
        if (best < 0 || dist[i] < nearest) {
            best = (int)i;
            nearest = dist[i];
        }
    }
    if (tie && best >= 0) {
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            if ((int)i != best && dist[i] >= 0 && dist[i] - nearest <= tolerance) {
                *tie = (int)i;
                break;
            }
        }
    }
    return best;
}

}  // namespace Surface::Geom
