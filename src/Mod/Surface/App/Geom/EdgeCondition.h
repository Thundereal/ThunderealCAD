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

#pragma once

#include <array>
#include <string>

#include <Geom_BSplineSurface.hxx>
#include <Geom_Curve.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <Mod/Surface/SurfaceGlobal.h>

namespace Surface::Geom
{

/// A side of a B-spline surface's parameter rectangle
enum class Boundary
{
    UMin,
    UMax,
    VMin,
    VMax
};

/// How a surface meets a neighbour along one of its boundaries
/// (docs/Surface.md sec 4.3): one meaning for every surface tool
struct EdgeCondition
{
    /// 0 = G0 (position only), 1 = G1 (tangent), 2 = G2 (curvature)
    int order = 0;
    /// The face to match; the boundary has to lie on it
    TopoDS_Face support;
    /// Scales the derivative across the boundary; 1 keeps its own length
    double weight = 1.0;
    /// The weight at the boundary's end, as its parameter runs, the weight
    /// at its start, linearly between; 0 is the weight all along
    double weightEnd = 0.0;
    /// With order 0: no curve of the user's is there (the side of a loft),
    /// so its poles may move for the boundaries that meet it
    bool free = false;
    /// The share of the surface across the boundary (0 to 1, from it) that
    /// may change; the rest stays exactly where it is
    double reach = 1.0;

    /// The weight `s` of the way along the boundary (0 to 1)
    double weightAt(double s) const
    {
        return weightEnd > 0 ? weight + (weightEnd - weight) * s : weight;
    }
};

struct MatchOptions
{
    /// How far the boundary may be from its support
    double tolerance = 1e-4;
    /// The angle between the surface and its support that counts as tangent (radians)
    double angularTolerance = 0.1 * 3.14159265358979323846 / 180;
    /// The difference of normal curvature across the boundary that counts as G2 (1/length)
    double curvatureTolerance = 1e-3;
    /// The most times the knots along the boundary are refined
    int maxRefinements = 6;
};

/// What a boundary achieved, measured along it
struct MatchReport
{
    /// Why it was not matched; empty when it was (even if not within tolerance)
    std::string error;
    /// The worst angle to the support (radians), and the worst normal
    /// curvature difference across the boundary (G2 only)
    double maxAngle = 0;
    double maxCurvatureGap = 0;
    /// Within the tolerances
    bool achieved = false;
    /// The worst point is at the boundary's first or last corner, where the
    /// curve that meets it there does not agree with the support: the corner
    /// is reported, not the edge loosened (docs/Surface.md sec 10, risk 1)
    bool firstCorner = false;
    bool lastCorner = false;
};

using Conditions = std::array<EdgeCondition, 4>;  // by Boundary
using Reports = std::array<MatchReport, 4>;

/** Match the boundaries of `surface` to their support faces: "match boundary"
 *
 * Each boundary and the poles of the boundaries that meet it stay where
 * they are. G1 sets the second row of poles from the boundary in, so
 * that the derivative across lies in the support's tangent plane, its own
 * length times the weight; G2 also sets the third, so that the normal
 * curvature across is the support's. Boundaries that meet at a corner share
 * the poles next to it, so all of them are solved together, in one least
 * squares over every pole they set. The knots along each boundary are
 * refined until both are within the tolerances, or as far as
 * maxRefinements allows; what was achieved is reported either way.
 */
SurfaceExport Reports matchBoundaries(Handle(Geom_BSplineSurface)& surface,
                                      const Conditions& conditions,
                                      const MatchOptions& options = {});

/// The boundary of `surface` that `curve` lies on (ends and middle within
/// tol), or -1
SurfaceExport int boundaryOf(const Handle(Geom_BSplineSurface) & surface,
                             const Handle(Geom_Curve) & curve,
                             double tol);

/// One boundary alone
SurfaceExport MatchReport matchBoundary(Handle(Geom_BSplineSurface)& surface,
                                        Boundary boundary,
                                        const EdgeCondition& condition,
                                        const MatchOptions& options = {});

/// Where a boundary's edge condition is shown in the view (docs/Surface.md
/// sec 8): the boundary's middle, the way across it into the surface --
/// square to it, in the surface's tangent plane -- and its length
struct BoundaryMarker
{
    gp_Pnt middle;
    gp_Dir inward;
    double length = 0;
};
SurfaceExport BoundaryMarker boundaryMarker(const Handle(Geom_BSplineSurface) & surface,
                                            Boundary boundary);

}  // namespace Surface::Geom
