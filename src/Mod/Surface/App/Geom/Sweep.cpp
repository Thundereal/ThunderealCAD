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
#include <algorithm>
#include <cmath>

#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <gp_Ax3.hxx>
#include <gp_Trsf.hxx>
#endif

#include "NetworkSurface.h"
#include "Sweep.h"

namespace Surface::Geom
{

namespace
{

using Curve = Handle(Geom_Curve);

/// A curve measured by its length: the parameter at a share of it and back
struct Measured
{
    Curve curve;
    double length = 0;

    explicit Measured(const Curve& c)
        : curve(c)
    {
        GeomAdaptor_Curve a(c);
        length = GCPnts_AbscissaPoint::Length(a);
    }

    double at(double f) const
    {
        if (f <= 0) {
            return curve->FirstParameter();
        }
        if (f >= 1) {
            return curve->LastParameter();
        }
        GeomAdaptor_Curve a(curve);
        GCPnts_AbscissaPoint p(a, f * length, curve->FirstParameter());
        if (p.IsDone()) {
            return p.Parameter();
        }
        return curve->FirstParameter() + f * (curve->LastParameter() - curve->FirstParameter());
    }

    double share(double t) const
    {
        GeomAdaptor_Curve a(curve);
        return length > 0 ? GCPnts_AbscissaPoint::Length(a, curve->FirstParameter(), t) / length
                          : 0;
    }

    gp_Pnt point(double f) const
    {
        return curve->Value(at(f));
    }

