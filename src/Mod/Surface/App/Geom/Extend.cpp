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
#include <vector>

#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array2OfReal.hxx>
#include <TColgp_Array2OfPnt.hxx>
#endif

#include "Extend.h"

namespace Surface::Geom
{

namespace
{

using BSurface = Handle(Geom_BSplineSurface);

/// The parameters along the boundary at which the length is measured
constexpr int Samples = 11;

/// A surface's arrays, in U-major order: row i is the poles at U index i
struct Net
{
    std::vector<std::vector<gp_XYZ>> hpoles;  // weighted
    std::vector<std::vector<double>> weights;
    std::vector<double> uknots, vknots;
    std::vector<int> umults, vmults;
    int udeg = 0, vdeg = 0;
    bool vperiodic = false;

    explicit Net(const BSurface& s)
        : udeg(s->UDegree())
        , vdeg(s->VDegree())
        , vperiodic(s->IsVPeriodic())
    {
        const int nu = s->NbUPoles(), nv = s->NbVPoles();
        hpoles.assign(nu, std::vector<gp_XYZ>(nv));
        weights.assign(nu, std::vector<double>(nv, 1.0));
        for (int i = 0; i < nu; ++i) {
            for (int j = 0; j < nv; ++j) {
                double w = s->Weight(i + 1, j + 1);
                weights[i][j] = w;
                hpoles[i][j] = s->Pole(i + 1, j + 1).XYZ() * w;
            }
        }
        for (int i = 1; i <= s->NbUKnots(); ++i) {
            uknots.push_back(s->UKnot(i));
            umults.push_back(s->UMultiplicity(i));
        }
        for (int i = 1; i <= s->NbVKnots(); ++i) {
            vknots.push_back(s->VKnot(i));
            vmults.push_back(s->VMultiplicity(i));
        }
    }

