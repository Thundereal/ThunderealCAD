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

#include <cmath>
#include <limits>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepFill.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <Precision.hxx>
#include <Geom_BSplineCurve.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Solid.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Vec.hxx>

#include <Base/Exception.h>

#include "Flat.h"

namespace PartDesign::SheetMetal
{

namespace
{

constexpr int samples = 24;

struct Mapper
{
    const Model& model;
    int root;
    gp_Trsf toXY;

    gp_Pnt map(int element, const gp_Pnt& p) const
    {
        return model.toFlat(element, p, root).Transformed(toXY);
    }

    /// An edge of a bend's face, mapped: a line where it maps to one, else
    /// a B-spline through samples
    TopoDS_Edge edge(int element, const TopoDS_Edge& e) const
    {
        BRepAdaptor_Curve c(e);
        const double f = c.FirstParameter(), l = c.LastParameter();
        std::vector<gp_Pnt> pts;
        for (int i = 0; i <= samples; ++i) {
            pts.push_back(map(element, c.Value(f + (l - f) * i / samples)));
        }
        const gp_Pnt &a = pts.front(), &b = pts.back();
        bool straight = a.Distance(b) > Precision::Confusion();
        if (straight) {
            const gp_Vec chord(a, b);
            for (const gp_Pnt& p : pts) {
                if (gp_Vec(a, p).Crossed(chord).Magnitude() / chord.Magnitude()
                    > Precision::Confusion()) {
                    straight = false;
                    break;
                }
            }
        }
        TopoDS_Edge res;
        if (straight) {
            res = BRepBuilderAPI_MakeEdge(a, b).Edge();
        }
        else {
            TColgp_Array1OfPnt arr(1, static_cast<int>(pts.size()));
            for (std::size_t i = 0; i < pts.size(); ++i) {
                arr.SetValue(static_cast<int>(i) + 1, pts[i]);
            }
            GeomAPI_PointsToBSpline fit(arr, 3, 8, GeomAbs_C2, Precision::Confusion());
            res = BRepBuilderAPI_MakeEdge(fit.Curve()).Edge();
        }
        res.Orientation(e.Orientation());
        return res;
    }

    TopoDS_Wire wire(int element, const TopoDS_Wire& w, const TopoDS_Face& face) const
    {
        BRepBuilderAPI_MakeWire mk;
        for (BRepTools_WireExplorer it(w, face); it.More(); it.Next()) {
            mk.Add(edge(element, it.Current()));
        }
        if (!mk.IsDone()) {
            THROWM(Base::ValueError, "A face of a bend does not unroll into a closed outline");
        }
        return mk.Wire();
    }

