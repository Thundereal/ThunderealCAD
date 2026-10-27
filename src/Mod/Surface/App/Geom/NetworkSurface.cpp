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
#include <numeric>
#include <sstream>

#include <BSplCLib.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAPI_ExtremaCurveCurve.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <GeomConvert.hxx>
#include <GeomFill_NetworkSurface.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <NCollection_Array2.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <math_Matrix.hxx>
#endif

#include <Eigen/Dense>

#include "NetworkSurface.h"

namespace Surface::Geom
{

namespace
{

using BSpline = Handle(Geom_BSplineCurve);

constexpr int Degree = 3;
// samples of each curve per knot span of its fit
constexpr int SamplesPerSpan = 8;

/// A curve of the network: its B-spline, turned if it had to be, and where it
/// crosses the curves of the other family, in their order
struct Member
{
    BSpline curve;
    std::vector<double> params;  // on this curve, one per crossing, increasing
};

/// A monotone cubic through (x_k, y_k), both increasing: the map from the
/// family's shared parameter to a curve's own (Fritsch and Butland's slopes,
/// which keep it monotone)
class MonotoneMap
{
public:
    MonotoneMap(std::vector<double> x, std::vector<double> y)
        : x(std::move(x))
        , y(std::move(y))
        , m(this->x.size())
    {
        size_t n = this->x.size();
        std::vector<double> h(n - 1), d(n - 1);
        for (size_t k = 0; k + 1 < n; ++k) {
            h[k] = this->x[k + 1] - this->x[k];
            d[k] = (this->y[k + 1] - this->y[k]) / h[k];
        }
        m[0] = d[0];
        m[n - 1] = d[n - 2];
        for (size_t k = 1; k + 1 < n; ++k) {
            if (d[k - 1] * d[k] <= 0) {
                m[k] = 0;
            }
            else {
                m[k] = 3 * (h[k - 1] + h[k])
                    / ((2 * h[k] + h[k - 1]) / d[k - 1] + (h[k] + 2 * h[k - 1]) / d[k]);
            }
        }
    }

