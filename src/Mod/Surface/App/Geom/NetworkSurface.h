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

#include <string>
#include <vector>

#include <Geom_BSplineSurface.hxx>
#include <Geom_Curve.hxx>

#include <Mod/Surface/SurfaceGlobal.h>

/// Surface's geometry layer: functions over OCCT geometry, no document
/// (docs/Surface.md sec 4.1)
namespace Surface::Geom
{

struct NetworkOptions
{
    /// How far a profile and a guide may pass each other and still cross
    double tolerance = 1e-4;
    /// How far a refitted curve may stray from the curve it was given
    double fitTolerance = 1e-4;
    /// The most knot spans a refitted curve gets between two crossings
    int maxSpansPerSegment = 16;
};

struct NetworkResult
{
    /// Degree 3 and C2 inside; null when it could not be built
    Handle(Geom_BSplineSurface) surface;
    /// Why it was not built; empty when it was
    std::string error;
    /// The input profiles in their order along V, and the guides along U
    std::vector<int> profileOrder;
    std::vector<int> guideOrder;
    /// By input index: the curves turned round to run with the others
    std::vector<bool> profileReversed;
    std::vector<bool> guideReversed;
    /// The worst distance between a profile and a guide where they cross
    double maxGap = 0;
    /// The worst distance of a refitted curve from the curve it was given
    double maxFitDeviation = 0;

    bool isDone() const
    {
        return !surface.IsNull();
    }
};

/** A surface through a network of curves: profiles one way, guides the other
 *
 * Every profile has to cross every guide once, within `tolerance`. The curves
 * may come in any order and either way round; they are sorted, turned to run
 * together, and cut back to the outermost crossings, so the outer curves are
 * the surface's boundary.
 *
 * OCCT's GeomFill_Gordon makes the crossings meet at shared parameters
 * exactly, with a knot of full multiplicity at each, which leaves the surface
 * C0 inside and, on a 12 x 12 network, 2004 x 2004 poles (docs/Surface.md sec
 * 3.4). Here each curve is refitted instead: degree 3 on a knot vector its
 * family shares, through the crossings, within `fitTolerance` of where it was
 * -- and only then spanned by GeomFill_NetworkSurface.
 */
SurfaceExport NetworkResult networkSurface(const std::vector<Handle(Geom_Curve)>& profiles,
                                           const std::vector<Handle(Geom_Curve)>& guides,
                                           const NetworkOptions& options = {});

struct FamiliesResult
{
    /// Input indices, in input order: the family of the first curve, and the other
    std::vector<int> profiles;
    std::vector<int> guides;
    /// Why the curves are not one network; empty when they are
    std::string error;

    bool isDone() const
    {
        return error.empty() && !profiles.empty();
    }
};

/** The curves of a whole network (a 3D sketch's, say) sorted into profiles and
 * guides (docs/Surface.md sec 6)
 *
 * Two curves meet when they pass within `tolerance`. A network is two
 * families in which no curve meets one of its own and every curve meets every
 * curve of the other, which decides the families; the first curve's is the
 * profiles. Anything else -- a curve that meets nothing, one that meets both
 * families, a profile short of a guide -- is an error naming the curves by
 * `names`, or by their place in the input, counted from 1.
 */
SurfaceExport FamiliesResult sortFamilies(const std::vector<Handle(Geom_Curve)>& curves,
                                          double tolerance,
                                          const std::vector<std::string>& names = {});

}  // namespace Surface::Geom
