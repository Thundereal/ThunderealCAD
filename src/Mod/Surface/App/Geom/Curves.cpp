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
#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <algorithm>

#include <BRepTopAdaptor_FClass2d.hxx>
#include <ElCLib.hxx>
#include <Geom2dAPI_InterCurveCurve.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <Geom2d_Line.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#endif

#include "Curves.h"

namespace Surface::Geom
{

namespace
{

using BCurve = Handle(Geom_BSplineCurve);

/// A curve's arrays: weighted poles, weights, knots and multiplicities
struct Poles
{
    std::vector<gp_XYZ> h;
    std::vector<double> w, knots;
    std::vector<int> mults;
    int degree = 0;

    explicit Poles(const BCurve& c)
        : degree(c->Degree())
    {
        for (int i = 1; i <= c->NbPoles(); ++i) {
            double wi = c->Weight(i);
            w.push_back(wi);
            h.push_back(c->Pole(i).XYZ() * wi);
        }
        for (int i = 1; i <= c->NbKnots(); ++i) {
            knots.push_back(c->Knot(i));
            mults.push_back(c->Multiplicity(i));
        }
    }

    BCurve make() const
    {
        const int n = (int)h.size();
        TColgp_Array1OfPnt poles(1, n);
        TColStd_Array1OfReal weights(1, n);
        bool rational = false;
        for (int i = 0; i < n; ++i) {
            poles.SetValue(i + 1, gp_Pnt(h[i] / w[i]));
            weights.SetValue(i + 1, w[i]);
            rational = rational || std::abs(w[i] - 1.0) > 1e-15;
        }
        TColStd_Array1OfReal k(1, (int)knots.size());
        TColStd_Array1OfInteger m(1, (int)mults.size());
        for (std::size_t i = 0; i < knots.size(); ++i) {
            k.SetValue((int)i + 1, knots[i]);
            m.SetValue((int)i + 1, mults[i]);
        }
        if (rational) {
            return new Geom_BSplineCurve(poles, weights, k, m, degree);
        }
        return new Geom_BSplineCurve(poles, k, m, degree);
    }
};

/// The blossom of the Bezier `h` with n1 of its p parameters at s1 and the
/// rest at s0
template<typename T>
T blossom(std::vector<T> h, double s0, double s1, int n1)
{
    const int p = (int)h.size() - 1;
    for (int k = 1; k <= p; ++k) {
        double t = k <= p - n1 ? s0 : s1;
        for (int i = 0; i + k <= p; ++i) {
            h[i] = h[i] * (1 - t) + h[i + 1] * t;
        }
    }
    return h[0];
}

/// The span at the end isolated: the knot next to it raised to the degree
std::pair<int, int> isolate(BCurve& c, bool atEnd)
{
    const int n = c->NbKnots();
    if (n <= 2) {
        return {0, 0};
    }
    int index = atEnd ? n - 1 : 2;
    int before = c->Multiplicity(index);
    if (before < c->Degree()) {
        c->IncreaseMultiplicity(index, c->Degree());
    }
    return {index, before};
}

/// The end span carried on by d in parameter, exactly
BCurve natural(const BCurve& c, bool atEnd, double d)
{
    Poles net(c);
    const int p = net.degree, n = (int)net.h.size();
    const double a = atEnd ? net.knots[net.knots.size() - 2] : net.knots.front();
    const double b = atEnd ? net.knots.back() : net.knots[1];
    const double s0 = atEnd ? 0 : -d / (b - a), s1 = atEnd ? 1 + d / (b - a) : 1;
    const int first = atEnd ? n - 1 - p : 0;
    std::vector<gp_XYZ> h(net.h.begin() + first, net.h.begin() + first + p + 1);
    std::vector<double> w(net.w.begin() + first, net.w.begin() + first + p + 1);
    for (int i = 0; i <= p; ++i) {
        net.h[first + i] = blossom(h, s0, s1, i);
        net.w[first + i] = blossom(w, s0, s1, i);
    }
    (atEnd ? net.knots.back() : net.knots.front()) = atEnd ? b + d : a - d;
    return net.make();
}

double param(const BCurve& c, bool atEnd)
{
    return atEnd ? c->LastParameter() : c->FirstParameter();
}

}  // namespace

std::string extendCurve(Handle(Geom_BSplineCurve)& curve, bool atEnd, double length, Extension mode)
{
    if (curve.IsNull()) {
        return "no curve";
    }
    if (!(length > 0)) {
        return "the length of an extension has to be more than 0";
    }
    if (curve->IsPeriodic()) {
        return "the curve is periodic: it has no end to extend";
    }
    try {
        BCurve c = BCurve::DownCast(curve->Copy());
        auto [index, before] = isolate(c, atEnd);
        const double end = param(c, atEnd);
        const gp_Pnt from = c->Value(end);
        gp_Pnt p0;
        gp_Vec d1;
        c->D1(end, p0, d1);
        if (d1.Magnitude() < 1e-12) {
            return "the curve does not move at that end";
        }
        if (mode == Extension::Natural) {
            // the parameter length whose chord is `length`: scaled from the
            // speed there until it is
            double d = length / d1.Magnitude();
            BCurve out;
            for (int k = 0; k < 40; ++k) {
                out = natural(c, atEnd, d);
                double chord = from.Distance(out->Value(param(out, atEnd)));
                if (std::abs(chord - length) < 1e-12 * std::max(1.0, length)) {
                    break;
                }
                d *= length / chord;
            }
            c = out;
            if (index > 0) {
                c->RemoveKnot(index, before, 1e-9);
            }
        }
        else {
            Poles net(c);
            const int p = net.degree, n = (int)net.h.size();
            const int edge = atEnd ? n - 1 : 0, next = atEnd ? n - 2 : 1;
            if (std::abs(net.w[edge] - net.w[next]) > 1e-12 * std::max(1.0, net.w[edge])) {
                return "the curve's weights vary at that end: a straight extension would "
                       "not be of the same form; extend it naturally";
            }
            // straight on, at the speed it ends with, so it is C1
            const double e = length / d1.Magnitude();
            gp_XYZ base = net.h[edge] / net.w[edge];
            gp_XYZ dir = d1.XYZ() * (atEnd ? 1.0 : -1.0);
            std::vector<gp_XYZ> rows;
            for (int i = 1; i <= p; ++i) {
                rows.push_back((base + dir * (e * i / p)) * net.w[edge]);
            }
            std::vector<double> ws(p, net.w[edge]);
            if (atEnd) {
                net.h.insert(net.h.end(), rows.begin(), rows.end());
                net.w.insert(net.w.end(), ws.begin(), ws.end());
                net.mults.back() = p;
                net.knots.push_back(net.knots.back() + e);
                net.mults.push_back(p + 1);
            }
            else {
                net.h.insert(net.h.begin(), rows.rbegin(), rows.rend());
                net.w.insert(net.w.begin(), ws.begin(), ws.end());
                net.mults.front() = p;
                net.knots.insert(net.knots.begin(), net.knots.front() - e);
                net.mults.insert(net.mults.begin(), p + 1);
            }
            c = net.make();
            if (p > 1) {
                c->RemoveKnot(atEnd ? c->NbKnots() - 1 : 2, p - 1, 1e-9);
            }
            if (index > 0) {
                c->RemoveKnot(atEnd ? index : index + 1, before, 1e-9);
            }
        }
        curve = c;
    }
    catch (Standard_Failure& e) {
        return std::string("the extension failed: ") + e.what();
    }
    return {};
}

std::vector<Handle(Geom_BSplineCurve)> joinCurves(const std::vector<TopoDS_Edge>& edges,
                                                  double tolerance,
                                                  double cornerAngle,
                                                  double gap,
                                                  std::string& error)
{
    std::vector<BCurve> out;
    if (edges.empty()) {
        error = "no curve to join";
        return out;
    }
    try {
        Handle(TopTools_HSequenceOfShape) in = new TopTools_HSequenceOfShape;
        for (const auto& e : edges) {
            in->Append(e);
        }
        Handle(TopTools_HSequenceOfShape) wires;
        ShapeAnalysis_FreeBounds::ConnectEdgesToWires(in, gap, false, wires);
        for (int w = 1; w <= wires->Length(); ++w) {
            const TopoDS_Wire& wire = TopoDS::Wire(wires->Value(w));
            // the wire's edges in order, as runs between its corners
            std::vector<std::vector<TopoDS_Edge>> runs(1);
            gp_Vec lastTangent;
            bool first = true;
            for (BRepTools_WireExplorer x(wire); x.More(); x.Next()) {
                const TopoDS_Edge& e = x.Current();
                BRepAdaptor_Curve c(e);
                bool reversed = e.Orientation() == TopAbs_REVERSED;
                double t0 = reversed ? c.LastParameter() : c.FirstParameter();
                double t1 = reversed ? c.FirstParameter() : c.LastParameter();
                gp_Pnt p;
                gp_Vec start, end;
                c.D1(t0, p, start);
                c.D1(t1, p, end);
                if (reversed) {
                    start.Reverse(), end.Reverse();
                }
                if (!first && start.Magnitude() > 1e-12 && lastTangent.Magnitude() > 1e-12
                    && start.Angle(lastTangent) > cornerAngle) {
                    runs.emplace_back();
                }
                runs.back().push_back(e);
                lastTangent = end;
                first = false;
            }
            const bool closed = BRep_Tool::IsClosed(wire) || wire.Closed();
            for (const auto& run : runs) {
                // points along the run by length, each edge its share
                std::vector<gp_Pnt> pts;
                for (const auto& e : run) {
                    BRepAdaptor_Curve c(e);
                    bool reversed = e.Orientation() == TopAbs_REVERSED;
                    const int n = 24;
                    double len = GCPnts_AbscissaPoint::Length(c);
                    for (int k = pts.empty() ? 0 : 1; k <= n; ++k) {
                        double s = len * k / n;
                        GCPnts_AbscissaPoint at(c,
                                                reversed ? -s : s,
                                                reversed ? c.LastParameter() : c.FirstParameter());
                        pts.push_back(c.Value(at.IsDone() ? at.Parameter() : c.FirstParameter()));
                    }
                }
                const bool periodic = closed && runs.size() == 1
                    && pts.front().Distance(pts.back()) <= gap;
                if (periodic) {
                    pts.pop_back();
                    Handle(TColgp_HArray1OfPnt) a = new TColgp_HArray1OfPnt(1, (int)pts.size());
                    for (std::size_t i = 0; i < pts.size(); ++i) {
                        a->SetValue((int)i + 1, pts[i]);
                    }
                    GeomAPI_Interpolate interp(a, true, Precision::Confusion());
                    interp.Perform();
                    if (interp.IsDone()) {
                        out.push_back(interp.Curve());
                    }
                    continue;
                }
                TColgp_Array1OfPnt a(1, (int)pts.size());
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    a.SetValue((int)i + 1, pts[i]);
                }
                GeomAPI_PointsToBSpline fit(a, 3, 8, GeomAbs_C2, tolerance);
                if (fit.IsDone()) {
                    out.push_back(fit.Curve());
                }
            }
        }
        if (out.empty()) {
            error = "the curves could not be joined";
        }
    }
    catch (Standard_Failure& e) {
        out.clear();
        error = std::string("joining the curves failed: ") + e.what();
    }
    return out;
}