    /// A face of a bend, unrolled: it must lie flat as a plane, but for a
    /// cut across the thickness at a slant (a miter's), which unrolls into a
    /// face ruled between its edges on the two skins
    TopoDS_Face face(int element, const TopoDS_Face& f, int index) const
    {
        const TopoDS_Wire outer = BRepTools::OuterWire(f);
        const TopoDS_Wire unrolled = wire(element, outer, f);
        BRepBuilderAPI_MakeFace mk(unrolled, true);
        if (!mk.IsDone()) {
            std::vector<TopoDS_Edge> edges;
            for (BRepTools_WireExplorer it(unrolled); it.More(); it.Next()) {
                edges.push_back(it.Current());
            }
            int wires = 0;
            for (TopExp_Explorer it(f, TopAbs_WIRE); it.More(); it.Next()) {
                ++wires;
            }
            auto straight = [&](std::size_t i) {
                return BRepAdaptor_Curve(edges[i % 4]).GetType() == GeomAbs_Line;
            };
            if (wires == 1 && edges.size() == 4) {
                // the two edges across the thickness are straight; the face
                // runs between the other two, set the same way round
                const std::size_t a = straight(1) && straight(3) ? 0 : 1;
                if (straight(a + 1) && straight(a + 3)) {
                    return BRepFill::Face(edges[a], TopoDS::Edge(edges[a + 2].Reversed()));
                }
            }
            FC_THROWM(Base::ValueError,
                      "Face" << index << " of " << model.elements[element].name
                             << " does not lie flat as a plane");
        }
        for (TopExp_Explorer it(f, TopAbs_WIRE); it.More(); it.Next()) {
            const TopoDS_Wire& w = TopoDS::Wire(it.Current());
            if (!w.IsSame(outer)) {
                mk.Add(wire(element, w, f));
            }
        }
        ShapeFix_Face fix(mk.Face());
        fix.Perform();
        return fix.Face();
    }
};

/// What takes the root wall's skin to XY, the material above
gp_Trsf rootToXY(const Model& model, int root, bool secondSkin)
{
    gp_Ax3 frame = model.elements[root].frame;
    if (secondSkin) {
        frame = gp_Ax3(frame.Location().Translated(gp_Vec(frame.Direction()) * model.thickness),
                       frame.Direction().Reversed(),
                       frame.XDirection());
    }
    gp_Trsf t;
    t.SetTransformation(frame);
    return t;
}

}  // namespace

FlatResult flatten(const TopoDS_Shape& folded, const Model& model, int root, bool secondSkin)
{
    if (root < 0 || root >= static_cast<int>(model.elements.size())
        || model.elements[root].kind != Element::Kind::Wall) {
        THROWM(Base::ValueError, "The flat's root is not a wall");
    }
    const Mapper mapper {model, root, rootToXY(model, root, secondSkin)};

    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(folded, TopAbs_FACE, faces);
    BRepBuilderAPI_Sewing sewing(1e-6);
    // each folded face and the face it lies flat as, before sewing
    std::vector<std::pair<TopoDS_Shape, TopoDS_Shape>> laid;
    std::vector<std::pair<double, double>> extent(model.elements.size(),
                                                  {std::numeric_limits<double>::max(),
                                                   -std::numeric_limits<double>::max()});
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face& face = TopoDS::Face(faces(i));
        const int owner = model.owner(face);
        if (owner < 0) {
            FC_THROWM(Base::ValueError, "Face" << i << " is in no wall or bend of the sheet");
        }
        const Element& element = model.elements[owner];
        if (element.kind == Element::Kind::Wall) {
            gp_Trsf move = mapper.toXY * model.placement(owner, root);
            laid.emplace_back(face, BRepBuilderAPI_Transform(face, move, true).Shape());
            sewing.Add(laid.back().second);
            continue;
        }
        laid.emplace_back(face, mapper.face(owner, face, i));
        sewing.Add(laid.back().second);
        // how far along its axis the bend goes, for its line
        const gp_Ax3& f = element.bend.frame;
        for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
            const double z = gp_Vec(f.Location(), BRep_Tool::Pnt(TopoDS::Vertex(it.Current())))
                                 .Dot(gp_Vec(f.Direction()));
            extent[owner].first = std::min(extent[owner].first, z);
            extent[owner].second = std::max(extent[owner].second, z);
        }
    }
    sewing.Perform();
    const TopoDS_Shape sewn = sewing.SewedShape();
    TopoDS_Shell shell;
    int shells = 0;
    for (TopExp_Explorer it(sewn, TopAbs_SHELL); it.More(); it.Next()) {
        shell = TopoDS::Shell(it.Current());
        ++shells;
    }
    if (shells != 1 || !BRep_Tool::IsClosed(shell)) {
        THROWM(Base::ValueError, "The flat does not close: faces overlap or are missing");
    }
    BRepBuilderAPI_MakeSolid solid(shell);
    ShapeFix_Solid fix(solid.Solid());
    fix.Perform();

    FlatResult res;
    res.solid = fix.Solid();
    // the faces as the solid has them: sewn, then as the fix left them
    Handle(ShapeBuild_ReShape) context = fix.Context();
    for (const auto& [from, to] : laid) {
        TopoDS_Shape sewnFace = sewing.IsModified(to) ? sewing.Modified(to) : to;
        if (!context.IsNull()) {
            sewnFace = context->Apply(sewnFace);
        }
        if (!sewnFace.IsNull()) {
            res.faces.emplace_back(from, sewnFace);
        }
    }
    for (std::size_t i = 0; i < model.elements.size(); ++i) {
        const Element& element = model.elements[i];
        if (element.kind != Element::Kind::Bend || extent[i].first > extent[i].second) {
            continue;
        }
        const CylBend& b = element.bend;
        // the line on the inner skin at an angle into the bend, flat
        auto across = [&](double angle) {
            const gp_Vec r = gp_Vec(b.frame.XDirection()) * (b.radius * std::cos(angle))
                + gp_Vec(b.frame.YDirection()) * (b.radius * std::sin(angle));
            auto at = [&](double z) {
                const gp_Vec v = r + gp_Vec(b.frame.Direction()) * z;
                return mapper.map(static_cast<int>(i), b.frame.Location().Translated(v));
            };
            return BRepBuilderAPI_MakeEdge(at(extent[i].first), at(extent[i].second)).Edge();
        };
        FlatResult::BendLine line;
        line.name = element.name;
        line.edge = across(b.angle / 2);
        line.tangents = {across(0), across(b.angle)};
        line.up = mapper.map(static_cast<int>(i),
                             b.frame.Location().Translated(gp_Vec(b.frame.XDirection()) * b.radius))
                      .Z()
            > model.thickness / 2;
        line.angle = b.angle;
        res.bendLines.push_back(line);
    }
    return res;
}

}  // namespace PartDesign::SheetMetal
