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
#include <limits>
#include <iomanip>
#include <numbers>
#include <sstream>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <Bnd_Box.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepFill.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepOffsetAPI_MakeFilling.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepTools.hxx>
#include <BRepTools_History.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom2dConvert.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomFill.hxx>
#include <TColStd_HArray1OfReal.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopTools_ListOfShape.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Precision.hxx>
#include <GProp_GProps.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_SurfaceOfLinearExtrusion.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Solid.hxx>
#include <ShapeFix_Wire.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <TopTools_DataMapOfShapeInteger.hxx>
#include <TopTools_DataMapOfShapeShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Lin.hxx>
#include <gp_Vec.hxx>

#include <Base/Exception.h>

#include "Flat.h"
#include "Pieces.h"

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

    /// Each edge mapped so far, forward: an edge two faces share is mapped
    /// once, so they meet on it
    mutable TopTools_DataMapOfShapeShape done;

    /// An edge of a bend's face, mapped: exactly on the bend's cylinders,
    /// else a line where it maps to one, else a B-spline through samples
    TopoDS_Edge edge(int element, const TopoDS_Edge& e, const TopoDS_Face& face) const
    {
        const TopoDS_Shape key = e.Oriented(TopAbs_FORWARD);
        TopoDS_Edge res;
        if (done.IsBound(key)) {
            res = TopoDS::Edge(done(key));
        }
        else {
            if (model.elements[element].kind == Element::Kind::Cone) {
                res = onCone(element, TopoDS::Edge(key));
            }
            else {
                res = onCylinder(element, TopoDS::Edge(key), face);
            }
            if (res.IsNull()) {
                res = sampled(element, TopoDS::Edge(key));
            }
            done.Bind(key, res);
        }
        res.Orientation(e.Orientation());
        return res;
    }

    /** An edge on a cylinder about the bend's axis, mapped exactly: unrolled,
     * the cylinder's (u, v) goes to the flat affinely, so the poles of the
     * edge's pcurve there, made a B-spline, are mapped. The hole a sketch
     * cuts across a bend lies flat as the shape drawn, and its outlines on
     * the two skins as one curve, the wall between them square to the flat.
     * Null for an edge on another surface, or with no pcurve on it.
     */
    TopoDS_Edge onCylinder(int element, const TopoDS_Edge& e, const TopoDS_Face& face) const
    {
        BRepAdaptor_Surface s(face);
        if (s.GetType() != GeomAbs_Cylinder) {
            return {};
        }
        const gp_Ax1 axis = s.Cylinder().Axis();
        const gp_Ax3& frame = model.elements[element].bend.frame;
        if (!axis.IsParallel(frame.Axis(), Precision::Angular())
            || gp_Lin(frame.Axis()).Distance(axis.Location()) > Precision::Confusion()) {
            return {};
        }
        double f, l;
        Handle(Geom2d_Curve) c2d = BRep_Tool::CurveOnSurface(e, face, f, l);
        if (c2d.IsNull()) {
            return {};
        }
        // the map about the face's middle, from where (u, v) moves it
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        const double um = (u0 + u1) / 2, vm = (v0 + v1) / 2;
        const double du = std::max((u1 - u0) / 4, 1e-3), dv = std::max((v1 - v0) / 4, 1.0);
        const gp_Pnt o = map(element, s.Value(um, vm));
        const gp_Vec alongU = gp_Vec(o, map(element, s.Value(um + du, vm))) / du;
        const gp_Vec alongV = gp_Vec(o, map(element, s.Value(um, vm + dv))) / dv;

        Handle(Geom2d_BSplineCurve) b =
            Geom2dConvert::CurveToBSplineCurve(new Geom2d_TrimmedCurve(c2d, f, l));
        const int n = b->NbPoles();
        TColgp_Array1OfPnt poles(1, n);
        for (int i = 1; i <= n; ++i) {
            const gp_Pnt2d p = b->Pole(i);
            poles.SetValue(i, o.Translated(alongU * (p.X() - um) + alongV * (p.Y() - vm)));
        }
        if (b->Degree() == 1 && n == 2) {
            if (poles(1).Distance(poles(2)) <= Precision::Confusion()) {
                return {};
            }
            return BRepBuilderAPI_MakeEdge(poles(1), poles(2)).Edge();
        }
        TColStd_Array1OfReal knots(1, b->NbKnots());
        TColStd_Array1OfInteger mults(1, b->NbKnots());
        b->Knots(knots);
        b->Multiplicities(mults);
        Handle(Geom_BSplineCurve) curve;
        if (b->IsRational()) {
            TColStd_Array1OfReal weights(1, n);
            b->Weights(weights);
            curve = new Geom_BSplineCurve(poles, weights, knots, mults, b->Degree(),
                                          b->IsPeriodic());
        }
        else {
            curve = new Geom_BSplineCurve(poles, knots, mults, b->Degree(), b->IsPeriodic());
        }
        BRepBuilderAPI_MakeEdge mk(curve);
        return mk.IsDone() ? mk.Edge() : TopoDS_Edge();
    }

    /** A circle about a cone's axis, mapped exactly: it keeps its distance
     * from the neutral cone's apex and its depth, so it lies flat as an arc
     * about the apex unrolled -- a line for a cylinder's. Null for any other
     * edge.
     */
    TopoDS_Edge onCone(int element, const TopoDS_Edge& e) const
    {
        BRepAdaptor_Curve c(e);
        if (c.GetType() != GeomAbs_Circle) {
            return {};
        }
        const gp_Ax3& frame = model.elements[element].cone.frame;
        const gp_Ax1 axis = c.Circle().Axis();
        if (!axis.IsParallel(frame.Axis(), Precision::Angular())
            || gp_Lin(frame.Axis()).Distance(axis.Location()) > Precision::Confusion()) {
            return {};
        }
        const double f = c.FirstParameter(), l = c.LastParameter();
        const gp_Pnt a = map(element, c.Value(f)), m = map(element, c.Value((f + l) / 2)),
                     b = map(element, c.Value(l));
        if (gp_Vec(a, m).Crossed(gp_Vec(a, b)).Magnitude()
            <= Precision::Confusion() * a.Distance(b)) {
            return BRepBuilderAPI_MakeEdge(a, b).Edge();
        }
        GC_MakeArcOfCircle arc(a, m, b);
        if (!arc.IsDone()) {
            return {};
        }
        BRepBuilderAPI_MakeEdge mk(arc.Value());
        return mk.IsDone() ? mk.Edge() : TopoDS_Edge();
    }

    /// An edge mapped by points: a line where it maps to one, else a
    /// B-spline through samples
    TopoDS_Edge sampled(int element, const TopoDS_Edge& e) const
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
        return res;
    }

    /// Points of a face inside its outline: of a grid over its parameters,
    /// those in it
    static std::vector<gp_Pnt> inside(const TopoDS_Face& f)
    {
        std::vector<gp_Pnt> res;
        BRepAdaptor_Surface s(f, false);
        double u0, u1, v0, v1;
        BRepTools::UVBounds(f, u0, u1, v0, v1);
        constexpr int n = 4;
        for (int i = 1; i < n; ++i) {
            for (int j = 1; j < n; ++j) {
                const gp_Pnt2d uv(u0 + (u1 - u0) * i / n, v0 + (v1 - v0) * j / n);
                BRepClass_FaceClassifier where(f, uv, 1e-7);
                if (where.State() == TopAbs_IN) {
                    res.push_back(s.Value(uv.X(), uv.Y()));
                }
            }
        }
        return res;
    }

    /// A face's outline unrolled. pastSeam: of a part of a face closed round
    /// on itself with its seam in it -- the seam is there and back, and the
    /// outline runs on past it both ways
    TopoDS_Wire wire(int element, const TopoDS_Wire& w, const TopoDS_Face& face,
                     bool pastSeam = false) const
    {
        BRepBuilderAPI_MakeWire mk;
        Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape;
        TopTools_DataMapOfShapeInteger passes;
        for (BRepTools_WireExplorer it(w, face); it.More() && pastSeam; it.Next()) {
            const TopoDS_Shape key = it.Current().Oriented(TopAbs_FORWARD);
            if (!passes.IsBound(key)) {
                passes.Bind(key, 0);
            }
            ++passes.ChangeFind(key);
        }
        for (BRepTools_WireExplorer it(w, face); it.More(); it.Next()) {
            if (pastSeam && passes.Find(it.Current().Oriented(TopAbs_FORWARD)) > 1) {
                continue;
            }
            const TopoDS_Edge e = edge(element, it.Current(), face);
            edges->Append(e);
            if (mk.Error() == BRepBuilderAPI_WireDone || edges->Length() == 1) {
                mk.Add(e);
            }
        }
        if (mk.IsDone() && edges->Length() == mk.Wire().NbChildren()) {
            return mk.Wire();
        }
        // Each edge is mapped from its own ends, so two meeting at a vertex
        // of a looser tolerance -- a corner relief's, cut by a boolean (5e-6)
        // -- end that far apart flat: joined within the folded wire's
        // tolerance
        double tol = Precision::Confusion();
        for (TopExp_Explorer it(w, TopAbs_VERTEX); it.More(); it.Next()) {
            tol = std::max(tol, 2 * BRep_Tool::Tolerance(TopoDS::Vertex(it.Current())));
        }
        Handle(TopTools_HSequenceOfShape) wires;
        ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges, tol, false, wires);
        if (wires.IsNull() || wires->Length() != 1 || !BRep_Tool::IsClosed(wires->Value(1))) {
            THROWM(Base::ValueError, "A face of a bend does not unroll into a closed outline");
        }
        ShapeFix_Wire fix(TopoDS::Wire(wires->Value(1)), TopoDS_Face(), tol);
        fix.FixConnected(tol);
        return fix.Wire();
    }

    /** A face across the sheet cut square to it (a hole's wall, a slot's
     * side): its edge on one skin lies straight over its edge on the other,
     * and it is upright, on the extrusion of the one along Z, bounded by the
     * outline's own edges. Ruled between the two instead, a hole's closed
     * outlines, parametrised apart, would twist it. The two lie apart by what
     * the boolean that cut them left (4e-6 for a hole made by refold()),
     * which goes into the edges' tolerance. Null where they do not lie so
     * (a miter's slant).
     */
    static TopoDS_Face upright(const TopoDS_Wire& outline, const TopoDS_Edge& a,
                               const TopoDS_Edge& b)
    {
        constexpr double tol = 1e-4;
        BRepAdaptor_Curve ca(a), cb(b);
        const double z = ca.Value(ca.FirstParameter()).Z();
        const double dz = cb.Value(cb.FirstParameter()).Z() - z;
        if (std::abs(dz) <= tol) {
            return {};
        }
        constexpr int n = 16;
        for (int i = 0; i <= n; ++i) {
            const double ta =
                ca.FirstParameter() + (ca.LastParameter() - ca.FirstParameter()) * i / n;
            const double tb =
                cb.FirstParameter() + (cb.LastParameter() - cb.FirstParameter()) * i / n;
            if (std::abs(ca.Value(ta).Z() - z) > tol
                || std::abs(cb.Value(tb).Z() - z - dz) > tol) {
                return {};
            }
            const gp_Pnt under = cb.Value(tb).Translated(gp_Vec(0, 0, -dz));
            BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(under).Vertex(), a);
            if (!dist.IsDone() || dist.Value() > tol) {
                return {};
            }
        }
        double f, l;
        Handle(Geom_Curve) curve = BRep_Tool::Curve(a, f, l);
        if (curve.IsNull()) {
            return {};
        }
        Handle(Geom_Surface) surface =
            new Geom_SurfaceOfLinearExtrusion(new Geom_TrimmedCurve(curve, f, l), gp::DZ());
        BRepBuilderAPI_MakeFace mk(surface, outline, true);
        if (!mk.IsDone()) {
            return {};
        }
        ShapeFix_Face fix(mk.Face());
        fix.Perform();
        return fix.Face();
    }

    /** A face of a bend closed round on itself -- the wall of a hole drilled
     * through the bend, a countersink's cone -- unrolled. No outline of it
     * lies flat as a plane, and its two rims meet only along its seam: it is
     * laid out as the surface through its points unrolled, taken along each
     * line of its surface across it from the one rim to the other, so the
     * surface's own ends are the rims: in bands across it, each ruled between
     * two curves round it. None for a face that is not so: closed round its
     * surface's U, two rims and the seam, no hole in it.
     */
    std::vector<TopoDS_Face> around(int element, const TopoDS_Face& f) const
    {
        BRepAdaptor_Surface s(f, false);
        double u0, u1, v0, v1;
        BRepTools::UVBounds(f, u0, u1, v0, v1);
        if (!s.IsUPeriodic() || std::abs(u1 - u0 - s.UPeriod()) > 1e-9) {
            return {};
        }
        int wires = 0;
        for (TopExp_Explorer it(f, TopAbs_WIRE); it.More(); it.Next()) {
            ++wires;
        }
        // its rims: the edges between the seam's two passes
        std::vector<TopoDS_Edge> order;
        std::vector<bool> seam;
        for (BRepTools_WireExplorer it(BRepTools::OuterWire(f), f); it.More(); it.Next()) {
            order.push_back(it.Current());
            seam.push_back(BRep_Tool::IsClosed(it.Current(), f));
        }
        const auto first = std::find(seam.begin(), seam.end(), true);
        if (wires != 1 || std::count(seam.begin(), seam.end(), true) != 2) {
            return {};
        }
        const std::size_t start = static_cast<std::size_t>(first - seam.begin());
        std::vector<TopoDS_Edge> rims[2];
        int rim = -1;
        for (std::size_t k = 0; k < order.size(); ++k) {
            const std::size_t i = (start + k) % order.size();
            if (seam[i]) {
                ++rim;
            }
            else {
                rims[rim].push_back(order[i]);
            }
        }
        if (rims[0].empty() || rims[1].empty()) {
            return {};
        }
        // where a rim crosses the surface's line at u: on its edge's curve,
        // which the face next to it shares, and how far along the line
        auto cross = [&](const std::vector<TopoDS_Edge>& edges, double u, gp_Pnt& on, double& v) {
            for (const TopoDS_Edge& e : edges) {
                double a, b;
                Handle(Geom2d_Curve) c2d = BRep_Tool::CurveOnSurface(e, f, a, b);
                if (c2d.IsNull()) {
                    return false;
                }
                const double ua = c2d->Value(a).X(), ub = c2d->Value(b).X();
                if (u < std::min(ua, ub) || u > std::max(ua, ub)) {
                    continue;
                }
                const bool rising = ub > ua;
                for (int k = 0; k < 64; ++k) {
                    const double m = (a + b) / 2;
                    ((c2d->Value(m).X() < u) == rising ? a : b) = m;
                }
                const double t = (a + b) / 2;
                v = c2d->Value(t).Y();
                on = BRepAdaptor_Curve(e).Value(t);
                return true;
            }
            return false;
        };
        // each rim once round, and where its edges meet: there the rim may
        // turn a corner -- from the skin's own edge to one a bend's line cut
        const double period = u1 - u0;
        std::vector<double> stops {u0, u1};
        for (const auto& edges : rims) {
            double covered = 0;
            for (const TopoDS_Edge& e : edges) {
                double a, b;
                Handle(Geom2d_Curve) c2d = BRep_Tool::CurveOnSurface(e, f, a, b);
                if (c2d.IsNull()) {
                    return {};
                }
                const double ua = c2d->Value(a).X(), ub = c2d->Value(b).X();
                covered += std::abs(ub - ua);
                stops.push_back(ua);
                stops.push_back(ub);
            }
            if (std::abs(covered - period) > 1e-6) {
                return {};
            }
        }
        std::sort(stops.begin(), stops.end());
        stops.erase(std::unique(stops.begin(),
                                stops.end(),
                                [](double a, double b) { return std::abs(a - b) < 1e-7; }),
                    stops.end());
        // Between each two of those, curves round it from the one rim to the
        // other, each through its points unrolled, by the surface's U, and
        // between each two curves a band ruled from the one to the other.
        // The rims' own are the first and the last.
        constexpr int across = 4;
        constexpr int round = 96;
        std::vector<TopoDS_Face> res;
        std::vector<gp_Pnt> closing;
        for (std::size_t stop = 0; stop + 1 < stops.size(); ++stop) {
            const double from = stops[stop], to = stops[stop + 1];
            const int n = std::max(8, static_cast<int>(std::ceil(round * (to - from) / period)));
            Handle(TColStd_HArray1OfReal) at = new TColStd_HArray1OfReal(1, n + 1);
            std::vector<Handle(TColgp_HArray1OfPnt)> lines;
            for (int k = 0; k <= across; ++k) {
                lines.push_back(new TColgp_HArray1OfPnt(1, n + 1));
            }
            for (int i = 0; i <= n; ++i) {
                // just inside the ends, where the next edge of a rim starts
                const double u =
                    std::clamp(from + (to - from) * i / n, from + 1e-10, to - 1e-10);
                at->SetValue(i + 1, from + (to - from) * i / n);
                gp_Pnt on[2];
                double v[2];
                if (!cross(rims[0], u, on[0], v[0]) || !cross(rims[1], u, on[1], v[1])) {
                    return {};
                }
                lines[0]->SetValue(i + 1, map(element, on[0]));
                lines[across]->SetValue(i + 1, map(element, on[1]));
                for (int k = 1; k < across; ++k) {
                    lines[k]->SetValue(
                        i + 1, map(element, s.Value(u, v[0] + (v[1] - v[0]) * k / across)));
                }
            }
            // closed: the last line round it is the first
            for (int k = 0; k <= across; ++k) {
                if (stop == 0) {
                    closing.push_back(lines[k]->Value(1));
                }
                if (stop + 2 == stops.size()) {
                    lines[k]->SetValue(n + 1, closing[k]);
                }
            }
            std::vector<Handle(Geom_BSplineCurve)> curves;
            for (const auto& line : lines) {
                GeomAPI_Interpolate through(line, at, false, Precision::Confusion());
                through.Perform();
                if (!through.IsDone()) {
                    return {};
                }
                curves.push_back(through.Curve());
            }
            for (int k = 0; k < across; ++k) {
                BRepBuilderAPI_MakeFace mk(GeomFill::Surface(curves[k], curves[k + 1]),
                                           Precision::Confusion());
                if (!mk.IsDone()) {
                    return {};
                }
                res.push_back(mk.Face());
            }
        }
        return res;
    }

    /// A face of a bend, unrolled: it must lie flat as a plane, but for a
    /// cut across the thickness at a slant (a miter's), which unrolls into a
    /// face ruled between its edges on the two skins, and for what is cut
    /// otherwise than square to the sheet (risk 2): the wall of a hole
    /// through the bend (around()), and any face of one outline, a surface
    /// filled to it, and with through, through points of the face unrolled
    /// (a slanted wall bulges between its edges; a miter's slant is filled
    /// to its edges alone, as it was); filled says so. pastSeam as wire()
    /// has it.
    TopoDS_Face face(int element, const TopoDS_Face& f, int index, bool& filled,
                     bool pastSeam = false, bool through = false) const
    {
        filled = false;
        const TopoDS_Wire outer = BRepTools::OuterWire(f);
        const TopoDS_Wire unrolled = wire(element, outer, f, pastSeam);
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
            // closed round on itself, its seam there and back: square to the
            // sheet it is upright; else its rims start apart, and it is laid
            // out by around(), not ruled between them nor filled
            bool closedRound = false;
            for (std::size_t a = 0; a < edges.size() && !closedRound; ++a) {
                for (std::size_t b = a + 1; b < edges.size() && !closedRound; ++b) {
                    closedRound = edges[a].IsSame(edges[b]);
                }
            }
            if (wires == 1 && edges.size() == 4) {
                // the two edges across the thickness are straight; the face
                // runs between the other two, set the same way round
                const std::size_t a = straight(1) && straight(3) ? 0 : 1;
                if (straight(a + 1) && straight(a + 3)) {
                    const TopoDS_Face square = upright(unrolled, edges[a], edges[a + 2]);
                    if (!square.IsNull()) {
                        return square;
                    }
                    if (!closedRound) {
                        return BRepFill::Face(edges[a], TopoDS::Edge(edges[a + 2].Reversed()));
                    }
                }
            }
            if (wires == 1 && edges.size() >= 3 && !closedRound) {
                // Across the thickness, cut by a corner relief as well as a
                // miter: neither plane nor ruled. A surface filled to its
                // outline, the edges its neighbours share as they are.
                BRepOffsetAPI_MakeFilling fill;
                for (const TopoDS_Edge& e : edges) {
                    fill.Add(e, GeomAbs_C0);
                }
                for (const gp_Pnt& p : through ? inside(f) : std::vector<gp_Pnt>()) {
                    fill.Add(map(element, p));
                }
                fill.Build();
                if (fill.IsDone()) {
                    for (TopExp_Explorer it(fill.Shape(), TopAbs_FACE); it.More(); it.Next()) {
                        filled = true;
                        return TopoDS::Face(it.Current());
                    }
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

/** The parts of a face of the folded sheet that lies in more than one
 * element -- a hole's wall across the line where a bend starts -- cut where
 * the bends it crosses meet their walls: at the half plane from each bend's
 * axis through where it starts, and where it ends. Each part then lies flat
 * by its own element. The face alone where it lies in one, as a skin does
 * and most faces across the sheet.
 */
std::vector<TopoDS_Face> partsOf(const TopoDS_Face& face, const Model& model, int owner)
{
    if (owner < 0 || model.elements[owner].kind == Element::Kind::Cone) {
        return {face};
    }
    const Element& element = model.elements[owner];
    BRepAdaptor_Surface surface(face);
    if (element.kind == Element::Kind::Wall) {
        if (surface.GetType() == GeomAbs_Plane
            && surface.Plane().Axis().Direction().IsParallel(element.frame.Direction(),
                                                             Precision::Angular())) {
            return {face};
        }
    }
    else if (surface.GetType() == GeomAbs_Cylinder) {
        const gp_Ax1 axis = surface.Cylinder().Axis();
        if (axis.IsParallel(element.bend.frame.Axis(), Precision::Angular())
            && gp_Lin(element.bend.frame.Axis()).Distance(axis.Location())
                   <= Precision::Confusion()) {
            return {face};
        }
    }
    // The bends it crosses, by its edges: a bend's own face by where they
    // leave the bend; a wall's by where they lie in a bend of the wall's,
    // off the planes the bend meets its walls at -- a wall's slab runs on
    // through its bends, and its end's own faces touch those planes
    std::vector<int> bends;
    constexpr double tol = 1e-6;
    auto within = [&](int index, const gp_Pnt& p) {
        const CylBend& bend = model.elements[index].bend;
        if (!model.contains(index, p, tol)) {
            return false;
        }
        for (double angle : {0.0, bend.angle}) {
            gp_Dir out = bend.frame.XDirection();
            out.Rotate(bend.frame.Axis(), angle);
            const gp_Vec normal(bend.frame.Direction().Crossed(out));
            if (std::abs(gp_Vec(bend.frame.Location(), p).Dot(normal)) <= tol) {
                return false;
            }
        }
        return true;
    };
    std::vector<int> candidates;
    if (element.kind == Element::Kind::Bend) {
        candidates.push_back(owner);
    }
    else {
        for (std::size_t b = 0; b < model.elements.size(); ++b) {
            const Element& bend = model.elements[b];
            if (bend.kind == Element::Kind::Bend && (bend.before == owner || bend.after == owner)) {
                candidates.push_back(static_cast<int>(b));
            }
        }
    }
    for (TopExp_Explorer it(face, TopAbs_EDGE); it.More() && !candidates.empty(); it.Next()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
        const double f = curve.FirstParameter(), l = curve.LastParameter();
        constexpr int n = 4;
        for (int k = 0; k <= n; ++k) {
            const gp_Pnt p = curve.Value(f + (l - f) * k / n);
            for (int index : candidates) {
                const bool crossed = element.kind == Element::Kind::Bend
                    ? !model.contains(index, p, tol)
                    : within(index, p);
                if (crossed && std::find(bends.begin(), bends.end(), index) == bends.end()) {
                    bends.push_back(index);
                }
            }
        }
    }
    if (bends.empty()) {
        return {face};
    }
    Bnd_Box box;
    BRepBndLib::Add(face, box);
    TopTools_ListOfShape tools;
    for (int index : bends) {
        const CylBend& bend = model.elements[index].bend;
        const gp_Ax3& frame = bend.frame;
        Bnd_Box reach = box;
        reach.Add(frame.Location());
        const double size = 2 * std::sqrt(reach.SquareExtent()) + 1;
        for (double angle : {0.0, bend.angle}) {
            gp_Dir out = frame.XDirection();
            out.Rotate(frame.Axis(), angle);
            const gp_Pln plane(gp_Ax3(frame.Location(), frame.Direction().Crossed(out), out));
            tools.Append(BRepBuilderAPI_MakeFace(plane, 0, size, -size, size).Face());
        }
    }
    // A hole's or a slanted pocket's wall goes round: cut along its own
    // lines too, every 30 deg about its axis, so that no part of it in a
    // bend turns far from a plane -- a surface is filled to each part's
    // outline (Mapper::face()), which a part wrapped half round defeats
    if (surface.GetType() == GeomAbs_Cylinder || surface.GetType() == GeomAbs_Cone) {
        const gp_Ax3 position = surface.GetType() == GeomAbs_Cylinder
            ? surface.Cylinder().Position()
            : surface.Cone().Position();
        Bnd_Box reach = box;
        reach.Add(position.Location());
        const double size = 2 * std::sqrt(reach.SquareExtent()) + 1;
        constexpr int sectors = 12;
        for (int k = 0; k < sectors; ++k) {
            gp_Dir out = position.XDirection();
            out.Rotate(position.Axis(), 2 * std::numbers::pi * k / sectors);
            const gp_Pln plane(
                gp_Ax3(position.Location(), position.Direction().Crossed(out), out));
            tools.Append(BRepBuilderAPI_MakeFace(plane, 0, size, -size, size).Face());
        }
    }
    TopTools_ListOfShape arguments;
    arguments.Append(face);
    BRepAlgoAPI_Splitter splitter;
    splitter.SetArguments(arguments);
    splitter.SetTools(tools);
    splitter.Build();
    std::vector<TopoDS_Face> res;
    if (splitter.IsDone()) {
        for (TopExp_Explorer it(splitter.Shape(), TopAbs_FACE); it.More(); it.Next()) {
            res.push_back(TopoDS::Face(it.Current()));
        }
    }
    if (res.size() < 2) {
        return {face};
    }
    return res;
}

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

/// What turns the flat about Z so that the image of edge runs along X
gp_Trsf alongXOf(const TopoDS_Shape& folded, const Mapper& mapper, const TopoDS_Edge& edge)
{
    TopTools_IndexedDataMapOfShapeListOfShape faces;
    TopExp::MapShapesAndAncestors(folded, TopAbs_EDGE, TopAbs_FACE, faces);
    const int index = faces.FindIndex(edge);
    int owner = -1;
    if (index > 0) {
        for (const TopoDS_Shape& face : faces(index)) {
            owner = mapper.model.owner(TopoDS::Face(face));
            if (owner >= 0) {
                break;
            }
        }
    }
    if (owner < 0) {
        THROWM(Base::ValueError, "The edge to lay the flat by is on no wall or bend of the part");
    }
    BRepAdaptor_Curve c(edge);
    const double f = c.FirstParameter(), l = c.LastParameter();
    std::vector<gp_Pnt> pts;
    for (int i = 0; i <= 8; ++i) {
        pts.push_back(mapper.map(owner, c.Value(f + (l - f) * i / 8)));
    }
    gp_Vec chord(pts.front(), pts.back());
    chord.SetZ(0);
    bool straight = chord.Magnitude() > Precision::Confusion();
    for (const gp_Pnt& p : pts) {
        if (!straight) {
            break;
        }
        gp_Vec v(pts.front(), p);
        v.SetZ(0);
        straight = v.Crossed(chord).Magnitude() / chord.Magnitude() <= Precision::Confusion();
    }
    if (!straight) {
        THROWM(Base::ValueError,
               "The edge to lay the flat by does not lie straight along the flat");
    }
    gp_Trsf turn;
    turn.SetRotation(gp::OZ(), -std::atan2(chord.Y(), chord.X()));
    return turn;
}

}  // namespace

std::vector<TopoDS_Edge> grainArrow(const TopoDS_Shape& solid, double angle)
{
    Bnd_Box box;
    BRepBndLib::AddOptimal(solid, box, /*useTriangulation*/ false, /*useShapeTolerance*/ false);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    const double length = 0.4 * std::min(x1 - x0, y1 - y0);
    const gp_Pnt mid((x0 + x1) / 2, (y0 + y1) / 2, z1);
    const gp_Vec along(std::cos(angle), std::sin(angle), 0);
    const gp_Pnt tip = mid.Translated(along * (length / 2));
    const gp_Pnt tail = mid.Translated(along * (-length / 2));
    std::vector<TopoDS_Edge> res {BRepBuilderAPI_MakeEdge(tail, tip)};
    for (double side : {1.0, -1.0}) {
        const double a = angle + std::numbers::pi + side * std::numbers::pi / 6;
        const gp_Vec back(std::cos(a), std::sin(a), 0);
        res.push_back(BRepBuilderAPI_MakeEdge(tip, tip.Translated(back * (length / 4))));
    }
    return res;
}

std::vector<TopoDS_Edge> onBlank(const TopoDS_Shape& solid, const TopoDS_Edge& line)
{
    constexpr double tol = 1e-6;
    gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(line));
    gp_Pnt b = BRep_Tool::Pnt(TopExp::LastVertex(line));
    if (a.Distance(b) <= Precision::Confusion()) {
        return {};
    }
    // the blank's faces in the line's plane, across Z
    BRep_Builder builder;
    TopoDS_Compound faces;
    builder.MakeCompound(faces);
    bool any = false;
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face& face = TopoDS::Face(it.Current());
        if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane) {
            continue;
        }
        bool inPlane = true;
        for (TopExp_Explorer v(face, TopAbs_VERTEX); v.More() && inPlane; v.Next()) {
            inPlane = std::abs(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).Z() - a.Z()) < tol;
        }
        if (inPlane) {
            builder.Add(faces, face);
            any = true;
        }
    }
    if (!any) {
        return {line};
    }
    BRepAlgoAPI_Common common(line, faces);
    common.SetFuzzyValue(1e-7);
    common.Build();
    if (!common.IsDone()) {
        return {line};
    }
    // the pieces as spans along the line, those the faces' seams part joined
    const gp_Vec along(a, b);
    const double length = along.Magnitude();
    const gp_Vec dir = along / length;
    std::vector<std::pair<double, double>> spans;
    for (TopExp_Explorer it(common.Shape(), TopAbs_EDGE); it.More(); it.Next()) {
        const TopoDS_Edge& e = TopoDS::Edge(it.Current());
        double s0 = gp_Vec(a, BRep_Tool::Pnt(TopExp::FirstVertex(e))).Dot(dir);
        double s1 = gp_Vec(a, BRep_Tool::Pnt(TopExp::LastVertex(e))).Dot(dir);
        if (s0 > s1) {
            std::swap(s0, s1);
        }
        if (s1 - s0 > tol) {
            spans.emplace_back(s0, s1);
        }
    }
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<double, double>> joined;
    for (const auto& span : spans) {
        if (!joined.empty() && span.first <= joined.back().second + tol) {
            joined.back().second = std::max(joined.back().second, span.second);
        }
        else {
            joined.push_back(span);
        }
    }
    std::vector<TopoDS_Edge> res;
    for (const auto& [s0, s1] : joined) {
        res.push_back(BRepBuilderAPI_MakeEdge(a.Translated(dir * std::max(s0, 0.0)),
                                              a.Translated(dir * std::min(s1, length)))
                          .Edge());
    }
    return res;
}

