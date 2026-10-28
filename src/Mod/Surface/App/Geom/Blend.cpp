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
#include <BRepClass_FaceClassifier.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <GeomConvert.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <gp_Pnt2d.hxx>
#endif

#include "Blend.h"
#include "NetworkSurface.h"

namespace Surface::Geom
{

namespace
{

/// The parameter of `c` at `s` of its length
double atLength(const Handle(Geom_Curve) & c, double s)
{
    GeomAdaptor_Curve a(c);
    double total = GCPnts_AbscissaPoint::Length(a);
    if (s <= 0 || total <= Precision::Confusion()) {
        return c->FirstParameter();
    }
    if (s >= 1) {
        return c->LastParameter();
    }
    GCPnts_AbscissaPoint at(a, s * total, c->FirstParameter());
    if (at.IsDone()) {
        return at.Parameter();
    }
    return c->FirstParameter() + s * (c->LastParameter() - c->FirstParameter());
}

/// Whether `p`, near `face`, is inside it
bool inside(const TopoDS_Face& face, const gp_Pnt& p)
{
    Handle(Geom_Surface) s = BRep_Tool::Surface(face);
    GeomAPI_ProjectPointOnSurf pr(p, s);
    if (pr.NbPoints() == 0) {
        return false;
    }
    double u, v;
    pr.LowerDistanceParameters(u, v);
    BRepClass_FaceClassifier c(face, gp_Pnt2d(u, v), Precision::Confusion());
    return c.State() == TopAbs_IN;
}

/// Across `side`'s curve at t in its support's tangent plane: the surface
/// normal crossed with the curve's tangent, unit, or null where either is
/// not defined
gp_Vec acrossAt(const BlendSide& side, double t)
{
    gp_Pnt p;
    gp_Vec tangent;
    side.curve->D1(t, p, tangent);
    Handle(Geom_Surface) s = BRep_Tool::Surface(side.condition.support);
    GeomAPI_ProjectPointOnSurf pr(p, s);
    if (pr.NbPoints() == 0 || tangent.Magnitude() < 1e-12) {
        return {};
    }
    double u, v;
    pr.LowerDistanceParameters(u, v);
    gp_Pnt q;
    gp_Vec du, dv;
    s->D1(u, v, q, du, dv);
    gp_Vec across = du.Crossed(dv).Crossed(tangent);
    return across.Magnitude() < 1e-12 ? gp_Vec() : across.Normalized();
}

/// Which way across is away from the face: +1 or -1, decided once at the
/// middle of the curve -- at its ends a step aside lands on the face's
/// next edge, which a classifier calls on the face, not in it
double awaySign(const BlendSide& side, double gap)
{
    const auto& c = side.curve;
    double t = (c->FirstParameter() + c->LastParameter()) / 2;
    gp_Vec across = acrossAt(side, t);
    if (across.Magnitude() == 0) {
        return 1;
    }
    return inside(side.condition.support, c->Value(t).Translated(across * (1e-3 * gap))) ? -1 : 1;
}

/// The direction the blend leaves `side`'s curve at t by: across it in the
/// support's tangent plane, away from the face (`sign`); with no support,
/// straight at `towards`
gp_Vec leaving(const BlendSide& side, double t, double sign, const gp_Pnt& towards)
{
    gp_Vec straight(side.curve->Value(t), towards);
    if (side.condition.support.IsNull() || side.condition.order == 0) {
        return straight.Magnitude() > 0 ? straight.Normalized() : gp_Vec(0, 0, 1);
    }
    gp_Vec across = acrossAt(side, t);
    if (across.Magnitude() == 0) {
        return straight.Normalized();
    }
    return across * sign;
}

}  // namespace

BlendResult blendSurface(const BlendSide& first,
                         const BlendSide& second,
                         const BlendOptions& options)
{
    BlendResult r;
    if (first.curve.IsNull() || second.curve.IsNull()) {
        r.error = "a blend needs two edges";
        return r;
    }
    for (const auto* c : {&first.curve, &second.curve}) {
        if (Precision::IsInfinite((*c)->FirstParameter())
            || Precision::IsInfinite((*c)->LastParameter())) {
            r.error = "an edge of the blend is not bounded";
            return r;
        }
    }
    try {
        BlendSide a = first, b = second;
        // the second edge run with the first
        gp_Pnt a0 = a.curve->Value(a.curve->FirstParameter());
        gp_Pnt a1 = a.curve->Value(a.curve->LastParameter());
        gp_Pnt b0 = b.curve->Value(b.curve->FirstParameter());
        gp_Pnt b1 = b.curve->Value(b.curve->LastParameter());
        if (a0.Distance(b1) + a1.Distance(b0) < a0.Distance(b0) + a1.Distance(b1)) {
            b.curve = Handle(Geom_Curve)::DownCast(b.curve->Reversed());
            r.reversed = true;
        }

        gp_Pnt ma = a.curve->Value((a.curve->FirstParameter() + a.curve->LastParameter()) / 2);
        gp_Pnt mb = b.curve->Value((b.curve->FirstParameter() + b.curve->LastParameter()) / 2);
        double signA = a.condition.support.IsNull() ? 1 : awaySign(a, ma.Distance(mb));
        double signB = b.condition.support.IsNull() ? 1 : awaySign(b, ma.Distance(mb));

        const int n = std::max(3, options.samples);
        std::vector<Handle(Geom_Curve)> across;
        for (int k = 0; k < n; ++k) {
            double s = double(k) / (n - 1);
            double ta = atLength(a.curve, s), tb = atLength(b.curve, s);
            gp_Pnt pa = a.curve->Value(ta), pb = b.curve->Value(tb);
            double gap = pa.Distance(pb);
            if (gap < Precision::Confusion()) {
                r.error = "the two edges touch: there is no gap to blend";
                return r;
            }
            gp_Vec da = leaving(a, ta, signA, pb), db = leaving(b, tb, signB, pa);
            // a quintic leaving each end straight along its direction, the
            // derivative there 3 * weight * gap / 3, as a cubic's would be
            double ha = a.condition.weight * gap / 3, hb = b.condition.weight * gap / 3;
            TColgp_Array1OfPnt poles(1, 6);
            poles.SetValue(1, pa);
            poles.SetValue(2, pa.Translated(da * (0.6 * ha)));
            poles.SetValue(3, pa.Translated(da * (1.2 * ha)));
            poles.SetValue(4, pb.Translated(db * (1.2 * hb)));
            poles.SetValue(5, pb.Translated(db * (0.6 * hb)));
            poles.SetValue(6, pb);
            across.push_back(GeomConvert::CurveToBSplineCurve(new Geom_BezierCurve(poles)));
        }

        auto net = networkSurface({a.curve, b.curve}, across);
        if (!net.isDone()) {
            r.error = net.error;
            return r;
        }
        Handle(Geom_BSplineSurface) surface = net.surface;
        int at[2] = {boundaryOf(surface, a.curve, 1e-4), boundaryOf(surface, b.curve, 1e-4)};
        if (at[0] < 0 || at[1] < 0) {
            r.error = "the blend's edges are not its boundary";
            return r;
        }
        // the curves laid across are the blend's own: its ends are free
        Conditions conditions;
        for (int k = 0; k < 4; ++k) {
            conditions[k].free = k != at[0] && k != at[1];
        }
        conditions[at[0]] = a.condition;
        conditions[at[1]] = b.condition;
        auto reports = matchBoundaries(surface, conditions, options.match);
        r.reports = {reports[at[0]], reports[at[1]]};
        for (const auto& rep : r.reports) {
            if (!rep.error.empty()) {
                r.error = rep.error;
                return r;
            }
        }
        r.surface = surface;
    }
    catch (Standard_Failure& e) {
        r.error = std::string("the blend failed: ") + e.what();
    }
    return r;
}

}  // namespace Surface::Geom