    double operator()(double t) const
    {
        size_t k = std::upper_bound(x.begin(), x.end(), t) - x.begin();
        k = std::clamp<size_t>(k, 1, x.size() - 1) - 1;
        double h = x[k + 1] - x[k];
        double s = (t - x[k]) / h;
        double s2 = s * s, s3 = s2 * s;
        return (2 * s3 - 3 * s2 + 1) * y[k] + (s3 - 2 * s2 + s) * h * m[k]
            + (-2 * s3 + 3 * s2) * y[k + 1] + (s3 - s2) * h * m[k + 1];
    }

private:
    std::vector<double> x, y, m;
};

/// The flat knot vector of a cubic on [0, 1] with `spans[k]` equal spans
/// between breaks[k] and breaks[k + 1]
std::vector<double> flatKnots(const std::vector<double>& breaks, const std::vector<int>& spans)
{
    std::vector<double> k(Degree + 1, breaks.front());
    for (size_t s = 0; s + 1 < breaks.size(); ++s) {
        for (int i = 1; i <= spans[s]; ++i) {
            k.push_back(breaks[s] + (breaks[s + 1] - breaks[s]) * i / spans[s]);
        }
    }
    k.back() = breaks.back();
    k.insert(k.end(), Degree, breaks.back());
    return k;
}

/// The B-spline basis of a cubic on `flat` at t: the first nonzero pole's
/// index (from 0), and the four values
int basis(const NCollection_Array1<double>& flat, double t, double out[Degree + 1])
{
    math_Matrix m(1, 1, 1, Degree + 1);
    int first = 0;
    BSplCLib::EvalBsplineBasis(0, Degree + 1, flat, t, first, m);
    for (int i = 0; i <= Degree; ++i) {
        out[i] = m(1, i + 1);
    }
    return first - 1;
}

/// One family refitted: every curve a cubic on the same knots, through its
/// crossings at the shared parameters `breaks`, and within `tol` of where it
/// was. Returns the worst deviation; `fits` the curves.
double fitFamily(const std::vector<Member>& family,
                 const std::vector<double>& breaks,
                 const std::vector<std::vector<gp_Pnt>>& crossings,  // [curve][crossing]
                 double tol,
                 int maxSpans,
                 std::vector<BSpline>& fits)
{
    const size_t nseg = breaks.size() - 1;
    std::vector<int> spans(nseg, 1);
    std::vector<MonotoneMap> maps;
    for (const auto& m : family) {
        maps.emplace_back(breaks, m.params);
    }

    double worst = 0;
    for (;;) {
        std::vector<double> flat = flatKnots(breaks, spans);
        NCollection_Array1<double> occFlat(1, (int)flat.size());
        for (size_t i = 0; i < flat.size(); ++i) {
            occFlat.SetValue((int)i + 1, flat[i]);
        }
        const int npoles = (int)flat.size() - Degree - 1;

        // the samples, the same parameters for every curve of the family
        std::vector<double> ts;
        std::vector<size_t> segOf;
        for (size_t s = 0; s < nseg; ++s) {
            int n = spans[s] * SamplesPerSpan;
            for (int i = (s == 0 ? 0 : 1); i <= n; ++i) {
                ts.push_back(breaks[s] + (breaks[s + 1] - breaks[s]) * i / n);
                segOf.push_back(s);
            }
        }
        const int nsamp = (int)ts.size();
        const int ncon = (int)breaks.size();

        // the KKT system of least squares through the crossings:
        // [A'A C'; C 0] [x; l] = [A'b; d]; its matrix is the family's own
        Eigen::MatrixXd A = Eigen::MatrixXd::Zero(nsamp, npoles);
        Eigen::MatrixXd C = Eigen::MatrixXd::Zero(ncon, npoles);
        double b[Degree + 1];
        for (int r = 0; r < nsamp; ++r) {
            int f = basis(occFlat, ts[r], b);
            for (int i = 0; i <= Degree; ++i) {
                A(r, f + i) = b[i];
            }
        }
        for (int r = 0; r < ncon; ++r) {
            int f = basis(occFlat, breaks[r], b);
            for (int i = 0; i <= Degree; ++i) {
                C(r, f + i) = b[i];
            }
        }
        Eigen::MatrixXd K = Eigen::MatrixXd::Zero(npoles + ncon, npoles + ncon);
        K.topLeftCorner(npoles, npoles) = A.transpose() * A;
        K.topRightCorner(npoles, ncon) = C.transpose();
        K.bottomLeftCorner(ncon, npoles) = C;
        Eigen::PartialPivLU<Eigen::MatrixXd> lu(K);

        std::vector<double> segWorst(nseg, 0);
        worst = 0;
        fits.clear();
        TColStd_Array1OfReal knots(1, (int)flat.size() - 2 * Degree);
        TColStd_Array1OfInteger mults(1, knots.Length());
        for (int i = 1; i <= knots.Length(); ++i) {
            knots.SetValue(i, flat[i + Degree - 1]);
            mults.SetValue(i, 1);
        }
        mults.SetValue(1, Degree + 1);
        mults.SetValue(knots.Length(), Degree + 1);

        for (size_t c = 0; c < family.size(); ++c) {
            const auto& curve = family[c].curve;
            std::vector<gp_Pnt> target(nsamp);
            Eigen::MatrixXd B(nsamp, 3);
            for (int r = 0; r < nsamp; ++r) {
                target[r] = curve->Value(maps[c](ts[r]));
                B(r, 0) = target[r].X();
                B(r, 1) = target[r].Y();
                B(r, 2) = target[r].Z();
            }
            Eigen::MatrixXd rhs(npoles + ncon, 3);
            rhs.topRows(npoles) = A.transpose() * B;
            for (int r = 0; r < ncon; ++r) {
                const gp_Pnt& p = crossings[c][r];
                rhs(npoles + r, 0) = p.X();
                rhs(npoles + r, 1) = p.Y();
                rhs(npoles + r, 2) = p.Z();
            }
            Eigen::MatrixXd x = lu.solve(rhs);
            TColgp_Array1OfPnt poles(1, npoles);
            for (int i = 0; i < npoles; ++i) {
                poles.SetValue(i + 1, gp_Pnt(x(i, 0), x(i, 1), x(i, 2)));
            }
            BSpline fit = new Geom_BSplineCurve(poles, knots, mults, Degree);
            // between the samples as well as at them
            for (int r = 0; r < nsamp; ++r) {
                double d = fit->Value(ts[r]).Distance(target[r]);
                if (r > 0) {
                    double tm = (ts[r - 1] + ts[r]) / 2;
                    d = std::max(d, fit->Value(tm).Distance(curve->Value(maps[c](tm))));
                }
                segWorst[segOf[r]] = std::max(segWorst[segOf[r]], d);
                worst = std::max(worst, d);
            }
            fits.push_back(fit);
        }

        bool refined = false;
        for (size_t s = 0; s < nseg; ++s) {
            if (segWorst[s] > tol && spans[s] < maxSpans) {
                spans[s] = std::min(spans[s] * 2, maxSpans);
                refined = true;
            }
        }
        if (!refined) {
            return worst;
        }
    }
}

/// Is `v` increasing (1), decreasing (-1), or neither (0)
int direction(const std::vector<double>& v)
{
    bool up = true, down = true;
    for (size_t k = 1; k < v.size(); ++k) {
        up = up && v[k] > v[k - 1];
        down = down && v[k] < v[k - 1];
    }
    return up ? 1 : (down ? -1 : 0);
}

/// The shared parameters of a family: the mean of where each curve crosses
/// the other family, by arc length from its first crossing, over the whole
std::vector<double> sharedParameters(const std::vector<Member>& family)
{
    size_t n = family.front().params.size();
    std::vector<double> mean(n, 0);
    for (const auto& m : family) {
        GeomAdaptor_Curve a(m.curve);
        std::vector<double> len(n, 0);
        for (size_t k = 1; k < n; ++k) {
            len[k] = len[k - 1] + GCPnts_AbscissaPoint::Length(a, m.params[k - 1], m.params[k]);
        }
        for (size_t k = 0; k < n; ++k) {
            mean[k] += len[k] / len.back();
        }
    }
    for (auto& v : mean) {
        v /= family.size();
    }
    mean.front() = 0;
    mean.back() = 1;
    return mean;
}

std::string curveName(bool profile, int index)
{
    return std::string(profile ? "profile " : "guide ") + std::to_string(index + 1);
}

}  // namespace

NetworkResult networkSurface(const std::vector<Handle(Geom_Curve)>& profiles,
                             const std::vector<Handle(Geom_Curve)>& guides,
                             const NetworkOptions& options)
{
    NetworkResult r;
    const int np = (int)profiles.size(), ng = (int)guides.size();
    if (np < 2 || ng < 2) {
        r.error = "a network needs at least two profiles and two guides";
        return r;
    }

    try {
        std::vector<BSpline> P, G;
        for (int i = 0; i < np + ng; ++i) {
            const auto& c = i < np ? profiles[i] : guides[i - np];
            if (c.IsNull() || Precision::IsInfinite(c->FirstParameter())
                || Precision::IsInfinite(c->LastParameter())) {
                r.error = curveName(i < np, i < np ? i : i - np) + " is not a bounded curve";
                return r;
            }
            (i < np ? P : G).push_back(GeomConvert::CurveToBSplineCurve(c));
        }

        // where each profile crosses each guide
        std::vector<std::vector<double>> up(np, std::vector<double>(ng));
        std::vector<std::vector<double>> vg(np, std::vector<double>(ng));
        std::vector<std::vector<gp_Pnt>> mid(np, std::vector<gp_Pnt>(ng));
        for (int i = 0; i < np; ++i) {
            for (int j = 0; j < ng; ++j) {
                GeomAPI_ExtremaCurveCurve ext(P[i], G[j]);
                double a = 0, b = 0;
                if (!ext.TotalLowerDistanceParameters(a, b)) {
                    r.error = curveName(true, i) + " and " + curveName(false, j)
                        + " do not meet";
                    return r;
                }
                double gap = ext.TotalLowerDistance();
                if (gap > options.tolerance) {
                    std::ostringstream ss;
                    ss << curveName(true, i) << " and " << curveName(false, j)
                       << " do not meet: " << gap << " apart, more than the tolerance "
                       << options.tolerance;
                    r.error = ss.str();
                    return r;
                }
                r.maxGap = std::max(r.maxGap, gap);
                up[i][j] = a;
                vg[i][j] = b;
                gp_Pnt pa = P[i]->Value(a), pb = G[j]->Value(b);
                mid[i][j] = gp_Pnt((pa.XYZ() + pb.XYZ()) / 2);
            }
        }

        // The first profile as given sets U, and the guides go in the order
        // it crosses them; the first of those as given sets V, and the
        // profiles go in the order it crosses them
        r.guideOrder.resize(ng);
        std::iota(r.guideOrder.begin(), r.guideOrder.end(), 0);
        std::sort(r.guideOrder.begin(), r.guideOrder.end(), [&](int a, int b) {
            return up[0][a] < up[0][b];
        });
        const int g0 = r.guideOrder.front();
        r.profileOrder.resize(np);
        std::iota(r.profileOrder.begin(), r.profileOrder.end(), 0);
        std::sort(r.profileOrder.begin(), r.profileOrder.end(), [&](int a, int b) {
            return vg[a][g0] < vg[b][g0];
        });

        std::vector<Member> pm, gm;
        r.profileReversed.assign(np, false);
        r.guideReversed.assign(ng, false);
        for (int pass = 0; pass < 2; ++pass) {
            bool prof = pass == 0;
            const auto& order = prof ? r.profileOrder : r.guideOrder;
            const auto& other = prof ? r.guideOrder : r.profileOrder;
            for (int idx : order) {
                Member m;
                m.curve = prof ? P[idx] : G[idx];
                for (int o : other) {
                    m.params.push_back(prof ? up[idx][o] : vg[o][idx]);
                }
                int dir = direction(m.params);
                if (dir == 0) {
                    r.error = curveName(prof, idx) + " crosses the "
                        + (prof ? "guides" : "profiles") + " out of order";
                    return r;
                }
                if (dir < 0) {
                    BSpline rev = BSpline::DownCast(m.curve->Reversed());
                    for (auto& t : m.params) {
                        t = m.curve->ReversedParameter(t);
                    }
                    m.curve = rev;
                    (prof ? r.profileReversed : r.guideReversed)[idx] = true;
                }
                (prof ? pm : gm).push_back(m);
            }
        }

        // the crossings each curve has to pass through, in order
        std::vector<std::vector<gp_Pnt>> pc(np), gc(ng);
        for (int a = 0; a < np; ++a) {
            for (int b = 0; b < ng; ++b) {
                const gp_Pnt& x = mid[r.profileOrder[a]][r.guideOrder[b]];
                pc[a].push_back(x);
                gc[b].push_back(x);
            }
        }

        std::vector<double> U = sharedParameters(pm), V = sharedParameters(gm);
        std::vector<BSpline> pf, gf;
        r.maxFitDeviation = std::max(
            fitFamily(pm, U, pc, options.fitTolerance, options.maxSpansPerSegment, pf),
            fitFamily(gm, V, gc, options.fitTolerance, options.maxSpansPerSegment, gf));

        NCollection_Array1<Handle(Geom_BSplineCurve)> aP(1, np), aG(1, ng);
        NCollection_Array1<double> aU(1, ng), aV(1, np);
        NCollection_Array2<gp_Pnt> grid(1, ng, 1, np);
        NCollection_Array2<double> weights(1, ng, 1, np);
        for (int a = 0; a < np; ++a) {
            aP.SetValue(a + 1, pf[a]);
            aV.SetValue(a + 1, V[a]);
        }
        for (int b = 0; b < ng; ++b) {
            aG.SetValue(b + 1, gf[b]);
            aU.SetValue(b + 1, U[b]);
            for (int a = 0; a < np; ++a) {
                grid.SetValue(b + 1, a + 1, pc[a][b]);
                weights.SetValue(b + 1, a + 1, 1.0);
            }
        }
        GeomFill_NetworkSurface net;
        net.Init(aP, aG, aV, aU, grid, weights, options.tolerance, false, false);
        net.Perform();
        if (!net.IsDone()) {
            r.error = "the network surface could not be built (GeomFill_NetworkSurface status "
                + std::to_string((int)net.Status()) + ")";
            return r;
        }
        r.surface = net.Surface();
    }
    catch (Standard_Failure& e) {
        r.surface.Nullify();
        r.error = std::string("the network surface failed: ") + e.what();
    }
    return r;
}

FamiliesResult sortFamilies(const std::vector<Handle(Geom_Curve)>& curves,
                            double tolerance,
                            const std::vector<std::string>& names)
{
    FamiliesResult r;
    const int n = (int)curves.size();
    auto name = [&](int i) {
        return i < (int)names.size() ? names[i] : "curve " + std::to_string(i + 1);
    };
    if (n < 4) {
        r.error = "a network needs two families of at least two curves";
        return r;
    }
    for (int i = 0; i < n; ++i) {
        const auto& c = curves[i];
        if (c.IsNull() || Precision::IsInfinite(c->FirstParameter())
            || Precision::IsInfinite(c->LastParameter())) {
            r.error = name(i) + " is not a bounded curve";
            return r;
        }
    }

    try {
        std::vector<std::vector<char>> meets(n, std::vector<char>(n, 0));
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                GeomAPI_ExtremaCurveCurve ext(curves[i], curves[j]);
                double a = 0, b = 0;
                if (ext.TotalLowerDistanceParameters(a, b)
                    && ext.TotalLowerDistance() <= tolerance) {
                    meets[i][j] = meets[j][i] = 1;
                }
            }
        }