std::vector<TopoDS_Face> facesOnXY(const TopoDS_Shape& solid)
{
    // the faces of one element each, merged where they lie in one plane
    ShapeUpgrade_UnifySameDomain unify(solid, true, true, false);
    unify.Build();
    const TopoDS_Shape merged = unify.Shape();

    std::vector<TopoDS_Face> res;
    for (TopExp_Explorer it(merged, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face& face = TopoDS::Face(it.Current());
        BRepAdaptor_Surface s(face);
        if (s.GetType() != GeomAbs_Plane) {
            continue;
        }
        bool onXY = true;
        for (TopExp_Explorer v(face, TopAbs_VERTEX); v.More() && onXY; v.Next()) {
            onXY = std::abs(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).Z()) < 1e-6;
        }
        if (onXY) {
            res.push_back(face);
        }
    }
    if (res.empty()) {
        THROWM(Base::ValueError, "The flat has no face on XY");
    }
    return res;
}

std::string crossBreakNote(const FlatResult::CrossBreak& cross, const char* degree)
{
    std::ostringstream res;
    res << "CROSS BREAK " << (cross.up ? "UP " : "DOWN ") << std::setprecision(6)
        << cross.angle * 180 / std::numbers::pi << (degree ? degree : "");
    return res.str();
}

