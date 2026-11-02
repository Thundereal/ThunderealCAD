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

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
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
#include <Geom2dConvert.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <GeomConvert.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_RectangularTrimmedSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Solid.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
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

#include <Base/Exception.h>

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
                const TopoDS_Face& part = TopoDS::Face(it.Current());
                TopoDS_Shape piece =
                    radial(b, part, b.radius - margin, b.radius + b.thickness + margin);
                // radial() takes the outer wire; a hole in the face stays
                const TopoDS_Wire outerWire = BRepTools::OuterWire(part);
                for (TopExp_Explorer w(part, TopAbs_WIRE); !piece.IsNull() && w.More(); w.Next()) {
                    if (w.Current().IsSame(outerWire)) {
                        continue;
                    }
                    const TopoDS_Face hole =
                        BRepBuilderAPI_MakeFace(TopoDS::Wire(w.Current()), true).Face();
                    const TopoDS_Shape island =
                        radial(b, hole, b.radius - 2 * margin, b.radius + b.thickness + 2 * margin);
                    if (!island.IsNull()) {
                        piece = cutAway(piece, island);
                    }
                }
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

namespace
{

/// The faces of a boolean's result
std::vector<TopoDS_Face> facesOf(const TopoDS_Shape& shape)
{
    std::vector<TopoDS_Face> res;
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
        res.push_back(TopoDS::Face(it.Current()));
    }
    return res;
}

/** A piece of a bend's skin laid in the bend's own flat (toFlat()'s): the
 * cylinder's (u, v) to the flat's (Y, Z) is affine, so each pcurve, made a
 * B-spline, keeps its knots and weights and has its poles mapped. A null
 * face if the piece is not on a cylinder about the bend's axis.
 */
TopoDS_Face bendPieceFlat(const CylBend& b, const TopoDS_Face& piece)
{
    Handle(Geom_Surface) surface = BRep_Tool::Surface(piece);
    if (auto trimmed = Handle(Geom_RectangularTrimmedSurface)::DownCast(surface)) {
        surface = trimmed->BasisSurface();
    }
    auto cylinder = Handle(Geom_CylindricalSurface)::DownCast(surface);
    if (cylinder.IsNull()) {
        return {};
    }
    // the cylinder's angle and height as the bend's
    const gp_Ax3& own = cylinder->Position();
    const gp_Vec x(b.frame.XDirection()), y(b.frame.YDirection()), z(b.frame.Direction());
    const gp_Vec ox(own.XDirection()), oy(own.YDirection()), oz(own.Direction());
    const double phi = std::atan2(ox.Dot(y), ox.Dot(x));
    const double turn = ox.Crossed(oy).Dot(z) > 0 ? 1 : -1;
    const double rise = oz.Dot(z) > 0 ? 1 : -1;
    const double lift = gp_Vec(b.frame.Location(), own.Location()).Dot(z);
    // the bend's own angles, about its middle (CylBend::from())
    double u0, u1, v0, v1;
    BRepTools::UVBounds(piece, u0, u1, v0, v1);
    double shift = 0;
    {
        const double from = b.angle / 2 - std::numbers::pi;
        double a = phi + turn * (u0 + u1) / 2;
        while (a < from) {
            a += 2 * std::numbers::pi;
            shift += 2 * std::numbers::pi;
        }
        while (a >= from + 2 * std::numbers::pi) {
            a -= 2 * std::numbers::pi;
            shift -= 2 * std::numbers::pi;
        }
    }
    const gp_Pnt o = b.frame.Location().Translated(x * b.radius);
    const double nu = b.neutral();
    auto flat = [&](const gp_Pnt2d& uv) {
        const double a = phi + turn * uv.X() + shift;
        return o.Translated(y * (a * nu) + z * (lift + rise * uv.Y()));
    };

    std::vector<TopoDS_Wire> wires;
    const TopoDS_Wire outerWire = BRepTools::OuterWire(piece);
    for (TopExp_Explorer w(piece, TopAbs_WIRE); w.More(); w.Next()) {
        const TopoDS_Wire& wire = TopoDS::Wire(w.Current());
        BRepBuilderAPI_MakeWire mk;
        for (BRepTools_WireExplorer it(wire, piece); it.More(); it.Next()) {
            const TopoDS_Edge& e = it.Current();
            double f, l;
            Handle(Geom2d_Curve) c = BRep_Tool::CurveOnSurface(e, piece, f, l);
            if (c.IsNull()) {
                return {};
            }
            Handle(Geom2d_BSplineCurve) s =
                Geom2dConvert::CurveToBSplineCurve(new Geom2d_TrimmedCurve(c, f, l));
            TColgp_Array1OfPnt poles(1, s->NbPoles());
            for (int i = 1; i <= s->NbPoles(); ++i) {
                poles.SetValue(i, flat(s->Pole(i)));
            }
            TColStd_Array1OfReal knots(1, s->NbKnots());
            TColStd_Array1OfInteger mults(1, s->NbKnots());
            s->Knots(knots);
            s->Multiplicities(mults);
            Handle(Geom_BSplineCurve) c3d;
            if (s->IsRational()) {
                TColStd_Array1OfReal weights(1, s->NbPoles());
                s->Weights(weights);
                c3d = new Geom_BSplineCurve(poles, weights, knots, mults, s->Degree());
            }
            else {
                c3d = new Geom_BSplineCurve(poles, knots, mults, s->Degree());
            }
            TopoDS_Edge on = BRepBuilderAPI_MakeEdge(c3d).Edge();
            if (e.Orientation() == TopAbs_REVERSED) {
                on.Reverse();
            }
            mk.Add(on);
        }
        if (!mk.IsDone()) {
            return {};
        }
        if (wire.IsSame(outerWire)) {
            wires.insert(wires.begin(), mk.Wire());
        }
        else {
            wires.push_back(mk.Wire());
        }
    }
    if (wires.empty()) {
        return {};
    }
    BRepBuilderAPI_MakeFace mk(gp_Pln(o, x), wires.front(), true);
    for (std::size_t i = 1; i < wires.size(); ++i) {
        mk.Add(wires[i]);
    }
    if (!mk.IsDone()) {
        return {};
    }
    ShapeFix_Face fix(mk.Face());
    fix.FixOrientation();
    fix.Perform();
    return fix.Face();
}

}  // namespace