        // two-colour the graph of meetings from the first curve: a network's
        // is connected and bipartite, so the colouring is the families
        std::vector<int> family(n, -1);
        std::vector<int> queue = {0};
        family[0] = 0;
        for (std::size_t k = 0; k < queue.size(); ++k) {
            int i = queue[k];
            for (int j = 0; j < n; ++j) {
                if (!meets[i][j]) {
                    continue;
                }
                if (family[j] < 0) {
                    family[j] = 1 - family[i];
                    queue.push_back(j);
                }
                else if (family[j] == family[i]) {
                    // three curves that meet one another cannot be two
                    // families; which of them is the stray is the user's call
                    for (int c = 0; c < n; ++c) {
                        if (meets[c][i] && meets[c][j]) {
                            int t[3] = {c, i, j};
                            std::sort(t, t + 3);
                            r.error = name(t[0]) + ", " + name(t[1]) + " and " + name(t[2])
                                + " all meet one another: a curve may only meet the other "
                                  "family";
                            return r;
                        }
                    }
                    r.error = name(std::min(i, j)) + " and " + name(std::max(i, j))
                        + " meet, but the other crossings put them in one family";
                    return r;
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            if (family[i] < 0) {
                bool none = std::none_of(meets[i].begin(), meets[i].end(), [](char m) {
                    return m != 0;
                });
                r.error = none ? name(i) + " meets none of the others"
                               : name(i) + " is not connected to " + name(0)
                        + " through crossings: the curves are more than one network";
                return r;
            }
        }
        for (int i = 0; i < n; ++i) {
            (family[i] == 0 ? r.profiles : r.guides).push_back(i);
        }
        if (r.profiles.size() < 2 || r.guides.size() < 2) {
            r.error = "a network needs two families of at least two curves";
            r.profiles.clear();
            r.guides.clear();
            return r;
        }
        for (int i : r.profiles) {
            for (int j : r.guides) {
                if (!meets[i][j]) {
                    std::ostringstream ss;
                    ss << name(std::min(i, j)) << " and " << name(std::max(i, j))
                       << " do not meet: every curve one way has to cross every curve "
                          "the other, within "
                       << tolerance;
                    r.error = ss.str();
                    r.profiles.clear();
                    r.guides.clear();
                    return r;
                }
            }
        }
    }
    catch (Standard_Failure& e) {
        r.profiles.clear();
        r.guides.clear();
        r.error = std::string("sorting the curves failed: ") + e.what();
    }
    return r;
}

}  // namespace Surface::Geom