TopoDS_Face crossBreakFace(const TopoDS_Shape& shape, const Model& model, int index)
{
    TopoDS_Face res;
    if (index < 0 || index >= static_cast<int>(model.elements.size())) {
        return res;
    }
    const Element& wall = model.elements[index];
    if (wall.kind != Element::Kind::Wall || wall.crossBreak == 0) {
        return res;
    }
    const gp_Dir rise = wall.crossBreak > 0 ? wall.frame.Direction()
                                            : wall.frame.Direction().Reversed();
    double best = 0;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face& face = TopoDS::Face(faces(i));
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) {
            continue;
        }
        gp_Dir normal = surface.Plane().Axis().Direction();
        if (face.Orientation() == TopAbs_REVERSED) {
            normal.Reverse();
        }
        if (normal.Dot(rise) < 1 - 1e-7 || model.owner(face) != index) {
            continue;
        }
        GProp_GProps props;
        BRepGProp::SurfaceProperties(face, props);
        if (props.Mass() > best) {
            best = props.Mass();
            res = face;
        }
    }
    return res;
}

namespace
{

/// A gusset: a form across a bend, owned by it
bool acrossBend(const Model& model, const Form& form)
{
    return form.wall >= 0 && form.wall < static_cast<int>(model.elements.size())
        && model.elements[form.wall].kind == Element::Kind::Bend;
}

/// The box of what a form takes of the part, in its frame
TopoDS_Shape formBox(const Form& form)
{
    const gp_Ax3& f = form.frame;
    const gp_Pnt corner = f.Location().Translated(gp_Vec(f.XDirection()) * -form.halfLength
                                                  + gp_Vec(f.YDirection()) * -form.halfWidth
                                                  + gp_Vec(f.Direction()) * form.zMin);
    return BRepPrimAPI_MakeBox(gp_Ax2(corner, f.Direction(), f.XDirection()),
                               2 * form.halfLength,
                               2 * form.halfWidth,
                               form.zMax - form.zMin)
        .Shape();
}

/** The part with each gusset's box put back as its bend and the bend's
 * walls were there, plain: a gusset is no wall's, carried flat with it, and
 * the bend it crosses lies flat as if it were not there. Anything else cut
 * in the box goes too. origins: each face the putting back made of folded
 * faces, one of those
 */
TopoDS_Shape plainAtGussets(const TopoDS_Shape& folded,
                            const Model& model,
                            TopTools_DataMapOfShapeShape& origins)
{
    BRep_Builder builder;
    TopoDS_Compound boxes;
    builder.MakeCompound(boxes);
    TopTools_ListOfShape plain;
    const double T = model.thickness;
    for (const Form& form : model.forms) {
        if (!acrossBend(model, form)) {
            continue;
        }
        const TopoDS_Shape box = formBox(form);
        builder.Add(boxes, box);
        const Element& element = model.elements[form.wall];
        const CylBend& b = element.bend;
        const gp_Vec a(b.frame.Direction()), x0(b.frame.XDirection()), y0(b.frame.YDirection());
        // the bend and its walls from before the box to past it along the
        // axis, and as far along each wall as the box reaches
        const double from =
            gp_Vec(b.frame.Location(), form.frame.Location()).Dot(a) - form.halfLength - 1;
        const double length = 2 * form.halfLength + 2;
        const double run = 2 * (form.halfWidth + form.zMax - form.zMin) + 1;
        CylBend at = b;
        at.frame.Translate(a * from);
        const gp_Pnt o = at.frame.Location();
        const double A = b.angle, R = b.radius;
        const gp_Dir nB(x0 * std::cos(A) + y0 * std::sin(A));
        const TopoDS_Shape sections[] = {
            bendSection(at, element.name).getShape(),
            wallSection(o.Translated(x0 * R), gp_Dir(-y0), run, gp_Dir(x0), T, element.name + "A")
                .getShape(),
            wallSection(o.Translated(gp_Vec(nB) * R),
                        gp_Dir(x0 * -std::sin(A) + y0 * std::cos(A)),
                        run,
                        nB,
                        T,
                        element.name + "B")
                .getShape(),
        };
        for (const TopoDS_Shape& section : sections) {
            BRepAlgoAPI_Common common(BRepPrimAPI_MakePrism(section, a * length).Shape(), box);
            common.Build();
            if (!common.IsDone()) {
                THROWM(Base::CADKernelError, "A gusset's bend could not be laid flat plain");
            }
            plain.Append(common.Shape());
        }
    }
    BRepAlgoAPI_Cut cut(folded, boxes);
    cut.Build();
    if (!cut.IsDone()) {
        THROWM(Base::CADKernelError, "A gusset's bend could not be laid flat plain");
    }
    TopTools_ListOfShape arguments;
    arguments.Append(cut.Shape());
    BRepAlgoAPI_Fuse fuse;
    fuse.SetArguments(arguments);
    fuse.SetTools(plain);
    fuse.Build();
    if (!fuse.IsDone()) {
        THROWM(Base::CADKernelError, "A gusset's bend could not be laid flat plain");
    }
    // The skins the boxes parted made one again, and nothing else: every
    // edge the part had, or a piece of it, kept, so faces it parted (a
    // plate's end, its bend's and its leg's, in one plane) stay apart
    Handle(BRepTools_History) history = cut.History();
    history->Merge(fuse.History());
    ShapeUpgrade_UnifySameDomain unify(fuse.Shape(), true, true, false);
    for (TopExp_Explorer it(folded, TopAbs_EDGE); it.More(); it.Next()) {
        if (history->IsRemoved(it.Current())) {
            continue;
        }
        const TopTools_ListOfShape& pieces = history->Modified(it.Current());
        if (pieces.IsEmpty()) {
            unify.KeepShape(it.Current());
        }
        for (const TopoDS_Shape& piece : pieces) {
            unify.KeepShape(piece);
        }
    }
    unify.Build();
    history->Merge(unify.History());
    for (TopExp_Explorer it(folded, TopAbs_FACE); it.More(); it.Next()) {
        for (const TopoDS_Shape& made : history->Modified(it.Current())) {
            if (!origins.IsBound(made)) {
                origins.Bind(made, it.Current());
            }
        }
    }
    TopExp_Explorer solid(unify.Shape(), TopAbs_SOLID);
    if (!solid.More()) {
        THROWM(Base::CADKernelError, "A gusset's bend could not be laid flat plain");
    }
    return solid.Current();
}

/// A gusset's footprint on the skins it rises from, folded: each edge
/// between a face of it and a face of the bend's or a wall's that faces into
/// the corner, the way it rises, with that face
std::vector<std::pair<TopoDS_Edge, TopoDS_Face>>
footprint(const TopoDS_Shape& folded, const Model& model, int form)
{
    const gp_Vec rise(model.forms[form].frame.Direction());
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(folded, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    TopTools_DataMapOfShapeInteger formOf;
    std::vector<std::pair<TopoDS_Edge, TopoDS_Face>> res;
    for (int i = 1; i <= edgeFaces.Extent(); ++i) {
        bool ofForm = false;
        TopoDS_Face skin;
        for (const TopoDS_Shape& f : edgeFaces(i)) {
            if (!formOf.IsBound(f)) {
                formOf.Bind(f, model.formOf(TopoDS::Face(f)));
            }
            if (formOf(f) == form) {
                ofForm = true;
            }
            else {
                skin = TopoDS::Face(f);
            }
        }
        if (!ofForm || skin.IsNull()) {
            continue;
        }
        const TopoDS_Edge& edge = TopoDS::Edge(edgeFaces.FindKey(i));
        BRepAdaptor_Curve c(edge);
        const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        GeomAPI_ProjectPointOnSurf onSkin(mid, BRep_Tool::Surface(skin));
        if (onSkin.NbPoints() == 0) {
            continue;
        }
        double u = 0, v = 0;
        onSkin.LowerDistanceParameters(u, v);
        gp_Pnt at;
        gp_Vec normal;
        BRepGProp_Face(skin).Normal(u, v, at, normal);
        if (normal.Magnitude() < Precision::Confusion()
            || normal.Normalized().Dot(rise) < 0.3) {
            continue;
        }
        res.emplace_back(edge, skin);
    }
    return res;
}

/// A plane face's bounds in the frame of its longest straight edge
struct PlaneBounds
{
    /// The edge along X, the plane's normal Z
    gp_Trsf toFrame;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

bool planeBounds(const TopoDS_Face& face, PlaneBounds& res)
{
    if (face.IsNull() || BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane) {
        return false;
    }
    gp_Pnt origin;
    gp_Vec x;
    double longest = 0;
    for (TopExp_Explorer it(face, TopAbs_EDGE); it.More(); it.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(it.Current()));
        if (c.GetType() != GeomAbs_Line) {
            continue;
        }
        const gp_Pnt p0 = c.Value(c.FirstParameter());
        const gp_Pnt p1 = c.Value(c.LastParameter());
        if (p0.Distance(p1) > longest) {
            longest = p0.Distance(p1);
            origin = p0;
            x = gp_Vec(p0, p1);
        }
    }
    if (longest <= Precision::Confusion()) {
        return false;
    }
    const gp_Dir z = BRepAdaptor_Surface(face).Plane().Axis().Direction();
    res.toFrame.SetTransformation(gp_Ax3(origin, z, gp_Dir(x)));
    res.x0 = res.y0 = std::numeric_limits<double>::max();
    res.x1 = res.y1 = -res.x0;
    for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
        const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(it.Current())).Transformed(res.toFrame);
        res.x0 = std::min(res.x0, p.X());
        res.x1 = std::max(res.x1, p.X());
        res.y0 = std::min(res.y0, p.Y());
        res.y1 = std::max(res.y1, p.Y());
    }
    return true;
}

}  // namespace

