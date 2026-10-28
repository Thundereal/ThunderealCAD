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

#include <algorithm>
#include <cmath>

#include "Geo3D.h"

namespace GCS
{

static inline double dvar(const double* p, const double* derivparam)
{
    return p == derivparam ? 1.0 : 0.0;
}

DeriVector3::DeriVector3(const Point3& p, const double* derivparam)
    : x(*p.x)
    , dx(dvar(p.x, derivparam))
    , y(*p.y)
    , dy(dvar(p.y, derivparam))
    , z(*p.z)
    , dz(dvar(p.z, derivparam))
{}

DeriVector3::DeriVector3(const VEC_pD& pvec, std::size_t i, const double* derivparam)
    : x(*pvec[i])
    , dx(dvar(pvec[i], derivparam))
    , y(*pvec[i + 1])
    , dy(dvar(pvec[i + 1], derivparam))
    , z(*pvec[i + 2])
    , dz(dvar(pvec[i + 2], derivparam))
{}

double DeriVector3::length() const
{
    return std::sqrt(x * x + y * y + z * z);
}

double DeriVector3::length(double& dlength) const
{
    double l = length();
    dlength = l == 0. ? 0. : (x * dx + y * dy + z * dz) / l;
    return l;
}

double DeriVector3::dot(const DeriVector3& v, double& ddot) const
{
    ddot = dx * v.x + x * v.dx + dy * v.y + y * v.dy + dz * v.z + z * v.dz;
    return x * v.x + y * v.y + z * v.z;
}

DeriVector3 DeriVector3::cross(const DeriVector3& v) const
{
    return {y * v.z - z * v.y,
            z * v.x - x * v.z,
            x * v.y - y * v.x,
            dy * v.z + y * v.dz - dz * v.y - z * v.dy,
            dz * v.x + z * v.dx - dx * v.z - x * v.dz,
            dx * v.y + x * v.dy - dy * v.x - y * v.dx};
}

double DeriVector3::component(int i, double& dc) const
{
    switch (i) {
        case 0:
            dc = dx;
            return x;
        case 1:
            dc = dy;
            return y;
        default:
            dc = dz;
            return z;
    }
}

DeriVector3 quaternionNormal(const VEC_pD& pvec, std::size_t i, const double* derivparam)
{
    return quaternionAxis(pvec, i, 2, derivparam);
}

DeriVector3 quaternionAxis(const VEC_pD& pvec, std::size_t i, int axis, const double* derivparam)
{
    double w = *pvec[i], x = *pvec[i + 1], y = *pvec[i + 2], z = *pvec[i + 3];
    double dw = dvar(pvec[i], derivparam), dx = dvar(pvec[i + 1], derivparam),
           dy = dvar(pvec[i + 2], derivparam), dz = dvar(pvec[i + 3], derivparam);

    // Column axis of R(q), unnormalised, and |q|^2.
    double nx, ny, nz, dnx, dny, dnz;
    switch (axis) {
        case 0:
            nx = w * w + x * x - y * y - z * z;
            ny = 2 * (x * y + w * z);
            nz = 2 * (x * z - w * y);
            dnx = 2 * (w * dw + x * dx - y * dy - z * dz);
            dny = 2 * (dx * y + x * dy + dw * z + w * dz);
            dnz = 2 * (dx * z + x * dz - dw * y - w * dy);
            break;
        case 1:
            nx = 2 * (x * y - w * z);
            ny = w * w - x * x + y * y - z * z;
            nz = 2 * (y * z + w * x);
            dnx = 2 * (dx * y + x * dy - dw * z - w * dz);
            dny = 2 * (w * dw - x * dx + y * dy - z * dz);
            dnz = 2 * (dy * z + y * dz + dw * x + w * dx);
            break;
        default:
            nx = 2 * (x * z + w * y);
            ny = 2 * (y * z - w * x);
            nz = w * w - x * x - y * y + z * z;
            dnx = 2 * (dx * z + x * dz + dw * y + w * dy);
            dny = 2 * (dy * z + y * dz - dw * x - w * dx);
            dnz = 2 * (w * dw - x * dx - y * dy + z * dz);
            break;
    }
    double s = w * w + x * x + y * y + z * z;
    double ds = 2 * (w * dw + x * dx + y * dy + z * dz);
    if (s == 0.) {
        return {};
    }
    double s2 = s * s;
    return {nx / s,
            ny / s,
            nz / s,
            (dnx * s - nx * ds) / s2,
            (dny * s - ny * ds) / s2,
            (dnz * s - nz * ds) / s2};
}

void BSpline3::push(VEC_pD& pvec) const
{
    for (const auto& p : poles) {
        p.push(pvec);
    }
    for (auto* w : weights) {
        pvec.push_back(w);
    }
}

std::size_t BSpline3View::span(double u) const
{
    // Clamped knots: the last span is closed at its end.
    std::size_t n = npoles - 1;
    auto p = static_cast<std::size_t>(degree);
    if (u >= flatknots[n + 1]) {
        return n;
    }
    if (u <= flatknots[p]) {
        return p;
    }
    std::size_t low = p, high = n + 1;
    std::size_t mid = (low + high) / 2;
    while (u < flatknots[mid] || u >= flatknots[mid + 1]) {
        if (u < flatknots[mid]) {
            high = mid;
        }
        else {
            low = mid;
        }
        mid = (low + high) / 2;
    }
    return mid;
}

void BSpline3View::eval(double u, std::size_t k, VEC_D& N, VEC_D& dN) const
{
    // The nonzero basis functions of span k and their first derivatives (The NURBS
    // Book, A2.3, with n = 1).
    auto p = static_cast<std::size_t>(degree);
    const VEC_D& U = flatknots;
    std::vector<VEC_D> ndu(p + 1, VEC_D(p + 1, 0.));
    VEC_D left(p + 1, 0.), right(p + 1, 0.);
    ndu[0][0] = 1.;
    for (std::size_t j = 1; j <= p; ++j) {
        left[j] = u - U[k + 1 - j];
        right[j] = U[k + j] - u;
        double saved = 0.;
        for (std::size_t r = 0; r < j; ++r) {
            ndu[j][r] = right[r + 1] + left[j - r];
            double temp = ndu[j][r] == 0. ? 0. : ndu[r][j - 1] / ndu[j][r];
            ndu[r][j] = saved + right[r + 1] * temp;
            saved = left[j - r] * temp;
        }
        ndu[j][j] = saved;
    }
    N.assign(p + 1, 0.);
    dN.assign(p + 1, 0.);
    for (std::size_t j = 0; j <= p; ++j) {
        N[j] = ndu[j][p];
    }
    if (p == 0) {
        return;
    }
    for (std::size_t r = 0; r <= p; ++r) {
        double d = 0.;
        if (r >= 1) {
            d += ndu[r - 1][p - 1] / ndu[p][r - 1];
        }
        if (r <= p - 1) {
            d -= ndu[r][p - 1] / ndu[p][r];
        }
        dN[r] = d * static_cast<double>(p);
    }
}

DeriVector3 BSpline3View::value(double u, double du, const double* derivparam) const
{
    std::size_t k = span(u);
    VEC_D N, dN;
    eval(u, k, N, dN);
    auto p = static_cast<std::size_t>(degree);
    std::size_t w0 = first + npoles * 3;

    // A = sum N w P, W = sum N w, and their derivatives by u.
    double A[3] = {0, 0, 0}, dA[3] = {0, 0, 0}, W = 0, dW = 0;
    for (std::size_t j = 0; j <= p; ++j) {
        std::size_t i = k - p + j;
        double w = *pvec[w0 + i];
        for (int c = 0; c < 3; ++c) {
            double P = *pvec[first + 3 * i + c];
            A[c] += N[j] * w * P;
            dA[c] += dN[j] * w * P;
        }
        W += N[j] * w;
        dW += dN[j] * w;
    }
    double C[3] = {A[0] / W, A[1] / W, A[2] / W};
    double dCdu[3];
    for (int c = 0; c < 3; ++c) {
        dCdu[c] = (dA[c] - C[c] * dW) / W;
    }

    // Derivative by the poles and weights in the span. A pointer may appear more than
    // once after the solver redirects equal parameters, so every match adds.
    double D[3] = {du * dCdu[0], du * dCdu[1], du * dCdu[2]};
    if (derivparam) {
        for (std::size_t j = 0; j <= p; ++j) {
            std::size_t i = k - p + j;
            double w = *pvec[w0 + i];
            for (int c = 0; c < 3; ++c) {
                if (pvec[first + 3 * i + c] == derivparam) {
                    D[c] += N[j] * w / W;
                }
            }
            if (pvec[w0 + i] == derivparam) {
                for (int c = 0; c < 3; ++c) {
                    D[c] += N[j] * (*pvec[first + 3 * i + c] - C[c]) / W;
                }
            }
        }
    }
    return {C[0], C[1], C[2], D[0], D[1], D[2]};
}

void BSpline3View::ders(double u, std::size_t k, int n, std::vector<VEC_D>& out) const
{
    const int p = degree;
    const VEC_D& U = flatknots;
    std::vector<VEC_D> ndu(p + 1, VEC_D(p + 1, 0.));
    VEC_D left(p + 1, 0.), right(p + 1, 0.);
    ndu[0][0] = 1.;
    for (int j = 1; j <= p; ++j) {
        left[j] = u - U[k + 1 - j];
        right[j] = U[k + j] - u;
        double saved = 0.;
        for (int r = 0; r < j; ++r) {
            ndu[j][r] = right[r + 1] + left[j - r];
            double temp = ndu[j][r] == 0. ? 0. : ndu[r][j - 1] / ndu[j][r];
            ndu[r][j] = saved + right[r + 1] * temp;
            saved = left[j - r] * temp;
        }
        ndu[j][j] = saved;
    }
    out.assign(n + 1, VEC_D(p + 1, 0.));
    for (int j = 0; j <= p; ++j) {
        out[0][j] = ndu[j][p];
    }
    auto div = [](double a, double b) {
        return b == 0. ? 0. : a / b;
    };
    const int m = std::min(n, p);  // above the degree they are zero
    std::vector<VEC_D> a(2, VEC_D(p + 1, 0.));
    for (int r = 0; r <= p; ++r) {
        int s1 = 0, s2 = 1;
        a[0][0] = 1.;
        for (int kk = 1; kk <= m; ++kk) {
            double d = 0.;
            int rk = r - kk, pk = p - kk;
            if (r >= kk) {
                a[s2][0] = div(a[s1][0], ndu[pk + 1][rk]);
                d = a[s2][0] * ndu[rk][pk];
            }
            int j1 = rk >= -1 ? 1 : -rk;
            int j2 = r - 1 <= pk ? kk - 1 : p - r;
            for (int j = j1; j <= j2; ++j) {
                a[s2][j] = div(a[s1][j] - a[s1][j - 1], ndu[pk + 1][rk + j]);
                d += a[s2][j] * ndu[rk + j][pk];
            }
            if (r <= pk) {
                a[s2][kk] = div(-a[s1][kk - 1], ndu[pk + 1][r]);
                d += a[s2][kk] * ndu[r][pk];
            }
            out[kk][r] = d;
            std::swap(s1, s2);
        }
    }
    double f = p;
    for (int kk = 1; kk <= m; ++kk) {
        for (int j = 0; j <= p; ++j) {
            out[kk][j] *= f;
        }
        f *= p - kk;
    }
}

void BSpline3View::jet(double u, double c[3], double d1[3], double d2[3]) const
{
    // C = A / W; C' = (A' - C W') / W; C'' = (A'' - 2 C' W' - C W'') / W.
    std::size_t k = span(u);
    std::vector<VEC_D> N;
    ders(u, k, 2, N);
    auto p = static_cast<std::size_t>(degree);
    std::size_t w0 = first + npoles * 3;
    double A[3][3] = {}, W[3] = {};
    for (std::size_t j = 0; j <= p; ++j) {
        std::size_t i = k - p + j;
        double w = *pvec[w0 + i];
        for (int o = 0; o < 3; ++o) {
            for (int q = 0; q < 3; ++q) {
                A[o][q] += N[o][j] * w * *pvec[first + 3 * i + q];
            }
            W[o] += N[o][j] * w;
        }
    }
    for (int q = 0; q < 3; ++q) {
        c[q] = A[0][q] / W[0];
        d1[q] = (A[1][q] - c[q] * W[1]) / W[0];
        d2[q] = (A[2][q] - 2 * d1[q] * W[1] - c[q] * W[2]) / W[0];
    }
}

DeriVector3 BSpline3View::tangent(double u, const double* derivparam) const
{
    // C' = (A' - C W') / W, differentiated by the poles and weights.
    std::size_t k = span(u);
    VEC_D N, dN;
    eval(u, k, N, dN);
    auto p = static_cast<std::size_t>(degree);
    std::size_t w0 = first + npoles * 3;

    double A[3] = {0, 0, 0}, dA[3] = {0, 0, 0}, W = 0, dW = 0;
    double gA[3] = {0, 0, 0}, gdA[3] = {0, 0, 0}, gW = 0, gdW = 0;  // by derivparam
    for (std::size_t j = 0; j <= p; ++j) {
        std::size_t i = k - p + j;
        double w = *pvec[w0 + i];
        double gw = pvec[w0 + i] == derivparam ? 1. : 0.;
        for (int c = 0; c < 3; ++c) {
            double P = *pvec[first + 3 * i + c];
            double gP = pvec[first + 3 * i + c] == derivparam ? 1. : 0.;
            A[c] += N[j] * w * P;
            dA[c] += dN[j] * w * P;
            gA[c] += N[j] * (gw * P + w * gP);
            gdA[c] += dN[j] * (gw * P + w * gP);
        }
        W += N[j] * w;
        dW += dN[j] * w;
        gW += N[j] * gw;
        gdW += dN[j] * gw;
    }
    double T[3], gT[3];
    for (int c = 0; c < 3; ++c) {
        double C = A[c] / W;
        double gC = (gA[c] - C * gW) / W;
        T[c] = (dA[c] - C * dW) / W;
        gT[c] = (gdA[c] - gC * dW - C * gdW) / W - T[c] * gW / W;
    }
    return {T[0], T[1], T[2], gT[0], gT[1], gT[2]};
}

}  // namespace GCS
