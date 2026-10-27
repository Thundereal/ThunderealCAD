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
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <BSplCLib.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <math_Matrix.hxx>
#endif

#include <Eigen/Dense>

#include "EdgeCondition.h"

namespace Surface::Geom
{

namespace
{

constexpr int Samples = 8;  // per knot span along a boundary

const char* boundaryName(int b)
{
    static const char* names[] = {"UMin", "UMax", "VMin", "VMax"};
    return names[b];
}

bool alongU(int b)
{
    return b == int(Boundary::VMin) || b == int(Boundary::VMax);
}

/// The surface's parameters, flat knots and pole counts
struct Frame
{
    double u0, u1, v0, v1;
    int nu, nv, du, dv;
    std::vector<double> uFlat, vFlat;

    explicit Frame(const Handle(Geom_BSplineSurface) & s)
    {
        s->Bounds(u0, u1, v0, v1);
        nu = s->NbUPoles();
        nv = s->NbVPoles();
        du = s->UDegree();
        dv = s->VDegree();
        TColStd_Array1OfReal ku(1, nu + du + 1), kv(1, nv + dv + 1);
        s->UKnotSequence(ku);
        s->VKnotSequence(kv);
        uFlat.assign(ku.begin(), ku.end());
        vFlat.assign(kv.begin(), kv.end());
    }

    int along(int b) const
    {
        return alongU(b) ? nu : nv;
    }
    int across(int b) const
    {
        return alongU(b) ? nv : nu;
    }
    /// The pole a along boundary b, r rows in from it, as (i, j) from 0
    std::pair<int, int> pole(int b, int a, int r) const
    {
        switch (Boundary(b)) {
            case Boundary::VMin:
                return {a, r};
            case Boundary::VMax:
                return {a, nv - 1 - r};
            case Boundary::UMin:
                return {r, a};
            case Boundary::UMax:
                return {nu - 1 - r, a};
        }
        return {0, 0};
    }
    /// (u, v) at parameter t along boundary b
    std::pair<double, double> uv(int b, double t) const
    {
        switch (Boundary(b)) {
            case Boundary::VMin:
                return {t, v0};
            case Boundary::VMax:
                return {t, v1};
            case Boundary::UMin:
                return {u0, t};
            case Boundary::UMax:
                return {u1, t};
        }
        return {0, 0};
    }
    /// The distinct knots along boundary b
    std::vector<double> breaks(int b) const
    {
        const auto& f = alongU(b) ? uFlat : vFlat;
        std::vector<double> k(f.begin(), f.end());
        k.erase(std::unique(k.begin(), k.end()), k.end());
        return k;
    }
};

/// The basis of `degree` on `flat` at t, derivatives 0..deriv: the first
/// nonzero pole (from 0) and values[d][k] for poles first + k
int basis(const std::vector<double>& flat,
          int degree,
          double t,
          int deriv,
          std::vector<std::vector<double>>& values)
{
    NCollection_Array1<double> f(1, (int)flat.size());
    for (std::size_t i = 0; i < flat.size(); ++i) {
        f.SetValue((int)i + 1, flat[i]);
    }
    math_Matrix m(1, deriv + 1, 1, degree + 1);
    int first = 0;
    BSplCLib::EvalBsplineBasis(deriv, degree + 1, f, t, first, m);
    values.assign(deriv + 1, std::vector<double>(degree + 1));
    for (int d = 0; d <= deriv; ++d) {
        for (int k = 0; k <= degree; ++k) {
            values[d][k] = m(d + 1, k + 1);
        }
    }
    return first - 1;
}

/// The support at a boundary point: its normal and its normal curvature in
/// a direction
struct Support
{
    Handle(Geom_Surface) surface;
    double su0 = 0, su1 = 0, sv0 = 0, sv1 = 0;

    explicit Support(const TopoDS_Face& face)
    {
        surface = BRep_Tool::Surface(face);
        BRepTools::UVBounds(face, su0, su1, sv0, sv1);
    }