std::vector<TopoDS_Edge> diagonals(const TopoDS_Face& face)
{
    std::vector<TopoDS_Edge> res;
    PlaneBounds bounds;
    if (!planeBounds(face, bounds)) {
        return res;
    }
    const double x0 = bounds.x0, y0 = bounds.y0, x1 = bounds.x1, y1 = bounds.y1;
    const gp_Trsf back = bounds.toFrame.Inverted();
    for (const auto& [a, b] : {std::pair(gp_Pnt(x0, y0, 0), gp_Pnt(x1, y1, 0)),
                               std::pair(gp_Pnt(x0, y1, 0), gp_Pnt(x1, y0, 0))}) {
        const gp_Pnt pa = a.Transformed(back);
        const gp_Pnt pb = b.Transformed(back);
        const TopoDS_Edge line = BRepBuilderAPI_MakeEdge(pa, pb);
        BRepAlgoAPI_Common common(line, face);
        common.SetFuzzyValue(1e-7);
        common.Build();
        if (!common.IsDone()) {
            continue;
        }
        // from where it first meets the face to where it last leaves it
        const gp_Vec dir = gp_Vec(pa, pb).Normalized();
        double s0 = std::numeric_limits<double>::max(), s1 = -s0;
        for (TopExp_Explorer it(common.Shape(), TopAbs_VERTEX); it.More(); it.Next()) {
            const double s = gp_Vec(pa, BRep_Tool::Pnt(TopoDS::Vertex(it.Current()))).Dot(dir);
            s0 = std::min(s0, s);
            s1 = std::max(s1, s);
        }
        if (s1 - s0 > Precision::Confusion()) {
            res.push_back(
                BRepBuilderAPI_MakeEdge(pa.Translated(dir * s0), pa.Translated(dir * s1)));
        }
    }
    return res;
}

