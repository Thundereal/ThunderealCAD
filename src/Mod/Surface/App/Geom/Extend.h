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

#include <Geom_BSplineSurface.hxx>

#include <Mod/Surface/SurfaceGlobal.h>

#include "EdgeCondition.h"

namespace Surface::Geom
{

enum class Extension
{
    /// The same surface carried on: the polynomial of its last span across
    /// the boundary, evaluated further. Smooth with itself to every order,
    /// G2 included
    Natural,
    /// Ruled along the derivative across the boundary: straight, tangent
    /// to the surface (G1)
    Linear,
};

/** Extend `surface` across one of its boundaries by `length` (docs/Surface.md
 * sec 5.2, Extend per edge)
 *
 * The length is the distance from the boundary to the new one along the
 * isocurves across it, averaged along the boundary: on a surface whose speed
 * across varies along the boundary, the extension is longer where it is
 * faster, as the surface itself is. The surface keeps its degree; what was
 * there stays exactly where it was. Returns why it cannot be extended (a
 * surface periodic across that boundary, a length not positive), or empty.
 */
SurfaceExport std::string extendBoundary(Handle(Geom_BSplineSurface)& surface,
                                         Boundary boundary,
                                         double length,
                                         Extension mode);

}  // namespace Surface::Geom
