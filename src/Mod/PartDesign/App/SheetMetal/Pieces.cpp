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

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepLib.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Plane.hxx>
#include <TopExp_Explorer.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Vec.hxx>

#include <Base/Exception.h>

#include "Pieces.h"

using Part::TopoShape;

namespace PartDesign::SheetMetal
{

TopoShape namedFace(const std::vector<std::pair<TopoDS_Edge, std::string>>& edges,
                    const std::string& name)
{
    BRepBuilderAPI_MakeWire wire;
    for (const auto& edge : edges) {
        wire.Add(edge.first);
    }
    if (!wire.IsDone()) {
        FC_THROWM(Base::CADKernelError, "Sheet metal: no outline for " << name);
    }
    BRepBuilderAPI_MakeFace face(wire.Wire(), true);
    if (!face.IsDone()) {
        FC_THROWM(Base::CADKernelError, "Sheet metal: no face for " << name);
    }
    TopoShape res(face.Face());
    res.setElementName(Data::IndexedName::fromConst("Face", 1), Data::MappedName(name));
    // the wire builder may hand back copies of the edges: find each by place
    for (int i = 1; i <= res.countSubShapes(TopAbs_EDGE); ++i) {
        const TopoDS_Edge e = TopoDS::Edge(res.getSubShape(TopAbs_EDGE, i));
        BRepAdaptor_Curve c(e);
        const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        for (const auto& edge : edges) {
            BRepAdaptor_Curve d(edge.first);
            if (d.Value((d.FirstParameter() + d.LastParameter()) / 2).Distance(mid)
                < Precision::Confusion()) {
                res.setElementName(Data::IndexedName::fromConst("Edge", i),
                                   Data::MappedName(edge.second));
                break;
            }
        }
    }
    return res;
}

void storePCurves(const TopoShape& shape)
{
    for (TopExp_Explorer f(shape.getShape(), TopAbs_FACE); f.More(); f.Next()) {
        const TopoDS_Face& face = TopoDS::Face(f.Current());
        if (!BRep_Tool::Surface(face)->IsKind(STANDARD_TYPE(Geom_Plane))) {
            continue;
        }
        for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
            BRepLib::BuildPCurveForEdgeOnPlane(TopoDS::Edge(e.Current()), face);
        }
    }
}

TopoDS_Edge lineEdge(const gp_Pnt& a, const gp_Pnt& b)
{
    return BRepBuilderAPI_MakeEdge(a, b).Edge();
}

TopoDS_Edge arcEdge(const gp_Ax3& frame, double radius, double angle, bool reversed)
{
    const gp_Ax2 ax(frame.Location(), frame.Direction(), frame.XDirection());
    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(gp_Circ(ax, radius), 0, angle).Edge();
    if (reversed) {
        e.Reverse();
    }
    return e;
}

TopoShape bendSection(const CylBend& bend, const std::string& name)
{
    const gp_Pnt c = bend.frame.Location();
    const gp_Dir x = bend.frame.XDirection();
    const gp_Dir x2 = x.Rotated(gp_Ax1(c, bend.frame.Direction()), bend.angle);
    const double inner = bend.radius, outer = bend.radius + bend.thickness;
    return namedFace(
        {{lineEdge(c.Translated(gp_Vec(x) * outer), c.Translated(gp_Vec(x) * inner)),
          name + "Start"},
         {arcEdge(bend.frame, inner, bend.angle, false), name + "Inner"},
         {lineEdge(c.Translated(gp_Vec(x2) * inner), c.Translated(gp_Vec(x2) * outer)),
          name + "End"},
         {arcEdge(bend.frame, outer, bend.angle, true), name + "Outer"}},
        name);
}

TopoShape wallSection(const gp_Pnt& start, const gp_Dir& along, double length,
                      const gp_Dir& across, double thickness, const std::string& name)
{
    const gp_Vec l = gp_Vec(along) * length, t = gp_Vec(across) * thickness;
    const gp_Pnt a = start, b = start.Translated(l), c = b.Translated(t), d = start.Translated(t);
    return namedFace({{lineEdge(a, b), name + "Skin1"},
                      {lineEdge(b, c), name + "End"},
                      {lineEdge(c, d), name + "Skin2"},
                      {lineEdge(d, a), name + "Start"}},
                     name);
}

}  // namespace PartDesign::SheetMetal