std::vector<CrossBreakPatch> crossBreakPatches(const TopoDS_Shape& shape, const Model& model)
{
    std::vector<CrossBreakPatch> res;
    TopTools_IndexedMapOfShape faces;
    for (std::size_t i = 0; i < model.elements.size(); ++i) {
        const int index = static_cast<int>(i);
        const TopoDS_Face from = crossBreakFace(shape, model, index);
        PlaneBounds bounds;
        if (from.IsNull() || !planeBounds(from, bounds)) {
            continue;
        }
        if (faces.IsEmpty()) {
            TopExp::MapShapes(shape, TopAbs_FACE, faces);
        }
        const Element& wall = model.elements[i];
        const gp_Dir rise = wall.crossBreak > 0 ? wall.frame.Direction()
                                                : wall.frame.Direction().Reversed();
        const gp_Trsf back = bounds.toFrame.Inverted();
        const gp_Pnt middle = gp_Pnt((bounds.x0 + bounds.x1) / 2, (bounds.y0 + bounds.y1) / 2, 0)
                                  .Transformed(back);
        const gp_Dir along = gp_Dir(1, 0, 0).Transformed(back);
        for (int k = 1; k <= faces.Extent(); ++k) {
            const TopoDS_Face& face = TopoDS::Face(faces(k));
            BRepAdaptor_Surface surface(face);
            if (surface.GetType() != GeomAbs_Plane || model.owner(face) != index) {
                continue;
            }
            const gp_Pln plane = surface.Plane();
            gp_Dir normal = plane.Axis().Direction();
            if (face.Orientation() == TopAbs_REVERSED) {
                normal.Reverse();
            }
            const double dot = normal.Dot(rise);
            if (std::abs(dot) < 1 - 1e-7) {
                continue;
            }
            CrossBreakPatch patch;
            patch.face = k - 1;
            patch.wall = index;
            // the middle carried onto this face's plane
            const gp_Vec off(plane.Location(), middle);
            patch.center = middle.Translated(-gp_Vec(rise) * off.Dot(gp_Vec(rise)));
            patch.direction = along;
            patch.normal = rise;
            patch.halfLength = (bounds.x1 - bounds.x0) / 2;
            patch.halfWidth = (bounds.y1 - bounds.y0) / 2;
            patch.angle = std::abs(wall.crossBreak);
            patch.rises = dot > 0;
            res.push_back(patch);
        }
    }
    return res;
}