    BSurface make() const
    {
        const int nu = (int)hpoles.size(), nv = (int)hpoles.front().size();
        TColgp_Array2OfPnt poles(1, nu, 1, nv);
        TColStd_Array2OfReal w(1, nu, 1, nv);
        bool rational = false;
        for (int i = 0; i < nu; ++i) {
            for (int j = 0; j < nv; ++j) {
                poles.SetValue(i + 1, j + 1, gp_Pnt(hpoles[i][j] / weights[i][j]));
                w.SetValue(i + 1, j + 1, weights[i][j]);
                rational = rational || std::abs(weights[i][j] - 1.0) > 1e-15;
            }
        }
        auto array = [](const auto& v, auto& a) {
            for (std::size_t i = 0; i < v.size(); ++i) {
                a.SetValue((int)i + 1, v[i]);
            }
        };
        TColStd_Array1OfReal uk(1, (int)uknots.size()), vk(1, (int)vknots.size());
        TColStd_Array1OfInteger um(1, (int)umults.size()), vm(1, (int)vmults.size());
        array(uknots, uk), array(vknots, vk), array(umults, um), array(vmults, vm);
        // never periodic across: extendBoundary refuses that; along, as it was
        if (rational) {
            return new Geom_BSplineSurface(poles, w, uk, vk, um, vm, udeg, vdeg, false, vperiodic);
        }
        return new Geom_BSplineSurface(poles, uk, vk, um, vm, udeg, vdeg, false, vperiodic);
    }
};

/// The blossom of the Bezier `h` (degree h.size() - 1) at p0 parameters s0
/// and the rest s1: de Casteljau's steps, each at its own parameter
gp_XYZ blossom(std::vector<gp_XYZ> h, double s0, double s1, int n1)
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
double blossom(std::vector<double> h, double s0, double s1, int n1)
{
    std::vector<gp_XYZ> x;
    for (double v : h) {
        x.emplace_back(v, 0, 0);
    }
    return blossom(x, s0, s1, n1).X();
}

/// The knot next to the extended side, raised to the degree, so that the
/// span at that side is a Bezier of its own rows. Returns the
/// index of that knot (in `s`'s 1-based numbering) and its multiplicity
/// before, or 0 when the side's span is the only one
std::pair<int, int> isolateSpan(BSurface& s, bool atEnd)
{
    const int n = s->NbUKnots();
    if (n <= 2) {
        return {0, 0};
    }
    int index = atEnd ? n - 1 : 2;
    int before = s->UMultiplicity(index);
    if (before < s->UDegree()) {
        s->IncreaseUMultiplicity(index, s->UDegree());
    }
    return {index, before};
}

/// The mean distance from the boundary at `from` to the iso at `to`
double meanChord(const BSurface& s, double from, double to)
{
    double v1, v2, u1, u2, sum = 0;
    s->Bounds(u1, u2, v1, v2);
    for (int k = 0; k < Samples; ++k) {
        double v = v1 + (v2 - v1) * k / (Samples - 1);
        sum += s->Value(from, v).Distance(s->Value(to, v));
    }
    return sum / Samples;
}

/// The natural extension in U by `d` in parameter, of `s` with its side
/// span isolated
BSurface natural(const BSurface& s, bool atEnd, double d)
{
    Net net(s);
    const int p = net.udeg, nu = (int)net.hpoles.size(), nv = (int)net.hpoles.front().size();
    const double a = atEnd ? net.uknots[net.uknots.size() - 2] : net.uknots.front();
    const double b = atEnd ? net.uknots.back() : net.uknots[1];
    // the side span's polynomial on [a, b + d] (or [a - d, b]), as Bezier
    // parameters: s0 and s1 in the span's own [0, 1]
    const double s0 = atEnd ? 0 : -d / (b - a), s1 = atEnd ? 1 + d / (b - a) : 1;
    const int first = atEnd ? nu - 1 - p : 0;
    for (int j = 0; j < nv; ++j) {
        std::vector<gp_XYZ> h;
        std::vector<double> w;
        for (int i = 0; i <= p; ++i) {
            h.push_back(net.hpoles[first + i][j]);
            w.push_back(net.weights[first + i][j]);
        }
        for (int i = 0; i <= p; ++i) {
            net.hpoles[first + i][j] = blossom(h, s0, s1, i);
            net.weights[first + i][j] = blossom(w, s0, s1, i);
        }
    }
    (atEnd ? net.uknots.back() : net.uknots.front()) = atEnd ? b + d : a - d;
    return net.make();
}

/// The linear extension in U of `s`, with its side span isolated: a span of
/// parameter length `e` added, ruled along the derivative across
std::string linear(BSurface& s, bool atEnd, double length)
{
    Net net(s);
    const int p = net.udeg, nu = (int)net.hpoles.size(), nv = (int)net.hpoles.front().size();
    const double a = atEnd ? net.uknots[net.uknots.size() - 2] : net.uknots.front();
    const double b = atEnd ? net.uknots.back() : net.uknots[1];
    const int edge = atEnd ? nu - 1 : 0, next = atEnd ? nu - 2 : 1;
    // the derivative across, a row of poles: the side span is a Bezier, and
    // its end derivative is the difference of its last two rows -- of their
    // points, which the weights must not vary between
    std::vector<gp_XYZ> derivative(nv);
    for (int j = 0; j < nv; ++j) {
        double w0 = net.weights[edge][j], w1 = net.weights[next][j];
        if (std::abs(w0 - w1) > 1e-12 * std::max(1.0, std::abs(w0))) {
            return "the surface's weights vary across the boundary: a straight extension "
                   "would not be of the same form; extend it naturally";
        }
        // outwards at either side: the last row less the one inside it
        derivative[j] = (net.hpoles[edge][j] - net.hpoles[next][j]) / w0 * (p / (b - a));
    }
    // the parameter length that makes the mean chord `length`: the chord at
    // a parameter along the boundary is e times the speed across there
    double u1, u2, v1, v2, speed = 0;
    s->Bounds(u1, u2, v1, v2);
    for (int k = 0; k < Samples; ++k) {
        double v = v1 + (v2 - v1) * k / (Samples - 1);
        gp_Pnt pt;
        gp_Vec du, dv;
        s->D1(atEnd ? u2 : u1, v, pt, du, dv);
        speed += du.Magnitude();
    }
    speed /= Samples;
    if (speed < 1e-12) {
        return "the surface does not move across the boundary";
    }
    const double e = length / speed;

    // the rows of the new span, after (or before) the boundary's own
    std::vector<std::vector<gp_XYZ>> rows;
    std::vector<std::vector<double>> wrows;
    for (int i = 1; i <= p; ++i) {
        std::vector<gp_XYZ> row(nv);
        for (int j = 0; j < nv; ++j) {
            gp_XYZ base = net.hpoles[edge][j] / net.weights[edge][j];
            row[j] = (base + derivative[j] * (e * i / p)) * net.weights[edge][j];
        }
        rows.push_back(row);
        wrows.push_back(net.weights[edge]);
    }
    if (atEnd) {
        net.hpoles.insert(net.hpoles.end(), rows.begin(), rows.end());
        net.weights.insert(net.weights.end(), wrows.begin(), wrows.end());
        net.umults.back() = p;
        net.uknots.push_back(b + e);
        net.umults.push_back(p + 1);
    }
    else {
        net.hpoles.insert(net.hpoles.begin(), rows.rbegin(), rows.rend());
        net.weights.insert(net.weights.begin(), wrows.rbegin(), wrows.rend());
        net.umults.front() = p;
        net.uknots.insert(net.uknots.begin(), a - e);
        net.umults.insert(net.umults.begin(), p + 1);
    }
    s = net.make();
    // the old boundary is a knot of the new surface, where it is C1 by
    // construction: say so in its knots
    if (p > 1) {
        int index = atEnd ? s->NbUKnots() - 1 : 2;
        s->RemoveUKnot(index, p - 1, 1e-9);
    }
    return {};
}

}  // namespace

std::string extendBoundary(Handle(Geom_BSplineSurface)& surface,
                           Boundary boundary,
                           double length,
                           Extension mode)
{
    if (surface.IsNull()) {
        return "no surface";
    }
    if (!(length > 0)) {
        return "the length of an extension has to be more than 0";
    }
    const bool inU = boundary == Boundary::UMin || boundary == Boundary::UMax;
    const bool atEnd = boundary == Boundary::UMax || boundary == Boundary::VMax;
    if (inU ? surface->IsUPeriodic() : surface->IsVPeriodic()) {
        return "the surface is periodic across that boundary: it has no edge there to extend";
    }

    try {
        // work across U: V's boundaries by exchanging the two, which keeps
        // every parameter as it was
        BSurface s = BSurface::DownCast(surface->Copy());
        if (!inU) {
            s->ExchangeUV();
        }
        auto [index, before] = isolateSpan(s, atEnd);
        double u1, u2, v1, v2;
        s->Bounds(u1, u2, v1, v2);
        const double from = atEnd ? u2 : u1;

        if (mode == Extension::Natural) {
            // the parameter length whose mean chord is `length`: a few
            // scalings from the speed across, the chord being near linear
            double speed = meanChord(s, from, from + (atEnd ? 1e-6 : -1e-6)) / 1e-6;
            if (speed < 1e-12) {
                return "the surface does not move across the boundary";
            }
            double d = length / speed;
            BSurface out;
            for (int k = 0; k < 40; ++k) {
                out = natural(s, atEnd, d);
                double chord = meanChord(out, from, atEnd ? from + d : from - d);
                if (std::abs(chord - length) < 1e-12 * std::max(1.0, length)) {
                    break;
                }
                d *= length / chord;
            }
            s = out;
            if (index > 0) {
                // the knot raised to isolate the span goes back down: the
                // polynomials on both sides of it are what they were
                s->RemoveUKnot(index, before, 1e-9);
            }
        }
        else {
            auto error = linear(s, atEnd, length);
            if (!error.empty()) {
                return error;
            }
            if (index > 0) {
                s->RemoveUKnot(atEnd ? index : index + 1, before, 1e-9);
            }
        }
        if (!inU) {
            s->ExchangeUV();
        }
        surface = s;
    }
    catch (Standard_Failure& e) {
        return std::string("the extension failed: ") + e.what();
    }
    return {};
}

}  // namespace Surface::Geom
