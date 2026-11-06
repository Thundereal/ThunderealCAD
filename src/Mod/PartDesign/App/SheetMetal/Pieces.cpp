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

#include <algorithm>
#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Plane.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Vec.hxx>

#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

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

TopoShape coneSection(const ConeBend& cone, double length, const std::string& name)
{
    const gp_Ax3 start = cone.startFrame();
    const gp_Vec along = gp_Vec(start.XDirection()) * length;
    const gp_Vec across = gp_Vec(start.Direction()) * cone.thickness;
    const gp_Pnt a = start.Location(), b = a.Translated(along), c = b.Translated(across),
                 d = a.Translated(across);
    return namedFace({{lineEdge(a, b), name + "Inner"},
                      {lineEdge(b, c), name + "Rim2"},
                      {lineEdge(c, d), name + "Outer"},
                      {lineEdge(d, a), name + "Rim1"}},
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

std::vector<TopoShape> reliefCuts(const Relief& relief, const gp_Pnt& inner, const gp_Dir& along,
                                  const gp_Dir& endOut, const gp_Dir& up, double thickness,
                                  double gap1, double span, double gap2, const std::string& name,
                                  const App::StringHasherRef& hasher)
{
    std::vector<TopoShape> cuts;
    const double tol = Precision::Confusion();
    const double w = relief.width;
    if (w < tol) {
        return cuts;
    }
    const bool obround = relief.shape == Relief::Shape::Obround;
    const gp_Vec e(endOut);
    // the straight part of an obround stops at its half circle's middle
    const double straight = obround ? std::max(relief.depth - w / 2, 0.0) : relief.depth;
    // each end with a gap, by where its relief starts along the edge
    std::vector<std::pair<int, double>> ends;
    if (gap1 > tol) {
        ends.emplace_back(1, gap1 - w);
    }
    if (gap2 > tol) {
        ends.emplace_back(2, gap1 + span);
    }
    for (const auto& [n, from] : ends) {
        const std::string named = name + std::to_string(n);
        const gp_Pnt start = inner.Translated(gp_Vec(along) * from);
        if (straight > tol) {
            cuts.push_back(TopoShape(0, hasher).makEPrism(
                wallSection(start.Translated(-e * straight), endOut, straight, up.Reversed(),
                            thickness, named),
                gp_Vec(along) * w));
        }
        if (obround) {
            const gp_Pnt center = start.Translated(gp_Vec(along) * (w / 2) - e * straight);
            cuts.push_back(TopoShape(
                BRepPrimAPI_MakeCylinder(gp_Ax2(center, up.Reversed()), w / 2, thickness)
                    .Solid()));
        }
    }
    return cuts;
}

namespace
{

/// A face's outward normal, at a planar face; false for another face
bool planeNormal(const TopoDS_Face& face, gp_Dir& normal)
{
    BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane) {
        return false;
    }
    normal = surface.Plane().Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    return true;
}

}  // namespace

WallEdge wallEdge(const TopoShape& base, const Model& model, const std::string& ref,
                  const std::string& label)
{
    const TopoDS_Shape sub = base.getSubShape(ref.c_str(), true);
    if (sub.IsNull() || sub.ShapeType() != TopAbs_EDGE) {
        FC_THROWM(Base::ValueError, "No edge " << label);
    }
    const TopoDS_Edge& edge = TopoDS::Edge(sub);
    if (BRepAdaptor_Curve(edge).GetType() != GeomAbs_Line) {
        FC_THROWM(Base::ValueError, "Edge " << label << " is not straight");
    }
    WallEdge res;
    TopoDS_Vertex v0, v1;
    TopExp::Vertices(edge, v0, v1);
    res.p0 = BRep_Tool::Pnt(v0);
    res.p1 = BRep_Tool::Pnt(v1);
    res.along = gp_Dir(gp_Vec(res.p0, res.p1));

    // the skin the edge is on, the wall it belongs to, and its end
    bool haveEnd = false;
    for (const auto& f : base.findAncestorsShapes(edge, TopAbs_FACE)) {
        const TopoDS_Face& face = TopoDS::Face(f);
        gp_Dir n;
        if (!planeNormal(face, n)) {
            continue;
        }
        const int owner = model.owner(face);
        if (owner >= 0 && model.elements[owner].kind == Element::Kind::Wall
            && n.IsParallel(model.elements[owner].frame.Direction(), Precision::Angular())) {
            res.wall = owner;
            res.skinOut = n;
        }
        else {
            res.endOut = n;
            haveEnd = true;
        }
    }
    if (res.wall < 0 || !haveEnd
        || std::abs(res.endOut.Dot(res.skinOut)) > Precision::Angular()) {
        FC_THROWM(Base::ValueError,
                  "Edge " << label << " is not where a wall's skin meets its end");
    }
    return res;
}


std::vector<TopoDS_Face> skinFaces(const TopoDS_Shape& shape, const Model& model, int wall,
                                   const gp_Pln& plane)
{
    std::vector<TopoDS_Face> res;
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face& face = TopoDS::Face(it.Current());
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) {
            continue;
        }
        const gp_Pln pln = surface.Plane();
        if (!pln.Axis().Direction().IsParallel(plane.Axis().Direction(), Precision::Angular())
            || plane.Distance(pln.Location()) > Precision::Confusion()
            || model.owner(face) != wall) {
            continue;
        }
        res.push_back(face);
    }
    return res;
}