std::vector<TopoDS_Shape>
notchTools(const FlatResult& flat, NotchKind kind, double depth, double width)
{
    std::vector<TopoDS_Shape> res;
    if (kind == NotchKind::None || flat.solid.IsNull() || flat.bendLines.empty()) {
        return res;
    }
    if (depth <= Precision::Confusion() || width <= Precision::Confusion()) {
        THROWM(Base::ValueError, "A notch needs a depth and a width above 0");
    }
    constexpr double tol = 1e-6;
    Bnd_Box box;
    BRepBndLib::AddOptimal(flat.solid, box, /*useTriangulation*/ false,
                           /*useShapeTolerance*/ false);
    if (box.IsVoid()) {
        return res;
    }
    const double bottom = box.CornerMin().Z();
    const double top = box.CornerMax().Z();

    // the blank's top, its faces merged, where the lines lie (flatten()),
    // and the edges of its outer outline
    ShapeUpgrade_UnifySameDomain unify(flat.solid, true, true, false);
    unify.Build();
    BRep_Builder builder;
    TopoDS_Compound faces;
    builder.MakeCompound(faces);
    std::vector<TopoDS_Edge> outline;
    for (TopExp_Explorer it(unify.Shape(), TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face& face = TopoDS::Face(it.Current());
        if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane) {
            continue;
        }
        bool onTop = true;
        for (TopExp_Explorer v(face, TopAbs_VERTEX); v.More() && onTop; v.Next()) {
            onTop = std::abs(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).Z() - top) < tol;
        }
        if (!onTop) {
            continue;
        }
        builder.Add(faces, face);
        for (TopExp_Explorer e(BRepTools::OuterWire(face), TopAbs_EDGE); e.More(); e.Next()) {
            outline.push_back(TopoDS::Edge(e.Current()));
        }
    }
    // A piece of the outline through p, straight, and its direction
    auto straightAt = [&](const gp_Pnt& p, gp_Vec& dir) {
        const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(p);
        for (const TopoDS_Edge& e : outline) {
            BRepExtrema_DistShapeShape dist(v, e);
            if (!dist.IsDone() || dist.Value() > tol) {
                continue;
            }
            BRepAdaptor_Curve c(e);
            const gp_Pnt p0 = c.Value(c.FirstParameter());
            const gp_Pnt p1 = c.Value(c.LastParameter());
            if (p0.Distance(p1) <= tol) {
                continue;
            }
            // a curve that is a line in fact counts, one that bows does not
            const gp_Lin chord(p0, gp_Dir(gp_Vec(p0, p1)));
            const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
            if (c.GetType() != GeomAbs_Line && chord.Distance(mid) > tol) {
                continue;
            }
            dir = gp_Vec(p0, p1).Normalized();
            return true;
        }
        return false;
    };
    // How far out from p along -u the blank is not, up to far
    auto clearance = [&](const gp_Pnt& p, const gp_Vec& u, double far) {
        const TopoDS_Edge ray = BRepBuilderAPI_MakeEdge(p, p.Translated(-u * far));
        BRepAlgoAPI_Common common(ray, faces);
        common.SetFuzzyValue(1e-7);
        common.Build();
        double res = far;
        if (!common.IsDone()) {
            return res;
        }
        for (TopExp_Explorer it(common.Shape(), TopAbs_VERTEX); it.More(); it.Next()) {
            const double s = gp_Vec(p, BRep_Tool::Pnt(TopoDS::Vertex(it.Current()))).Dot(-u);
            if (s > tol) {
                res = std::min(res, s);
            }
        }
        return res;
    };
    const double square = std::sin(10 * std::numbers::pi / 180);
    const double reach = std::max(depth, width) / 2;
    for (const auto& line : flat.bendLines) {
        for (const TopoDS_Edge& segment : line.segments) {
            const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(segment));
            const gp_Pnt b = BRep_Tool::Pnt(TopExp::LastVertex(segment));
            // too short for a notch at each end
            if (a.Distance(b) <= 2 * depth) {
                continue;
            }
            for (const auto& [end, other] : {std::pair(a, b), std::pair(b, a)}) {
                const gp_Vec u = gp_Vec(end, other).Normalized();
                gp_Vec dir;
                if (!straightAt(end, dir) || std::abs(dir.Dot(u)) > square) {
                    continue;
                }
                const gp_Vec n(-u.Y(), u.X(), 0);
                // past the outline, so the cut opens it cleanly, but half way
                // at most to the blank across a gap
                const double margin = std::min(reach, clearance(end, u, 4 * reach) / 2);
                const gp_Pnt base(end.X(), end.Y(), bottom - 1);
                std::vector<gp_Pnt> pts;
                if (kind == NotchKind::V) {
                    // the sides through the outline width apart
                    const double half = width / 2 * (depth + margin) / depth;
                    pts = {base.Translated(u * depth),
                           base.Translated(-u * margin + n * half),
                           base.Translated(-u * margin - n * half)};
                }
                else {
                    pts = {base.Translated(-u * margin + n * (width / 2)),
                           base.Translated(u * depth + n * (width / 2)),
                           base.Translated(u * depth - n * (width / 2)),
                           base.Translated(-u * margin - n * (width / 2))};
                }
                BRepBuilderAPI_MakePolygon polygon;
                for (const gp_Pnt& p : pts) {
                    polygon.Add(p);
                }
                polygon.Close();
                const TopoDS_Face face = BRepBuilderAPI_MakeFace(polygon.Wire(), true);
                res.push_back(
                    BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, top - bottom + 2)).Shape());
            }
        }
    }
    return res;
}

