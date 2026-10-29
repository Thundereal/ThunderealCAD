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

namespace Surface::Geom
{

struct SweepOptions
{
    /// How many places along the rails a section is put, the sections'
    /// own places among them
    int stations = 9;
    /// How far a section's end may be from its rail
    double tolerance = 1e-4;
    /// How far the surface may stray from the curves (the network's)
    double fitTolerance = 1e-4;
};

struct SweepResult
{
    Handle(Geom_BSplineSurface) surface;
    std::string error;

    bool isDone() const
    {
        return !surface.IsNull();
    }
};

/** Sections swept between two rails that scale them (docs/Surface.md sec
 * 5.1, Two-rail sweep)
 *
 * Every section runs from the first rail to the second, either way round.
 * The rails are matched by their share of their own length; a section is
 * put at a place along them by the similarity that takes its ends to the
 * rails there, keeping its shape about its chord and the direction the
 * rails run. Between two sections the two, put there, are blended by
 * where the place is between them. The sections so placed and the rails are
 * spanned as a network, so the rails are the surface's sides.
 */
SurfaceExport SweepResult twoRailSweep(const Handle(Geom_Curve) & rail1,
                                       const Handle(Geom_Curve) & rail2,
                                       const std::vector<Handle(Geom_Curve)>& sections,
                                       const SweepOptions& options = {});

}  // namespace Surface::Geom