std::vector<TopoDS_Edge> isocurves(const TopoDS_Face& face, int nu, int nv)
{
    std::vector<TopoDS_Edge> out;
    if (face.IsNull()) {
        return out;
    }
    double u1, u2, v1, v2;
    BRepTools::UVBounds(face, u1, u2, v1, v2);
    TopLoc_Location loc;
    Handle(Geom_Surface) surface = BRep_Tool::Surface(face, loc);
    const double tol = Precision::PConfusion();
    BRepTopAdaptor_FClass2d classifier(face, Precision::Confusion());
    for (int pass = 0; pass < 2; ++pass) {
        const int n = pass == 0 ? nu : nv;
        for (int i = 1; i <= n; ++i) {
            double f = double(i) / (n + 1);
            gp_Pnt2d start = pass == 0 ? gp_Pnt2d(u1 + (u2 - u1) * f, v1)
                                       : gp_Pnt2d(u1, v1 + (v2 - v1) * f);
            gp_Dir2d dir = pass == 0 ? gp_Dir2d(0, 1) : gp_Dir2d(1, 0);
            const double len = pass == 0 ? v2 - v1 : u2 - u1;
            Handle(Geom2d_Line) line = new Geom2d_Line(start, dir);
            Handle(Geom2d_TrimmedCurve) seg = new Geom2d_TrimmedCurve(line, 0, len);

            // where the face's boundary crosses it, in its own parameter
            std::vector<double> cuts = {0, len};
            for (TopExp_Explorer x(face, TopAbs_EDGE); x.More(); x.Next()) {
                double a = 0, b = 0;
                Handle(Geom2d_Curve) pc =
                    BRep_Tool::CurveOnSurface(TopoDS::Edge(x.Current()), face, a, b);
                if (pc.IsNull()) {
                    continue;
                }
                Geom2dAPI_InterCurveCurve inter(seg, new Geom2d_TrimmedCurve(pc, a, b), tol);
                for (int k = 1; k <= inter.NbPoints(); ++k) {
                    cuts.push_back(ElCLib::Parameter(line->Lin2d(), inter.Point(k)));
                }
            }
            std::sort(cuts.begin(), cuts.end());
            // each piece between two cuts is all in the face or all out of
            // it: kept when its middle is in
            for (std::size_t k = 0; k + 1 < cuts.size(); ++k) {
                double a = cuts[k], b = cuts[k + 1];
                if (b - a < 10 * tol) {
                    continue;
                }
                if (classifier.Perform(line->Value((a + b) / 2)) != TopAbs_IN) {
                    continue;
                }
                TopoDS_Edge piece = BRepBuilderAPI_MakeEdge(line, surface, a, b);
                BRepLib::BuildCurves3d(piece);
                piece.Location(loc);
                out.push_back(piece);
            }
        }
    }
    return out;
}

}  // namespace Surface::Geom