FlatResult flatten(const TopoDS_Shape& folded,
                   const Model& model,
                   int root,
                   bool secondSkin,
                   const TopoDS_Edge& alongX,
                   FormDisplay formDisplay)
{
    if (root < 0 || root >= static_cast<int>(model.elements.size())
        || model.elements[root].kind == Element::Kind::Bend) {
        THROWM(Base::ValueError, "The flat's root is not a wall or a cone");
    }
    const int closing = model.closingBend();
    if (closing >= 0) {
        FC_THROWM(Base::ValueError,
                  "The sheet is closed: " << model.elements[closing].name
                                          << " closes a loop of bends, which does not lie "
                                             "flat; Rip it open");
    }
    Mapper mapper {model, root, rootToXY(model, root, secondSkin)};
    if (!alongX.IsNull()) {
        mapper.toXY = alongXOf(folded, mapper, alongX) * mapper.toXY;
    }

    // Gussets, the forms across a bend: no wall carries one flat. Where each
    // is, the part is put back as its bend and the bend's walls were, plain,
    // and its footprint on the skins it rises from is its outline; a form on
    // a wall is closed over instead, below. A face the putting back made is
    // named after a folded face it was made of.
    std::vector<std::vector<std::pair<TopoDS_Edge, TopoDS_Face>>> footprints(model.forms.size());
    bool gussets = false;
    for (std::size_t k = 0; k < model.forms.size(); ++k) {
        if (acrossBend(model, model.forms[k])) {
            footprints[k] = footprint(folded, model, static_cast<int>(k));
            gussets = true;
        }
    }
    TopTools_DataMapOfShapeShape origins;
    const TopoDS_Shape shape = gussets ? plainAtGussets(folded, model, origins) : folded;
    auto originOf = [&](const TopoDS_Shape& face) {
        return origins.IsBound(face) ? origins(face) : face;
    };

    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    BRepBuilderAPI_Sewing sewing(1e-6);
    // each folded face and the face it lies flat as, before sewing
    std::vector<std::pair<TopoDS_Shape, TopoDS_Shape>> laid;
    std::vector<std::pair<double, double>> extent(model.elements.size(),
                                                  {std::numeric_limits<double>::max(),
                                                   -std::numeric_limits<double>::max()});
    // each face's element; and the edges of the bends' skins mapped first,
    // exactly (Mapper::onCylinder()), for the faces across the sheet that
    // share them
    std::vector<int> owners(faces.Extent() + 1, -1);
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face& face = TopoDS::Face(faces(i));
        owners[i] = model.owner(face);
        if (owners[i] < 0 || model.elements[owners[i]].kind == Element::Kind::Wall) {
            continue;
        }
        const GeomAbs_SurfaceType type = BRepAdaptor_Surface(face).GetType();
        if (model.elements[owners[i]].kind == Element::Kind::Bend
                ? type != GeomAbs_Cylinder
                : type != GeomAbs_Cone && type != GeomAbs_Cylinder) {
            continue;
        }
        for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
            mapper.edge(owners[i], TopoDS::Edge(e.Current()), face);
        }
    }
    // Forms not kept: their faces left out, and each hole they leave in a
    // skin -- an inner wire all of whose edges are a form's -- closed. Its
    // edges on the skin the form rises from are its outline.
    const bool unformed = formDisplay != FormDisplay::Formed && !model.forms.empty();
    std::vector<int> formOf(faces.Extent() + 1, -1);
    TopTools_IndexedMapOfShape formEdges;
    std::vector<std::vector<TopoDS_Edge>> outlines(model.forms.size());
    if (unformed) {
        for (int i = 1; i <= faces.Extent(); ++i) {
            formOf[i] = model.formOf(TopoDS::Face(faces(i)));
            if (formOf[i] >= 0) {
                TopExp::MapShapes(faces(i), TopAbs_EDGE, formEdges);
            }
        }
    }
    // a skin face with its forms' holes closed, or the face as it is
    auto closed = [&](const TopoDS_Face& face) -> TopoDS_Face {
        if (!unformed) {
            return face;
        }
        const TopoDS_Wire outer = BRepTools::OuterWire(face);
        TopoDS_Face res = TopoDS::Face(face.EmptyCopied());
        BRep_Builder builder;
        bool changed = false;
        for (TopoDS_Iterator it(face); it.More(); it.Next()) {
            if (it.Value().ShapeType() != TopAbs_WIRE) {
                continue;
            }
            const TopoDS_Wire& wire = TopoDS::Wire(it.Value());
            int form = -1;
            bool ofForm = !wire.IsSame(outer);
            for (TopExp_Explorer e(wire, TopAbs_EDGE); e.More() && ofForm; e.Next()) {
                ofForm = formEdges.Contains(e.Current());
                if (ofForm && form < 0) {
                    BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
                    const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
                    for (std::size_t k = 0; k < model.forms.size() && form < 0; ++k) {
                        if (model.forms[k].contains(mid, 1e-6)) {
                            form = static_cast<int>(k);
                        }
                    }
                }
            }
            if (!ofForm) {
                builder.Add(res, wire);
                continue;
            }
            changed = true;
            // on the skin it rises from: the face's outer normal its way
            BRepAdaptor_Surface surface(face);
            if (form >= 0 && surface.GetType() == GeomAbs_Plane) {
                gp_Dir normal = surface.Plane().Axis().Direction();
                if (face.Orientation() == TopAbs_REVERSED) {
                    normal.Reverse();
                }
                if (normal.Dot(model.forms[form].frame.Direction()) > 0.5) {
                    for (TopExp_Explorer e(wire, TopAbs_EDGE); e.More(); e.Next()) {
                        outlines[form].push_back(TopoDS::Edge(e.Current()));
                    }
                }
            }
        }
        return changed ? res : face;
    };
    // the folded faces laid out approximately (FlatResult::approximated)
    std::vector<TopoDS_Shape> approximated;
    // a face, or a part of one across a bend's line, laid out by its element
    auto lay = [&](const TopoDS_Face& face, int owner, int i, bool part) {
        if (owner < 0) {
            FC_THROWM(Base::ValueError, "Face" << i << " is in no wall or bend of the sheet");
        }
        const TopoDS_Shape origin = originOf(faces(i));
        const Element& element = model.elements[owner];
        if (element.kind == Element::Kind::Wall) {
            gp_Trsf move = mapper.toXY * model.placement(owner, root);
            laid.emplace_back(origin, BRepBuilderAPI_Transform(closed(face), move, true).Shape());
            sewing.Add(laid.back().second);
            return;
        }
        // As it unrolls: a plane, or square across the sheet. Failing that,
        // what is cut otherwise than square to it (risk 2): a hole's wall
        // through a bend, closed round on itself, in bands; else a surface
        // filled to its outline, past its seam if it has one in it.
        std::vector<TopoDS_Face> bands;
        bool filled = false;
        TopoDS_Face flat;
        auto other = [&]() {
            if (element.kind == Element::Kind::Bend) {
                bands = mapper.around(owner, face);
            }
            if (bands.empty()) {
                flat = mapper.face(owner, face, i, filled, true, true);
            }
        };
        try {
            flat = mapper.face(owner, face, i, filled, false, part);
        }
        catch (const Base::Exception&) {
            other();
        }
        catch (const Standard_Failure&) {
            other();
        }
        if (bands.empty()) {
            laid.emplace_back(origin, flat);
            sewing.Add(flat);
        }
        for (const TopoDS_Face& band : bands) {
            laid.emplace_back(origin, band);
            sewing.Add(band);
        }
        // a miter's slant, filled to its outline as before, is not listed
        if (!bands.empty() || (filled && part)) {
            approximated.push_back(origin);
        }
        if (element.kind != Element::Kind::Bend) {
            return;
        }
        // how far along its axis the bend goes, for its line
        const gp_Ax3& f = element.bend.frame;
        for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
            const double z = gp_Vec(f.Location(), BRep_Tool::Pnt(TopoDS::Vertex(it.Current())))
                                 .Dot(gp_Vec(f.Direction()));
            extent[owner].first = std::min(extent[owner].first, z);
            extent[owner].second = std::max(extent[owner].second, z);
        }
    };
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face& face = TopoDS::Face(faces(i));
        if (owners[i] < 0) {
            FC_THROWM(Base::ValueError, "Face" << i << " is in no wall or bend of the sheet");
        }
        if (formOf[i] >= 0) {
            continue;
        }
        // across a bend's line: each part of it by the element it lies in
        const std::vector<TopoDS_Face> parts = partsOf(face, model, owners[i]);
        if (parts.size() == 1) {
            lay(face, owners[i], i, false);
            continue;
        }
        for (const TopoDS_Face& part : parts) {
            lay(part, model.owner(part), i, true);
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
    res.toXY = mapper.toXY;
    res.solid = fix.Solid();
    for (const TopoDS_Shape& face : approximated) {
        if (std::none_of(res.approximated.begin(), res.approximated.end(),
                         [&](const TopoDS_Shape& have) { return have.IsSame(face); })) {
            res.approximated.push_back(face);
        }
    }
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
        line.segments = onBlank(res.solid, line.edge);
        for (std::size_t k = 0; k < line.tangents.size(); ++k) {
            line.tangentSegments[k] = onBlank(res.solid, line.tangents[k]);
        }
        res.bendLines.push_back(line);
    }
    // each wall's cross break: the diagonals of the face it rises from, laid
    // flat; up where that face is the blank's top
    for (std::size_t i = 0; i < model.elements.size(); ++i) {
        const TopoDS_Face from = crossBreakFace(folded, model, static_cast<int>(i));
        if (from.IsNull()) {
            continue;
        }
        for (const auto& [face, laidFace] : res.faces) {
            if (!face.IsSame(from) || laidFace.ShapeType() != TopAbs_FACE) {
                continue;
            }
            const std::vector<TopoDS_Edge> lines = diagonals(TopoDS::Face(laidFace));
            if (lines.size() != 2) {
                break;
            }
            FlatResult::CrossBreak cross;
            cross.name = model.elements[i].name;
            cross.diagonals = {lines[0], lines[1]};
            cross.up = BRep_Tool::Pnt(TopExp::FirstVertex(lines[0])).Z() > model.thickness / 2;
            cross.angle = std::abs(model.elements[i].crossBreak);
            res.crossBreaks.push_back(cross);
            break;
        }
    }
    // each form's outline or cross, moved with its wall; a gusset's laid
    // flat with the bend, Formed too
    if (unformed || gussets) {
        for (std::size_t k = 0; k < model.forms.size(); ++k) {
            const Form& form = model.forms[k];
            FlatResult::FormMark mark;
            mark.name = form.name;
            mark.centre = formDisplay == FormDisplay::CentreMark;
            if (acrossBend(model, form)) {
                if (mark.centre) {
                    // a cross on the bend's inner skin at its middle
                    const CylBend& b = model.elements[form.wall].bend;
                    const gp_Vec a(b.frame.Direction());
                    const gp_Pnt o = b.frame.Location().Translated(
                        a * gp_Vec(b.frame.Location(), form.frame.Location()).Dot(a));
                    const double arm = std::max(0.5, std::min(form.halfLength, form.halfWidth) / 4);
                    auto skin = [&](double along, double angle) {
                        const gp_Vec r = gp_Vec(b.frame.XDirection()) * std::cos(angle)
                            + gp_Vec(b.frame.YDirection()) * std::sin(angle);
                        return mapper.map(form.wall, o.Translated(r * b.radius + a * along));
                    };
                    const double mid = b.angle / 2, turn = arm / b.radius;
                    mark.edges.push_back(BRepBuilderAPI_MakeEdge(skin(-arm, mid), skin(arm, mid)));
                    mark.edges.push_back(
                        BRepBuilderAPI_MakeEdge(skin(0, mid - turn), skin(0, mid + turn)));
                }
                else {
                    for (const auto& [edge, face] : footprints[k]) {
                        const int owner = model.owner(face);
                        if (owner < 0) {
                            continue;
                        }
                        if (model.elements[owner].kind == Element::Kind::Bend) {
                            mark.edges.push_back(mapper.edge(owner, edge, face));
                        }
                        else {
                            const gp_Trsf move = mapper.toXY * model.placement(owner, root);
                            mark.edges.push_back(
                                TopoDS::Edge(BRepBuilderAPI_Transform(edge, move, true).Shape()));
                        }
                    }
                }
                if (!mark.edges.empty()) {
                    res.forms.push_back(mark);
                }
                continue;
            }
            if (!unformed) {
                continue;
            }
            const gp_Trsf move = mapper.toXY * model.placement(form.wall, root);
            std::vector<TopoDS_Edge> edges;
            if (mark.centre) {
                const double across = form.halfWidth > 0
                    ? std::min(form.halfLength, form.halfWidth)
                    : form.halfLength;
                const double arm = std::max(0.5, across / 4);
                const gp_Pnt c = form.frame.Location();
                for (const gp_Dir& d : {form.frame.XDirection(), form.frame.YDirection()}) {
                    edges.push_back(BRepBuilderAPI_MakeEdge(c.Translated(gp_Vec(d) * -arm),
                                                            c.Translated(gp_Vec(d) * arm)));
                }
            }
            else {
                edges = outlines[k];
            }
            for (const TopoDS_Edge& e : edges) {
                mark.edges.push_back(
                    TopoDS::Edge(BRepBuilderAPI_Transform(e, move, true).Shape()));
            }
            if (!mark.edges.empty()) {
                res.forms.push_back(mark);
            }
        }
    }
    // Every marking on the blank's top, the side a drawing of the flat looks
    // at: a down bend's line, on its inner skin, is on the underside, where a
    // drawing's hidden-line removal drops it (TechDraw keeps no hidden line by
    // default). Which way a bend goes is the line's up, not where it lies.
    Bnd_Box box;
    BRepBndLib::AddOptimal(res.solid, box, /*useTriangulation*/ false,
                           /*useShapeTolerance*/ false);
    if (!box.IsVoid()) {
        const double top = box.CornerMax().Z();
        auto lift = [top](TopoDS_Edge& edge) {
            if (edge.IsNull()) {
                return;
            }
            const double z = BRep_Tool::Pnt(TopExp::FirstVertex(edge)).Z();
            if (top - z > Precision::Confusion()) {
                gp_Trsf up;
                up.SetTranslation(gp_Vec(0, 0, top - z));
                edge = TopoDS::Edge(BRepBuilderAPI_Transform(edge, up, true).Shape());
            }
        };
        for (auto& cross : res.crossBreaks) {
            for (auto& edge : cross.diagonals) {
                lift(edge);
            }
        }
        for (auto& form : res.forms) {
            for (auto& edge : form.edges) {
                lift(edge);
            }
        }
        for (auto& line : res.bendLines) {
            lift(line.edge);
            for (auto& edge : line.segments) {
                lift(edge);
            }
            for (std::size_t k = 0; k < line.tangents.size(); ++k) {
                lift(line.tangents[k]);
                for (auto& edge : line.tangentSegments[k]) {
                    lift(edge);
                }
            }
        }
    }
    return res;
}

}  // namespace PartDesign::SheetMetal
