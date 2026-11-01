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
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Standard_Failure.hxx>
#endif

#include "Continuity.h"

namespace Surface::Geom
{

namespace
{

/// A face at a point near it: the point on it, its outward normal (the
/// face's orientation taken in), and its normal curvature along `dir`
/// against that normal
struct Local
{
    bool ok = false;
    gp_Pnt point;
    gp_Dir normal;
    gp_Vec du, dv;
    Handle(Geom_Surface) surface;
    double u = 0, v = 0;
    bool reversed = false;
};

Local localAt(const TopoDS_Face& face, const gp_Pnt& p)
{
    Local l;
    TopLoc_Location loc;
    Handle(Geom_Surface) s = BRep_Tool::Surface(face, loc);
    if (s.IsNull()) {
        return l;
    }
    if (!loc.IsIdentity()) {
        s = Handle(Geom_Surface)::DownCast(s->Transformed(loc.Transformation()));
    }
    GeomAPI_ProjectPointOnSurf pr(p, s);
    if (pr.NbPoints() == 0) {
        return l;
    }
    pr.LowerDistanceParameters(l.u, l.v);
    s->D1(l.u, l.v, l.point, l.du, l.dv);
    gp_Vec n = l.du.Crossed(l.dv);
    if (n.Magnitude() < 1e-12) {
        return l;
    }
    l.reversed = face.Orientation() == TopAbs_REVERSED;
    l.normal = gp_Dir(l.reversed ? -n : n);
    l.surface = s;
    l.ok = true;
    return l;
}

/// The normal curvature of `l`'s surface along `dir` (in its tangent
/// plane), signed against `l.normal`
double curvatureAlong(const Local& l, const gp_Dir& dir)
{
    GeomLProp_SLProps props(l.surface, l.u, l.v, 2, 1e-9);
    if (!props.IsCurvatureDefined()) {
        return 0;
    }
    double k;
    if (props.IsUmbilic()) {
        k = props.MaxCurvature();
    }
    else {
        gp_Dir dmax, dmin;
        props.CurvatureDirections(dmax, dmin);
        double c = dir.Dot(dmax), s = dir.Dot(dmin);
        double w = c * c + s * s;
        k = w > 1e-12 ? (props.MaxCurvature() * c * c + props.MinCurvature() * s * s) / w : 0;
    }
    // the surface's own normal is du x dv; against the face's
    return l.reversed ? -k : k;
}

}  // namespace

Continuity edgeContinuity(const TopoDS_Edge& edge,
                          const TopoDS_Face& face1,
                          const TopoDS_Face& face2,
                          int samples)
{
    Continuity r;
    if (edge.IsNull() || face1.IsNull() || face2.IsNull()) {
        r.error = "an edge and two faces are needed";
        return r;
    }
    try {
        BRepAdaptor_Curve c(edge);
        const int n = std::max(2, samples);
        bool any = false;
        for (int k = 0; k <= n; ++k) {
            // a little inside the ends, where both faces have a tangent plane
            double f = 0.002 + 0.996 * k / n;
            double t = c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * f;
            gp_Pnt p;
            gp_Vec tangent;
            c.D1(t, p, tangent);
            Local a = localAt(face1, p), b = localAt(face2, p);
            if (!a.ok || !b.ok || tangent.Magnitude() < 1e-12) {
                continue;
            }
            if (!any) {
                // a measure that stays 0 is worst where it is first taken,
                // on the edge, not at the origin
                r.worstGap = r.worstAngle = r.worstCurvature = p;
            }
            any = true;
            double gap = a.point.Distance(b.point);
            if (gap > r.maxGap) {
                r.maxGap = gap;
                r.worstGap = p;
            }
            double angle = a.normal.Angle(b.normal);
            bool flipped = angle > M_PI / 2;
            angle = std::min(angle, M_PI - angle);
            if (angle > r.maxAngle) {
                r.maxAngle = angle;
                r.worstAngle = p;
            }
            // across the edge, in the first face's tangent plane
            gp_Dir across(gp_Vec(a.normal).Crossed(tangent));
            double ka = curvatureAlong(a, across);
            double kb = curvatureAlong(b, across);
            double gapk = std::abs(ka - (flipped ? -kb : kb));
            if (gapk > r.maxCurvatureGap) {
                r.maxCurvatureGap = gapk;
                r.worstCurvature = p;
            }
        }
        if (!any) {
            r.error = "the edge is not on both faces";
        }
    }
    catch (Standard_Failure& e) {
        r.error = std::string("the continuity could not be measured: ") + e.what();
    }
    return r;
}

}  // namespace Surface::Geom
