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
#include <numbers>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <GeomConvert.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Solid.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <Mod/Part/App/TopoShapeOpCode.h>

#include "Refold.h"

using Part::TopoShape;

namespace PartDesign::SheetMetal
{

namespace
{

/// A box as large as size about near taken onto the plane through on, on
/// its +n side: a plane's point may lie anywhere on it, far from where the
/// box is wanted
TopoDS_Shape halfSpace(const gp_Pnt& on, const gp_Dir& n, const gp_Pnt& near, double size)
{
    const gp_Pnt at = near.Translated(gp_Vec(n) * -gp_Vec(on, near).Dot(gp_Vec(n)));
    const gp_Ax2 frame(at, n);
    const gp_Pnt corner = at.Translated(gp_Vec(frame.XDirection()) * (-size / 2)
                                        + gp_Vec(frame.YDirection()) * (-size / 2));
    return BRepPrimAPI_MakeBox(gp_Ax2(corner, n, frame.XDirection()), size, size, size).Solid();
}

TopoDS_Shape cutAway(const TopoDS_Shape& shape, const TopoDS_Shape& away)
{
    TopoShape res;
    res.makEBoolean(Part::OpCodes::Cut, {TopoShape(shape), TopoShape(away)});
    return res.getShape();
}

double volumeOf(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

/// A bend's end, where the wall after it starts, and the way that wall runs
void bendEnd(const CylBend& b, gp_Pnt& at, gp_Dir& along)
{
    const gp_Dir x2 = b.frame.XDirection().Rotated(
        gp_Ax1(b.frame.Location(), b.frame.Direction()), b.angle);
    at = b.frame.Location().Translated(gp_Vec(x2) * b.radius);
    along = b.frame.Direction().Crossed(x2);
}

/// A bend's start, and the way out of the wall before it
void bendStart(const CylBend& b, gp_Pnt& at, gp_Dir& out)
{
    const gp_Dir up = b.frame.XDirection().Reversed();
    at = b.frame.Location().Translated(gp_Vec(up) * -b.radius);
    out = up.Crossed(b.frame.Direction());
}

/** A planar face of a bend's flat, in its own frame (toFlat()'s), taken
 * onto its cylinder through the sheet: each point at its angle and height,
 * every radius from inner to outer. The outline's curves go on the
 * cylinders exactly: the flat's (Y, Z) to the cylinder's (u, v) is affine,
 * Y over the neutral radius, so each curve, made a B-spline, keeps its
 * knots and weights and has its poles mapped.
 */
TopoDS_Shape radial(const CylBend& b, const TopoDS_Face& flat, double inner, double outer)
{
    const gp_Pnt o = b.frame.Location();
    const gp_Vec y(b.frame.YDirection()), z(b.frame.Direction());
    const double nu = b.neutral();
    auto uv = [&](const gp_Pnt& p) {
        const gp_Vec v(o, p);
        return gp_Pnt2d(v.Dot(y) / nu, v.Dot(z));
    };
    TopoDS_Wire wires[2];
    Handle(Geom_CylindricalSurface) surfaces[2];
    const double radii[2] = {inner, outer};
    for (int k = 0; k < 2; ++k) {
        surfaces[k] = new Geom_CylindricalSurface(b.frame, radii[k]);
        BRepBuilderAPI_MakeWire mk;
        const TopoDS_Wire outerWire = BRepTools::OuterWire(flat);
        for (BRepTools_WireExplorer it(outerWire, flat); it.More(); it.Next()) {
            const TopoDS_Edge& e = it.Current();
            double f, l;
            Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
            Handle(Geom_BSplineCurve) s =
                GeomConvert::CurveToBSplineCurve(new Geom_TrimmedCurve(c, f, l));
            TColgp_Array1OfPnt2d poles(1, s->NbPoles());
            for (int i = 1; i <= s->NbPoles(); ++i) {
                poles.SetValue(i, uv(s->Pole(i)));
            }
            TColStd_Array1OfReal knots(1, s->NbKnots());
            TColStd_Array1OfInteger mults(1, s->NbKnots());
            s->Knots(knots);
            s->Multiplicities(mults);
            Handle(Geom2d_BSplineCurve) c2d;
            if (s->IsRational()) {
                TColStd_Array1OfReal weights(1, s->NbPoles());
                s->Weights(weights);
                c2d = new Geom2d_BSplineCurve(poles, weights, knots, mults, s->Degree());
            }
            else {
                c2d = new Geom2d_BSplineCurve(poles, knots, mults, s->Degree());
            }
            TopoDS_Edge on = BRepBuilderAPI_MakeEdge(c2d, surfaces[k]).Edge();
            BRepLib::BuildCurves3d(on);
            if (e.Orientation() == TopAbs_REVERSED) {
                on.Reverse();
            }
            mk.Add(on);
        }
        if (!mk.IsDone()) {
            return {};
        }
        wires[k] = mk.Wire();
    }
    BRepOffsetAPI_ThruSections side(false, true);
    side.AddWire(wires[0]);
    side.AddWire(wires[1]);
    side.Build();
    if (!side.IsDone()) {
        return {};
    }
    BRepBuilderAPI_Sewing sew(1e-6);
    for (int k = 0; k < 2; ++k) {
        BRepBuilderAPI_MakeFace cap(surfaces[k], wires[k], true);
        if (!cap.IsDone()) {
            return {};
        }
        ShapeFix_Face fix(cap.Face());
        fix.Perform();
        sew.Add(fix.Face());
    }
    for (TopExp_Explorer it(side.Shape(), TopAbs_FACE); it.More(); it.Next()) {
        sew.Add(it.Current());
    }
    sew.Perform();
    TopoDS_Shell shell;
    for (TopExp_Explorer it(sew.SewedShape(), TopAbs_SHELL); it.More(); it.Next()) {
        shell = TopoDS::Shell(it.Current());
        break;
    }
    if (shell.IsNull()) {
        return {};
    }
    BRepBuilderAPI_MakeSolid solid(shell);
    ShapeFix_Solid fix(solid.Solid());
    fix.Perform();
    TopoDS_Shape res = fix.Solid();
    if (volumeOf(res) < 0) {
        res.Reverse();
    }
    return res;
}

}  // namespace

std::vector<TopoShape> refold(const Model& model, int root, const TopoDS_Face& face,
                              const std::vector<int>& elements)
{
    std::vector<TopoShape> res;
    const double T = model.thickness;
    const double margin = T / 10;
    // as large as the face and the sheet about it
    Bnd_Box box;
    BRepBndLib::Add(face, box);
    const double size = 4 * (std::sqrt(box.SquareExtent()) + 10 * T) + 1;
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    const gp_Pnt centre((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);

    for (int index : elements) {
        if (index < 0 || index >= static_cast<int>(model.elements.size())) {
            continue;
        }
        const Element& el = model.elements[index];
        const gp_Trsf back = model.placement(index, root).Inverted();
        // the face's middle, where this element puts it folded
        const gp_Pnt middle = centre.Transformed(back);
        TopoDS_Shape tool;
        if (el.kind == Element::Kind::Wall) {
            // rigidly, then through the wall's thickness
            const TopoDS_Shape moved = BRepBuilderAPI_Transform(face, back, true).Shape();
            const gp_Dir n = el.frame.Direction();
            gp_Pnt at;
            {
                TopExp_Explorer v(moved, TopAbs_VERTEX);
                at = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
            }
            const double depth = gp_Vec(el.frame.Location(), at).Dot(gp_Vec(n));
            gp_Trsf down;
            down.SetTranslation(gp_Vec(n) * (-depth - margin));
            const TopoDS_Shape start = BRepBuilderAPI_Transform(moved, down, true).Shape();
            tool = BRepPrimAPI_MakePrism(start, gp_Vec(n) * (T + 2 * margin)).Shape();
            // only the wall's own: behind where each bend from it starts,
            // past where each bend into it ends
            for (const Element& b : model.elements) {
                if (b.kind != Element::Kind::Bend) {
                    continue;
                }
                gp_Pnt p;
                gp_Dir d;
                if (b.before == index) {
                    bendStart(b.bend, p, d);
                    tool = cutAway(tool, halfSpace(p, d, middle, size));
                }
                if (b.after == index) {
                    bendEnd(b.bend, p, d);
                    tool = cutAway(tool, halfSpace(p, d.Reversed(), middle, size));
                }
            }
        }
        else {
            // in the bend's own flat, its strip only, onto its cylinder
            const CylBend& b = el.bend;
            const TopoDS_Shape local = BRepBuilderAPI_Transform(face, back, true).Shape();
            const gp_Pnt o = b.frame.Location();
            const gp_Dir y = b.frame.YDirection(), z = b.frame.Direction();
            gp_Pnt at;
            {
                TopExp_Explorer v(local, TopAbs_VERTEX);
                at = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
            }
            // on the face's plane, about its middle along the axis: the
            // frame's origin may lie far down it, where a span was carried on
            const gp_Vec x(b.frame.XDirection());
            const gp_Pnt mid = centre.Transformed(back);
            const gp_Pnt base =
                o.Translated(x * gp_Vec(o, at).Dot(x) + gp_Vec(z) * gp_Vec(o, mid).Dot(gp_Vec(z)));
            const double length = b.angle * b.neutral();
            const gp_Pnt c0 = base.Translated(gp_Vec(z) * (-size / 2));
            BRepBuilderAPI_MakePolygon strip(c0,
                                             c0.Translated(gp_Vec(y) * length),
                                             c0.Translated(gp_Vec(y) * length + gp_Vec(z) * size),
                                             c0.Translated(gp_Vec(z) * size),
                                             true);
            const TopoDS_Face band = BRepBuilderAPI_MakeFace(strip.Wire(), true).Face();
            BRepAlgoAPI_Common common(local, band);
            if (!common.IsDone()) {
                continue;
            }
            TopoDS_Compound pieces;
            BRep_Builder builder;
            builder.MakeCompound(pieces);
            bool any = false;
            for (TopExp_Explorer it(common.Shape(), TopAbs_FACE); it.More(); it.Next()) {
                const TopoDS_Shape piece = radial(b,
                                                  TopoDS::Face(it.Current()),
                                                  b.radius - margin,
                                                  b.radius + b.thickness + margin);
                if (!piece.IsNull()) {
                    builder.Add(pieces, piece);
                    any = true;
                }
            }
            if (!any) {
                continue;
            }
            tool = pieces;
        }
        for (const gp_Pln& limit : el.limits) {
            tool = cutAway(tool,
                           halfSpace(limit.Location(), limit.Axis().Direction(), middle, size));
        }
        if (!tool.IsNull() && volumeOf(tool) > Precision::Confusion()) {
            res.emplace_back(tool);
        }
    }
    return res;
}

TopoDS_Face cornerReliefFace(const Model& model, int root, int bendA, int bendB, int shape,
                             double size)
{
    if ((shape != 1 && shape != 2) || size < Precision::Confusion()) {
        return {};
    }
    // each bend's line in the flat: the middle of its strip, along its axis
    gp_Pnt p[2];
    gp_Dir d[2];
    int bends[2] = {bendA, bendB};
    for (int i = 0; i < 2; ++i) {
        const CylBend& b = model.elements[bends[i]].bend;
        const gp_Trsf move = model.placement(bends[i], root);
        p[i] = b.frame.Location()
                   .Translated(gp_Vec(b.frame.XDirection()) * b.radius
                               + gp_Vec(b.frame.YDirection()) * (b.angle * b.neutral() / 2))
                   .Transformed(move);
        d[i] = b.frame.Direction().Transformed(move);
    }
    // where they cross, or come nearest
    const gp_Vec w(p[1], p[0]);
    const gp_Vec u(d[0]), v(d[1]);
    const double a = u.Dot(u), bb = u.Dot(v), c = v.Dot(v), dd = u.Dot(w), e = v.Dot(w);
    const double den = a * c - bb * bb;
    if (std::abs(den) < Precision::Angular()) {
        return {};
    }
    const double s = (bb * e - c * dd) / den, t = (a * e - bb * dd) / den;
    const gp_Pnt q0 = p[0].Translated(u * s), q1 = p[1].Translated(v * t);
    if (q0.Distance(q1) > 1e-6) {
        return {};
    }
    // onto the root wall's first skin
    const gp_Ax3& frame = model.elements[root].frame;
    const gp_Dir n = frame.Direction();
    gp_Pnt centre((q0.XYZ() + q1.XYZ()) / 2);
    centre.Translate(gp_Vec(n) * -gp_Vec(frame.Location(), centre).Dot(gp_Vec(n)));
    if (shape == 1) {
        const gp_Circ circle(gp_Ax2(centre, n), size / 2);
        const TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(circle).Edge();
        return BRepBuilderAPI_MakeFace(BRepBuilderAPI_MakeWire(edge).Wire(), true).Face();
    }
    const gp_Vec x = gp_Vec(d[0]) * (size / 2);
    const gp_Vec y = gp_Vec(n).Crossed(gp_Vec(d[0])).Normalized() * (size / 2);
    BRepBuilderAPI_MakePolygon square(centre.Translated(-x - y),
                                      centre.Translated(x - y),
                                      centre.Translated(x + y),
                                      centre.Translated(-x + y),
                                      true);
    return BRepBuilderAPI_MakeFace(square.Wire(), true).Face();
}

std::vector<TopoShape> cornerReliefs(const Model& model,
                                     const std::vector<std::pair<int, int>>& corners, int shape,
                                     double size)
{
    std::vector<TopoShape> res;
    for (const auto& [a, b] : corners) {
        const Element& ea = model.elements[a];
        const Element& eb = model.elements[b];
        if (ea.before != eb.before) {
            continue;
        }
        const int root = ea.before;
        const TopoDS_Face face = cornerReliefFace(model, root, a, b, shape, size);
        if (face.IsNull()) {
            continue;
        }
        for (TopoShape& tool : refold(model, root, face, {root, a, b, ea.after, eb.after})) {
            res.push_back(std::move(tool));
        }
    }
    return res;
}

}  // namespace PartDesign::SheetMetal