std::vector<TopoShape> cutoutTools(const Model& model, const TopoDS_Shape& part,
                                   const TopoDS_Shape& profile, const gp_Dir& way, bool firstOnly)
{
    const double T = model.thickness;
    const double margin = T / 10;
    Bnd_Box box;
    BRepBndLib::Add(part, box);
    BRepBndLib::Add(profile, box);
    const double size = 4 * (std::sqrt(box.SquareExtent()) + 10 * T) + 1;

    // the sketch's prism on along the way; and from just behind it, for a
    // skin it is drawn on: what lies behind the sketch is not in its way
    const TopoDS_Shape ahead = BRepPrimAPI_MakePrism(profile, gp_Vec(way) * size).Shape();
    gp_Trsf behind;
    behind.SetTranslation(gp_Vec(way) * -margin);
    const TopoDS_Shape onPlane =
        BRepPrimAPI_MakePrism(BRepBuilderAPI_Transform(profile, behind, true).Shape(),
                              gp_Vec(way) * (size + margin))
            .Shape();
    gp_Pnt sketchAt;
    {
        TopExp_Explorer v(profile, TopAbs_VERTEX);
        if (!v.More()) {
            THROWM(Base::ValueError, "A cutout's sketch is empty");
        }
        sketchAt = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
    }

    int root = -1;
    for (std::size_t i = 0; i < model.elements.size(); ++i) {
        if (model.elements[i].kind == Element::Kind::Wall) {
            root = static_cast<int>(i);
            break;
        }
    }
    if (root < 0) {
        THROWM(Base::ValueError, "A cutout builds on a sheet with a wall");
    }

    // what the prism covers on each skin facing the sketch
    std::vector<std::pair<int, TopoDS_Face>> pieces;
    const double facing = 1e-6;
    for (TopExp_Explorer it(part, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face& face = TopoDS::Face(it.Current());
        const int index = model.owner(face);
        if (index < 0) {
            continue;
        }
        const Element& el = model.elements[index];
        BRepAdaptor_Surface surface(face);
        TopoDS_Shape keep;
        const TopoDS_Shape* prism = &ahead;
        if (el.kind == Element::Kind::Wall) {
            if (surface.GetType() != GeomAbs_Plane) {
                continue;
            }
            gp_Dir n = surface.Plane().Axis().Direction();
            if (face.Orientation() == TopAbs_REVERSED) {
                n.Reverse();
            }
            if (!n.IsParallel(el.frame.Direction(), Precision::Angular())
                || n.Dot(way) > -facing) {
                continue;
            }
            const gp_Pnt on = surface.Plane().Location();
            if (n.IsParallel(way, Precision::Angular())
                && std::abs(gp_Vec(sketchAt, on).Dot(gp_Vec(way))) < Precision::Confusion()) {
                prism = &onPlane;
            }
        }
        else {
            if (surface.GetType() != GeomAbs_Cylinder) {
                continue;
            }
            const gp_Ax1 axis = surface.Cylinder().Axis();
            if (!axis.Direction().IsParallel(el.bend.frame.Direction(), Precision::Angular())) {
                continue;
            }
            // the way across the axis: none along it
            const gp_Vec along(axis.Direction());
            const gp_Vec across = gp_Vec(way) - along * gp_Vec(way).Dot(along);
            if (across.Magnitude() < facing) {
                continue;
            }
            // the skin's normal out of the material is outward (+1) or in
            double u0, u1, v0, v1;
            BRepTools::UVBounds(face, u0, u1, v0, v1);
            auto radialAt = [&](double u, gp_Pnt& p) {
                gp_Vec du, dv;
                surface.D1(u, (v0 + v1) / 2, p, du, dv);
                const gp_Vec r(axis.Location(), p);
                return r - along * r.Dot(along);
            };
            double sense;
            {
                gp_Pnt p;
                gp_Vec du, dv;
                surface.D1((u0 + u1) / 2, (v0 + v1) / 2, p, du, dv);
                gp_Vec normal = du.Crossed(dv);
                if (face.Orientation() == TopAbs_REVERSED) {
                    normal.Reverse();
                }
                gp_Pnt q;
                sense = normal.Dot(radialAt((u0 + u1) / 2, q)) > 0 ? 1 : -1;
            }
            // facing where the normal runs against the way: all of it, none,
            // or the half of the cylinder on that side of its axis
            bool any = false, all = true;
            const int samples = 16;
            for (int i = 0; i <= samples; ++i) {
                gp_Pnt p;
                const gp_Vec r = radialAt(u0 + (u1 - u0) * i / samples, p);
                const double g = sense * r.Normalized().Dot(across.Normalized());
                any = any || g < -facing;
                all = all && g < facing;
            }
            if (!any) {
                continue;
            }
            if (!all) {
                gp_Pnt middle;
                radialAt((u0 + u1) / 2, middle);
                keep = halfSpace(axis.Location(), gp_Dir(across * -sense), middle, size);
            }
        }
        BRepAlgoAPI_Common common(face, *prism);
        if (!common.IsDone()) {
            continue;
        }
        TopoDS_Shape covered = common.Shape();
        if (!keep.IsNull()) {
            BRepAlgoAPI_Common half(covered, keep);
            if (!half.IsDone()) {
                continue;
            }
            covered = half.Shape();
        }
        for (const TopoDS_Face& piece : facesOf(covered)) {
            pieces.emplace_back(index, piece);
        }
    }

    // the first met only: each piece less what lies behind the others
    if (firstOnly && pieces.size() > 1) {
        std::vector<TopoDS_Shape> shadows;
        for (const auto& [index, piece] : pieces) {
            shadows.push_back(BRepPrimAPI_MakePrism(piece, gp_Vec(way) * size).Shape());
        }
        std::vector<std::pair<int, TopoDS_Face>> seen;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            TopoDS_Compound others;
            BRep_Builder builder;
            builder.MakeCompound(others);
            for (std::size_t j = 0; j < shadows.size(); ++j) {
                if (j != i) {
                    builder.Add(others, shadows[j]);
                }
            }
            BRepAlgoAPI_Cut cut(pieces[i].second, others);
            if (!cut.IsDone()) {
                continue;
            }
            for (const TopoDS_Face& piece : facesOf(cut.Shape())) {
                seen.emplace_back(pieces[i].first, piece);
            }
        }
        pieces = std::move(seen);
    }

    std::vector<TopoShape> res;
    for (const auto& [index, piece] : pieces) {
        const Element& el = model.elements[index];
        TopoDS_Face flat;
        if (el.kind == Element::Kind::Wall) {
            flat = piece;
        }
        else {
            flat = bendPieceFlat(el.bend, piece);
            if (flat.IsNull()) {
                FC_THROWM(Base::ValueError,
                          "A cutout could not lay its piece on " << el.name << " flat");
            }
        }
        flat = TopoDS::Face(
            BRepBuilderAPI_Transform(flat, model.placement(index, root), true).Shape());
        for (TopoShape& tool : refold(model, root, flat, {index})) {
            res.push_back(std::move(tool));
        }
    }
    if (res.empty()) {
        THROWM(Base::ValueError, "A cutout's sketch meets no skin of the sheet facing it");
    }
    return res;
}

}  // namespace PartDesign::SheetMetal