TopoShape cutDroppingScrap(const TopoShape& base, const std::vector<TopoShape>& cuts,
                           const std::vector<TopoShape>& pieces,
                           const App::StringHasherRef& hasher)
{
    const TopoShape res = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                                           {base, TopoShape().makECompound(cuts)});
    const auto before = base.getSubTopoShapes(TopAbs_SOLID);
    const auto after = res.getSubTopoShapes(TopAbs_SOLID);
    if (after.size() <= before.size()) {
        return res;
    }
    auto volume = [](const TopoDS_Shape& shape) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(shape, props);
        return props.Mass();
    };
    // what each solid of the base parted into
    std::vector<std::vector<std::size_t>> parted(before.size());
    std::vector<bool> keep(after.size(), true);
    for (std::size_t j = 0; j < after.size(); ++j) {
        for (std::size_t i = 0; i < before.size(); ++i) {
            BRepAlgoAPI_Common common(after[j].getShape(), before[i].getShape());
            if (common.IsDone() && volume(common.Shape()) > Precision::Confusion()) {
                parted[i].push_back(j);
                break;
            }
        }
    }
    const TopoDS_Shape added = TopoShape().makECompound(pieces).getShape();
    for (const auto& part : parted) {
        if (part.size() < 2) {
            continue;
        }
        // the pieces stand on what stays; none touched, the largest stays
        std::vector<std::size_t> touching;
        for (std::size_t j : part) {
            BRepExtrema_DistShapeShape dist(after[j].getShape(), added);
            if (dist.IsDone() && dist.Value() <= Precision::Confusion()) {
                touching.push_back(j);
            }
        }
        if (touching.empty()) {
            touching.push_back(*std::max_element(part.begin(), part.end(),
                                                 [&](std::size_t a, std::size_t b) {
                                                     return volume(after[a].getShape())
                                                         < volume(after[b].getShape());
                                                 }));
        }
        for (std::size_t j : part) {
            keep[j] = std::find(touching.begin(), touching.end(), j) != touching.end();
        }
    }
    std::vector<TopoShape> kept;
    for (std::size_t j = 0; j < after.size(); ++j) {
        if (keep[j]) {
            kept.push_back(after[j]);
        }
    }
    if (kept.size() == after.size()) {
        return res;
    }
    return kept.size() == 1 ? kept.front() : TopoShape(0, hasher).makECompound(kept);
}

}  // namespace PartDesign::SheetMetal