    /// The distance to it, and its properties there
    bool at(const gp_Pnt& p, double& dist, gp_Dir& normal, GeomLProp_SLProps& props) const
    {
        GeomAPI_ProjectPointOnSurf pr(p, surface, su0, su1, sv0, sv1);
        if (pr.NbPoints() == 0) {
            return false;
        }
        double u, v;
        pr.LowerDistanceParameters(u, v);
        dist = pr.LowerDistance();
        props.SetParameters(u, v);
        if (!props.IsNormalDefined()) {
            return false;
        }
        normal = props.Normal();
        return true;
    }
};

/// The normal curvature of a surface at props in direction d (on its
/// tangent plane), against its own normal
double normalCurvature(GeomLProp_SLProps& props, const gp_Dir& d)
{
    if (!props.IsCurvatureDefined()) {
        return 0;
    }
    if (props.IsUmbilic()) {
        return props.MaxCurvature();
    }
    gp_Dir dmax, dmin;
    props.CurvatureDirections(dmax, dmin);
    double c = d.Dot(dmax), s = d.Dot(dmin);
    return props.MaxCurvature() * c * c + props.MinCurvature() * s * s;
}

/// What one boundary has: measured along it, where the worst point is
struct Measured
{
    double angle = 0, curvature = 0;
    double worstT = 0;
};

Measured measure(const Handle(Geom_BSplineSurface) & s,
                 const Frame& fr,
                 int b,
                 const EdgeCondition& c,
                 const Support& sup,
                 const MatchOptions& opt)
{
    Measured m;
    double worst = -1;
    auto br = fr.breaks(b);
    GeomLProp_SLProps sp(s, 2, 1e-9);
    GeomLProp_SLProps pp(sup.surface, 2, 1e-9);
    for (std::size_t k = 0; k + 1 < br.size(); ++k) {
        for (int i = (k == 0 ? 0 : 1); i <= Samples; ++i) {
            double t = br[k] + (br[k + 1] - br[k]) * i / Samples;
            auto [u, v] = fr.uv(b, t);
            sp.SetParameters(u, v);
            if (!sp.IsNormalDefined()) {
                continue;
            }
            gp_Dir n1 = sp.Normal(), n2;
            double dist;
            if (!sup.at(sp.Value(), dist, n2, pp)) {
                continue;
            }
            double angle = std::acos(std::min(1.0, std::abs(n1.Dot(n2))));
            double gap = 0;
            if (c.order >= 2) {
                gp_Vec along = alongU(b) ? sp.D1U() : sp.D1V();
                if (along.Magnitude() > 1e-12) {
                    gp_Dir across = n1.Crossed(gp_Dir(along));
                    double k1 = normalCurvature(sp, across);
                    double k2 = normalCurvature(pp, across);
                    gap = std::abs(k1 - (n1.Dot(n2) < 0 ? -k2 : k2));
                }
            }
            m.angle = std::max(m.angle, angle);
            m.curvature = std::max(m.curvature, gap);
            double score = std::max(angle / opt.angularTolerance,
                                    c.order >= 2 ? gap / opt.curvatureTolerance : 0.0);
            if (score > worst) {
                worst = score;
                m.worstT = t;
            }
        }
    }
    return m;
}

bool within(const Measured& m, const EdgeCondition& c, const MatchOptions& opt)
{
    return m.angle <= opt.angularTolerance
        && (c.order < 2 || m.curvature <= opt.curvatureTolerance);
}

/// The cross derivatives each boundary is to have, taken from the surface
/// as it was: the first projected on the support's tangent plane times the
/// weight, the second with its normal part set from the support's curvature
struct Targets
{
    std::vector<double> t;
    std::vector<gp_Vec> d1, d2;
};

/// Solve every conditioned boundary's rows together on `s` (which has the
/// knots to use and the shape of the original)
void solve(Handle(Geom_BSplineSurface) & s,
           const Frame& fr,
           const Conditions& cond,
           const std::array<Targets, 4>& targets)
{
    // the poles to set: r rows in from each conditioned boundary, the
    // boundaries that meet it left where they are
    std::map<std::pair<int, int>, int> unknown;
    std::vector<std::pair<int, int>> poleOf;
    for (int b = 0; b < 4; ++b) {
        for (int r = 1; r <= cond[b].order; ++r) {
            for (int a = 1; a + 1 < fr.along(b); ++a) {
                auto ij = fr.pole(b, a, r);
                if (unknown.emplace(ij, (int)poleOf.size()).second) {
                    poleOf.push_back(ij);
                }
            }
        }
    }
    const int nx = (int)poleOf.size();
    if (nx == 0) {
        return;
    }

    std::vector<Eigen::RowVectorXd> rows;
    std::vector<Eigen::RowVector3d> rhs;
    for (int b = 0; b < 4; ++b) {
        if (cond[b].order < 1) {
            continue;
        }
        const bool au = alongU(b);
        const auto& alongFlat = au ? fr.uFlat : fr.vFlat;
        const auto& acrossFlat = au ? fr.vFlat : fr.uFlat;
        const int alongDeg = au ? fr.du : fr.dv, acrossDeg = au ? fr.dv : fr.du;
        const bool atMax = b == int(Boundary::VMax) || b == int(Boundary::UMax);
        std::vector<std::vector<double>> cb;
        int cfirst = basis(acrossFlat,
                           acrossDeg,
                           atMax ? acrossFlat.back() : acrossFlat.front(),
                           cond[b].order,
                           cb);
        const auto& tg = targets[b];
        for (int d = 1; d <= cond[b].order; ++d) {
            const auto& want = d == 1 ? tg.d1 : tg.d2;
            double scale = 0;
            for (const auto& w : want) {
                scale = std::max(scale, w.Magnitude());
            }
            scale = scale > 1e-12 ? 1 / scale : 1;
            for (std::size_t k = 0; k < tg.t.size(); ++k) {
                std::vector<std::vector<double>> ab;
                int afirst = basis(alongFlat, alongDeg, tg.t[k], 0, ab);
                Eigen::RowVectorXd row = Eigen::RowVectorXd::Zero(nx);
                Eigen::RowVector3d known(want[k].X(), want[k].Y(), want[k].Z());
                for (int ia = 0; ia <= alongDeg; ++ia) {
                    int a = afirst + ia;
                    for (int ic = 0; ic <= acrossDeg; ++ic) {
                        double w = ab[0][ia] * cb[d][ic];
                        if (w == 0) {
                            continue;
                        }
                        // the pole's row in from the boundary
                        int q = cfirst + ic;
                        int r = atMax ? fr.across(b) - 1 - q : q;
                        auto ij = fr.pole(b, a, r);
                        auto it = unknown.find(ij);
                        if (it != unknown.end()) {
                            row(it->second) += w;
                        }
                        else {
                            const gp_Pnt& p = s->Pole(ij.first + 1, ij.second + 1);
                            known -= w * Eigen::RowVector3d(p.X(), p.Y(), p.Z());
                        }
                    }
                }
                rows.push_back(row * scale);
                rhs.push_back(known * scale);
            }
        }
    }

    Eigen::MatrixXd A(rows.size(), nx);
    Eigen::MatrixXd B(rows.size(), 3);
    for (std::size_t k = 0; k < rows.size(); ++k) {
        A.row(k) = rows[k];
        B.row(k) = rhs[k];
    }
    // a light pull to where the poles are keeps a pole no sample reaches put
    Eigen::MatrixXd N = A.transpose() * A;
    Eigen::MatrixXd R = A.transpose() * B;
    double lambda = 1e-10 * std::max(1.0, N.diagonal().maxCoeff());
    for (int x = 0; x < nx; ++x) {
        const gp_Pnt& p = s->Pole(poleOf[x].first + 1, poleOf[x].second + 1);
        N(x, x) += lambda;
        R.row(x) += lambda * Eigen::RowVector3d(p.X(), p.Y(), p.Z());
    }
    Eigen::MatrixXd X = N.ldlt().solve(R);
    for (int x = 0; x < nx; ++x) {
        s->SetPole(poleOf[x].first + 1,
                   poleOf[x].second + 1,
                   gp_Pnt(X(x, 0), X(x, 1), X(x, 2)));
    }
}

}  // namespace

Reports matchBoundaries(Handle(Geom_BSplineSurface)& surface,
                        const Conditions& cond,
                        const MatchOptions& opt)
{
    Reports reports;
    bool any = false;
    for (int b = 0; b < 4; ++b) {
        any = any || cond[b].order > 0;
        if (cond[b].order > 2 || cond[b].order < 0) {
            reports[b].error = std::string(boundaryName(b)) + ": G" + std::to_string(cond[b].order)
                + " is not supported (G0 to G2)";
            return reports;
        }
        if (cond[b].order > 0 && cond[b].support.IsNull()) {
            reports[b].error = std::string(boundaryName(b)) + " has no support face";
            return reports;
        }
    }
    if (!any || surface.IsNull()) {
        return reports;
    }
    if (surface->IsURational() || surface->IsVRational() || surface->IsUPeriodic()
        || surface->IsVPeriodic()) {
        for (int b = 0; b < 4; ++b) {
            if (cond[b].order > 0) {
                reports[b].error = "a rational or periodic surface is not matched yet";
            }
        }
        return reports;
    }

    try {
        // degree 3 at least, and room across for every boundary's rows
        Handle(Geom_BSplineSurface) base =
            Handle(Geom_BSplineSurface)::DownCast(surface->Copy());
        base->IncreaseDegree(std::max(3, base->UDegree()), std::max(3, base->VDegree()));
        int needU = cond[int(Boundary::UMin)].order + cond[int(Boundary::UMax)].order + 2;
        int needV = cond[int(Boundary::VMin)].order + cond[int(Boundary::VMax)].order + 2;
        for (int guard = 0; guard < 16 && base->NbUPoles() < needU + 1; ++guard) {
            Frame fr(base);
            auto k = fr.breaks(int(Boundary::VMin));
            base->InsertUKnot((k[0] + k[1]) / 2, 1, 1e-9);
        }
        for (int guard = 0; guard < 16 && base->NbVPoles() < needV + 1; ++guard) {
            Frame fr(base);
            auto k = fr.breaks(int(Boundary::UMin));
            base->InsertVKnot((k[0] + k[1]) / 2, 1, 1e-9);
        }

        std::array<std::unique_ptr<Support>, 4> sup;
        for (int b = 0; b < 4; ++b) {
            if (cond[b].order > 0) {
                sup[b] = std::make_unique<Support>(cond[b].support);
            }
        }

        // the knots added along each direction, kept across rounds
        std::set<double> addU, addV;
        Handle(Geom_BSplineSurface) result;
        std::array<Measured, 4> measured;
        for (int round = 0; round <= opt.maxRefinements; ++round) {
            Handle(Geom_BSplineSurface) s = Handle(Geom_BSplineSurface)::DownCast(base->Copy());
            for (double u : addU) {
                s->InsertUKnot(u, 1, 1e-9);
            }
            for (double v : addV) {
                s->InsertVKnot(v, 1, 1e-9);
            }
            Frame fr(s);

            std::array<Targets, 4> targets;
            for (int b = 0; b < 4; ++b) {
                if (cond[b].order < 1 || !reports[b].error.empty()) {
                    continue;
                }
                GeomLProp_SLProps pp(sup[b]->surface, 2, 1e-9);
                auto br = fr.breaks(b);
                auto& tg = targets[b];
                for (std::size_t k = 0; k + 1 < br.size(); ++k) {
                    for (int i = (k == 0 ? 0 : 1); i <= Samples; ++i) {
                        double t = br[k] + (br[k + 1] - br[k]) * i / Samples;
                        auto [u, v] = fr.uv(b, t);
                        gp_Pnt p;
                        gp_Vec du, dv, duu, dvv, duv;
                        base->D2(u, v, p, du, dv, duu, dvv, duv);
                        gp_Dir n;
                        double dist = 0;
                        if (!sup[b]->at(p, dist, n, pp) || dist > opt.tolerance) {
                            std::ostringstream ss;
                            ss << boundaryName(b) << " is not on its support face";
                            if (sup[b]->at(p, dist, n, pp)) {
                                ss << " (" << dist << " from it)";
                            }
                            reports[b].error = ss.str();
                            break;
                        }
                        gp_Vec c1 = alongU(b) ? dv : du;
                        gp_Vec c2 = alongU(b) ? dvv : duu;
                        gp_Vec t1 = (c1 - gp_Vec(n) * c1.Dot(gp_Vec(n))) * cond[b].weight;
                        gp_Vec t2 = c2;
                        if (cond[b].order >= 2 && t1.Magnitude() > 1e-12) {
                            double ks = normalCurvature(pp, gp_Dir(t1));
                            t2 += gp_Vec(n) * (ks * t1.SquareMagnitude() - c2.Dot(gp_Vec(n)));
                        }
                        tg.t.push_back(t);
                        tg.d1.push_back(t1);
                        tg.d2.push_back(t2);
                    }
                    if (!reports[b].error.empty()) {
                        break;
                    }
                }
            }
            Conditions active = cond;
            for (int b = 0; b < 4; ++b) {
                if (!reports[b].error.empty()) {
                    active[b].order = 0;
                }
            }
            solve(s, fr, active, targets);
            result = s;

            // measure, and refine the spans that miss: the angle or the
            // curvature, or the derivative across its target by more than 1%,
            // which the weight is
            bool done = true;
            for (int b = 0; b < 4; ++b) {
                if (active[b].order < 1) {
                    continue;
                }
                measured[b] = measure(s, fr, b, active[b], *sup[b], opt);
                auto br = fr.breaks(b);
                const auto& tg = targets[b];
                for (std::size_t k = 0; k < tg.t.size(); ++k) {
                    auto [u, v] = fr.uv(b, tg.t[k]);
                    gp_Pnt p;
                    gp_Vec du, dv;
                    s->D1(u, v, p, du, dv);
                    gp_Vec c1 = alongU(b) ? dv : du;
                    if ((c1 - tg.d1[k]).Magnitude() > 0.01 * tg.d1[k].Magnitude()) {
                        auto it = std::upper_bound(br.begin(), br.end(), tg.t[k]);
                        std::size_t span = std::min<std::size_t>(
                            std::max<std::ptrdiff_t>(it - br.begin(), 1) - 1,
                            br.size() - 2);
                        (alongU(b) ? addU : addV).insert((br[span] + br[span + 1]) / 2);
                        done = false;
                    }
                }
                if (within(measured[b], active[b], opt)) {
                    continue;
                }
                done = false;
                for (std::size_t k = 0; k + 1 < br.size(); ++k) {
                    // the span's own worst, sampled as measure() does
                    GeomLProp_SLProps sp(s, 2, 1e-9);
                    GeomLProp_SLProps pp(sup[b]->surface, 2, 1e-9);
                    bool miss = false;
                    for (int i = 0; i <= Samples && !miss; ++i) {
                        double t = br[k] + (br[k + 1] - br[k]) * i / Samples;
                        auto [u, v] = fr.uv(b, t);
                        sp.SetParameters(u, v);
                        gp_Dir n2;
                        double dist;
                        if (!sp.IsNormalDefined() || !sup[b]->at(sp.Value(), dist, n2, pp)) {
                            continue;
                        }
                        gp_Dir n1 = sp.Normal();
                        double angle = std::acos(std::min(1.0, std::abs(n1.Dot(n2))));
                        miss = angle > opt.angularTolerance;
                        if (!miss && active[b].order >= 2) {
                            gp_Vec along = alongU(b) ? sp.D1U() : sp.D1V();
                            if (along.Magnitude() > 1e-12) {
                                gp_Dir across = n1.Crossed(gp_Dir(along));
                                double k1 = normalCurvature(sp, across);
                                double k2 = normalCurvature(pp, across);
                                miss = std::abs(k1 - (n1.Dot(n2) < 0 ? -k2 : k2))
                                    > opt.curvatureTolerance;
                            }
                        }
                    }
                    if (miss) {
                        (alongU(b) ? addU : addV).insert((br[k] + br[k + 1]) / 2);
                    }
                }
            }
            if (done) {
                break;
            }
        }

        Frame fr(result);
        for (int b = 0; b < 4; ++b) {
            if (cond[b].order < 1 || !reports[b].error.empty()) {
                continue;
            }
            auto& r = reports[b];
            r.maxAngle = measured[b].angle;
            r.maxCurvatureGap = measured[b].curvature;
            r.achieved = within(measured[b], cond[b], opt);
            if (!r.achieved) {
                auto br = fr.breaks(b);
                r.firstCorner = measured[b].worstT <= br[1];
                r.lastCorner = measured[b].worstT >= br[br.size() - 2];
            }
        }
        surface = result;
    }
    catch (Standard_Failure& e) {
        for (int b = 0; b < 4; ++b) {
            if (cond[b].order > 0) {
                reports[b].error = std::string("match boundary failed: ") + e.what();
            }
        }
    }
    return reports;
}

MatchReport matchBoundary(Handle(Geom_BSplineSurface)& surface,
                          Boundary boundary,
                          const EdgeCondition& condition,
                          const MatchOptions& options)
{
    Conditions c;
    c[int(boundary)] = condition;
    return matchBoundaries(surface, c, options)[int(boundary)];
}

}  // namespace Surface::Geom