    gp_Vec tangent(double f) const
    {
        gp_Pnt p;
        gp_Vec d;
        curve->D1(at(f), p, d);
        return d.Magnitude() > 0 ? d.Normalized() : d;
    }
};

Curve reversed(const Curve& c)
{
    return Curve::DownCast(c->Reversed());
}

/// The frame a section has at a place: its chord, and the way the rails run
/// made square to it
bool frame(const gp_Pnt& p1, const gp_Pnt& p2, const gp_Vec& run, gp_Ax3& ax, double& scale)
{
    gp_Vec x(p1, p2);
    scale = x.Magnitude();
    if (scale < Precision::Confusion()) {
        return false;
    }
    x /= scale;
    gp_Vec z = run - x * run.Dot(x);
    if (z.Magnitude() < 1e-9) {
        return false;
    }
    ax = gp_Ax3(p1, gp_Dir(z), gp_Dir(x));
    return true;
}

struct Section
{
    Curve curve;
    double share = 0;  // on the first rail
    double share2 = 0;  // on the second
    gp_Ax3 ax;
    double scale = 1;
};

/// `s` put where its frame is `ax` at `scale`
Curve place(const Section& s, const gp_Ax3& ax, double scale)
{
    gp_Trsf rigid;
    rigid.SetDisplacement(s.ax, ax);
    gp_Trsf size;
    size.SetScale(ax.Location(), scale / s.scale);
    return Curve::DownCast(s.curve->Transformed(size * rigid));
}

/// Points of `c` evenly along its length
std::vector<gp_Pnt> samples(const Curve& c, int n)
{
    Measured m(c);
    std::vector<gp_Pnt> pts;
    for (int k = 0; k < n; ++k) {
        pts.push_back(m.point(double(k) / (n - 1)));
    }
    return pts;
}

}  // namespace

SweepResult twoRailSweep(const Handle(Geom_Curve) & rail1,
                         const Handle(Geom_Curve) & rail2,
                         const std::vector<Handle(Geom_Curve)>& sections,
                         const SweepOptions& options)
{
    SweepResult r;
    if (rail1.IsNull() || rail2.IsNull()) {
        r.error = "a two-rail sweep needs two rails";
        return r;
    }
    if (sections.empty()) {
        r.error = "a two-rail sweep needs a section";
        return r;
    }
    try {
        Curve r1 = rail1, r2 = rail2;
        // the second rail run with the first
        gp_Pnt a0 = r1->Value(r1->FirstParameter()), a1 = r1->Value(r1->LastParameter());
        gp_Pnt b0 = r2->Value(r2->FirstParameter()), b1 = r2->Value(r2->LastParameter());
        if (a0.Distance(b1) + a1.Distance(b0) < a0.Distance(b0) + a1.Distance(b1)) {
            r2 = reversed(r2);
        }
        Measured m1(r1), m2(r2);

        // each section, from the first rail to the second, where it is on them
        std::vector<Section> secs;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            Curve c = sections[i];
            gp_Pnt s0 = c->Value(c->FirstParameter()), s1 = c->Value(c->LastParameter());
            GeomAPI_ProjectPointOnCurve p0(s0, r1), p1(s1, r1);
            if (p0.NbPoints() == 0 || p1.NbPoints() == 0) {
                r.error = "section " + std::to_string(i + 1) + " is not on the first rail";
                return r;
            }
            if (p1.LowerDistance() < p0.LowerDistance()) {
                c = reversed(c);
                std::swap(s0, s1);
            }
            GeomAPI_ProjectPointOnCurve on1(s0, r1), on2(s1, r2);
            if (on1.NbPoints() == 0 || on1.LowerDistance() > options.tolerance) {
                r.error = "section " + std::to_string(i + 1) + " does not start on a rail";
                return r;
            }
            if (on2.NbPoints() == 0 || on2.LowerDistance() > options.tolerance) {
                r.error = "section " + std::to_string(i + 1) + " does not end on the other rail";
                return r;
            }
            Section s;
            s.curve = c;
            s.share = m1.share(on1.LowerDistanceParameter());
            s.share2 = m2.share(on2.LowerDistanceParameter());
            gp_Vec run = m1.tangent(s.share) + m2.tangent(s.share2);
            if (!frame(s0, s1, run, s.ax, s.scale)) {
                r.error = "section " + std::to_string(i + 1)
                    + " runs along the rails, or its ends are one point";
                return r;
            }
            secs.push_back(s);
        }
        std::sort(secs.begin(), secs.end(), [](const Section& a, const Section& b) {
            return a.share < b.share;
        });

        // the second rail's share at a share of the first: through the
        // sections' own, the ends to the ends
        auto second = [&](double f) {
            double fa = 0, ga = 0;
            for (const auto& s : secs) {
                if (f <= s.share) {
                    double fb = s.share, gb = s.share2;
                    return fb - fa > 1e-12 ? ga + (gb - ga) * (f - fa) / (fb - fa) : gb;
                }
                fa = s.share, ga = s.share2;
            }
            return 1 - fa > 1e-12 ? ga + (1 - ga) * (f - fa) / (1 - fa) : 1.0;
        };

        // the places: evenly along the rails, and the sections' own
        const int n = std::max(2, options.stations);
        std::vector<double> places;
        for (int k = 0; k < n; ++k) {
            places.push_back(double(k) / (n - 1));
        }
        for (const auto& s : secs) {
            places.push_back(s.share);
        }
        std::sort(places.begin(), places.end());
        places.erase(std::unique(places.begin(),
                                 places.end(),
                                 [](double a, double b) {
                                     return std::abs(a - b) < 1e-6;
                                 }),
                     places.end());

        std::vector<Curve> profiles;
        for (double f : places) {
            double g = second(f);
            gp_Pnt p1 = m1.point(f), p2 = m2.point(g);
            gp_Ax3 ax;
            double scale;
            if (!frame(p1, p2, m1.tangent(f) + m2.tangent(g), ax, scale)) {
                r.error = "the rails meet, or run across each other, where a section would be";
                return r;
            }
            // the sections either side of the place, put there and blended
            std::size_t after = 0;
            while (after < secs.size() && secs[after].share < f - 1e-9) {
                ++after;
            }
            const Section& next = secs[std::min(after, secs.size() - 1)];
            const Section& prev = secs[after == 0 ? 0 : after - 1];
            Curve a = place(prev, ax, scale);
            if (&prev == &next || std::abs(next.share - prev.share) < 1e-12) {
                profiles.push_back(a);
                continue;
            }
            Curve b = place(next, ax, scale);
            double w = (f - prev.share) / (next.share - prev.share);
            const int m = 31;
            auto pa = samples(a, m), pb = samples(b, m);
            Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, m);
            for (int k = 0; k < m; ++k) {
                pts->SetValue(k + 1, gp_Pnt(pa[k].XYZ() * (1 - w) + pb[k].XYZ() * w));
            }
            GeomAPI_Interpolate interp(pts, false, Precision::Confusion());
            interp.Perform();
            if (!interp.IsDone()) {
                r.error = "the sections could not be blended between their places";
                return r;
            }
            profiles.push_back(interp.Curve());
        }

        NetworkOptions net;
        net.tolerance = options.tolerance;
        net.fitTolerance = options.fitTolerance;
        auto result = networkSurface(profiles, {r1, r2}, net);
        if (!result.isDone()) {
            r.error = result.error;
            return r;
        }
        r.surface = result.surface;
    }
    catch (Standard_Failure& e) {
        r.error = std::string("the sweep failed: ") + e.what();
    }
    return r;
}

}  // namespace Surface::Geom
