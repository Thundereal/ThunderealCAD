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

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <Mod/Surface/SurfaceGlobal.h>

namespace Surface::Geom
{

/** The edges of `tools` laid onto `face` (docs/Surface.md sec 5.2): along
 * `direction`, or, when it is null, along the face's normal. An edge already
 * on the face (within `tolerance`) is kept as it is. What misses the face
 * altogether is left out; the result says nothing about whether it crosses
 * the face from side to side, which is the split's to find.
 */
SurfaceExport std::vector<TopoDS_Shape> projectOnto(const TopoDS_Face& face,
                                                    const std::vector<TopoDS_Shape>& tools,
                                                    const gp_Dir* direction,
                                                    double tolerance);

/// The piece of `pieces` nearest `point`, or -1 when there are none. Two
/// pieces nearer than `tolerance` to each other in their distance to it are
/// a tie, reported in `tie` (the second), for the caller to break
SurfaceExport int nearestPiece(const std::vector<TopoDS_Face>& pieces,
                               const gp_Pnt& point,
                               double tolerance,
                               int* tie = nullptr);

}  // namespace Surface::Geom
