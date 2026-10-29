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

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>

#include <Mod/Surface/SurfaceGlobal.h>

namespace Surface::Geom
{

/// How two faces meet along an edge, sampled (docs/Surface.md sec 7, Edge
/// continuity): the readout behind every "achieved" number
struct Continuity
{
    /// The worst distance between the two faces at the edge
    double maxGap = 0;
    /// The worst angle between them (radians), whichever way their normals
    /// point: 0 is tangent
    double maxAngle = 0;
    /// The worst difference of their normal curvature across the edge
    /// (1/length), their normals turned the same way
    double maxCurvatureGap = 0;
    /// Where each worst is
    gp_Pnt worstGap, worstAngle, worstCurvature;
    /// Why it could not be measured; empty when it was
    std::string error;

    /// The continuity the faces meet with within the tolerances: 0, 1 or 2;
    /// -1 when not even in position
    int order(double tolerance = 1e-4,
              double angularTolerance = 0.1 * 3.14159265358979323846 / 180,
              double curvatureTolerance = 1e-3) const
    {
        if (maxGap > tolerance) {
            return -1;
        }
        if (maxAngle > angularTolerance) {
            return 0;
        }
        return maxCurvatureGap > curvatureTolerance ? 1 : 2;
    }
};

/// How `face1` and `face2` meet along `edge`, which lies on both, at
/// `samples` points along it
SurfaceExport Continuity edgeContinuity(const TopoDS_Edge& edge,
                                        const TopoDS_Face& face1,
                                        const TopoDS_Face& face2,
                                        int samples = 50);

}  // namespace Surface::Geom
