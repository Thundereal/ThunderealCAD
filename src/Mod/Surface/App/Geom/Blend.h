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

#include <Mod/Surface/SurfaceGlobal.h>

#include "EdgeCondition.h"

namespace Surface::Geom
{

/// One side of a blend: an edge's curve, and how the blend meets the face it
/// is on (`condition.support`, which the edge has to lie on)
struct BlendSide
{
    Handle(Geom_Curve) curve;
    EdgeCondition condition;
};

struct BlendOptions
{
    /// How many curves are laid across the gap to shape it
    int samples = 7;
    MatchOptions match;
};

struct BlendResult
{
    Handle(Geom_BSplineSurface) surface;
    std::string error;
    /// What each side achieved, in the order given
    std::array<MatchReport, 2> reports;
    /// Whether the second side was turned round to run with the first
    bool reversed = false;

    bool isDone() const
    {
        return !surface.IsNull();
    }
};

/** A surface bridging two edges on two faces (docs/Surface.md sec 5.1, Blend
 * surface)
 *
 * Curves are laid across the gap, leaving each edge across its face's
 * tangent plane, away from the face, as far as its weight says (a third of
 * the gap, times the weight); the two edges and those curves are spanned as
 * a network, and each side's condition is met by match boundary, the ends
 * of the blend free to move for the corners. A side of order 0, or with no
 * support, is met in position alone. The second edge is turned round when it
 * runs against the first.
 */
SurfaceExport BlendResult blendSurface(const BlendSide& first,
                                       const BlendSide& second,
                                       const BlendOptions& options = {});

}  // namespace Surface::Geom
