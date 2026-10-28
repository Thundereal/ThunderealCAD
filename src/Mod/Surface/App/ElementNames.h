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

#include <vector>

#include <Mod/Part/App/TopoShape.h>
#include <Mod/Surface/SurfaceGlobal.h>

namespace Surface
{

/// Op codes of the Surface features that Part has none for
namespace OpCodes
{
constexpr const char* Extend = "SXT";
constexpr const char* BlendCurve = "SBC";
constexpr const char* Network = "SNW";
constexpr const char* Split = "SSP";
constexpr const char* Trim = "STR";
constexpr const char* Untrim = "SUT";
}  // namespace OpCodes

/** Name a shape built from curves alone, which has no shape history
 *
 * Each edge of `result` that one of `edges` lies on -- ends and midpoint
 * within `tol` or the edges' own tolerance -- is modified from it, and so are
 * its ends. Every face is generated from all the inputs, so an input that
 * lies on no edge (a loft's middle section) still names the face. The rest
 * is named from those, as makESHAPE does.
 */
SurfaceExport Part::TopoShape nameFromEdges(const TopoDS_Shape& result,
                                            const std::vector<Part::TopoShape>& edges,
                                            const App::StringHasherRef& hasher,
                                            const char* op,
                                            double tol = 1e-6);

}  // namespace Surface
